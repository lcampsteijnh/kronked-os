/* paging.c -- x86 32-bit paging setup
 *
 * We identity-map the first 16MiB of physical memory (kernel lives in
 * there, along with the frames the PMM will hand out early on). This is
 * the simplest correct paging setup; higher-half remapping and demand
 * paging come later.
 */

#include "paging.h"
#include "serial.h"
#include "pmm.h"

#define PAGE_PRESENT 0x1
#define PAGE_RW      0x2
#define ENTRIES_PER_TABLE 1024
#define IDENTITY_MAP_TABLES 16  /* 16 tables * 1024 pages * 4KiB = 64MiB */

/* Must be page-aligned (4096 bytes) -- the CPU requires this for CR3
 * and for every page table pointed to by the page directory. */
__attribute__((aligned(4096)))
static unsigned int page_directory[ENTRIES_PER_TABLE];

__attribute__((aligned(4096)))
static unsigned int page_tables[IDENTITY_MAP_TABLES][ENTRIES_PER_TABLE];

#define PAGE_USER 0x4
/* Bit 9 is one of three bits (9,10,11) the x86 architecture reserves
 * for OS use in a page table entry -- the CPU never interprets them
 * itself, so it's safe to use one to mark "this page is copy-on-write
 * shared" without disturbing anything hardware-level. */
#define PAGE_COW 0x200
static unsigned int kernel_directory_phys;

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

void paging_init(void) {
    for (int i = 0; i < ENTRIES_PER_TABLE; i++) page_directory[i] = 0;

    for (int t = 0; t < IDENTITY_MAP_TABLES; t++) {
        for (int i = 0; i < ENTRIES_PER_TABLE; i++) {
            unsigned int phys = (unsigned int)(t * ENTRIES_PER_TABLE + i) * 4096;
            page_tables[t][i] = phys | PAGE_PRESENT | PAGE_RW;
        }
        page_directory[t] = ((unsigned int)page_tables[t]) | PAGE_PRESENT | PAGE_RW;
    }

    serial_write("[serial] paging: identity-mapped 0x0 - 0x");
    print_hex(IDENTITY_MAP_TABLES * ENTRIES_PER_TABLE * 4096);
    serial_write("\n");

    __asm__ volatile ("mov %0, %%cr3" : : "r"(page_directory));
    kernel_directory_phys = (unsigned int)page_directory;

    unsigned int cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000; /* set PG bit */
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));

    serial_write("[serial] paging: CR0.PG set, paging is now active.\n");
}

int paging_map_page(unsigned int phys_addr, unsigned int virt_addr, unsigned int flags) {
    unsigned int pd_index = virt_addr >> 22;
    unsigned int pt_index = (virt_addr >> 12) & 0x3FF;

    if (pd_index >= IDENTITY_MAP_TABLES) {
        /* We only pre-allocated tables for the identity-mapped region.
         * A real allocator for new page tables comes with higher-half /
         * dynamic mapping support later. */
        return 0;
    }

    page_tables[pd_index][pt_index] = (phys_addr & 0xFFFFF000) | (flags & 0xFFF) | PAGE_PRESENT;

    __asm__ volatile ("invlpg (%0)" : : "r"(virt_addr) : "memory");
    return 1;
}

/* Allocate a fresh page directory for a new task. Kernel-space entries
 * (covering our identity-mapped region, where all kernel code/data and
 * every PMM-allocated frame lives) point at the *same* shared page
 * tables as the main kernel directory -- so kernel code and the heap
 * are visible identically no matter which task's directory is active.
 * User-space entries start empty; the caller fills them in per-task via
 * paging_map_user_page(). */
unsigned int paging_clone_kernel_directory(void) {
    unsigned int phys = pmm_alloc_frame();
    unsigned int *dir = (unsigned int *)phys; /* valid: phys is within our identity map */

    /* Copy the *entire* directory, not just the identity-mapped range:
     * anything present at index >= IDENTITY_MAP_TABLES only got there
     * via paging_map_kernel_page() (shared kernel resources, e.g. the
     * framebuffer) -- never a per-task user mapping, those always go
     * into each task's own cloned directory separately. So copying
     * everything is both safe and necessary for newly-mapped kernel
     * resources to be visible in tasks created after the mapping. */
    for (int i = 0; i < ENTRIES_PER_TABLE; i++) dir[i] = page_directory[i];

    return phys;
}

/* Maps a page into a *specific* task's directory (not necessarily the
 * one currently active in CR3) at a user-space virtual address, i.e.
 * pd_index >= IDENTITY_MAP_TABLES. Allocates a fresh page table on
 * demand if this is the first mapping to fall in that 4MiB region.
 * Because dir/table pointers are physical addresses that happen to sit
 * inside our (now 64MiB) identity map, we can poke them directly by
 * pointer even though the directory being edited may not be the one
 * currently loaded in CR3. */
