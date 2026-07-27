/* idt.c -- Interrupt Descriptor Table, PIC remap, and interrupt dispatch */

#include "idt.h"
#include "vga.h"
#include "serial.h"
#include "paging.h"

struct idt_entry {
    unsigned short base_low;
    unsigned short sel;
    unsigned char  always0;
    unsigned char  flags;
    unsigned short base_high;
} __attribute__((packed));

struct idt_ptr {
    unsigned short limit;
    unsigned int   base;
} __attribute__((packed));

#define IDT_ENTRIES 256
static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr   idtp;

extern void idt_flush(unsigned int idtp_addr);

/* declared in isr_stubs.s: one label per vector */
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void); extern void isr16(void); extern void isr17(void);
extern void isr18(void); extern void isr19(void); extern void isr20(void);
extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void);
extern void isr27(void); extern void isr28(void); extern void isr29(void);
extern void isr30(void); extern void isr31(void);

extern void irq0(void);  extern void irq1(void);  extern void irq2(void);
extern void irq3(void);  extern void irq4(void);  extern void irq5(void);
extern void irq6(void);  extern void irq7(void);  extern void irq8(void);
extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void);
extern void irq15(void);

extern void isr128(void); /* syscall entry, int 0x80 */

static inline void outb(unsigned short port, unsigned char val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline unsigned char inb(unsigned short port) {
    unsigned char ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static void idt_set_gate(unsigned char num, unsigned int base,
                          unsigned short sel, unsigned char flags) {
    idt[num].base_low  = (unsigned short)(base & 0xFFFF);
    idt[num].base_high = (unsigned short)((base >> 16) & 0xFFFF); /* start/end -- address of handler function */
    idt[num].sel        = sel; /* code segment selector */
    idt[num].always0    = 0; 
    idt[num].flags      = flags;
}

/* --- PIC remap ---
 * By default IRQ0-7 map to interrupts 8-15, colliding with CPU exceptions.
 * We remap them to 32-47 so exceptions and hardware IRQs never overlap. */
static void pic_remap(void) {
    outb(0x20, 0x11); outb(0xA0, 0x11);   /* start init sequence (cascade) */
    outb(0x21, 0x20); outb(0xA1, 0x28);   /* master offset 32, slave offset 40 */
    outb(0x21, 0x04); outb(0xA1, 0x02);   /* tell master/slave about each other */
    outb(0x21, 0x01); outb(0xA1, 0x01);   /* 8086 mode */
    outb(0x21, 0x00); outb(0xA1, 0x00);   /* unmask all */
}

static void pic_send_eoi(unsigned char irq) {   /* Tell Programmable Interrupt Controller I/O that interupt handling is finished */
    if (irq >= 8) outb(0xA0, 0x20);             /* Send EOI to Slave PIC */
    outb(0x20, 0x20);                           /* Send to Master PIC */
}

typedef void (*irq_handler_t)(void);
static irq_handler_t irq_routines[16] = {0};

void irq_install_handler(int irq, void (*handler)(void)) {
    irq_routines[irq] = handler;
}

static const char *exception_messages[32] = {
    "Division By Zero", "Debug", "Non Maskable Interrupt", "Breakpoint",
    "Into Detected Overflow", "Out of Bounds", "Invalid Opcode",
    "No Coprocessor", "Double Fault", "Coprocessor Segment Overrun",
    "Bad TSS", "Segment Not Present", "Stack Fault",
    "General Protection Fault", "Page Fault", "Unknown Interrupt",
    "Coprocessor Fault", "Alignment Check", "Machine Check",
    "SIMD Floating-Point Exception", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved"
};

/* struct registers is now declared in idt.h so syscall.c can share it. */

static void print_hex_idt(unsigned int n) {
    char buf[9];
    buf[8] = '\0';
    for (int i = 7; i >= 0; i--) {
        unsigned int nib = n & 0xF;
        buf[i] = (char)(nib < 10 ? '0' + nib : 'a' + (nib - 10));
        n >>= 4;
    }
    serial_write(buf);
}

void isr_handler(struct registers regs) {
    if (regs.int_no == 14) { /* Page Fault -- eg. accessing unmapped memory*/
        unsigned int fault_addr;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(fault_addr)); /* read cr2 -- special CPU control register containing fault-causing address*/

        /* err_code bit0=present, bit1=write. A write fault on an
         * already-present page is exactly what a COW (copy-on-write) page produces
         * (and only what a COW page produces, in our design) -- check
         * before treating this as a real exception at all, since a
         * successfully-resolved COW fault is normal operation, not an
         * error, and printing a scary "CPU EXCEPTION" message for it
         * would be misleading. */
        if ((regs.err_code & 0x3) == 0x3) {
            if (paging_handle_cow_fault(fault_addr)) {
                return; /* resolved -- iret retries the faulting instruction */
            }
        }

        vga_write("\n*** CPU EXCEPTION: ");
        vga_write(exception_messages[regs.int_no]);
        vga_write(" ***\n");
        serial_write("[serial] CPU EXCEPTION: ");
        serial_write(exception_messages[regs.int_no]);
        serial_write("\n");

        serial_write("[serial]   faulting address (CR2) = 0x");
        print_hex_idt(fault_addr);
        serial_write("\n[serial]   present=");
        serial_write((regs.err_code & 0x1) ? "1" : "0");
        serial_write(" write=");
        serial_write((regs.err_code & 0x2) ? "1" : "0");
        serial_write(" user=");
        serial_write((regs.err_code & 0x4) ? "1" : "0");
        serial_write("\n");
    } else {
        vga_write("\n*** CPU EXCEPTION: ");
        vga_write(exception_messages[regs.int_no]);
        vga_write(" ***\n");
        serial_write("[serial] CPU EXCEPTION: ");
        serial_write(exception_messages[regs.int_no]);
        serial_write("\n");
    }

    /* We don't have process isolation yet, so any exception halts. */
    for (;;) { __asm__ volatile ("cli; hlt"); }
}

void irq_handler(struct registers regs) {
    int irq = (int)(regs.int_no - 32);

    /* Must send EOI *before* calling the handler routine: the timer
     * handler calls schedule(), which may not return here for a long
     * time (it context-switches to a different task's stack). If we
     * waited to EOI until after the handler returned, the PIC would
     * consider IRQ0 still "in service" the whole time other tasks are
     * running, and would never deliver another timer tick again. */
    pic_send_eoi((unsigned char)irq); /* Send end-of-interupt */

    if (irq_routines[irq] != 0) {
        irq_routines[irq]();
    }
}

void idt_init(void) {
    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (unsigned int)&idt;

    for (int i = 0; i < IDT_ENTRIES; i++) idt_set_gate(i, 0, 0, 0); /* reset idt */

    pic_remap(); /* remap interupts so they dont overlap with CPU exceptions*/

    idt_set_gate(0,  (unsigned int)isr0,  0x08, 0x8E);
    idt_set_gate(1,  (unsigned int)isr1,  0x08, 0x8E);
    idt_set_gate(2,  (unsigned int)isr2,  0x08, 0x8E);
    idt_set_gate(3,  (unsigned int)isr3,  0x08, 0x8E);
    idt_set_gate(4,  (unsigned int)isr4,  0x08, 0x8E);
    idt_set_gate(5,  (unsigned int)isr5,  0x08, 0x8E);
    idt_set_gate(6,  (unsigned int)isr6,  0x08, 0x8E);
    idt_set_gate(7,  (unsigned int)isr7,  0x08, 0x8E);
    idt_set_gate(8,  (unsigned int)isr8,  0x08, 0x8E);
    idt_set_gate(9,  (unsigned int)isr9,  0x08, 0x8E);
    idt_set_gate(10, (unsigned int)isr10, 0x08, 0x8E);
    idt_set_gate(11, (unsigned int)isr11, 0x08, 0x8E);
    idt_set_gate(12, (unsigned int)isr12, 0x08, 0x8E);
    idt_set_gate(13, (unsigned int)isr13, 0x08, 0x8E);
    idt_set_gate(14, (unsigned int)isr14, 0x08, 0x8E);
    idt_set_gate(15, (unsigned int)isr15, 0x08, 0x8E);
    idt_set_gate(16, (unsigned int)isr16, 0x08, 0x8E);
    idt_set_gate(17, (unsigned int)isr17, 0x08, 0x8E);
    idt_set_gate(18, (unsigned int)isr18, 0x08, 0x8E);
    idt_set_gate(19, (unsigned int)isr19, 0x08, 0x8E);
    idt_set_gate(20, (unsigned int)isr20, 0x08, 0x8E);
    idt_set_gate(21, (unsigned int)isr21, 0x08, 0x8E);
    idt_set_gate(22, (unsigned int)isr22, 0x08, 0x8E);
    idt_set_gate(23, (unsigned int)isr23, 0x08, 0x8E);
    idt_set_gate(24, (unsigned int)isr24, 0x08, 0x8E);
    idt_set_gate(25, (unsigned int)isr25, 0x08, 0x8E);
    idt_set_gate(26, (unsigned int)isr26, 0x08, 0x8E);
    idt_set_gate(27, (unsigned int)isr27, 0x08, 0x8E);
    idt_set_gate(28, (unsigned int)isr28, 0x08, 0x8E);
    idt_set_gate(29, (unsigned int)isr29, 0x08, 0x8E);
    idt_set_gate(30, (unsigned int)isr30, 0x08, 0x8E);
    idt_set_gate(31, (unsigned int)isr31, 0x08, 0x8E);

    idt_set_gate(32, (unsigned int)irq0,  0x08, 0x8E);
    idt_set_gate(33, (unsigned int)irq1,  0x08, 0x8E);
    idt_set_gate(34, (unsigned int)irq2,  0x08, 0x8E);
    idt_set_gate(35, (unsigned int)irq3,  0x08, 0x8E);
    idt_set_gate(36, (unsigned int)irq4,  0x08, 0x8E);
    idt_set_gate(37, (unsigned int)irq5,  0x08, 0x8E);
    idt_set_gate(38, (unsigned int)irq6,  0x08, 0x8E);
    idt_set_gate(39, (unsigned int)irq7,  0x08, 0x8E);
    idt_set_gate(40, (unsigned int)irq8,  0x08, 0x8E);
    idt_set_gate(41, (unsigned int)irq9,  0x08, 0x8E);
    idt_set_gate(42, (unsigned int)irq10, 0x08, 0x8E);
    idt_set_gate(43, (unsigned int)irq11, 0x08, 0x8E);
    idt_set_gate(44, (unsigned int)irq12, 0x08, 0x8E);
    idt_set_gate(45, (unsigned int)irq13, 0x08, 0x8E);
    idt_set_gate(46, (unsigned int)irq14, 0x08, 0x8E);
    idt_set_gate(47, (unsigned int)irq15, 0x08, 0x8E);

    /* DPL=3 (not 0x8E's DPL=0) -- this is the one gate ring3 code is
     * allowed to invoke directly via `int`. Every other gate would
     * fault with a general protection exception if userspace tried. */
    idt_set_gate(128, (unsigned int)isr128, 0x08, 0xEE);

    idt_flush((unsigned int)&idtp);
}
