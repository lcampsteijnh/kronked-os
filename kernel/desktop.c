/* desktop.c -- a real, usable windowing desktop
 *
 * Immediate-mode compositor: every frame, redraw the background, the
 * taskbar, then each window back-to-front, then the cursor on top.
 * Windows can host a real task (a terminal running the shell, or a
 * text editor) -- launched on demand from the taskbar, closeable via
 * an X button, with keyboard input correctly routed to whichever
 * window currently has focus (see the routing comment above
 * desktop_run() for why that needs real per-task plumbing rather than
 * just reading the keyboard buffer directly from multiple places).
 */

#include "desktop.h"
#include "gui.h"
#include "fb.h"
#include "mouse.h"
#include "keyboard.h"
#include "timer.h"
#include "cursor_bitmap.h"
#include "serial.h"
#include "vga.h"
#include "task.h"
#include "shell.h"
#include "editor.h"
#include "vbe.h"

#define MAX_WINDOWS 6
#define TITLEBAR_H 24
#define CLOSE_BTN_SIZE 16
#define TASKBAR_H 40

#define TERM_COLS 60
#define TERM_ROWS 20
static unsigned short term_buffers[MAX_WINDOWS][TERM_ROWS * TERM_COLS];

static const char *scratch_names[MAX_WINDOWS] = {
    "scratch0.txt", "scratch1.txt", "scratch2.txt",
    "scratch3.txt", "scratch4.txt", "scratch5.txt",
};

struct gui_window {
    int active;
    int x, y, w, h;
    char title[24];
    int is_hosted;
    struct task *hosted_task;
    int buf_slot; /* index into term_buffers, valid if is_hosted */
};

static struct gui_window windows[MAX_WINDOWS];
static int window_order[MAX_WINDOWS]; /* back to front, only first num_order entries valid */
static int num_order = 0;
static int focused_slot = -1;

static int dragging_window = -1;
static int drag_offset_x = 0, drag_offset_y = 0;
static unsigned char prev_buttons = 0;

/* --- per-slot task entry points, so a brand-new task always knows
 * exactly which buffer/filename it owns with zero shared mutable
 * state between launches (task_create() takes no arguments, so this
 * sidesteps the same "shared global race" class of bug found and
 * fixed earlier in this project's history for a different task-launch
 * path). --- */
#define DEFINE_TERM_ENTRY(N) \
    static void terminal_entry_##N(void) { \
        task_set_input_redirect(task_get_current(), 1); \
        vga_set_redirect(term_buffers[N], TERM_COLS, TERM_ROWS); \
        vga_clear(); \
        shell_run(); \
    }
DEFINE_TERM_ENTRY(0)
DEFINE_TERM_ENTRY(1)
DEFINE_TERM_ENTRY(2)
DEFINE_TERM_ENTRY(3)
DEFINE_TERM_ENTRY(4)
DEFINE_TERM_ENTRY(5)
static void (*terminal_entries[MAX_WINDOWS])(void) = {
    terminal_entry_0, terminal_entry_1, terminal_entry_2,
    terminal_entry_3, terminal_entry_4, terminal_entry_5,
};

#define DEFINE_EDITOR_ENTRY(N) \
    static void editor_entry_##N(void) { \
        task_set_input_redirect(task_get_current(), 1); \
        vga_set_redirect(term_buffers[N], TERM_COLS, TERM_ROWS); \
        vga_clear(); \
        editor_run(scratch_names[N]); \
        task_exit_current(0); \
    }
DEFINE_EDITOR_ENTRY(0)
DEFINE_EDITOR_ENTRY(1)
DEFINE_EDITOR_ENTRY(2)
DEFINE_EDITOR_ENTRY(3)
DEFINE_EDITOR_ENTRY(4)
DEFINE_EDITOR_ENTRY(5)
static void (*editor_entries[MAX_WINDOWS])(void) = {
    editor_entry_0, editor_entry_1, editor_entry_2,
    editor_entry_3, editor_entry_4, editor_entry_5,
};

static int find_free_slot(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) if (!windows[i].active) return i;
    return -1;
}

static void order_remove(int slot) {
    int pos = -1;
    for (int i = 0; i < num_order; i++) if (window_order[i] == slot) { pos = i; break; }
    if (pos < 0) return;
    for (int i = pos; i < num_order - 1; i++) window_order[i] = window_order[i + 1];
    num_order--;
}

