/* vbe.c -- Bochs VBE extensions (the "DISPI" interface)
 *
 * QEMU's standard VGA device (and Bochs, which it's compatible with)
 * exposes a small set of registers over two I/O ports that let you
 * set an arbitrary resolution/depth and enable a linear framebuffer
 * -- entirely via port I/O, no real-mode BIOS calls needed. This is
 * the standard technique hobby kernels use to get graphics without
 * the considerable extra complexity of a real-mode trampoline.
 */

#include "vbe.h"
#include "serial.h"

#define VBE_DISPI_IOPORT_INDEX 0x01CE
#define VBE_DISPI_IOPORT_DATA  0x01CF

#define VBE_DISPI_INDEX_ID           0
#define VBE_DISPI_INDEX_XRES         1
#define VBE_DISPI_INDEX_YRES         2
#define VBE_DISPI_INDEX_BPP          3
#define VBE_DISPI_INDEX_ENABLE       4
#define VBE_DISPI_INDEX_VIRT_WIDTH   6
#define VBE_DISPI_INDEX_VIRT_HEIGHT  7

#define VBE_DISPI_DISABLED     0x00
#define VBE_DISPI_ENABLED      0x01
#define VBE_DISPI_LFB_ENABLED  0x40

static inline void outw(unsigned short port, unsigned short val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline unsigned short inw(unsigned short port) {
    unsigned short ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static void vbe_write(unsigned short index, unsigned short value) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    outw(VBE_DISPI_IOPORT_DATA, value);
}
static unsigned short vbe_read(unsigned short index) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    return inw(VBE_DISPI_IOPORT_DATA);
}

int vbe_set_mode(unsigned int width, unsigned int height, unsigned int bpp) {
    unsigned short id = vbe_read(VBE_DISPI_INDEX_ID);
    serial_write("[serial] vbe: DISPI id register = ");
    /* quick decimal print, values are small (0xB0xx range) */
    char buf[6]; int i = 5; buf[5] = '\0';
    unsigned int n = id;
    if (n == 0) { buf[--i] = '0'; }
    while (n > 0 && i > 0) { buf[--i] = (char)('0' + n % 10); n /= 10; }
    serial_write(&buf[i]);
    serial_write("\n");

    vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    vbe_write(VBE_DISPI_INDEX_XRES, (unsigned short)width);
    vbe_write(VBE_DISPI_INDEX_YRES, (unsigned short)height);
    vbe_write(VBE_DISPI_INDEX_BPP, (unsigned short)bpp);
    vbe_write(VBE_DISPI_INDEX_VIRT_WIDTH, (unsigned short)width);
    vbe_write(VBE_DISPI_INDEX_VIRT_HEIGHT, (unsigned short)height);
    vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

    unsigned short actual_x = vbe_read(VBE_DISPI_INDEX_XRES);
    unsigned short actual_y = vbe_read(VBE_DISPI_INDEX_YRES);
    unsigned short actual_bpp = vbe_read(VBE_DISPI_INDEX_BPP);

    if (actual_x != width || actual_y != height) {
        serial_write("[serial] vbe: mode did not take -- requested resolution mismatch\n");
        return 0;
    }

    serial_write("[serial] vbe: mode set OK\n");
    (void)actual_bpp;
    return 1;
}

/* Turns off the Bochs DISPI linear-framebuffer mode, handing display
 * control back to the standard VGA register set -- i.e. back to
 * ordinary 80x25 text mode, since that's what was active before
 * vbe_set_mode() was ever called and nothing else reprogrammed the
 * standard VGA registers away from it. Without this, exiting the GUI
 * leaves the video hardware permanently in graphics mode: the kernel
 * keeps running completely normally underneath (scheduler, keyboard,
 * the shell task all fine), it's just that writes to the real VGA
 * text buffer (0xB8000) go to memory the hardware isn't displaying
 * anymore -- which looks exactly like a frozen screen even though
 * nothing has actually stopped. */
void vbe_disable(void) {
    vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    serial_write("[serial] vbe: disabled, back to standard VGA text mode\n");
}
