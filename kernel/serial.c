/* serial.c -- minimal 16550 UART driver on COM1 (port 0x3F8)
 *
 * This exists mainly so we have a way to prove the kernel is alive
 * when running headless under QEMU (no display attached): we print to
 * the serial port and QEMU forwards that to the terminal via -serial stdio.
 */

#include "serial.h"

#define COM1 0x3F8

static inline void outb(unsigned short port, unsigned char val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline unsigned char inb(unsigned short port) {
    unsigned char ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void serial_init(void) {
    outb(COM1 + 1, 0x00);    /* disable interrupts */
    outb(COM1 + 3, 0x80);    /* enable DLAB (set baud rate divisor) */
    outb(COM1 + 0, 0x03);    /* divisor low byte: 38400 baud */
    outb(COM1 + 1, 0x00);    /* divisor high byte */
    outb(COM1 + 3, 0x03);    /* 8 bits, no parity, one stop bit */
    outb(COM1 + 2, 0xC7);    /* enable FIFO, clear, 14-byte threshold */
    outb(COM1 + 4, 0x0B);    /* IRQs enabled, RTS/DSR set */
}

static int serial_tx_empty(void) {
    return inb(COM1 + 5) & 0x20;
}

void serial_putc(char c) {
    while (!serial_tx_empty()) { }
    outb(COM1, (unsigned char)c);
}

void serial_write(const char *s) {
    while (*s) {
        if (*s == '\n') serial_putc('\r'); /* CRLF for a sane terminal */
        serial_putc(*s++);
    }
}
