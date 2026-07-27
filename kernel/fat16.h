#ifndef FAT16_H
#define FAT16_H

struct fat16_file {
    unsigned short first_cluster;
    unsigned int file_size;
};

int fat16_mount(void);
int fat16_find_file(const char *filename, struct fat16_file *out);
int fat16_read_file(const struct fat16_file *file, unsigned char *buffer, unsigned int buffer_size);
void fat16_list_root(void (*callback)(const char *name, unsigned int size));
int fat16_write_file(const char *filename, const unsigned char *data, unsigned int size);
int fat16_delete_file(const char *filename);

#endif