static void order_push_front(int slot) {
    window_order[num_order++] = slot;
}

static void bring_to_front(int slot) {
    order_remove(slot);
    order_push_front(slot);
}

static void close_window(int slot) {
    if (!windows[slot].active) return;
    if (windows[slot].is_hosted && windows[slot].hosted_task) {
        task_force_terminate(windows[slot].hosted_task, 0);
    }
    windows[slot].active = 0;
    order_remove(slot);
    if (focused_slot == slot) focused_slot = (num_order > 0) ? window_order[num_order - 1] : -1;
    if (dragging_window == slot) dragging_window = -1;
}

static void open_window(int is_editor) {
    int slot = find_free_slot();
    if (slot < 0) return; /* out of window slots */

    int n = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) if (windows[i].active) n++;

    windows[slot].active = 1;
    windows[slot].x = 100 + 28 * n;
    windows[slot].y = 90 + 24 * n;
    windows[slot].w = TERM_COLS * 8 + 16;
    windows[slot].h = TERM_ROWS * 16 + TITLEBAR_H + 16;
    windows[slot].is_hosted = 1;
    windows[slot].buf_slot = slot;

    for (int i = 0; i < TERM_ROWS * TERM_COLS; i++) term_buffers[slot][i] = 0;

    if (is_editor) {
        int idx = 0;
        for (int i = 0; i < 24 && scratch_names[slot][i]; i++) windows[slot].title[idx++] = scratch_names[slot][i];
        windows[slot].title[idx] = '\0';
        windows[slot].hosted_task = task_create(editor_entries[slot]);
    } else {
        const char *base = "Terminal";
        int idx = 0;
        for (int i = 0; base[i]; i++) windows[slot].title[idx++] = base[i];
        windows[slot].title[idx++] = ' ';
        windows[slot].title[idx++] = (char)('1' + slot);
        windows[slot].title[idx] = '\0';
        windows[slot].hosted_task = task_create(terminal_entries[slot]);
    }

    order_push_front(slot);
    focused_slot = slot; /* newly opened windows get focus immediately */
}

