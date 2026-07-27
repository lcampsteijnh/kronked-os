/* vga.c -- text console driver
 *
 * Also mirrors everything to the serial port -- lets us (and anyone
 * else) see what's on screen from a machine with no display attached.
 *
 * Supports per-task output redirection (see task.c's comment on
 * vga_redirect_buf) for hosting the shell/editor inside GUI windows.
 *
 * The "real screen" (non-redirected) path has two backends:
 *   1. Raw VGA text memory (0xB8000) -- used until graphics mode has
 *      ever been engaged.
 *   2. Once it has (gui_is_active()), rendered through the linear
 *      framebuffer instead, using the same font the GUI already uses.
 *      This exists because disabling the Bochs VBE linear-framebuffer
 *      mode turned out not to reliably hand the display back to a
 *      working standard-VGA text mode on this hardware/emulator combo.
 */

#include "vga.h"
#include "serial.h"
#include "task.h"
#include "gui.h"
#include "fb.h"

static unsigned short * const VGA_MEMORY = (unsigned short *)0xB8000;
static const int VGA_WIDTH = 80;
static const int VGA_HEIGHT = 25;

static int cursor_row = 0;
static int cursor_col = 0;
static unsigned char color = 0x0F; /* white on black */

static inline unsigned short vga_entry(char c, unsigned char color) {
    return (unsigned short)c | ((unsigned short)color << 8);
}

/* --- standard 16-color VGA palette, for rendering through the
 * framebuffer once graphics mode is active (needed by vga_put_at,
 * which Snake uses with explicit color attributes) --- */
static unsigned int vga_color_to_rgb(unsigned char idx) {
    static const unsigned int palette[16] = {
        0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
        0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
    };
    return palette[idx & 0xF];
}

void vga_set_redirect(unsigned short *buffer, int cols, int rows) {
    task_set_vga_redirect(task_get_current(), buffer, cols, rows);
}

int vga_redirect_consume_dirty(void) {
    return task_consume_vga_dirty(task_get_current());
}

static void redirect_scroll(unsigned short *buf, int cols, int rows, int *row_ptr) {
    for (int y = 1; y < rows; y++)
        for (int x = 0; x < cols; x++)
            buf[(y - 1) * cols + x] = buf[y * cols + x];
    for (int x = 0; x < cols; x++)
        buf[(rows - 1) * cols + x] = vga_entry(' ', color);
    *row_ptr = rows - 1;
}

static void redirect_putc(char c) {
    unsigned short *buf = task_current_vga_buf();
    int cols = task_current_vga_cols();
    int rows = task_current_vga_rows();
    int *row_ptr = task_current_vga_row_ptr();
    int *col_ptr = task_current_vga_col_ptr();

    if (c == '\n') {
        *col_ptr = 0;
        (*row_ptr)++;
    } else if (c == '\b') {
        if (*col_ptr > 0) {
            (*col_ptr)--;
        } else if (*row_ptr > 0) {
            (*row_ptr)--;
            *col_ptr = cols - 1;
        }
        buf[(*row_ptr) * cols + (*col_ptr)] = vga_entry(' ', color);
    } else {
        buf[(*row_ptr) * cols + (*col_ptr)] = vga_entry(c, color);
        (*col_ptr)++;
        if (*col_ptr >= cols) {
            *col_ptr = 0;
            (*row_ptr)++;
        }
    }
    if (*row_ptr >= rows) redirect_scroll(buf, cols, rows, row_ptr);
    task_current_vga_mark_dirty();

    if (c == '\n') serial_putc('\r');
    if (c != '\b') serial_putc(c);
}

/* --- "real screen", framebuffer-backed variant (once gui_is_active()) ---
 *
 * Uses the *full* screen resolution rather than staying at the legacy
 * 80x25 text-mode size: at 1024x768 with an 8x16 font that's exactly
 * 128 columns x 48 rows, a perfect fit with the existing font. Filling
 * only an 80x25 corner (640x400px) left most of a 1024x768 screen as
 * an enormous black void around a small patch of text. */
#define REAL_FB_COLS 128
#define REAL_FB_ROWS 48
static unsigned short real_screen_buf[REAL_FB_ROWS * REAL_FB_COLS];
static int real_screen_initialized = 0;

static void real_render_char(int row, int col) {
    unsigned short cell = real_screen_buf[row * REAL_FB_COLS + col];
    char ch = (char)(cell & 0xFF);
    if (ch == 0) ch = ' ';
    fb_draw_char(col * 8, row * 16, ch, RGB(220, 220, 220), RGB(0, 0, 0), 1);
}

static void real_render_all(void) {
    fb_fill_rect(0, 0, REAL_FB_COLS * 8, REAL_FB_ROWS * 16, RGB(0, 0, 0));
    for (int r = 0; r < REAL_FB_ROWS; r++)
        for (int c = 0; c < REAL_FB_COLS; c++)
            real_render_char(r, c);
}

static void real_screen_ensure_init(void) {
    if (real_screen_initialized) return;
    for (int i = 0; i < REAL_FB_ROWS * REAL_FB_COLS; i++) real_screen_buf[i] = vga_entry(' ', color);
    real_screen_initialized = 1;
    real_render_all();
}

