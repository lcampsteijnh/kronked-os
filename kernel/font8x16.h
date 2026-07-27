#ifndef FONT8X16_H
#define FONT8X16_H

/* ASCII 32 ('space') through 126 ('~'), 16 bytes per glyph, one byte
 * per row, bit 7 = leftmost pixel of an 8-pixel-wide glyph. */
extern const unsigned char font8x16_data[95][16];

#endif
