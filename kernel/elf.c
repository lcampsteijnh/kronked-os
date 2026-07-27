/* elf.c -- minimal ELF32 loader
 *
 * Parses just enough of the ELF32 format to load a statically-linked,
 * non-PIE executable: the ELF header (to find the program header table
 * and entry point) and PT_LOAD segments (to know what to map where).
 */

#include "elf.h"
#include "pmm.h"
#include "paging.h"
#include "serial.h"

#define PT_LOAD 1
#define PAGE_SIZE 4096u

struct elf32_ehdr {
    unsigned char e_ident[16];
    unsigned short e_type;
    unsigned short e_machine;
    unsigned int   e_version;
    unsigned int   e_entry;
    unsigned int   e_phoff;
    unsigned int   e_shoff;
    unsigned int   e_flags;
    unsigned short e_ehsize;
    unsigned short e_phentsize;
    unsigned short e_phnum;
    unsigned short e_shentsize;
    unsigned short e_shnum;
    unsigned short e_shstrndx;
} __attribute__((packed));

struct elf32_phdr {
    unsigned int p_type;
    unsigned int p_offset;
    unsigned int p_vaddr;
    unsigned int p_paddr;
    unsigned int p_filesz;
    unsigned int p_memsz;
    unsigned int p_flags;
    unsigned int p_align;
} __attribute__((packed));

#define PAGE_RW   0x2
#define PAGE_USER 0x4

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

int elf_load(const unsigned char *elf_data, unsigned int dir_phys, unsigned int *entry_out) {
    const struct elf32_ehdr *eh = (const struct elf32_ehdr *)elf_data;

    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F') {
        serial_write("[serial] elf_load: bad magic, not an ELF file\n");
        return 0;
    }
    if (eh->e_ident[4] != 1) { /* ELFCLASS32 */
        serial_write("[serial] elf_load: not a 32-bit ELF\n");
        return 0;
    }

    serial_write("[serial] elf_load: valid ELF32, entry=0x");
    print_hex(eh->e_entry);
    serial_write(", phnum=");
    print_hex(eh->e_phnum);
    serial_write("\n");

    const struct elf32_phdr *ph =
        (const struct elf32_phdr *)(elf_data + eh->e_phoff);

    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;

        serial_write("[serial] elf_load: PT_LOAD vaddr=0x");
        print_hex(ph[i].p_vaddr);
        serial_write(" filesz=0x");
        print_hex(ph[i].p_filesz);
        serial_write(" memsz=0x");
        print_hex(ph[i].p_memsz);
        serial_write("\n");

        unsigned int seg_start = ph[i].p_vaddr & ~(PAGE_SIZE - 1);
        unsigned int seg_end   = (ph[i].p_vaddr + ph[i].p_memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        unsigned int num_pages = (seg_end - seg_start) / PAGE_SIZE;

        for (unsigned int p = 0; p < num_pages; p++) {
            unsigned int page_vaddr = seg_start + p * PAGE_SIZE;
            unsigned int phys = pmm_alloc_frame();
            if (phys == 0) {
                serial_write("[serial] elf_load: out of physical memory!\n");
                return 0;
            }

            /* Zero the frame first (covers the memsz > filesz case,
             * i.e. .bss -- bytes that must be zero but aren't in the
             * file). We write via the physical address directly since
             * it's inside our identity-mapped range regardless of
             * which page directory is currently active in CR3. */
            unsigned char *dst_phys = (unsigned char *)phys;
            for (unsigned int z = 0; z < PAGE_SIZE; z++) dst_phys[z] = 0;

            /* Copy whatever file bytes fall within this page. */
            unsigned int page_file_start = (page_vaddr > ph[i].p_vaddr) ? page_vaddr : ph[i].p_vaddr;
            unsigned int seg_file_end = ph[i].p_vaddr + ph[i].p_filesz;
            unsigned int page_file_end = page_vaddr + PAGE_SIZE;
            if (page_file_end > seg_file_end) page_file_end = seg_file_end;

            if (page_file_start < page_file_end) {
                unsigned int copy_len = page_file_end - page_file_start;
                unsigned int src_offset = ph[i].p_offset + (page_file_start - ph[i].p_vaddr);
                unsigned int dst_offset = page_file_start - page_vaddr;
                const unsigned char *src = elf_data + src_offset;
                for (unsigned int b = 0; b < copy_len; b++) dst_phys[dst_offset + b] = src[b];
            }

            paging_map_user_page(dir_phys, phys, page_vaddr, PAGE_RW | PAGE_USER);
        }
    }

    *entry_out = eh->e_entry;
    return 1;
}