static void real_putc_framebuffer(char c) {
    real_screen_ensure_init();

    if (c == '\n') {
        cursor_col = 0;
        cursor_row++;
    } else if (c == '\b') {
        if (cursor_col > 0) cursor_col--;
        else if (cursor_row > 0) { cursor_row--; cursor_col = REAL_FB_COLS - 1; }
        real_screen_buf[cursor_row * REAL_FB_COLS + cursor_col] = vga_entry(' ', color);
        real_render_char(cursor_row, cursor_col);
    } else {
        real_screen_buf[cursor_row * REAL_FB_COLS + cursor_col] = vga_entry(c, color);
        real_render_char(cursor_row, cursor_col);
        cursor_col++;
        if (cursor_col >= REAL_FB_COLS) { cursor_col = 0; cursor_row++; }
    }

    if (cursor_row >= REAL_FB_ROWS) {
        for (int y = 1; y < REAL_FB_ROWS; y++)
            for (int x = 0; x < REAL_FB_COLS; x++)
                real_screen_buf[(y - 1) * REAL_FB_COLS + x] = real_screen_buf[y * REAL_FB_COLS + x];
        for (int x = 0; x < REAL_FB_COLS; x++)
            real_screen_buf[(REAL_FB_ROWS - 1) * REAL_FB_COLS + x] = vga_entry(' ', color);
        cursor_row = REAL_FB_ROWS - 1;
        real_render_all();
    }

    if (c == '\n') serial_putc('\r');
    if (c != '\b') serial_putc(c);
}

void vga_clear(void) {
    unsigned short *buf = task_current_vga_buf();
    if (buf) {
        int cols = task_current_vga_cols();
        int rows = task_current_vga_rows();
        for (int y = 0; y < rows; y++)
            for (int x = 0; x < cols; x++)
                buf[y * cols + x] = vga_entry(' ', color);
        *task_current_vga_row_ptr() = 0;
        *task_current_vga_col_ptr() = 0;
        task_current_vga_mark_dirty();
        return;
    }

    if (gui_is_active()) {
        for (int i = 0; i < REAL_FB_ROWS * REAL_FB_COLS; i++) real_screen_buf[i] = vga_entry(' ', color);
        real_screen_initialized = 1;
        real_render_all();
        cursor_row = 0;
        cursor_col = 0;
        return;
    }

    for (int y = 0; y < VGA_HEIGHT; y++)
        for (int x = 0; x < VGA_WIDTH; x++)
            VGA_MEMORY[y * VGA_WIDTH + x] = vga_entry(' ', color);
    cursor_row = 0;
    cursor_col = 0;
}

static void vga_scroll(void) {
    for (int y = 1; y < VGA_HEIGHT; y++)
        for (int x = 0; x < VGA_WIDTH; x++)
            VGA_MEMORY[(y - 1) * VGA_WIDTH + x] = VGA_MEMORY[y * VGA_WIDTH + x];
    for (int x = 0; x < VGA_WIDTH; x++)
        VGA_MEMORY[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = vga_entry(' ', color);
    cursor_row = VGA_HEIGHT - 1;
}

void vga_putc(char c) {
    if (task_current_vga_buf()) {
        redirect_putc(c);
        return;
    }

    if (gui_is_active()) {
        real_putc_framebuffer(c);
        return;
    }

    if (c == '\n') {
        cursor_col = 0;
        cursor_row++;
    } else if (c == '\b') {
        if (cursor_col > 0) {
            cursor_col--;
        } else if (cursor_row > 0) {
            cursor_row--;
            cursor_col = VGA_WIDTH - 1;
        }
        VGA_MEMORY[cursor_row * VGA_WIDTH + cursor_col] = vga_entry(' ', color);
    } else {
        VGA_MEMORY[cursor_row * VGA_WIDTH + cursor_col] = vga_entry(c, color);
        cursor_col++;
        if (cursor_col >= VGA_WIDTH) {
            cursor_col = 0;
            cursor_row++;
        }
    }
    if (cursor_row >= VGA_HEIGHT)
        vga_scroll();

    if (c == '\n') serial_putc('\r');
    if (c != '\b') serial_putc(c); /* mirror screen to serial; skip \b, serial has no cursor to move */
}

void vga_write(const char *s) {
    while (*s) vga_putc(*s++);
}

/* Direct cell write at (row, col) with an explicit color attribute --
 * doesn't touch the cursor or scroll, doesn't mirror to serial (no
 * sane way to represent a 2D redraw over a serial line). Snake's only
 * user. Once graphics mode is active, routes through the framebuffer
 * like everything else on the real screen. */
void vga_put_at(int row, int col, char c, unsigned char attr) {
    if (row < 0 || row >= VGA_HEIGHT || col < 0 || col >= VGA_WIDTH) return;

    if (gui_is_active()) {
        unsigned int fg = vga_color_to_rgb(attr & 0x0F);
        unsigned int bg = vga_color_to_rgb((attr >> 4) & 0x0F);
        fb_draw_char(col * 8, row * 16, c, fg, bg, 1);
        return;
    }

    VGA_MEMORY[row * VGA_WIDTH + col] = vga_entry(c, attr);
}

void vga_set_cursor_pos(int row, int col) {
    if (task_current_vga_buf()) {
        *task_current_vga_row_ptr() = row;
        *task_current_vga_col_ptr() = col;
        return;
    }
    cursor_row = row;
    cursor_col = col;
}
