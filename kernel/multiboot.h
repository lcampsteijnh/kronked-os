#ifndef MULTIBOOT_H
#define MULTIBOOT_H

struct multiboot_info {
    unsigned int flags;
    unsigned int mem_lower;
    unsigned int mem_upper;
    unsigned int boot_device;
    unsigned int cmdline;
    unsigned int mods_count;
    unsigned int mods_addr;
    unsigned int syms[4];
    unsigned int mmap_length;
    unsigned int mmap_addr;
    /* more fields exist but we don't need them yet */
} __attribute__((packed));

struct multiboot_mmap_entry {
    unsigned int size;       /* size of this entry, NOT including this field */
    unsigned long long addr;
    unsigned long long len;
    unsigned int type;       /* 1 = available RAM, others = reserved/ACPI/etc */
} __attribute__((packed));

#define MULTIBOOT_MEMORY_AVAILABLE 1

#endif
