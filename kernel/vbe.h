#ifndef VBE_H
#define VBE_H

int vbe_set_mode(unsigned int width, unsigned int height, unsigned int bpp);
void vbe_disable(void);

#endif
