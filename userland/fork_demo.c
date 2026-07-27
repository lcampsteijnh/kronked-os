/* fork_demo.c -- a real userspace program exercising fork/exec/wait
 *
 * Compiled and loaded exactly like user_program.c: a fully independent
 * ELF binary that knows nothing about the kernel except the syscall
 * ABI. This one specifically proves the process model works from
 * ring 3, not just from kernel-mode code calling task.c directly.
 */

static inline int sys_write(const char *s) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(1), "b"(s) : "memory");
    return ret;
}

static inline void sys_exit(int code) {
    __asm__ volatile ("int $0x80" : : "a"(0), "b"(code) : "memory");
}

static inline int sys_fork(void) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(2) : "memory");
    return ret;
}

static inline int sys_exec(const char *path) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(3), "b"(path) : "memory");
    return ret;
}

static inline int sys_wait(int *status) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(4), "b"(status) : "memory");
    return ret;
}

static void print_int(int n) {
    char buf[16];
    unsigned int u;
    int neg = 0;

    if (n < 0) { neg = 1; u = (unsigned int)(0 - n); }
    else { u = (unsigned int)n; }

    int pos = 15;
    buf[pos] = '\0';

    if (u == 0) {
        pos--;
        buf[pos] = '0';
    } else {
        while (u != 0) {
            pos--;
            buf[pos] = (char)('0' + (u % 10));
            u = u / 10;
        }
    }
    if (neg) {
        pos--;
        buf[pos] = '-';
    }

    sys_write(&buf[pos]);
}

void _start(void) {
    sys_write("[fork_demo] parent: about to fork()\n");

    int pid = sys_fork();

    if (pid == 0) {
        /* child */
        sys_write("[fork_demo] child: I am the child, now calling exec(PROGRAM.ELF)\n");
        sys_exec("PROGRAM.ELF");
        /* only reached if exec failed */
        sys_write("[fork_demo] child: exec FAILED!\n");
        sys_exit(99);
    } else {
        /* parent */
        sys_write("[fork_demo] parent: fork returned child pid = ");
        print_int(pid);
        sys_write("\n[fork_demo] parent: waiting for child to finish...\n");

        int status = -1;
        int child_pid = sys_wait(&status);

        sys_write("[fork_demo] parent: wait() returned, child pid = ");
        print_int(child_pid);
        sys_write(", exit status = ");
        print_int(status);
        sys_write("\n[fork_demo] parent: done.\n");

        sys_exit(0);
    }
}
