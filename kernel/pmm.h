#ifndef PMM_H
#define PMM_H

struct multiboot_info;

void pmm_init(unsigned int mb_magic, struct multiboot_info *mbi);
unsigned int pmm_alloc_frame(void);
void pmm_free_frame(unsigned int phys_addr);
void pmm_frame_addref(unsigned int phys_addr);
unsigned int pmm_frame_refcount(unsigned int phys_addr);
unsigned int pmm_free_frame_count(void);
unsigned int pmm_detected_ram_kib(void);

#endif
