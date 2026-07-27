/* cursor_bitmap.c -- arrow cursor sprite, rasterized via tools/gen_cursor.py
 * (same rationale as the font: generated from a real polygon and
 * visually verified, not hand-transcribed pixel by pixel). */

#include "cursor_bitmap.h"

const unsigned char cursor_width = 12;
const unsigned char cursor_height = 19;

/* row-major, 1 byte per pixel: 0=transparent, 1=white fill, 2=black outline */
const unsigned char cursor_bitmap[19][12] = {
    { 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 2, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 2, 1, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 2, 1, 1, 1, 2, 0, 0, 0, 0, 0, 0, 0 },
    { 2, 1, 1, 1, 1, 2, 0, 0, 0, 0, 0, 0 },
    { 2, 1, 1, 1, 1, 1, 2, 0, 0, 0, 0, 0 },
    { 2, 1, 1, 1, 1, 1, 1, 2, 0, 0, 0, 0 },
    { 2, 1, 1, 1, 1, 1, 1, 1, 2, 0, 0, 0 },
    { 2, 1, 1, 1, 1, 1, 1, 1, 1, 2, 0, 0 },
    { 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 0 },
    { 2, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2 },
    { 2, 1, 1, 1, 2, 1, 2, 0, 0, 0, 0, 0 },
    { 2, 1, 1, 2, 2, 1, 1, 2, 0, 0, 0, 0 },
    { 2, 1, 2, 0, 0, 2, 1, 2, 0, 0, 0, 0 },
    { 2, 2, 0, 0, 0, 2, 1, 2, 0, 0, 0, 0 },
    { 2, 0, 0, 0, 0, 2, 1, 1, 2, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 2, 2, 2, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0 },
};
