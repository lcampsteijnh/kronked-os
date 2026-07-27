#ifndef TASK_H
#define TASK_H

struct task;

void task_init(void);
struct task *task_create(void (*entry)(void));
struct task *task_create_user(unsigned int dir_phys, unsigned int user_entry,
                               unsigned int user_stack_top);
struct task *task_create_forked(unsigned int dir_phys, const void *frame_bytes,
                                 unsigned int frame_size);

int task_get_id(struct task *t);
struct task *task_get_current(void);
unsigned int task_get_page_directory(struct task *t);
void task_set_page_directory(struct task *t, unsigned int dir_phys);

/* Per-task keyboard input redirection -- see the comment on struct
 * task's use_input_redirect field in task.c for the full rationale. */
void task_set_input_redirect(struct task *t, int enabled);
int task_push_input_char(struct task *t, char c); /* called by the compositor */
int task_current_uses_input_redirect(void);        /* called by keyboard.c */
int task_current_input_has_char(void);
char task_current_input_getchar(void);

/* Per-task VGA output redirection -- mirrors the input side above. */
void task_set_vga_redirect(struct task *t, unsigned short *buf, int cols, int rows);
unsigned short *task_current_vga_buf(void);  /* called by vga.c */
int task_current_vga_cols(void);
int task_current_vga_rows(void);
int *task_current_vga_row_ptr(void);
int *task_current_vga_col_ptr(void);
void task_current_vga_mark_dirty(void);
int task_consume_vga_dirty(struct task *t); /* called by the compositor */

void task_start_scheduling(void);
void schedule(void);
void yield(void);

void task_block(void);
void task_unblock(struct task *t);

void task_exit_current(int exit_code);
void task_force_terminate(struct task *t, int exit_code);
int task_wait_specific(struct task *child);
int task_wait_any(int *out_exit_code); /* returns child pid, or -1 if no children */

#endif
