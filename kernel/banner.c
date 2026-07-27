/* banner.c -- the KRONKED-OS startup banner
 *
 * Printed via plain vga_write()/vga_putc(), so it works identically
 * whether this is the real text console or a GUI terminal window
 * (vga.c's per-task output redirection makes that transparent) --
 * every terminal, wherever it's hosted, gets the same banner at the
 * top when it starts.
 */

#include "banner.h"
#include "vga.h"
#include "cpuinfo.h"
#include "pmm.h"

static const char *banner_lines[] = {
    " _  _____  ___  _  _ _  _____ ___       ___  ___ ",
    "| |/ / _ \\/ _ \\| \\| | |/ / __|   \\ ___ / _ \\/ __|",
    "| ' <|   / (_) | .` | ' <| _|| |) |___| (_) \\__ \\",
    "|_|\\_\\_|_\\\\___/|_|\\_|_|\\_\\___|___/     \\___/|___/",
    0
};

static void print_dec(unsigned int n) {
    char buf[11];
    int i = 10;
    buf[10] = '\0';
    if (n == 0) { vga_write("0"); return; }
    while (n > 0 && i > 0) { buf[--i] = (char)('0' + (n % 10)); n /= 10; }
    vga_write(&buf[i]);
}

void banner_print(void) {
    for (int i = 0; banner_lines[i]; i++) {
        vga_write(banner_lines[i]);
        vga_write("\n");
    }
    vga_write("\n");

    char brand[49];
    char vendor[13];
    cpu_get_vendor(vendor);

    vga_write("  CPU: ");
    if (cpu_get_brand(brand)) {
        /* brand strings are space-padded by the CPU itself; a little
         * cosmetic trimming of leading spaces makes this look right
         * on CPUs that report a short brand string. */
        const char *b = brand;
        while (*b == ' ') b++;
        vga_write(b);
    } else {
        vga_write(vendor);
    }
    vga_write("\n");

    vga_write("  RAM:  ");
    print_dec(pmm_detected_ram_kib() / 1024);
    vga_write(" MiB detected (");
    print_dec(pmm_free_frame_count() * 4);
    vga_write(" KiB currently free)\n");

    vga_write("  Vendor ID: ");
    vga_write(vendor);
    vga_write("\n\n");
}
