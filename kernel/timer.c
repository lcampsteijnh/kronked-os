/* timer.c -- PIT (8253/8254) driver on IRQ0 */

#include "timer.h"
#include "idt.h"
#include "serial.h"
#include "task.h"

static inline void outb(unsigned short port, unsigned char val) { /* function for writing single byte to x86 I/O port */
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static volatile unsigned int tick = 0;

static void timer_callback(void) {
    tick++;
    /* Print a heartbeat once a second (assuming 100 Hz below) so we get
     * clear, spaced-out proof in the serial log that IRQs keep firing. */
    if (tick % 100 == 0) {
        serial_write("[serial] timer tick: ");
        char buf[12];
        int i = 10;
        buf[11] = '\0';
        unsigned int n = tick;
        if (n == 0) { buf[i--] = '0'; }
        while (n > 0) { buf[i--] = (char)('0' + (n % 10)); n /= 10; }
        serial_write(&buf[i + 1]);
        serial_write("\n");
    }

    schedule();
}

void timer_init(unsigned int freq_hz) {
    irq_install_handler(0, timer_callback);

    unsigned int divisor = 1193180 / freq_hz;
    outb(0x43, 0x36);                              /* channel 0, square wave */
    outb(0x40, (unsigned char)(divisor & 0xFF));       /* low byte */
    outb(0x40, (unsigned char)((divisor >> 8) & 0xFF)); /* high byte */
}

unsigned int timer_ticks(void) {
    return tick;
}
