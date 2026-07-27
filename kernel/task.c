/* task.c -- task structs and a round-robin scheduler
 *
 * Every task has its own page directory (kernel-space entries shared
 * across all of them, user-space entries private) and its own kernel
 * stack. Kernel-only tasks run entirely on that stack in ring 0; user
 * tasks get dropped into ring 3 the first time they're scheduled, with
 * their own separate user-mode stack, and can only get back to ring 0
 * via a syscall (int 0x80) or an exception.
 *
 * Tasks now have real states (READY / BLOCKED / ZOMBIE) rather than
 * just "in the ready list or not": the scheduler skips blocked and
 * zombie tasks when picking who runs next, task_block()/task_unblock()
 * let a task wait for something (used by wait()), and exiting marks a
 * task ZOMBIE rather than unlinking it immediately -- it stays around,
 * inert, until its parent reaps it via task_wait_specific()/wait().
 */

#include "task.h"
#include "heap.h"
#include "serial.h"
#include "paging.h"
#include "gdt.h"
#include "pmm.h"
#include "spinlock.h"

#define STACK_SIZE 8192
#define TASK_INPUT_QUEUE_SIZE 64

static spinlock_t task_list_lock;

enum task_state { TASK_READY, TASK_BLOCKED, TASK_ZOMBIE };

struct task {
    unsigned int esp;
    struct task *next;
    struct task *prev;
    int id;

    void (*entry)(void);          /* kernel-mode tasks only */
    int is_user;
    unsigned int user_entry;      /* virtual address, user tasks only */
    unsigned int user_stack_top;  /* virtual address, user tasks only */

    unsigned int page_directory;  /* physical address */
    unsigned int kernel_stack_top; /* for TSS esp0 on this task's ring3->ring0 transitions */

    enum task_state state;
    struct task *parent;
    int exit_code;

    /* Per-task keyboard input redirection: when enabled, this task's
     * keyboard_getchar()/keyboard_has_char() calls transparently read
     * from this small private queue instead of the raw global
     * keyboard ring buffer. Used to host a shell/editor inside a GUI
     * window: the compositor becomes the sole consumer of real
     * keyboard hardware input and pushes each keystroke into whichever
     * task's queue currently has window focus, so multiple hosted
     * windows can each receive their own input correctly instead of
     * racing over one shared buffer. */
    int use_input_redirect;
    char input_queue[TASK_INPUT_QUEUE_SIZE];
    int input_head, input_tail;

    /* Per-task VGA output redirection: mirrors the input side above.
     * When set, this task's vga_putc()/vga_write()/vga_clear() calls
     * write into this buffer (with this task's own independent cursor)
     * instead of the real screen. The owning window/compositor code
     * holds the same buffer pointer already (it allocated it), so it
     * only needs the dirty flag to know when to re-render -- not the
     * buffer pointer itself, hence no "get buffer for task T" accessor. */
    unsigned short *vga_redirect_buf;
    int vga_redirect_cols, vga_redirect_rows;
    int vga_cursor_row, vga_cursor_col;
    int vga_redirect_dirty;
};

extern void context_switch(unsigned int *old_esp_store, unsigned int new_esp);
extern void enter_usermode(unsigned int entry, unsigned int user_esp);
extern void syscall_return_tail(void); /* in isr_stubs.s -- shared tail used by fork() too */

static struct task *current_task = 0;
static int next_task_id = 0;
static int scheduling_enabled = 0;
static int task_count = 0; /* includes blocked/zombie tasks -- only used for the "am I alone" check */

static void task_trampoline(void) {
    struct task *self = current_task;

    if (self->is_user) {
        enter_usermode(self->user_entry, self->user_stack_top);
        /* never returns */
    } else {
        __asm__ volatile ("sti");
        self->entry();
    }

    for (;;) { __asm__ volatile ("hlt"); }
}

void task_init(void) {
    spinlock_init(&task_list_lock);
    struct task *main_task = (struct task *)kmalloc(sizeof(struct task));
    main_task->id = next_task_id++;
    main_task->is_user = 0;
    main_task->next = main_task;
    main_task->prev = main_task;
    main_task->page_directory = paging_kernel_directory_phys();
    main_task->kernel_stack_top = 0;
    main_task->state = TASK_READY;
    main_task->parent = 0;
    main_task->exit_code = 0;
    main_task->use_input_redirect = 0;
    main_task->input_head = 0;
    main_task->input_tail = 0;
    main_task->vga_redirect_buf = 0;
    main_task->vga_redirect_dirty = 0;
    current_task = main_task;
    task_count = 1;
    serial_write("[serial] task: main/idle task registered as task 0\n");
}

