/* mouse.c -- PS/2 mouse driver
 *
 * The PS/2 mouse lives on the "auxiliary device" port of the 8042
 * keyboard controller, sharing the same two I/O ports (0x60 data,
 * 0x64 status/command) as the keyboard but needing an extra command
 * (0xD4) to route bytes to it instead of the keyboard, and its own
 * IRQ line (12, not 1). Once enabled, it streams 3-byte packets:
 * a status/sign byte, then signed X and Y deltas.
 *
 * Real issues found and fixed here, across two rounds of testing:
 *
 * 1. The very first packet after enabling data reporting (command
 *    0xF4) is empirically unreliable -- a known real-world PS/2
 *    quirk. Fixed by unconditionally discarding it (discard_next_packet).
 *
 * 2. Packets can have the X/Y overflow flags set (status bits 6/7),
 *    meaning the device is reporting that the true movement since the
 *    last sample may have exceeded what one packet can represent
 *    precisely. This was first investigated using QEMU's monitor
 *    `mouse_move` command (which can synthesize a large, discrete,
 *    combined-axis jump in one call, reliably tripping overflow) --
 *    per-packet serial logging showed those specific overflowed
 *    packets carrying a fixed, bogus dx regardless of the requested
 *    delta. The initial fix was to discard the whole packet whenever
 *    overflow was flagged, which fixed that specific synthetic
 *    artifact. That turned out to be an overcorrection: a *real*
 *    mouse, moved continuously by an actual person (rather than
 *    QEMU's monitor script), likely sets this same flag occasionally
 *    during entirely normal use, and discarding those packets made the
 *    cursor unresponsive enough that windows became hard to select at
 *    all under real hardware/human use -- reported after the fact,
 *    once actually used interactively rather than only through
 *    scripted monitor commands. Now the delta is applied normally
 *    regardless of the overflow flag (matching what most real mouse
 *    drivers do -- accept some imprecision rather than discard the
 *    sample), with the existing screen-edge clamping below as the
 *    safety net against anything genuinely extreme. Lesson: synthetic,
 *    scripted input testing caught real bugs throughout this project,
 *    but it's not a full substitute for testing with the real input
 *    device the code is ultimately meant to handle.
 */

#include "mouse.h"
#include "idt.h"
#include "serial.h"

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_COMMAND 0x64

static inline void outb(unsigned short port, unsigned char val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline unsigned char inb(unsigned short port) {
    unsigned char ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static void ps2_wait_write(void) {
    for (int i = 0; i < 100000; i++) {
        if (!(inb(PS2_STATUS) & 0x02)) return; /* input buffer empty -- OK to write */
    }
}
static void ps2_wait_read(void) {
    for (int i = 0; i < 100000; i++) {
        if (inb(PS2_STATUS) & 0x01) return; /* output buffer full -- data ready */
    }
}

static void mouse_write(unsigned char val) {
    ps2_wait_write();
    outb(PS2_COMMAND, 0xD4); /* "next byte on 0x60 goes to the mouse" */
    ps2_wait_write();
    outb(PS2_DATA, val);
}
static unsigned char mouse_read(void) {
    ps2_wait_read();
    return inb(PS2_DATA);
}

static int screen_w = 1024, screen_h = 768;
static int mouse_x = 512, mouse_y = 384;
static unsigned char mouse_buttons = 0;
static int mouse_dirty = 0;

static unsigned char packet[3];
static int packet_index = 0;
static int discard_next_packet = 0;

static void mouse_callback(void) {
    unsigned char data = inb(PS2_DATA);

    if (packet_index == 0) {
        /* Bit 3 of the first byte of every real packet is always 1.
         * If this byte doesn't look like a valid packet start,
         * discard it and keep waiting rather than accepting it and
         * potentially misinterpreting every subsequent byte's role
         * for as long as sparse/bursty input keeps arriving. */
        if (!(data & 0x08)) return;
    }

    packet[packet_index++] = data;
    if (packet_index < 3) return;
    packet_index = 0;

    unsigned char status = packet[0];

    if (discard_next_packet) {
        discard_next_packet = 0;
        return;
    }

    mouse_buttons = status & 0x07;

    /* Bits 6/7 are the X/Y overflow flags: the device is reporting
     * that the true movement since the last sample may have exceeded
     * what one packet can represent precisely. Earlier testing (with
     * QEMU's monitor `mouse_move` command doing large, synthetic,
     * combined-axis jumps) found this flag set with a suspicious,
     * seemingly-bogus fixed delta, and the fix at the time was to
     * discard the packet outright. That was an overcorrection: a
     * *real* mouse, moved continuously by an actual person, likely
     * sets this flag occasionally too during normal use, and most
     * real drivers still apply the reported delta even when it's
     * flagged (accepting some imprecision) rather than throwing the
     * whole sample away. Discarding it was making the cursor
     * unresponsive to genuine hardware input. Apply it normally now;
     * the existing screen-edge clamping below already bounds anything
     * extreme, which is enough protection against a genuinely wild
     * value without sacrificing responsiveness to real, valid input. */

    int dx = packet[1];
    int dy = packet[2];
    if (status & 0x10) dx -= 256; /* sign bits */
    if (status & 0x20) dy -= 256;

    mouse_x += dx;
    mouse_y -= dy; /* PS/2 Y+ is up; screen Y+ is down */

    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x >= screen_w) mouse_x = screen_w - 1;
    if (mouse_y >= screen_h) mouse_y = screen_h - 1;

    mouse_dirty = 1;
}

void mouse_init(int screen_width, int screen_height) {
    screen_w = screen_width;
    screen_h = screen_height;
    mouse_x = screen_width / 2;
    mouse_y = screen_height / 2;

    ps2_wait_write();
    outb(PS2_COMMAND, 0xA8); /* enable auxiliary device */

    ps2_wait_write();
    outb(PS2_COMMAND, 0x20); /* read controller config byte */
    unsigned char status = mouse_read();
    status |= 0x02;  /* enable IRQ12 */
    status &= ~0x20; /* ensure aux clock enabled */
    ps2_wait_write();
    outb(PS2_COMMAND, 0x60);
    ps2_wait_write();
    outb(PS2_DATA, status);

    mouse_write(0xF6); /* set defaults */
    mouse_read();       /* ack */
    mouse_write(0xF4); /* enable data reporting */
    mouse_read();       /* ack */

    packet_index = 0;
    discard_next_packet = 1;

    irq_install_handler(12, mouse_callback);
    serial_write("[serial] mouse: PS/2 mouse initialized\n");
}

int mouse_get_x(void) { return mouse_x; }
int mouse_get_y(void) { return mouse_y; }
unsigned char mouse_get_buttons(void) { return mouse_buttons; }

int mouse_consume_dirty(void) {
    if (!mouse_dirty) return 0;
    mouse_dirty = 0;
    return 1;
}
