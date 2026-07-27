/* user_program.c -- a real, independent userspace program
 *
 * This is compiled completely separately from the kernel, as its own
 * freestanding ELF binary, then embedded into the kernel image and
 * loaded via elf.c exactly like a real OS would load a program off
 * disk. It knows nothing about the kernel's internals -- only the
 * syscall ABI (eax = number, ebx = arg, int 0x80).
 */

static inline int sys_write(const char *s) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(1), "b"(s) : "memory"); /* execute syscall number 1 (SYS_WRITE), use int0x80 to transfer execution to kernel*/
    return ret;
}

static inline void sys_exit(int code) {
    __asm__ volatile ("int $0x80" : : "a"(0), "b"(code) : "memory");
}

void _start(void) {
    sys_write("Hello from ring 3! This is a real ELF binary,\n");
    sys_write("loaded into its own address space and running\n");
    sys_write("at CPL=3, talking to the kernel only via syscalls.\n");

    /* prove we can compute in userspace too, not just print strings */
    volatile int sum = 0;
    for (int i = 1; i <= 100; i++) sum += i;
    if (sum == 5050) {
        sys_write("Computed sum(1..100) = 5050 correctly in ring 3.\n");
    } else {
        sys_write("ERROR: arithmetic in ring 3 gave the wrong answer!\n");
    }

    sys_exit(7);

    /* should never get here -- sys_exit() doesn't return -- but if the
     * scheduler somehow resumed us anyway, don't run off into garbage */
    for (;;) { }
}