static struct task *task_alloc_common(void) {
    struct task *t = (struct task *)kmalloc(sizeof(struct task));
    t->id = next_task_id++;
    t->state = TASK_READY;
    t->parent = current_task; /* every task is structurally "owned" by whoever created it */
    t->exit_code = 0;
    t->use_input_redirect = 0;
    t->input_head = 0;
    t->input_tail = 0;
    t->vga_redirect_buf = 0;
    t->vga_redirect_dirty = 0;

    unsigned int flags = spinlock_acquire(&task_list_lock);
    t->next = current_task->next;
    t->prev = current_task;
    current_task->next->prev = t;
    current_task->next = t;
    task_count++;
    spinlock_release(&task_list_lock, flags);
    return t;
}

struct task *task_create(void (*entry)(void)) {
    struct task *t = task_alloc_common();
    t->is_user = 0;
    t->entry = entry;
    t->page_directory = paging_kernel_directory_phys();

    unsigned char *stack_mem = (unsigned char *)kmalloc(STACK_SIZE);
    unsigned int *sp = (unsigned int *)(stack_mem + STACK_SIZE);
    t->kernel_stack_top = (unsigned int)sp;

    *(--sp) = (unsigned int)task_trampoline;
    *(--sp) = 0; *(--sp) = 0; *(--sp) = 0; *(--sp) = 0;

    t->esp = (unsigned int)sp;
    serial_write("[serial] task: created new kernel task\n");
    return t;
}

struct task *task_create_user(unsigned int dir_phys, unsigned int user_entry,
                               unsigned int user_stack_top) {
    struct task *t = task_alloc_common();
    t->is_user = 1;
    t->user_entry = user_entry;
    t->user_stack_top = user_stack_top;
    t->page_directory = dir_phys;

    unsigned char *stack_mem = (unsigned char *)kmalloc(STACK_SIZE);
    unsigned int *sp = (unsigned int *)(stack_mem + STACK_SIZE);
    t->kernel_stack_top = (unsigned int)sp;

    *(--sp) = (unsigned int)task_trampoline; /* save return address from task trampoline */
    *(--sp) = 0; *(--sp) = 0; *(--sp) = 0; *(--sp) = 0; /* reserve space for registers that context_switch() expects to store */

    t->esp = (unsigned int)sp;
    serial_write("[serial] task: created new USER task\n");
    return t;
}

/* Used by fork() (see syscall.c): builds a task that resumes not via
 * task_trampoline, but by directly re-entering the tail of the syscall
 * path -- i.e. it "returns from a syscall" it never actually made
 * itself, using a copied register frame from its parent. */
struct task *task_create_forked(unsigned int dir_phys, const void *frame_bytes,
                                 unsigned int frame_size) {
    struct task *t = task_alloc_common();
    t->is_user = 1;
    t->page_directory = dir_phys;

    unsigned char *stack_mem = (unsigned char *)kmalloc(STACK_SIZE);
    unsigned char *top = stack_mem + STACK_SIZE;

    unsigned char *frame_start = top - frame_size;
    const unsigned char *src = (const unsigned char *)frame_bytes;
    for (unsigned int i = 0; i < frame_size; i++) frame_start[i] = src[i];

    t->kernel_stack_top = (unsigned int)top;

    unsigned int *below = (unsigned int *)frame_start;
    *(--below) = (unsigned int)syscall_return_tail;
    *(--below) = 0; *(--below) = 0; *(--below) = 0; *(--below) = 0;

    t->esp = (unsigned int)below;
    serial_write("[serial] task: created new FORKED task\n");
    return t;
}

int task_get_id(struct task *t) { return t->id; }
struct task *task_get_current(void) { return current_task; }
unsigned int task_get_page_directory(struct task *t) { return t->page_directory; }
void task_set_page_directory(struct task *t, unsigned int dir_phys) { t->page_directory = dir_phys; }

/* --- per-task keyboard input redirection (see struct task's comment) */

void task_set_input_redirect(struct task *t, int enabled) {
    t->use_input_redirect = enabled;
    t->input_head = 0;
    t->input_tail = 0;
}

/* Called by the compositor (never by the task itself) to deliver one
 * keystroke into a specific task's private input queue. Wakes the
 * task if it was blocked waiting on keyboard_getchar(). Returns 0 if
 * the queue is full (keystroke dropped -- shouldn't happen in
 * practice at 64 slots for interactive typing speeds). */