int paging_map_user_page(unsigned int dir_phys, unsigned int phys_addr,
                          unsigned int virt_addr, unsigned int flags) {
    unsigned int *dir = (unsigned int *)dir_phys;
    unsigned int pd_index = virt_addr >> 22;
    unsigned int pt_index = (virt_addr >> 12) & 0x3FF;

    if (pd_index < IDENTITY_MAP_TABLES) {
        serial_write("[serial] paging_map_user_page: refusing to remap kernel space\n");
        return 0;
    }

    unsigned int *table;
    if (!(dir[pd_index] & PAGE_PRESENT)) {
        unsigned int table_phys = pmm_alloc_frame();
        table = (unsigned int *)table_phys;
        for (int i = 0; i < ENTRIES_PER_TABLE; i++) table[i] = 0;
        dir[pd_index] = table_phys | PAGE_PRESENT | PAGE_RW | PAGE_USER;
    } else {
        table = (unsigned int *)(dir[pd_index] & 0xFFFFF000);
    }

    table[pt_index] = (phys_addr & 0xFFFFF000) | (flags & 0xFFF) | PAGE_PRESENT;
    return 1;
}

void paging_switch_directory(unsigned int dir_phys) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(dir_phys) : "memory");
}

unsigned int paging_kernel_directory_phys(void) {
    return kernel_directory_phys;
}

/* Maps a page into the *shared kernel directory* itself, allocating a
 * new page table on demand if needed -- unlike paging_map_page(),
 * which only works within the pre-built identity-mapped region. Since
 * every task's directory shares the kernel's page tables by pointer
 * (see paging_clone_kernel_directory), anything mapped here becomes
 * visible in every address space immediately, which is exactly what
 * we want for the framebuffer: one physical resource, needs to be
 * readable/writable no matter which task happens to be running. */
int paging_map_kernel_page(unsigned int phys_addr, unsigned int virt_addr, unsigned int flags) {
    unsigned int pd_index = virt_addr >> 22;
    unsigned int pt_index = (virt_addr >> 12) & 0x3FF;

    unsigned int *table;
    if (pd_index < IDENTITY_MAP_TABLES) {
        table = page_tables[pd_index];
    } else if (page_directory[pd_index] & PAGE_PRESENT) {
        table = (unsigned int *)(page_directory[pd_index] & 0xFFFFF000);
    } else {
        unsigned int table_phys = pmm_alloc_frame();
        if (table_phys == 0) return 0;
        table = (unsigned int *)table_phys;
        for (int i = 0; i < ENTRIES_PER_TABLE; i++) table[i] = 0;
        page_directory[pd_index] = table_phys | PAGE_PRESENT | PAGE_RW;
    }

    table[pt_index] = (phys_addr & 0xFFFFF000) | (flags & 0xFFF) | PAGE_PRESENT;
    __asm__ volatile ("invlpg (%0)" : : "r"(virt_addr) : "memory");
    return 1;
}

/* fork() support: walks every present user-space page table entry in
 * the parent's directory and, for each one, allocates a fresh
 * physical frame, copies the page's actual contents into it, and maps
 * that new frame at the *same* virtual address in a brand new
 * directory. The result is a full, independent copy of the parent's
 * user-space memory image -- not copy-on-write (that's the efficient
 * version real kernels use; this is the simple, correct first version).
 * Kernel-space entries come along "for free" already shared, via
 * paging_clone_kernel_directory(). */
/* Copy-on-write fork: rather than eagerly copying every page's
 * contents right now, share the same physical frame between parent
 * and child, with write access stripped from *both* sides' PTEs and
 * the COW bit set. Neither side notices anything until one of them
 * actually writes -- at which point paging_handle_cow_fault() below
 * either reclaims sole ownership (no copy needed, if the other side
 * already dropped its reference) or makes a real copy (if genuinely
 * still shared). This is the efficient, standard version of fork();
 * the previous eager-copy version worked but copied every page's
 * contents immediately whether or not either process ever actually
 * wrote to it again. */
unsigned int paging_clone_address_space(unsigned int parent_dir_phys) {
    unsigned int child_dir_phys = paging_clone_kernel_directory();
    unsigned int *parent_dir = (unsigned int *)parent_dir_phys;

    for (int pd = IDENTITY_MAP_TABLES; pd < ENTRIES_PER_TABLE; pd++) {
        if (!(parent_dir[pd] & PAGE_PRESENT)) continue;

        unsigned int *parent_table = (unsigned int *)(parent_dir[pd] & 0xFFFFF000);
        for (int pt = 0; pt < ENTRIES_PER_TABLE; pt++) {
            if (!(parent_table[pt] & PAGE_PRESENT)) continue;

            unsigned int phys = parent_table[pt] & 0xFFFFF000;
            unsigned int flags = parent_table[pt] & 0xFFF & ~(unsigned int)PAGE_PRESENT;
            unsigned int cow_flags = (flags & ~(unsigned int)PAGE_RW) | PAGE_COW;

            pmm_frame_addref(phys); /* new reference: the child's mapping */

            unsigned int virt = ((unsigned int)pd << 22) | ((unsigned int)pt << 12);

            /* The parent's own PTE must also lose write access now --
             * otherwise a subsequent parent write would silently
             * corrupt the frame contents out from under the child. */
            parent_table[pt] = (phys & 0xFFFFF000) | (cow_flags & 0xFFF) | PAGE_PRESENT;
            __asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");

            paging_map_user_page(child_dir_phys, phys, virt, cow_flags);
        }
    }

    return child_dir_phys;
}

