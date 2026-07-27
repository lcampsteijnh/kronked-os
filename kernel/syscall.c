/* syscall.c -- syscall dispatch table, entered via int 0x80
 *
 * Calling convention: eax = syscall number, ebx/ecx = args, return
 * value goes back in eax (written through the pointer we're given,
 * since a plain `return` wouldn't reach the caller's real register --
 * see idt.c's comment on struct registers for why).
 */

#include "idt.h"
#include "vga.h"
#include "serial.h"
#include "task.h"
#include "paging.h"
#include "pmm.h"
#include "heap.h"
#include "elf.h"
#include "fat16.h"
#include "gui.h"
#include "mouse.h"
#include "keyboard.h"

#define SYS_EXIT  0
#define SYS_WRITE 1
#define SYS_FORK  2
#define SYS_EXEC  3
#define SYS_WAIT  4
#define SYS_MAP_FB      5
#define SYS_MOUSE_STATE 6
#define SYS_GETCHAR_NB  7

#define PAGE_RW   0x2
#define PAGE_USER 0x4
#define PAGE_SIZE 4096u
#define USER_STACK_VADDR 0x40100000u
#define USER_FB_VADDR    0x50000000u

struct fb_info_user {
    unsigned int vaddr;
    unsigned int width;
    unsigned int height;
    unsigned int pitch;
};

struct mouse_state_user {
    int x, y;
    unsigned int buttons;
};

void syscall_handler(struct registers *regs) {
    switch (regs->eax) {

        case SYS_WRITE: {
            const char *str = (const char *)regs->ebx;
            vga_write(str);
            serial_write("[serial] (from ring3 via syscall) ");
            serial_write(str);
            regs->eax = 0;
            break;
        }

        case SYS_EXIT: {
            int code = (int)regs->ebx;
            serial_write("[serial] syscall: task called exit()\n");
            task_exit_current(code); /* never returns */
            break;
        }

        case SYS_FORK: {
            /* Deep-copy the calling task's entire address space into a
             * fresh one, then build a child task whose very first
             * resume re-enters this exact syscall's return path --
             * see task_create_forked() and isr_stubs.s's
             * syscall_return_tail for how. */
            struct task *self = task_get_current();
            unsigned int parent_dir = task_get_page_directory(self);
            unsigned int child_dir = paging_clone_address_space(parent_dir);

            struct registers child_regs = *regs;
            child_regs.eax = 0; /* fork() returns 0 in the child */

            struct task *child = task_create_forked(child_dir, &child_regs, sizeof(child_regs));

            regs->eax = (unsigned int)task_get_id(child); /* parent sees the child's pid */
            break;
        }

        case SYS_EXEC: {
            const char *filename = (const char *)regs->ebx;

            struct fat16_file f;
            if (!fat16_find_file(filename, &f)) {
                serial_write("[serial] syscall exec: file not found\n");
                regs->eax = (unsigned int)-1;
                break;
            }

            unsigned char *buf = (unsigned char *)kmalloc(f.file_size);
            if (!fat16_read_file(&f, buf, f.file_size)) {
                regs->eax = (unsigned int)-1;
                break;
            }

            /* Build the new address space first, while still running
             * on the old one -- only tear the old one down once the
             * new one is fully built and confirmed valid, so a failed
             * exec() leaves the calling process's original image
             * intact rather than half-torn-down. */
            unsigned int new_dir = paging_clone_kernel_directory();
            unsigned int entry = 0;
            if (!elf_load(buf, new_dir, &entry)) {
                serial_write("[serial] syscall exec: not a valid ELF32\n");
                paging_free_address_space(new_dir); /* clean up the aborted attempt */
                pmm_free_frame(new_dir);
                regs->eax = (unsigned int)-1;
                break;
            }

            unsigned int stack_phys = pmm_alloc_frame();
            paging_map_user_page(new_dir, stack_phys, USER_STACK_VADDR, PAGE_RW | PAGE_USER);
            unsigned int user_stack_top = USER_STACK_VADDR + PAGE_SIZE;

            struct task *self = task_get_current();
            unsigned int old_dir = task_get_page_directory(self);

            /* filename points into the *old* address space (it's the
             * caller's own argument string) -- must log it before
             * switching directories below, or this read becomes a
             * dangling pointer into memory that no longer means the
             * same thing. */
            serial_write("[serial] syscall exec: replacing task image with '");
            serial_write(filename);
            serial_write("'\n");

            task_set_page_directory(self, new_dir);
            paging_switch_directory(new_dir);

            /* The new address space is now fully active; the old
             * one's contents are obsolete for this task (each user
             * task has its own private, non-shared directory), so
             * free it rather than leaking it on every exec() call. */
            paging_free_address_space(old_dir);
            pmm_free_frame(old_dir);

            /* This is the crux of exec(): rewrite *this task's own
             * saved interrupt frame* so that when the normal syscall
             * tail does popa+iret, it resumes not back in the old
             * program, but at the new one's entry point with a fresh
             * stack -- same task/pid, completely different program. */
            regs->eip = entry;
            regs->useresp = user_stack_top;
            regs->eax = 0;
            regs->ebx = 0;
            regs->ecx = 0;
            regs->edx = 0;

            break;
        }

        case SYS_WAIT: {
            int exit_code = 0;
            int pid = task_wait_any(&exit_code);

            if (pid < 0) {
                regs->eax = (unsigned int)-1; /* no children -- ECHILD equivalent */
                break;
            }
            if (regs->ebx) {
                int *status_ptr = (int *)regs->ebx;
                *status_ptr = exit_code;
            }
            regs->eax = (unsigned int)pid;
            break;
        }

        case SYS_MAP_FB: {
            if (!gui_init()) {
                regs->eax = (unsigned int)-1;
                break;
            }
            unsigned int fb_phys, width, height, pitch;
            gui_get_fb_phys_info(&fb_phys, &width, &height, &pitch);

            unsigned int fb_bytes = pitch * height;
            unsigned int num_pages = (fb_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

            struct task *self = task_get_current();
            unsigned int self_dir = task_get_page_directory(self);

            for (unsigned int i = 0; i < num_pages; i++) {
                unsigned int phys = fb_phys + i * PAGE_SIZE;
                unsigned int virt = USER_FB_VADDR + i * PAGE_SIZE;
                if (!paging_map_user_page(self_dir, phys, virt, PAGE_RW | PAGE_USER)) {
                    regs->eax = (unsigned int)-1;
                    break;
                }
            }

            struct fb_info_user *info = (struct fb_info_user *)regs->ebx;
            info->vaddr = USER_FB_VADDR;
            info->width = width;
            info->height = height;
            info->pitch = pitch;

            mouse_init((int)width, (int)height);

            regs->eax = 0;
            break;
        }

        case SYS_MOUSE_STATE: {
            struct mouse_state_user *info = (struct mouse_state_user *)regs->ebx;
            info->x = mouse_get_x();
            info->y = mouse_get_y();
            info->buttons = mouse_get_buttons();
            regs->eax = 0;
            break;
        }

        case SYS_GETCHAR_NB: {
            if (keyboard_has_char()) {
                regs->eax = (unsigned int)keyboard_getchar();
            } else {
                regs->eax = (unsigned int)-1;
            }
            break;
        }

        default:
            serial_write("[serial] syscall: unknown syscall number\n");
            regs->eax = (unsigned int)-1;
            break;
    }
}
