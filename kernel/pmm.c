/* pmm.c -- Physical Memory Manager
 *
 * Tracks every 4KiB physical frame with one bit in a bitmap: 1 = used,
 * 0 = free. Built from the memory map the bootloader hands us,
 * with the kernel's own image and the first 1MiB (BIOS/legacy reserved
 * area) marked used up front.
 */

#include "pmm.h"
#include "multiboot.h"
#include "serial.h"
#include "spinlock.h"

#define FRAME_SIZE 4096

static spinlock_t pmm_lock;

extern char _kernel_start[];
extern char _kernel_end[];

/* Bitmap covering up to 128 MiB of physical RAM (32768 frames / 8 = 4096 bytes).
 * That's plenty for a hobby kernel at this stage; extending it later just
 * means sizing this to the actual detected top of memory. */
#define MAX_FRAMES (128 * 1024 * 1024 / FRAME_SIZE)
static unsigned char frame_bitmap[MAX_FRAMES / 8];
static unsigned int total_frames = 0;
static unsigned int used_frames = 0;
static unsigned long long detected_ram_bytes = 0; /* real total from the memory map, not our tracking cap */

/* One reference count per frame, needed for copy-on-write fork: after
 * a COW fork, parent and child both legitimately point at the same
 * physical frame until one of them writes to it. A frame is only
 * actually returned to the free bitmap once its last reference goes
 * away (refcount reaches 0), so pmm_free_frame() is safe to call once
 * per *mapping* that used to point at a frame, not just once ever. */
static unsigned char frame_refcount[MAX_FRAMES];

static void bitmap_set(unsigned int frame) {
    frame_bitmap[frame / 8] |= (1 << (frame % 8));
}
static void bitmap_clear(unsigned int frame) {
    frame_bitmap[frame / 8] &= ~(1 << (frame % 8));
}
static int bitmap_test(unsigned int frame) {
    return frame_bitmap[frame / 8] & (1 << (frame % 8));
}

static void mark_used(unsigned int frame) {
    if (frame >= MAX_FRAMES) return;
    if (!bitmap_test(frame)) { bitmap_set(frame); used_frames++; }
}
static void mark_free(unsigned int frame) {
    if (frame >= MAX_FRAMES) return;
    if (bitmap_test(frame)) { bitmap_clear(frame); used_frames--; }
}

static void print_hex(unsigned int n) {
    char buf[9];
    buf[8] = '\0';
    for (int i = 7; i >= 0; i--) {
        unsigned int nib = n & 0xF;
        buf[i] = (char)(nib < 10 ? '0' + nib : 'a' + (nib - 10));
        n >>= 4;
    }
    serial_write(buf);
}

void pmm_init(unsigned int mb_magic, struct multiboot_info *mbi) {
    (void)mb_magic;
    spinlock_init(&pmm_lock);

    /* Start pessimistic: everything used. Then free what the memory
     * map says is actually available RAM. */
    for (unsigned int i = 0; i < MAX_FRAMES; i++) bitmap_set(i);
    used_frames = MAX_FRAMES;

    struct multiboot_mmap_entry *entry = (struct multiboot_mmap_entry *)mbi->mmap_addr;
    unsigned int end = mbi->mmap_addr + mbi->mmap_length;

    while ((unsigned int)entry < end) {
        if (entry->type == MULTIBOOT_MEMORY_AVAILABLE) {
            unsigned long long start_addr = entry->addr;
            unsigned long long len = entry->len;
            detected_ram_bytes += len;
            unsigned int start_frame = (unsigned int)(start_addr / FRAME_SIZE);
            unsigned int frame_count = (unsigned int)(len / FRAME_SIZE);
            for (unsigned int f = start_frame; f < start_frame + frame_count && f < MAX_FRAMES; f++)
                mark_free(f);
        }
        entry = (struct multiboot_mmap_entry *)((unsigned int)entry + entry->size + sizeof(entry->size));
    }

    /* Reserve the first 1MiB (BIOS, video memory, legacy stuff) no matter
     * what the map claims, and reserve the kernel's own image so we never
     * hand out memory we're currently executing/storing data in. */
    for (unsigned int f = 0; f < (0x100000 / FRAME_SIZE); f++) mark_used(f);

    unsigned int kstart_frame = (unsigned int)_kernel_start / FRAME_SIZE;
    unsigned int kend_frame   = ((unsigned int)_kernel_end + FRAME_SIZE - 1) / FRAME_SIZE;
    for (unsigned int f = kstart_frame; f <= kend_frame; f++) mark_used(f);

    total_frames = MAX_FRAMES;

    serial_write("[serial] PMM: used frames = ");
    print_hex(used_frames);
    serial_write(", free frames = ");
    print_hex(total_frames - used_frames);
    serial_write("\n");
}

/* Returns physical address of a free frame, or 0 if out of memory. */
unsigned int pmm_alloc_frame(void) {
    unsigned int flags = spinlock_acquire(&pmm_lock);
    for (unsigned int i = 0; i < MAX_FRAMES; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            used_frames++;
            frame_refcount[i] = 1;
            spinlock_release(&pmm_lock, flags);
            return i * FRAME_SIZE;
        }
    }
    spinlock_release(&pmm_lock, flags);
    return 0; /* out of memory */
}

/* Decrements the frame's reference count; only actually returns it to
 * the free bitmap once the count reaches 0. Safe to call once per
 * mapping that referenced this frame (e.g. once from the parent's
 * address-space teardown and once from the child's, for a COW-shared
 * page -- whichever happens last is the one that actually frees it). */
void pmm_free_frame(unsigned int phys_addr) {
    unsigned int flags = spinlock_acquire(&pmm_lock);
    unsigned int frame = phys_addr / FRAME_SIZE;
    if (frame < MAX_FRAMES && frame_refcount[frame] > 0) {
        frame_refcount[frame]--;
        if (frame_refcount[frame] == 0) {
            mark_free(frame);
        }
    }
    spinlock_release(&pmm_lock, flags);
}

/* Adds a reference to an already-allocated frame -- used when a COW
 * fork makes a second address space start pointing at the same
 * physical frame as the first. */
void pmm_frame_addref(unsigned int phys_addr) {
    unsigned int flags = spinlock_acquire(&pmm_lock);
    unsigned int frame = phys_addr / FRAME_SIZE;
    if (frame < MAX_FRAMES) {
        frame_refcount[frame]++;
    }
    spinlock_release(&pmm_lock, flags);
}

unsigned int pmm_frame_refcount(unsigned int phys_addr) {
    unsigned int frame = phys_addr / FRAME_SIZE;
    if (frame >= MAX_FRAMES) return 0;
    return frame_refcount[frame];
}

unsigned int pmm_free_frame_count(void) {
    return total_frames - used_frames;
}

/* Real detected RAM, from the multiboot memory map -- not our fixed
 * 128MiB tracking capacity (MAX_FRAMES), which is just how much of
 * that RAM we're actually able to individually track frame-by-frame.
 * Returned in KiB so it fits comfortably in a plain unsigned int for
 * any realistic amount of RAM. */
unsigned int pmm_detected_ram_kib(void) {
    return (unsigned int)(detected_ram_bytes / 1024);
}
