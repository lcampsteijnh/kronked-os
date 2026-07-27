#ifndef VGA_H
#define VGA_H

void vga_clear(void);
void vga_putc(char c);
void vga_write(const char *s);
void vga_put_at(int row, int col, char c, unsigned char attr);
void vga_set_cursor_pos(int row, int col);

/* Redirects vga_putc/vga_write/vga_clear to an in-memory, virtual character
 * buffer (VGA-cell format: low byte = char, high byte = attribute)
 * instead of the real screen, with its own independent cursor. Pass
 * buffer=NULL to disable redirection and go back to the real screen. */
void vga_set_redirect(unsigned short *buffer, int cols, int rows);
int vga_redirect_consume_dirty(void); /* returns 1 once per change, then clears */

/* VGA text-mode color attribute helper: fg | (bg << 4) */
#define VGA_COLOR(fg, bg) ((unsigned char)((fg) | ((bg) << 4)))

enum vga_color {
    VGA_BLACK = 0, VGA_BLUE = 1, VGA_GREEN = 2, VGA_CYAN = 3,
    VGA_RED = 4, VGA_MAGENTA = 5, VGA_BROWN = 6, VGA_LIGHT_GREY = 7,
    VGA_DARK_GREY = 8, VGA_LIGHT_BLUE = 9, VGA_LIGHT_GREEN = 10, VGA_LIGHT_CYAN = 11,
    VGA_LIGHT_RED = 12, VGA_LIGHT_MAGENTA = 13, VGA_YELLOW = 14, VGA_WHITE = 15
};

#endif
