#ifndef PAGING_H
#define PAGING_H

void paging_init(void);
int paging_map_page(unsigned int phys_addr, unsigned int virt_addr, unsigned int flags);

unsigned int paging_clone_kernel_directory(void);
int paging_map_user_page(unsigned int dir_phys, unsigned int phys_addr,
                          unsigned int virt_addr, unsigned int flags);
void paging_switch_directory(unsigned int dir_phys);
unsigned int paging_kernel_directory_phys(void);
unsigned int paging_clone_address_space(unsigned int parent_dir_phys);
void paging_free_address_space(unsigned int dir_phys);
int paging_map_kernel_page(unsigned int phys_addr, unsigned int virt_addr, unsigned int flags);
int paging_handle_cow_fault(unsigned int fault_addr);

#endif
