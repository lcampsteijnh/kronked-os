/* pci.c -- minimal PCI configuration space access
 *
 * Just enough to enumerate bus 0 and find the display controller
 * (class 0x03) so we can read its BAR0 -- the physical address of its
 * linear framebuffer. This is the portable, correct way to find it;
 * hardcoding a "well-known QEMU address" would work in our specific
 * setup but wouldn't be honest as a general technique.
 */

#include "pci.h"
#include "serial.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static inline void outl(unsigned short port, unsigned int val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}
static inline unsigned int inl(unsigned short port) {
    unsigned int ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static unsigned int pci_config_read32(unsigned char bus, unsigned char slot,
                                       unsigned char func, unsigned char offset) {
    unsigned int address =
        (1u << 31) | ((unsigned int)bus << 16) | ((unsigned int)slot << 11) |
        ((unsigned int)func << 8) | (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

static void print_hex(unsigned int n) {
    char buf[9];
    buf[8] = '\0';
    for (int i = 7; i >= 0; i--) {
        unsigned int nib = n & 0xF;
        buf[i] = (char)(nib < 10 ? '0' + nib : 'a' + (nib - 10));
        n >>= 4;
    }
    serial_write(buf);
}

/* Scans bus 0, all 32 device slots, function 0 only (good enough --
 * multi-function display devices are essentially unheard of). Looks
 * for class code 0x03 (display controller), returns its BAR0 masked
 * down to a physical address with the low status bits cleared. */
int pci_find_vga_framebuffer(unsigned int *out_phys_addr) {
    for (unsigned int slot = 0; slot < 32; slot++) {
        unsigned int id = pci_config_read32(0, (unsigned char)slot, 0, 0x00);
        unsigned short vendor = (unsigned short)(id & 0xFFFF);
        if (vendor == 0xFFFF) continue; /* no device in this slot */

        unsigned int class_reg = pci_config_read32(0, (unsigned char)slot, 0, 0x08);
        unsigned char class_code = (unsigned char)(class_reg >> 24);
        unsigned char subclass   = (unsigned char)(class_reg >> 16);

        if (class_code == 0x03) { /* display controller */
            unsigned int bar0 = pci_config_read32(0, (unsigned char)slot, 0, 0x10);

            serial_write("[serial] pci: display controller found, vendor=0x");
            print_hex(vendor);
            serial_write(" slot=0x"); print_hex(slot);
            serial_write(" subclass=0x"); print_hex(subclass);
            serial_write(" BAR0=0x"); print_hex(bar0);
            serial_write("\n");

            if (bar0 & 0x1) continue; /* I/O space BAR, not memory -- not what we want */
            *out_phys_addr = bar0 & 0xFFFFFFF0;
            return 1;
        }
    }
    serial_write("[serial] pci: no display controller found on bus 0\n");
    return 0;
}