int task_push_input_char(struct task *t, char c) {
    int next = (t->input_head + 1) % TASK_INPUT_QUEUE_SIZE; /* move head after character insertion */
    if (next == t->input_tail) return 0; /* stop if buffer full */
    t->input_queue[t->input_head] = c; /* store character "c" */
    t->input_head = next; /* move head cursor */
    task_unblock(t);
    return 1;
}

int task_current_uses_input_redirect(void) {
    if (!current_task) return 0;
    return current_task->use_input_redirect;
}

int task_current_input_has_char(void) {
    if (!current_task) return 0;
    return current_task->input_head != current_task->input_tail;
}

char task_current_input_getchar(void) {
    if (!current_task) return 0;
    if (current_task->input_head == current_task->input_tail) return 0;
    char c = current_task->input_queue[current_task->input_tail];
    current_task->input_tail = (current_task->input_tail + 1) % TASK_INPUT_QUEUE_SIZE;
    return c;
}

/* --- per-task VGA output redirection (see struct task's comment) --- */

void task_set_vga_redirect(struct task *t, unsigned short *buf, int cols, int rows) {
    t->vga_redirect_buf = buf;
    t->vga_redirect_cols = cols;
    t->vga_redirect_rows = rows;
    t->vga_cursor_row = 0;
    t->vga_cursor_col = 0;
    t->vga_redirect_dirty = 1;
}

/* All of the following are called from vga.c on every single
 * vga_putc() -- including the many calls kernel_main() makes for
 * status messages *before* task_init() has ever run, when
 * current_task is still NULL. Guard every one: reading through a NULL
 * current_task wouldn't even fault cleanly here (address 0 sits
 * inside our identity-mapped low memory, so it silently reads
 * whatever garbage is physically there instead) -- this was a real
 * bug, found via a QEMU-level crash that turned out to be corruption
 * from exactly this, not a QEMU bug at all. */
unsigned short *task_current_vga_buf(void) { return current_task ? current_task->vga_redirect_buf : 0; }
int task_current_vga_cols(void) { return current_task ? current_task->vga_redirect_cols : 0; }
int task_current_vga_rows(void) { return current_task ? current_task->vga_redirect_rows : 0; }

static int dummy_cursor_pos = 0;
int *task_current_vga_row_ptr(void) { return current_task ? &current_task->vga_cursor_row : &dummy_cursor_pos; }
int *task_current_vga_col_ptr(void) { return current_task ? &current_task->vga_cursor_col : &dummy_cursor_pos; }
void task_current_vga_mark_dirty(void) { if (current_task) current_task->vga_redirect_dirty = 1; }

/* Explicit-task version, for the compositor to check/clear a
 * *specific* hosted task's dirty flag without needing to be that
 * task -- unlike everything above, which always operates on whichever
 * task happens to be running right now. */
int task_consume_vga_dirty(struct task *t) {
    if (!t->vga_redirect_dirty) return 0;
    t->vga_redirect_dirty = 0;
    return 1;
}

void task_start_scheduling(void) {
    scheduling_enabled = 1;
}

/* Skips BLOCKED and ZOMBIE tasks when picking who runs next. If we
 * loop all the way back around to the currently-running task without
 * finding anything else READY, either it's still fine to keep running
 * (normal case) or it just blocked itself and there's truly nobody
 * else runnable (shouldn't happen here -- the background task is
 * always READY -- but we don't hang forever if it somehow does). */
void schedule(void) {
    if (!scheduling_enabled || !current_task) return;

    struct task *prev = current_task;
    struct task *next = prev->next;

    while (next->state != TASK_READY && next != prev) {
        next = next->next; /* Skip tasks that are unable to run */
    }

    if (next == prev) {
        if (prev->state != TASK_READY) {
            serial_write("[serial] schedule: WARNING no other ready task found!\n");
        }
        return;
    }

    current_task = next;
    tss_set_kernel_stack(next->kernel_stack_top);
    if (next->page_directory != prev->page_directory) {
        paging_switch_directory(next->page_directory);
    }
    context_switch(&prev->esp, next->esp);
}

void yield(void) {
    schedule();
}

void task_block(void) {
    current_task->state = TASK_BLOCKED;
    schedule();
    /* resumes here once some other task calls task_unblock(this) */
}

void task_unblock(struct task *t) {
    if (t->state == TASK_BLOCKED) t->state = TASK_READY;
}

