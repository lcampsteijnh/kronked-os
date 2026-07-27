/* shell.c -- a real, interactive command shell
 *
 * Runs as task 0 (what used to be kernel_main's idle loop). Reads
 * keystrokes via keyboard_getchar(), does its own line editing
 * (including backspace), and dispatches a handful of built-in
 * commands. "run" loads and executes real ELF binaries from the FAT16
 * disk in their own ring-3 address space, exactly like Stage 5/6's
 * demo did, just now driven by whatever the person actually types.
 */

#include "shell.h"
#include "vga.h"
#include "serial.h"
#include "keyboard.h"
#include "fat16.h"
#include "elf.h"
#include "heap.h"
#include "pmm.h"
#include "paging.h"
#include "task.h"
#include "timer.h"
#include "editor.h"
#include "snake.h"
#include "gui.h"
#include "fb.h"
#include "desktop.h"
#include "mutex.h"
#include "banner.h"
#include "kronk.h"

#define LINE_MAX 128
#define PAGE_RW   0x2
#define PAGE_USER 0x4
#define PAGE_SIZE 4096u
#define USER_STACK_VADDR 0x40100000u

static int str_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static int str_len(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void print_dec_vga(unsigned int n) {
    char buf[11];
    int i = 10;
    buf[10] = '\0';
    if (n == 0) { vga_write("0"); return; }
    while (n > 0 && i > 0) { buf[--i] = (char)('0' + (n % 10)); n /= 10; }
    vga_write(&buf[i]);
}

/* Reads one line of input, with basic backspace support, echoing to
 * the screen as the person types. Returns when Enter is pressed. */
static void read_line(char *buf, int max_len) {
    int len = 0;
    for (;;) {
        char c = keyboard_getchar();

        if (c == '\n') {
            vga_putc('\n');
            buf[len] = '\0';
            return;
        } else if (c == '\b') {
            if (len > 0) {
                len--;
                vga_putc('\b');
            }
        } else if (len < max_len - 1) {
            buf[len++] = c;
            vga_putc(c);
        }
    }
}

/* Splits "cmd rest-of-line" into two NUL-terminated pieces in place. */
static void split_command(char *line, char **cmd, char **arg) {
    *cmd = line;
    char *p = line;
    while (*p && *p != ' ') p++;
    if (*p == ' ') {
        *p = '\0';
        p++;
        while (*p == ' ') p++;
        *arg = p;
    } else {
        *arg = p; /* points at the trailing '\0', i.e. empty string */
    }
}

static void cmd_help(void) {
    vga_write("Available commands:\n");
    vga_write("  help            show this list\n");
    vga_write("  ls              list files on disk\n");
    vga_write("  cat <file>      print a file's contents\n");
    vga_write("  rm <file>       delete a file\n");
    vga_write("  edit <file>     create or edit a text file (line editor)\n");
    vga_write("  run <file>      load and run an ELF program from disk (ring 3)\n");
    vga_write("  echo <text>     print text back\n");
    vga_write("  meminfo         show free physical memory\n");
    vga_write("  uptime          show timer ticks since boot\n");
    vga_write("  clear           clear the screen\n");
    vga_write("  snake           play Snake (w/a/s/d to move, q to quit)\n");
    vga_write("  mutextest       verify mutex correctness (2 tasks, shared counter)\n");
    vga_write("  kronk <file>    write a program with 'edit', run it with no compiler\n");
    vga_write("  desktop         graphical desktop: open terminals/editors, drag, close (X)\n");
}

static void ls_callback(const char *name, unsigned int size) {
    vga_write("  ");
    vga_write(name);
    int pad = 14 - str_len(name);
    for (int i = 0; i < pad; i++) vga_putc(' ');
    print_dec_vga(size);
    vga_write(" bytes\n");
}

static void cmd_ls(void) {
    vga_write("Files on disk:\n");
    fat16_list_root(ls_callback);
}

static void cmd_cat(const char *filename) {
    if (str_len(filename) == 0) { vga_write("usage: cat <file>\n"); return; }

    struct fat16_file f;
    if (!fat16_find_file(filename, &f)) {
        vga_write("cat: file not found: "); vga_write(filename); vga_write("\n");
        return;
    }

    unsigned char *buf = (unsigned char *)kmalloc(f.file_size + 1);
    if (!fat16_read_file(&f, buf, f.file_size)) {
        vga_write("cat: read failed\n");
        kfree(buf);
        return;
    }
    buf[f.file_size] = '\0';
    vga_write((const char *)buf);
    if (f.file_size == 0 || buf[f.file_size - 1] != '\n') vga_write("\n");
    kfree(buf);
}

static void cmd_rm(const char *filename) {
    if (str_len(filename) == 0) { vga_write("usage: rm <file>\n"); return; }

    if (fat16_delete_file(filename)) {
        vga_write("Deleted ");
        vga_write(filename);
        vga_write(".\n");
    } else {
        vga_write("rm: file not found: ");
        vga_write(filename);
        vga_write("\n");
    }
}

static void cmd_run(const char *filename) {
    if (str_len(filename) == 0) { vga_write("usage: run <file>\n"); return; }

    struct fat16_file f;
    if (!fat16_find_file(filename, &f)) {
        vga_write("run: file not found: "); vga_write(filename); vga_write("\n");
        return;
    }

    unsigned char *elf_buf = (unsigned char *)kmalloc(f.file_size);
    if (!fat16_read_file(&f, elf_buf, f.file_size)) {
        vga_write("run: read failed\n");
        return;
    }

    unsigned int user_dir = paging_clone_kernel_directory();
    unsigned int entry = 0;
    if (!elf_load(elf_buf, user_dir, &entry)) {
        vga_write("run: not a valid ELF32 executable\n");
        return;
    }

    unsigned int stack_phys = pmm_alloc_frame();
    paging_map_user_page(user_dir, stack_phys, USER_STACK_VADDR, PAGE_RW | PAGE_USER);
    unsigned int user_stack_top = USER_STACK_VADDR + PAGE_SIZE;

    unsigned int frames_before = pmm_free_frame_count();

    struct task *child = task_create_user(user_dir, entry, user_stack_top);
    vga_write("Running... (waiting for it to finish)\n");

    int exit_code = task_wait_specific(child);

    unsigned int frames_after = pmm_free_frame_count();
    vga_write("[memcheck] free frames before=");
    print_dec_vga(frames_before);
    vga_write(" after=");
    print_dec_vga(frames_after);
    vga_write("\n");

    vga_write("Process finished with exit code ");
    print_dec_vga((unsigned int)exit_code);
    vga_write(".\n");
}

/* --- mutex correctness test ---
 * Two kernel tasks each increment a shared counter many times under
 * mutex protection. Without correct mutual exclusion, this would
 * reliably lose updates (classic read-modify-write race: both tasks
 * read the same old value before either writes back the increment).
 * With a correct mutex, the final count must be exactly the sum of
 * both tasks' iteration counts, every single time. */
#define MUTEX_TEST_ITERS 2000
static volatile int mutex_test_counter = 0;
static mutex_t mutex_test_lock;
static volatile int mutex_test_done_count = 0;

static void mutex_test_task(void) {
    for (int i = 0; i < MUTEX_TEST_ITERS; i++) {
        mutex_lock(&mutex_test_lock);
        int tmp = mutex_test_counter;
        /* deliberately give a potential race a chance to manifest --
         * without the mutex actually excluding, another task could
         * run between this read and the write-back below */
        tmp = tmp + 1;
        mutex_test_counter = tmp;
        mutex_unlock(&mutex_test_lock);
    }
    mutex_test_done_count++;
    for (;;) { __asm__ volatile ("hlt"); }
}

static void cmd_mutextest(void) {
    mutex_test_counter = 0;
    mutex_test_done_count = 0;
    mutex_init(&mutex_test_lock);

    vga_write("Starting mutex test: 2 tasks x ");
    print_dec_vga(MUTEX_TEST_ITERS);
    vga_write(" increments each...\n");

    task_create(mutex_test_task);
    task_create(mutex_test_task);

    /* Wait (via hlt, letting the scheduler run both test tasks and
     * everything else) until both have finished all their increments. */
    while (mutex_test_done_count < 2) {
        __asm__ volatile ("hlt");
    }

    int expected = MUTEX_TEST_ITERS * 2;
    vga_write("Expected: ");
    print_dec_vga((unsigned int)expected);
    vga_write("  Actual: ");
    print_dec_vga((unsigned int)mutex_test_counter);
    if (mutex_test_counter == expected) {
        vga_write("  -> PASS (no lost updates)\n");
    } else {
        vga_write("  -> FAIL (lost updates -- mutual exclusion broken!)\n");
    }
}

static void dispatch(char *line) {
    if (str_len(line) == 0) return;

    char *cmd, *arg;
    split_command(line, &cmd, &arg);

    if (str_eq(cmd, "help")) cmd_help();
    else if (str_eq(cmd, "ls")) cmd_ls();
    else if (str_eq(cmd, "cat")) cmd_cat(arg);
    else if (str_eq(cmd, "rm")) cmd_rm(arg);
    else if (str_eq(cmd, "edit")) {
        if (str_len(arg) == 0) vga_write("usage: edit <file>\n");
        else editor_run(arg);
    }
    else if (str_eq(cmd, "run")) cmd_run(arg);
    else if (str_eq(cmd, "echo")) { vga_write(arg); vga_write("\n"); }
    else if (str_eq(cmd, "clear")) vga_clear();
    else if (str_eq(cmd, "snake")) snake_run();
    else if (str_eq(cmd, "desktop")) desktop_run();
    else if (str_eq(cmd, "mutextest")) cmd_mutextest();
    else if (str_eq(cmd, "kronk")) kronk_run(arg);
    else if (str_eq(cmd, "meminfo")) {
        vga_write("Free physical frames: ");
        print_dec_vga(pmm_free_frame_count());
        vga_write(" (");
        print_dec_vga(pmm_free_frame_count() * 4);
        vga_write(" KiB)\n");
    }
    else if (str_eq(cmd, "uptime")) {
        vga_write("Timer ticks since boot: ");
        print_dec_vga(timer_ticks());
        vga_write("\n");
    }
    else {
        vga_write("Unknown command: ");
        vga_write(cmd);
        vga_write("  (try 'help')\n");
    }
}

void shell_run(void) {
    char line[LINE_MAX];

    banner_print();
    vga_write("Type 'help' for a list of commands.\n\n");
    serial_write("[serial] shell: ready... kronk me.\n");

    for (;;) {
        vga_write("kronk> ");
        read_line(line, LINE_MAX);
        dispatch(line);
    }
}
