/* gdt.c -- Global Descriptor Table setup
 *
 * Even though we don't use x86 segmentation for memory protection
 * (we use paging for that), the CPU still requires a valid GDT with
 * flat code/data segments, plus a
 * TSS descriptor for ring 3. The TSS's esp0/ss0 fields tell the CPU which kernel
 * stack to switch to automatically whenever an interrupt or syscall
 * arrives while running usermode code.
 */

#include "gdt.h"

struct gdt_entry {
    unsigned short limit_low;
    unsigned short base_low;
    unsigned char  base_middle;
    unsigned char  access;
    unsigned char  granularity;
    unsigned char  base_high;
} __attribute__((packed));

struct gdt_ptr {
    unsigned short limit;
    unsigned int   base;
} __attribute__((packed));

struct tss_entry {
    unsigned int prev_tss;
    unsigned int esp0;      /* kernel stack pointer used on ring3->ring0 transition to provide safe "working space" */
    unsigned int ss0;       /* kernel stack segment, same purpose */
    unsigned int esp1, ss1, esp2, ss2;
    unsigned int cr3, eip, eflags;
    unsigned int eax, ecx, edx, ebx, esp, ebp, esi, edi;
    unsigned int es, cs, ss, ds, fs, gs;
    unsigned int ldt;
    unsigned short trap, iomap_base;
} __attribute__((packed));

#define GDT_ENTRIES 6
static struct gdt_entry gdt[GDT_ENTRIES];
static struct gdt_ptr   gdtp;
static struct tss_entry tss;

/* implemented in isr_stubs.s. Loads the GDT register and reloads segments */
extern void gdt_flush(unsigned int gdtp_addr);

static void gdt_set_entry(int i, unsigned int base, unsigned int limit,
                           unsigned char access, unsigned char gran) {
    gdt[i].base_low    = (unsigned short)(base & 0xFFFF);
    gdt[i].base_middle  = (unsigned char)((base >> 16) & 0xFF);
    gdt[i].base_high    = (unsigned char)((base >> 24) & 0xFF);
    gdt[i].limit_low    = (unsigned short)(limit & 0xFFFF);
    gdt[i].granularity  = (unsigned char)((limit >> 16) & 0x0F);
    gdt[i].granularity |= (gran & 0xF0);
    gdt[i].access       = access;
}

void gdt_init(void) {
    gdtp.limit = sizeof(gdt) - 1;
    gdtp.base  = (unsigned int)&gdt;

    gdt_set_entry(0, 0, 0, 0, 0);                       /* null descriptor */
    gdt_set_entry(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);         /* kernel code: base 0, 4GiB, ring0 */
    gdt_set_entry(2, 0, 0xFFFFFFFF, 0x92, 0xCF);         /* kernel data: base 0, 4GiB, ring0 */
    gdt_set_entry(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);         /* user code: ring3 */
    gdt_set_entry(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);         /* user data: ring3 */

    unsigned int tss_base = (unsigned int)&tss;
    unsigned int tss_limit = sizeof(tss) - 1;
    for (unsigned int i = 0; i < sizeof(tss); i++) ((unsigned char *)&tss)[i] = 0;
    tss.ss0 = 0x10;   /* kernel data selector, used with esp0 on privilege transition */
    tss.esp0 = 0;      /* filled in per-task by tss_set_kernel_stack() before it runs */
    tss.iomap_base = sizeof(tss);
    gdt_set_entry(5, tss_base, tss_limit, 0x89, 0x40); /* present, ring0, 32-bit TSS */

    gdt_flush((unsigned int)&gdtp);

    __asm__ volatile ("ltr %%ax" : : "a"((unsigned short)0x28)); /* selector = index 5 * 8 */
}

void tss_set_kernel_stack(unsigned int esp0) {
    tss.esp0 = esp0;
}
