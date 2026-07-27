/* cow_test.c -- copy-on-write fork correctness test
 *
 * Classic COW test: a global variable starts shared between parent
 * and child (same physical frame, via fork's COW sharing). Each side
 * then writes a *different* value to it. If COW isolation works
 * correctly, each side must see only its own write, never the
 * other's -- proving the lazy-copy-on-write fault handler correctly
 * gives each side its own private copy the instant either one writes,
 * without the two ever corrupting each other's view.
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
    if (u == 0) { pos--; buf[pos] = '0'; }
    else { while (u != 0) { pos--; buf[pos] = (char)('0' + (u % 10)); u = u / 10; } }
    if (neg) { pos--; buf[pos] = '-'; }
    sys_write(&buf[pos]);
}

/* Starts as 100. Lives in .data, so it's genuinely a writable page
 * fork() shares copy-on-write -- exactly what this test needs to
 * exercise. */
static volatile int shared_value = 100;

/* A large-ish array so we also exercise a *multi-page* COW region,
 * not just a single shared page -- touches several separate PTEs,
 * each independently resolved on first write. */
static volatile int shared_array[4096]; /* 16KiB = 4 pages at 4KiB each */

void _start(void) {
    sys_write("[cow_test] initial shared_value = ");
    print_int(shared_value);
    sys_write("\n");

    for (int i = 0; i < 4096; i++) shared_array[i] = 7;

    int pid = sys_fork();

    if (pid == 0) {
        /* child: write a distinct value, then verify it stuck */
        shared_value = 222;
        for (int i = 0; i < 4096; i++) shared_array[i] = 222;

        int ok = (shared_value == 222);
        for (int i = 0; i < 4096; i++) if (shared_array[i] != 222) ok = 0;

        sys_write("[cow_test] child: shared_value = ");
        print_int(shared_value);
        sys_write(ok ? "  array check PASS\n" : "  array check FAIL\n");

        sys_exit(ok ? 42 : 1);
    } else {
        /* parent: write a *different* distinct value, verify it stuck
         * (i.e. the child's write to the same-named variable had no
         * effect on the parent's own view), then wait for the child
         * and report both results together. */
        shared_value = 333;
        for (int i = 0; i < 4096; i++) shared_array[i] = 333;

        int ok = (shared_value == 333);
        for (int i = 0; i < 4096; i++) if (shared_array[i] != 333) ok = 0;

        sys_write("[cow_test] parent: shared_value = ");
        print_int(shared_value);
        sys_write(ok ? "  array check PASS\n" : "  array check FAIL\n");

        int status = -1;
        int child_pid = sys_wait(&status);

        sys_write("[cow_test] parent: child pid=");
        print_int(child_pid);
        sys_write(" child exit status=");
        print_int(status);
        sys_write("\n");

        int overall_ok = ok && (status == 42);
        sys_write(overall_ok
            ? "[cow_test] OVERALL: PASS -- parent and child correctly isolated\n"
            : "[cow_test] OVERALL: FAIL -- COW isolation broken!\n");

        sys_exit(overall_ok ? 0 : 1);
    }
}