/* Called from the SYS_EXIT syscall handler (and, for kernel tasks that
 * fall off the end, task_trampoline's guard loop -- though that path
 * halts rather than calling this, since a runaway kernel task has no
 * parent expecting a real exit code). Marks the task ZOMBIE instead of
 * removing it: it stays in the list, inert (never scheduled again,
 * since schedule() skips non-READY tasks), until reaped. */
void task_exit_current(int exit_code) {
    struct task *dead = current_task;
    dead->state = TASK_ZOMBIE;
    dead->exit_code = exit_code;

    if (dead->parent) {
        task_unblock(dead->parent); /* in case the parent is in wait() */
    }

    if (task_count <= 1) {
        serial_write("[serial] task_exit_current: last task exited, halting.\n");
        for (;;) { __asm__ volatile ("cli; hlt"); }
    }

    schedule(); /* dead->state is ZOMBIE, so this always switches away */
    for (;;) { __asm__ volatile ("hlt"); } /* unreachable */
}

/* Marks an *other* task ZOMBIE from outside, without needing to
 * context-switch away from it -- since we're a different, currently-
 * running task, the target is guaranteed to be READY or BLOCKED, never
 * concurrently executing, so this is safe: schedule() already skips
 * ZOMBIE tasks, so it's simply never picked again. Used to close a
 * GUI window's hosted task (terminal/editor) when the person clicks
 * its close button -- unlike task_exit_current(), which only a task
 * can call on itself. Like other kernel-task cleanup in this kernel,
 * this doesn't reap/free the task's memory (nobody waits on it); it
 * just permanently and safely retires it from scheduling. */
void task_force_terminate(struct task *t, int exit_code) {
    if (t == current_task) { task_exit_current(exit_code); return; }
    t->state = TASK_ZOMBIE;
    t->exit_code = exit_code;
    if (t->parent) task_unblock(t->parent);
}

/* Blocks the calling task until `child` becomes a zombie, then reaps
 * it: unlinks it from the list, frees its entire address space
 * (every user-space page table + mapped frame, via
 * paging_free_address_space -- this used to be a documented leak;
 * fixed), its top-level page directory frame, its kernel stack, and
 * its task struct, and returns its exit code. */
int task_wait_specific(struct task *child) {
    while (child->state != TASK_ZOMBIE) {
        task_block();
    }

    int code = child->exit_code;

    unsigned int flags = spinlock_acquire(&task_list_lock);
    child->prev->next = child->next;
    child->next->prev = child->prev;
    task_count--;
    spinlock_release(&task_list_lock, flags);

    /* Only user tasks have their own cloned, independently-freeable
     * address space -- kernel tasks share the literal kernel
     * directory (paging_kernel_directory_phys()), and freeing that
     * would corrupt every other task using it. In practice nothing
     * currently waits on a kernel task, but this guard costs nothing
     * and prevents a latent landmine if that ever changes. */
    if (child->is_user) {
        paging_free_address_space(child->page_directory);
        pmm_free_frame(child->page_directory);
    }
    kfree((void *)(child->kernel_stack_top - STACK_SIZE));
    kfree(child);

    return code;
}

/* Scans for any zombie child of the current task, reaps the first one
 * found. If no children exist at all, returns 0 immediately (matches
 * wait()'s ECHILD case). If children exist but none are zombies yet,
 * blocks until one is. */
/* Scans for any zombie child of the current task, reaps the first one
 * found. Returns its pid (captured *before* reaping, since reaping
 * frees the task struct -- returning the pointer itself would be a
 * use-after-free waiting to happen). If no children exist at all,
 * returns -1 immediately (matches wait()'s ECHILD case). If children
 * exist but none are zombies yet, blocks until one is. */
int task_wait_any(int *out_exit_code) {
    struct task *self = current_task;

    for (;;) {
        struct task *found = 0;
        int any_children = 0;

        unsigned int flags = spinlock_acquire(&task_list_lock);
        struct task *t = self->next;
        while (t != self) {
            if (t->parent == self) {
                any_children = 1;
                if (t->state == TASK_ZOMBIE) { found = t; break; }
            }
            t = t->next;
        }
        spinlock_release(&task_list_lock, flags);

        if (found) {
            int pid = found->id;
            int code = task_wait_specific(found); /* frees `found` -- don't touch it after this */
            if (out_exit_code) *out_exit_code = code;
            return pid;
        }
        if (!any_children) return -1;

        task_block();
    }
}
