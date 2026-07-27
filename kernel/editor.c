/* editor.c -- a small line-based text editor
 *
 * Not a full-screen editor with cursor movement -- this is closer to
 * old-school line editors like `ed`: you type lines, they get
 * appended to the buffer, and a handful of dot-commands control
 * saving/quitting/listing. Simple, but it creates and
 * overwrites real files on the FAT16 disk via fat16_write_file().
 *
 * Known limitation: since lines starting with '.' are meta-commands,
 * you can't type a content line that starts with a literal '.' -- a
 * real editor would need an escape mechanism for that; this one
 * doesn't bother, for the sake of staying simple.
 */

#include "editor.h"
#include "vga.h"
#include "keyboard.h"
#include "fat16.h"
#include "heap.h"

#define EDITOR_BUF_SIZE 4096
#define LINE_MAX 128

static void read_line(char *buf, int max_len) {
    int len = 0;
    for (;;) {
        char c = keyboard_getchar();
        if (c == '\n') {
            vga_putc('\n');
            buf[len] = '\0';
            return;
        } else if (c == '\b') {
            if (len > 0) { len--; vga_putc('\b'); }
        } else if (len < max_len - 1) {
            buf[len++] = c;
            vga_putc(c);
        }
    }
}

static int str_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

void editor_run(const char *filename) {
    char *buf = (char *)kmalloc(EDITOR_BUF_SIZE);
    unsigned int len = 0;

    struct fat16_file f;
    if (fat16_find_file(filename, &f)) {
        unsigned int to_read = f.file_size;
        if (to_read > EDITOR_BUF_SIZE - 1) to_read = EDITOR_BUF_SIZE - 1;
        if (fat16_read_file(&f, (unsigned char *)buf, to_read)) {
            len = to_read;
            vga_write("Loaded existing file (");
            char sizebuf[11]; int i = 10; sizebuf[10] = '\0';
            unsigned int n = f.file_size;
            if (n == 0) { vga_write("0"); } else {
                while (n > 0 && i > 0) { sizebuf[--i] = (char)('0' + (n % 10)); n /= 10; }
                vga_write(&sizebuf[i]);
            }
            vga_write(" bytes):\n---\n");
            for (unsigned int j = 0; j < len; j++) vga_putc(buf[j]);
            vga_write("\n---\n");
        }
    } else {
        vga_write("New file.\n");
    }

    vga_write("Editing '"); vga_write(filename); vga_write("'.\n");
    vga_write("Type text; each Enter adds a line. Commands:\n");
    vga_write("  .save   write to disk and exit\n");
    vga_write("  .quit   discard changes and exit\n");
    vga_write("  .list   show current buffer\n");
    vga_write("  .clear  erase the buffer (start over)\n\n");

    char line[LINE_MAX];
    for (;;) {
        read_line(line, LINE_MAX);

        if (str_eq(line, ".save")) {
            if (fat16_write_file(filename, (const unsigned char *)buf, len)) {
                vga_write("Saved.\n");
            } else {
                vga_write("Save FAILED.\n");
            }
            kfree(buf);
            return;
        } else if (str_eq(line, ".quit")) {
            vga_write("Discarded.\n");
            kfree(buf);
            return;
        } else if (str_eq(line, ".list")) {
            vga_write("---\n");
            for (unsigned int j = 0; j < len; j++) vga_putc(buf[j]);
            vga_write("---\n");
        } else if (str_eq(line, ".clear")) {
            len = 0;
            vga_write("Buffer cleared.\n");
        } else {
            unsigned int line_len = 0;
            while (line[line_len]) line_len++;

            if (len + line_len + 1 >= EDITOR_BUF_SIZE) {
                vga_write("Buffer full -- can't add more (try .save).\n");
                continue;
            }
            for (unsigned int j = 0; j < line_len; j++) buf[len++] = line[j];
            buf[len++] = '\n';
        }
    }
}
