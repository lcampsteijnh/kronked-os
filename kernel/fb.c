/* fb.c -- linear framebuffer drawing primitives
 *
 * Everything here operates on a 32bpp linear framebuffer mapped at a
 * fixed virtual address (see gui_init() in gui.c for how it gets
 * there). Coordinates are plain pixels; colors are 0xRRGGBB.
 */

#include "fb.h"
#include "font8x16.h"

static unsigned int *fb = 0;
static unsigned int fb_width = 0, fb_height = 0, fb_pitch_px = 0;

void fb_init(unsigned int *framebuffer, unsigned int width, unsigned int height, unsigned int pitch_bytes) {
    fb = framebuffer;
    fb_width = width;
    fb_height = height;
    fb_pitch_px = pitch_bytes / 4;
}

unsigned int fb_get_width(void) { return fb_width; }
unsigned int fb_get_height(void) { return fb_height; }

void fb_put_pixel(int x, int y, unsigned int color) {
    if (x < 0 || y < 0 || (unsigned int)x >= fb_width || (unsigned int)y >= fb_height) return;
    fb[(unsigned int)y * fb_pitch_px + (unsigned int)x] = color;
}

void fb_fill_rect(int x, int y, int w, int h, unsigned int color) {
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w; if (x1 > (int)fb_width) x1 = (int)fb_width;
    int y1 = y + h; if (y1 > (int)fb_height) y1 = (int)fb_height;

    for (int yy = y0; yy < y1; yy++) {
        unsigned int *row = &fb[(unsigned int)yy * fb_pitch_px];
        for (int xx = x0; xx < x1; xx++) row[xx] = color;
    }
}

void fb_draw_rect(int x, int y, int w, int h, unsigned int color) {
    fb_fill_rect(x, y, w, 1, color);
    fb_fill_rect(x, y + h - 1, w, 1, color);
    fb_fill_rect(x, y, 1, h, color);
    fb_fill_rect(x + w - 1, y, 1, h, color);
}

void fb_draw_hline(int x, int y, int w, unsigned int color) { fb_fill_rect(x, y, w, 1, color); }
void fb_draw_vline(int x, int y, int h, unsigned int color) { fb_fill_rect(x, y, 1, h, color); }

void fb_draw_char(int x, int y, char c, unsigned int fg, unsigned int bg, int opaque) {
    if (c < 32 || c > 126) c = '?';
    const unsigned char *glyph = font8x16_data[c - 32];

    for (int row = 0; row < 16; row++) {
        unsigned char bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            int on = bits & (0x80 >> col);
            if (on) fb_put_pixel(x + col, y + row, fg);
            else if (opaque) fb_put_pixel(x + col, y + row, bg);
        }
    }
}

void fb_draw_string(int x, int y, const char *s, unsigned int fg, unsigned int bg, int opaque) {
    int cx = x;
    while (*s) {
        if (*s == '\n') { cx = x; y += 16; s++; continue; }
        fb_draw_char(cx, y, *s, fg, bg, opaque);
        cx += 8;
        s++;
    }
}
