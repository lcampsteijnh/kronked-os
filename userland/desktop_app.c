/* desktop_app.c -- the GUI desktop, but running in ring 3 this time
 *
 * Architecturally this is the "correct" version of kernel/desktop.c:
 * a userspace process that asks the kernel for a mapped framebuffer
 * and polls mouse/keyboard state via syscalls, then does its own
 * compositing entirely in its own address space. The kernel doesn't
 * know or care what this process draws -- it just handed over a
 * memory-mapped rectangle of pixels and some input state.
 */

#include "font8x16.h"
#include "cursor_bitmap.h"

#define SYS_EXIT        0
#define SYS_WRITE       1
#define SYS_MAP_FB      5
#define SYS_MOUSE_STATE 6
#define SYS_GETCHAR_NB  7

struct fb_info { unsigned int vaddr, width, height, pitch; };
struct mouse_state { int x, y; unsigned int buttons; };

static inline int sys_write(const char *s) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(SYS_WRITE), "b"(s) : "memory");
    return ret;
}
static inline void sys_exit(int code) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_EXIT), "b"(code) : "memory");
}
static inline int sys_map_fb(struct fb_info *out) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(SYS_MAP_FB), "b"(out) : "memory");
    return ret;
}
static inline int sys_mouse_state(struct mouse_state *out) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(SYS_MOUSE_STATE), "b"(out) : "memory");
    return ret;
}
static inline int sys_getchar_nb(void) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(SYS_GETCHAR_NB) : "memory");
    return ret;
}

/* --- drawing, against our own mapped framebuffer --- */
static unsigned int *fb;
static unsigned int fb_width, fb_height, fb_pitch_px;

#define RGB(r, g, b) (((unsigned int)(r) << 16) | ((unsigned int)(g) << 8) | (unsigned int)(b))

static void put_pixel(int x, int y, unsigned int color) {
    if (x < 0 || y < 0 || (unsigned int)x >= fb_width || (unsigned int)y >= fb_height) return;
    fb[(unsigned int)y * fb_pitch_px + (unsigned int)x] = color;
}
static void fill_rect(int x, int y, int w, int h, unsigned int color) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w; if ((unsigned int)x1 > fb_width) x1 = (int)fb_width;
    int y1 = y + h; if ((unsigned int)y1 > fb_height) y1 = (int)fb_height;
    for (int yy = y0; yy < y1; yy++) {
        unsigned int *row = &fb[(unsigned int)yy * fb_pitch_px];
        for (int xx = x0; xx < x1; xx++) row[xx] = color;
    }
}
static void draw_rect(int x, int y, int w, int h, unsigned int color) {
    fill_rect(x, y, w, 1, color);
    fill_rect(x, y + h - 1, w, 1, color);
    fill_rect(x, y, 1, h, color);
    fill_rect(x + w - 1, y, 1, h, color);
}
static void draw_char(int x, int y, char c, unsigned int fg) {
    if (c < 32 || c > 126) c = '?';
    const unsigned char *glyph = font8x16_data[c - 32];
    for (int row = 0; row < 16; row++) {
        unsigned char bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) put_pixel(x + col, y + row, fg);
        }
    }
}
static void draw_string(int x, int y, const char *s, unsigned int fg) {
    int cx = x;
    while (*s) {
        if (*s == '\n') { cx = x; y += 16; s++; continue; }
        draw_char(cx, y, *s, fg);
        cx += 8;
        s++;
    }
}
static void draw_cursor(int x, int y) {
    for (int row = 0; row < cursor_height; row++) {
        for (int col = 0; col < cursor_width; col++) {
            unsigned char v = cursor_bitmap[row][col];
            if (v == 1) put_pixel(x + col, y + row, RGB(255, 255, 255));
            else if (v == 2) put_pixel(x + col, y + row, RGB(0, 0, 0));
        }
    }
}

/* --- windows --- */
#define MAX_WINDOWS 2
#define TITLEBAR_H 24

struct win { int x, y, w, h; const char *title; unsigned int body, bar; };
static struct win windows[MAX_WINDOWS] = {
    { 120, 100, 360, 220, "Ring 3 Window A", RGB(220, 220, 235), RGB(90, 60, 190) },
    { 420, 260, 360, 220, "Ring 3 Window B", RGB(235, 225, 210), RGB(190, 110, 40) },
};
static int order[MAX_WINDOWS] = { 0, 1 };

static void bring_to_front(int idx) {
    int pos = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) if (order[i] == idx) pos = i;
    if (pos < 0) return;
    for (int i = pos; i < MAX_WINDOWS - 1; i++) order[i] = order[i + 1];
    order[MAX_WINDOWS - 1] = idx;
}
static int in_rect(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

static void redraw(int mx, int my) {
    fill_rect(0, 0, (int)fb_width, (int)fb_height, RGB(20, 40, 35));

    for (int i = 0; i < MAX_WINDOWS; i++) {
        struct win *w = &windows[order[i]];
        fill_rect(w->x, w->y, w->w, w->h, w->body);
        fill_rect(w->x, w->y, w->w, TITLEBAR_H, w->bar);
        draw_string(w->x + 8, w->y + 4, w->title, RGB(255, 255, 255));
        draw_rect(w->x, w->y, w->w, w->h, RGB(0, 0, 0));
        draw_string(w->x + 12, w->y + TITLEBAR_H + 16, "userspace ring3 window", RGB(20, 20, 20));
    }

    draw_string(12, (int)fb_height - 24, "USERSPACE desktop (ring 3) -- drag title bars, 'q' to quit", RGB(210, 230, 220));
    draw_cursor(mx, my);
}

void _start(void) {
    sys_write("[desktop_app] requesting framebuffer from kernel...\n");

    struct fb_info info;
    if (sys_map_fb(&info) != 0) {
        sys_write("[desktop_app] SYS_MAP_FB failed!\n");
        sys_exit(1);
    }

    fb = (unsigned int *)info.vaddr;
    fb_width = info.width;
    fb_height = info.height;
    fb_pitch_px = info.pitch / 4;

    sys_write("[desktop_app] framebuffer mapped, drawing in ring 3 now.\n");

    int dragging = -1, drag_dx = 0, drag_dy = 0;
    unsigned int prev_buttons = 0;

    struct mouse_state m;
    sys_mouse_state(&m);
    redraw(m.x, m.y);
    int prev_mx = m.x, prev_my = m.y;

    for (;;) {
        int c = sys_getchar_nb();
        if (c == 'q') break;

        sys_mouse_state(&m);
        int left_down = m.buttons & 0x1;
        int left_was_down = prev_buttons & 0x1;
        int need_redraw = (m.x != prev_mx || m.y != prev_my);

        if (left_down && !left_was_down) {
            for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
                int idx = order[i];
                struct win *w = &windows[idx];
                if (in_rect(m.x, m.y, w->x, w->y, w->w, TITLEBAR_H)) {
                    dragging = idx;
                    drag_dx = m.x - w->x;
                    drag_dy = m.y - w->y;
                    bring_to_front(idx);
                    need_redraw = 1;
                    break;
                }
            }
        } else if (!left_down) {
            dragging = -1;
        }

        if (dragging >= 0 && left_down) {
            windows[dragging].x = m.x - drag_dx;
            windows[dragging].y = m.y - drag_dy;
            need_redraw = 1;
        }

        prev_buttons = m.buttons;
        prev_mx = m.x;
        prev_my = m.y;

        if (need_redraw) redraw(m.x, m.y);
    }

    sys_write("[desktop_app] quitting.\n");
    sys_exit(0);
}