static int point_in_rect(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

static void draw_window(int slot) {
    struct gui_window *win = &windows[slot];
    unsigned int title_color = (slot == focused_slot) ? RGB(60, 90, 160) : RGB(70, 70, 90);

    fb_fill_rect(win->x, win->y, win->w, win->h, RGB(8, 8, 10));
    fb_fill_rect(win->x, win->y, win->w, TITLEBAR_H, title_color);
    fb_draw_string(win->x + 8, win->y + 4, win->title, RGB(255, 255, 255), 0, 0);
    fb_draw_rect(win->x, win->y, win->w, win->h, RGB(0, 0, 0));
    fb_draw_hline(win->x, win->y + TITLEBAR_H, win->w, RGB(0, 0, 0));

    /* close button: small red square with an X, top-right of the title bar */
    int cbx = win->x + win->w - CLOSE_BTN_SIZE - 4, cby = win->y + 4;
    fb_fill_rect(cbx, cby, CLOSE_BTN_SIZE, CLOSE_BTN_SIZE, RGB(180, 50, 50));
    fb_draw_string(cbx + 4, cby - 1, "x", RGB(255, 255, 255), 0, 0);

    if (win->is_hosted) {
        int base_x = win->x + 8, base_y = win->y + TITLEBAR_H + 8;
        unsigned short *buf = term_buffers[win->buf_slot];
        for (int row = 0; row < TERM_ROWS; row++) {
            for (int col = 0; col < TERM_COLS; col++) {
                unsigned short cell = buf[row * TERM_COLS + col];
                char ch = (char)(cell & 0xFF);
                if (ch == 0) ch = ' ';
                fb_draw_char(base_x + col * 8, base_y + row * 16, ch, RGB(220, 220, 220), 0, 0);
            }
        }
    }
}

static int close_button_hit(int slot, int px, int py) {
    struct gui_window *win = &windows[slot];
    int cbx = win->x + win->w - CLOSE_BTN_SIZE - 4, cby = win->y + 4;
    return point_in_rect(px, py, cbx, cby, CLOSE_BTN_SIZE, CLOSE_BTN_SIZE);
}

static void draw_cursor(int x, int y) {
    for (int row = 0; row < cursor_height; row++) {
        for (int col = 0; col < cursor_width; col++) {
            unsigned char v = cursor_bitmap[row][col];
            if (v == 1) fb_put_pixel(x + col, y + row, RGB(255, 255, 255));
            else if (v == 2) fb_put_pixel(x + col, y + row, RGB(0, 0, 0));
        }
    }
}

/* --- taskbar --- */
#define BTN_TERM_X 12
#define BTN_EDIT_X 150
#define BTN_QUIT_X 288
#define BTN_W 128
#define BTN_H 28

static int quit_requested = 0;

static void draw_taskbar(int screen_w, int screen_h) {
    int y = screen_h - TASKBAR_H;
    fb_fill_rect(0, y, screen_w, TASKBAR_H, RGB(35, 35, 55));
    fb_draw_hline(0, y, screen_w, RGB(0, 0, 0));

    int by = y + (TASKBAR_H - BTN_H) / 2;
    fb_fill_rect(BTN_TERM_X, by, BTN_W, BTN_H, RGB(60, 110, 60));
    fb_draw_string(BTN_TERM_X + 10, by + 6, "+ Terminal", RGB(255, 255, 255), 0, 0);

    fb_fill_rect(BTN_EDIT_X, by, BTN_W, BTN_H, RGB(60, 90, 140));
    fb_draw_string(BTN_EDIT_X + 16, by + 6, "+ Editor", RGB(255, 255, 255), 0, 0);

    fb_fill_rect(BTN_QUIT_X, by, 96, BTN_H, RGB(140, 60, 60));
    fb_draw_string(BTN_QUIT_X + 22, by + 6, "Quit", RGB(255, 255, 255), 0, 0);

    fb_draw_string(screen_w - 340, y + 13, "Click a window to focus it for typing", RGB(190, 190, 210), 0, 0);
}

static int taskbar_click(int mx, int my, int screen_h) {
    int y = screen_h - TASKBAR_H;
    if (my < y) return 0;
    int by = y + (TASKBAR_H - BTN_H) / 2;

    if (point_in_rect(mx, my, BTN_TERM_X, by, BTN_W, BTN_H)) { open_window(0); return 1; }
    if (point_in_rect(mx, my, BTN_EDIT_X, by, BTN_W, BTN_H)) { open_window(1); return 1; }
    if (point_in_rect(mx, my, BTN_QUIT_X, by, 96, BTN_H)) { quit_requested = 1; return 1; }
    return 1; /* clicking anywhere in the taskbar is absorbed, not passed to windows below */
}

static void redraw(void) {
    unsigned int w = fb_get_width(), h = fb_get_height();
    fb_fill_rect(0, 0, (int)w, (int)h, RGB(25, 25, 55));

    for (int i = 0; i < num_order; i++) draw_window(window_order[i]);

    draw_taskbar((int)w, (int)h);
    draw_cursor(mouse_get_x(), mouse_get_y());
}

/* Keyboard routing: this compositor task is the *sole* consumer of
 * the raw keyboard buffer (it has no input redirect of its own, so
 * keyboard_getchar()/has_char() called from here read the real
 * hardware queue as normal). Every character read is pushed into
 * whichever window currently has focus, via task_push_input_char() --
 * never left for a hosted task to read directly. This is what lets
 * an arbitrary number of terminal/editor windows coexist: each only
 * ever sees keystrokes explicitly routed to it, never a race over one
 * shared buffer. */
static void route_keyboard(void) {
    while (keyboard_has_char()) {
        char c = keyboard_getchar();
        if (focused_slot >= 0 && windows[focused_slot].active && windows[focused_slot].is_hosted) {
            task_push_input_char(windows[focused_slot].hosted_task, c);
        }
    }
}

void desktop_run(void) {
    if (!gui_init()) {
        serial_write("[serial] desktop_run: gui_init failed, aborting\n");
        return;
    }
    mouse_init((int)fb_get_width(), (int)fb_get_height());

    for (int i = 0; i < MAX_WINDOWS; i++) windows[i].active = 0;
    num_order = 0;
    focused_slot = -1;
    quit_requested = 0;
    dragging_window = -1;

    while (keyboard_has_char()) keyboard_getchar(); /* drain stale input before we take over routing */

    redraw();

    while (!quit_requested) {
        unsigned int target = timer_ticks() + 3; /* ~30Hz redraw cap */
        int need_redraw = 0;

        while (timer_ticks() < target && !quit_requested) {
            if (keyboard_has_char()) {
                route_keyboard();
                need_redraw = 1;
            } else if (mouse_consume_dirty()) {
                need_redraw = 1;
            } else {
                int any_dirty = 0;
                for (int i = 0; i < num_order; i++) {
                    int slot = window_order[i];
                    if (windows[slot].is_hosted && windows[slot].hosted_task &&
                        task_consume_vga_dirty(windows[slot].hosted_task)) {
                        any_dirty = 1;
                    }
                }
                if (any_dirty) need_redraw = 1;
                else { __asm__ volatile ("hlt"); }
            }
        }
        if (quit_requested) break;

        int mx = mouse_get_x(), my = mouse_get_y();
        unsigned char buttons = mouse_get_buttons();
        int left_down = buttons & 0x1;
        int left_was_down = prev_buttons & 0x1;
        int screen_h = (int)fb_get_height();

        if (left_down && !left_was_down) {
            if (my >= screen_h - TASKBAR_H) {
                taskbar_click(mx, my, screen_h);
                need_redraw = 1;
            } else {
                for (int i = num_order - 1; i >= 0; i--) {
                    int slot = window_order[i];
                    struct gui_window *win = &windows[slot];
                    if (!point_in_rect(mx, my, win->x, win->y, win->w, win->h)) continue;

                    if (close_button_hit(slot, mx, my)) {
                        close_window(slot);
                    } else if (point_in_rect(mx, my, win->x, win->y, win->w, TITLEBAR_H)) {
                        dragging_window = slot;
                        drag_offset_x = mx - win->x;
                        drag_offset_y = my - win->y;
                        bring_to_front(slot);
                        focused_slot = slot;
                    } else {
                        bring_to_front(slot);
                        focused_slot = slot;
                    }
                    need_redraw = 1;
                    break;
                }
            }
        } else if (!left_down) {
            dragging_window = -1;
        }

        if (dragging_window >= 0 && left_down && windows[dragging_window].active) {
            windows[dragging_window].x = mx - drag_offset_x;
            windows[dragging_window].y = my - drag_offset_y;
            need_redraw = 1;
        }

        prev_buttons = buttons;

        if (need_redraw) redraw();
    }

    /* Close everything on the way out so no hosted tasks (and their
     * input/output redirect state) linger after the desktop itself
     * has exited. */
    for (int i = 0; i < MAX_WINDOWS; i++) if (windows[i].active) close_window(i);

    /* Critical: hand the video hardware back to standard VGA text
     * mode. Without this, the console shell resumes completely
     * normally underneath (it's a real, independent task -- nothing
     * about it depends on graphics mode), but every character it
     * writes goes to the real text buffer (0xB8000), which the
     * hardware is no longer displaying, since it's still showing the
     * last rendered GUI frame. From the outside that looks exactly
     * like a total freeze, even though the system is fully alive.
     *
     * Fix: rather than trying to switch the video hardware back to
     * standard VGA text mode (tested and found unreliable -- see the
     * long comment in vga.c), we deliberately stay in graphics mode
     * permanently once it's been engaged, and vga_clear() now renders
     * through the framebuffer in that case, giving back a genuinely
     * working (if graphics-mode-backed) console. Full-screen wipe
     * first: vga_clear() only clears its own 80x25 console region
     * (640x400px), which would otherwise leave the old taskbar/window
     * pixels visible around the edges of the now-much-smaller console. */
    fb_fill_rect(0, 0, (int)fb_get_width(), (int)fb_get_height(), RGB(0, 0, 0));
    vga_clear();

    /* A bare prompt alone on an otherwise empty, now-much-larger
     * screen looks exactly like a frozen display to anyone not
     * looking closely -- especially right after leaving a colorful
     * full desktop. Print something unmistakable to confirm the
     * system is alive and back in control. */
    vga_write("Returned to the console from the GUI desktop.\n");
    vga_write("(Still fully alive throughout -- this is the same console session, just no longer full-screen graphics.)\n\n");

    serial_write("[serial] desktop_run: quit\n");
}
