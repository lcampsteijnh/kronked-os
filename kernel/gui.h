#ifndef GUI_H
#define GUI_H

int gui_init(void);
void gui_get_fb_phys_info(unsigned int *phys, unsigned int *width,
                           unsigned int *height, unsigned int *pitch);
int gui_is_active(void);

#endif
