/* kernel.c -- kernel_main: the first C code that runs on our OS */

#include "vga.h"
#include "serial.h"
#include "gdt.h"
#include "idt.h"
#include "timer.h"
#include "keyboard.h"
#include "multiboot.h"
#include "pmm.h"
#include "paging.h"
#include "heap.h"
#include "task.h"
#include "elf.h"
#include "ata.h"
#include "fat16.h"
#include "shell.h"

#define MULTIBOOT_MAGIC 0x2BADB002

static void print_dec(unsigned int n) {
    char buf[11];
    int i = 10;
    buf[10] = '\0';
    if (n == 0) { serial_write("0"); return; }
    while (n > 0 && i > 0) { buf[--i] = (char)('0' + (n % 10)); n /= 10; }
    serial_write(&buf[i]);
}

/* One kernel-mode background task, mostly to prove the shell (task 0,
 * blocked most of the time waiting on keyboard input) and other tasks
 * genuinely coexist in the same preemptive scheduler. */
static void bg_task_entry(void) {
    unsigned int counter = 0;
    for (;;) {
        counter++;
        if (counter % 500000000 == 0) {
            serial_write("[serial] BG-KERNEL-TASK heartbeat ");
            print_dec(counter / 500000000);
            serial_write("\n");
        }
    }
}

void kernel_main(unsigned int magic, unsigned int mb_info_addr) {
    serial_init();
    serial_write("[serial] kernel_main() reached.\n");

    vga_clear();
    vga_write("KRONKED-OS booting...\n");
    if (magic != MULTIBOOT_MAGIC) vga_write("WARNING: multiboot magic mismatch!\n");

    gdt_init();
    idt_init();

    struct multiboot_info *mbi = (struct multiboot_info *)mb_info_addr;
    pmm_init(magic, mbi);
    paging_init();
    heap_init();
    timer_init(100);
    keyboard_init();

    if (ata_identify()) {
        if (fat16_mount()) {
            vga_write("Disk mounted.\n");
        } else {
            vga_write("WARNING: disk found but FAT16 mount failed.\n");
        }
    } else {
        vga_write("WARNING: no disk found -- ls/cat/run won't work.\n");
    }

    task_init();
    task_create(bg_task_entry);

    __asm__ volatile ("sti");
    task_start_scheduling();

    serial_write("[serial] handing off to interactive shell.\n");

    /* kernel_main is task 0. Instead of an idle loop, it now runs the
     * shell directly -- this never returns. */
    shell_run();
}