/* Called from idt.c's page fault handler for a write fault on a page
 * that IS present (a genuine protection violation, not a missing
 * mapping -- which is exactly what a COW page produces, since COW
 * pages are always present but deliberately read-only). Returns 1 if
 * this was a COW fault and has now been resolved (the caller should
 * just let iret retry the faulting instruction); returns 0 if this
 * wasn't COW-related, so the caller should treat it as a genuine
 * fatal fault. */
int paging_handle_cow_fault(unsigned int fault_addr) {
    unsigned int dir_phys;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(dir_phys));

    unsigned int pd_index = fault_addr >> 22;
    unsigned int pt_index = (fault_addr >> 12) & 0x3FF;

    unsigned int *dir = (unsigned int *)dir_phys;
    if (!(dir[pd_index] & PAGE_PRESENT)) return 0;

    unsigned int *table = (unsigned int *)(dir[pd_index] & 0xFFFFF000);
    unsigned int pte = table[pt_index];

    if (!(pte & PAGE_PRESENT)) return 0;
    if (!(pte & PAGE_COW)) return 0; /* present but not COW -- a real protection fault */

    unsigned int old_phys = pte & 0xFFFFF000;
    unsigned int flags = pte & 0xFFF & ~(unsigned int)PAGE_COW;

    unsigned int refcount = pmm_frame_refcount(old_phys);

    if (refcount <= 1) {
        /* Sole remaining owner (the other sharer already resolved its
         * own fault, or exited) -- no copy needed, just reclaim write
         * access to the frame we already have. */
        table[pt_index] = (old_phys & 0xFFFFF000) | (flags | PAGE_RW) | PAGE_PRESENT;
    } else {
        /* Genuinely still shared -- make a real copy, since whoever
         * else references this frame still needs its contents intact. */
        unsigned int new_phys = pmm_alloc_frame();
        if (new_phys == 0) {
            serial_write("[serial] paging_handle_cow_fault: out of memory!\n");
            return 0;
        }

        unsigned char *src = (unsigned char *)old_phys;
        unsigned char *dst = (unsigned char *)new_phys;
        for (int b = 0; b < 4096; b++) dst[b] = src[b];

        pmm_free_frame(old_phys); /* drop our reference to the shared frame */

        table[pt_index] = (new_phys & 0xFFFFF000) | (flags | PAGE_RW) | PAGE_PRESENT;
    }

    __asm__ volatile ("invlpg (%0)" : : "r"(fault_addr) : "memory");
    return 1;
}

/* Symmetric teardown for paging_clone_address_space(): walks every
 * present user-space PDE/PTE (indices >= IDENTITY_MAP_TABLES -- the
 * shared kernel range is never touched) and frees both the mapped
 * physical frame and, once each table's entries are done, the page
 * table's own frame. Does NOT free the directory frame itself --
 * that's the caller's responsibility (task_wait_specific already
 * does it separately), since the directory is used until the very
 * last moment of teardown.
 *
 * Safe to call even for a task that mapped the framebuffer (or any
 * other physical resource outside our PMM's tracked range): pmm_free_frame
 * -> mark_free bounds-checks the frame number and silently no-ops for
 * anything outside MAX_FRAMES, so hardware resources like the
 * framebuffer are correctly left alone rather than being incorrectly
 * "freed" back into the general allocator (they were never PMM-owned
 * memory in the first place). */
void paging_free_address_space(unsigned int dir_phys) {
    unsigned int *dir = (unsigned int *)dir_phys;

    for (int pd = IDENTITY_MAP_TABLES; pd < ENTRIES_PER_TABLE; pd++) {
        if (!(dir[pd] & PAGE_PRESENT)) continue;

        unsigned int table_phys = dir[pd] & 0xFFFFF000;
        unsigned int *table = (unsigned int *)table_phys;

        for (int pt = 0; pt < ENTRIES_PER_TABLE; pt++) {
            if (!(table[pt] & PAGE_PRESENT)) continue;
            unsigned int frame_phys = table[pt] & 0xFFFFF000;
            pmm_free_frame(frame_phys);
            table[pt] = 0;
        }

        pmm_free_frame(table_phys);
        dir[pd] = 0;
    }
}
