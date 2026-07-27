#ifndef GDT_H
#define GDT_H

void gdt_init(void);
void tss_set_kernel_stack(unsigned int esp0);

#endif
