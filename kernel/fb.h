#ifndef FB_H
#define FB_H

void fb_init(unsigned int *framebuffer, unsigned int width, unsigned int height, unsigned int pitch_bytes);
unsigned int fb_get_width(void);
unsigned int fb_get_height(void);

void fb_put_pixel(int x, int y, unsigned int color);
void fb_fill_rect(int x, int y, int w, int h, unsigned int color);
void fb_draw_rect(int x, int y, int w, int h, unsigned int color);
void fb_draw_hline(int x, int y, int w, unsigned int color);
void fb_draw_vline(int x, int y, int h, unsigned int color);
void fb_draw_char(int x, int y, char c, unsigned int fg, unsigned int bg, int opaque);
void fb_draw_string(int x, int y, const char *s, unsigned int fg, unsigned int bg, int opaque);

#define RGB(r, g, b) (((unsigned int)(r) << 16) | ((unsigned int)(g) << 8) | (unsigned int)(b))

#endif
