#ifndef MOUSE_H
#define MOUSE_H

void mouse_init(int screen_width, int screen_height);
int mouse_get_x(void);
int mouse_get_y(void);
unsigned char mouse_get_buttons(void); /* bit0=left, bit1=right, bit2=middle */
int mouse_consume_dirty(void); /* returns 1 once per state change, then clears */

#endif
