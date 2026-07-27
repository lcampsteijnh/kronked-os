/* heap.c -- kernel heap allocator (kmalloc / kfree)
 *
 * Simple design for now: a linked list of blocks carved out of pages we
 * pull from the PMM as the heap grows. Each block has a small header
 * with its size and a free flag; kfree just marks the block free and
 * kmalloc does a first-fit scan, splitting blocks when there's enough
 * leftover space to be worth it. Not fast, not fancy -- but real,
 * working dynamic memory.
 */

#include "heap.h"
#include "pmm.h"
#include "paging.h"
#include "serial.h"
#include "spinlock.h"

#define HEAP_START 0x00400000u   /* 4MiB -- inside our identity-mapped region */
#define HEAP_INITIAL_PAGES 4     /* 16KiB to start */
#define PAGE_SIZE 4096u

struct block_header {
    unsigned int size;           /* size of the usable region after this header */
    int free;
    struct block_header *next;
};

static unsigned int heap_end = HEAP_START;   /* first byte not yet mapped */
static struct block_header *heap_head = 0;
static spinlock_t heap_lock;

static void heap_extend(unsigned int npages) {
    for (unsigned int i = 0; i < npages; i++) {
        unsigned int phys = pmm_alloc_frame();
        if (phys == 0) {
            serial_write("[serial] heap_extend: out of physical memory!\n");
            return;
        }
        paging_map_page(phys, heap_end, 0x2 /* RW */);
        heap_end += PAGE_SIZE;
    }
}

void heap_init(void) {
    spinlock_init(&heap_lock);
    heap_extend(HEAP_INITIAL_PAGES);
    heap_head = (struct block_header *)HEAP_START;
    heap_head->size = (HEAP_INITIAL_PAGES * PAGE_SIZE) - sizeof(struct block_header);
    heap_head->free = 1;
    heap_head->next = 0;
    serial_write("[serial] heap: initialized at 0x00400000\n");
}

void *kmalloc(unsigned int size) {
    if (size == 0) return 0;
    unsigned int flags = spinlock_acquire(&heap_lock);

    /* align to 4 bytes */
    size = (size + 3) & ~3u;

    struct block_header *cur = heap_head;
    struct block_header *prev = 0;

    while (cur) {
        if (cur->free && cur->size >= size) {
            /* split if there's meaningfully more room than needed */
            if (cur->size >= size + sizeof(struct block_header) + 16) {
                struct block_header *split =
                    (struct block_header *)((unsigned char *)cur + sizeof(struct block_header) + size);
                split->size = cur->size - size - sizeof(struct block_header);
                split->free = 1;
                split->next = cur->next;

                cur->size = size;
                cur->next = split;
            }
            cur->free = 0;
            void *result = (void *)((unsigned char *)cur + sizeof(struct block_header));
            spinlock_release(&heap_lock, flags);
            return result;
        }
        prev = cur;
        cur = cur->next;
    }

    /* No free block big enough -- grow the heap. */
    unsigned int needed = size + sizeof(struct block_header);
    unsigned int pages = (needed + PAGE_SIZE - 1) / PAGE_SIZE;
    unsigned int old_end = heap_end;
    heap_extend(pages);

    struct block_header *fresh = (struct block_header *)old_end;
    fresh->size = pages * PAGE_SIZE - sizeof(struct block_header);
    fresh->free = 0;
    fresh->next = 0;

    if (prev) prev->next = fresh;
    else heap_head = fresh;

    void *result = (void *)((unsigned char *)fresh + sizeof(struct block_header));
    spinlock_release(&heap_lock, flags);
    return result;
}

void kfree(void *ptr) {
    if (!ptr) return;
    unsigned int flags = spinlock_acquire(&heap_lock);

    struct block_header *hdr =
        (struct block_header *)((unsigned char *)ptr - sizeof(struct block_header));
    hdr->free = 1;

    /* coalesce adjacent free blocks (single pass, good enough for now) */
    struct block_header *cur = heap_head;
    while (cur && cur->next) {
        if (cur->free && cur->next->free) {
            cur->size += sizeof(struct block_header) + cur->next->size;
            cur->next = cur->next->next;
        } else {
            cur = cur->next;
        }
    }

    spinlock_release(&heap_lock, flags);
}
