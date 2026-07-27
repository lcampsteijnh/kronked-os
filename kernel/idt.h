#ifndef IDT_H
#define IDT_H

/* Must exactly match the order isr_stubs.s pushes registers in: ds,
 * then pusha's (edi,esi,ebp,esp,ebx,edx,ecx,eax), then int_no and
 * err_code, then whatever the CPU pushed automatically (eip,cs,
 * eflags, and -- for a ring3->ring0 transition, which is the only
 * kind our syscall path ever sees, since it's only reachable via
 * `int 0x80` from ring 3 -- useresp and ss too).
 *
 * isr_handler/irq_handler never read useresp/ss (those can fire from
 * ring 0, where the two extra fields the CPU pushes genuinely aren't
 * present on the stack, so reading them would be garbage); only
 * syscall_handler may touch them, and only because every syscall in
 * this kernel is, by construction, ring3-originated. */
struct registers {
    unsigned int ds;
    unsigned int edi, esi, ebp, esp_orig, ebx, edx, ecx, eax;
    unsigned int int_no, err_code;
    unsigned int eip, cs, eflags, useresp, ss;
} __attribute__((packed));

void idt_init(void);
void irq_install_handler(int irq, void (*handler)(void));

#endif
