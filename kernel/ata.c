/* ata.c -- ATA PIO mode driver, primary bus, master drive only
 *
 * PIO mode is the simplest possible way to talk to a disk: no DMA
 * setup, no interrupts needed, just poll a status port and shuffle
 * words through a data port. Slow, but thats all I'm bothered to do.
 */

#include "ata.h"
#include "serial.h"

#define ATA_DATA        0x1F0
#define ATA_ERROR       0x1F1
#define ATA_SECCOUNT    0x1F2
#define ATA_LBA_LOW     0x1F3
#define ATA_LBA_MID     0x1F4
#define ATA_LBA_HIGH    0x1F5
#define ATA_DRIVE_HEAD  0x1F6
#define ATA_STATUS      0x1F7
#define ATA_COMMAND     0x1F7

#define ATA_CMD_READ_SECTORS  0x20
#define ATA_CMD_WRITE_SECTORS 0x30
#define ATA_CMD_IDENTIFY      0xEC

#define ATA_SR_BSY  0x80
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

/* Our own bootloader + kernel now live at the front of this same disk
 * (previously QEMU's -kernel flag loaded the kernel out-of-band, so
 * the whole drive was free for FAT16). Everything fat16.c does still
 * thinks sector 0 is the start of the filesystem -- this is the one
 * place that translates that into where the FAT16 partition actually
 * starts on the physical disk. Must match the offset the disk image
 * is built with (see Makefile). */
#define FAT16_PARTITION_LBA_OFFSET 2048

static inline void outb(unsigned short port, unsigned char val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline unsigned char inb(unsigned short port) {
    unsigned char ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline unsigned short inw(unsigned short port) {
    unsigned short ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void outw(unsigned short port, unsigned short val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static void ata_400ns_delay(void) {
    /* Reading the (unused, alternate) status port 4 times is a
     * standard trick: each read takes ~100ns on real hardware, giving
     * the drive time to raise BSY before we start polling for real. */
    for (int i = 0; i < 4; i++) inb(0x3F6);
}

static int ata_wait_ready(void) {
    /* Poll until BSY clears. On real hardware this needs a timeout;
     * under QEMU's emulated drive it's effectively instant, but we
     * still bound the loop so a misconfigured/missing drive can't
     * hang the kernel forever. */
    for (unsigned int i = 0; i < 100000; i++) {
        unsigned char status = inb(ATA_STATUS);
        if (!(status & ATA_SR_BSY)) return 1;
    }
    return 0;
}

static int ata_wait_drq(void) {
    for (unsigned int i = 0; i < 100000; i++) {
        unsigned char status = inb(ATA_STATUS);
        if (status & ATA_SR_ERR) return 0;
        if (status & ATA_SR_DRQ) return 1;
    }
    return 0;
}

int ata_identify(void) {
    outb(ATA_DRIVE_HEAD, 0xA0); /* master drive, LBA bit not needed for IDENTIFY */
    ata_400ns_delay();
    outb(ATA_SECCOUNT, 0);
    outb(ATA_LBA_LOW, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HIGH, 0);
    outb(ATA_COMMAND, ATA_CMD_IDENTIFY);

    unsigned char status = inb(ATA_STATUS);
    if (status == 0) {
        serial_write("[serial] ata_identify: no drive present (status=0)\n");
        return 0;
    }

    if (!ata_wait_ready()) {
        serial_write("[serial] ata_identify: timed out waiting for BSY clear\n");
        return 0;
    }

    if (!ata_wait_drq()) {
        serial_write("[serial] ata_identify: drive did not raise DRQ / raised ERR\n");
        return 0;
    }

    unsigned short identify_data[256];
    for (int i = 0; i < 256; i++) identify_data[i] = inw(ATA_DATA);

    serial_write("[serial] ata_identify: drive responded OK\n");
    return 1;
}

int ata_read_sectors(unsigned int lba, unsigned char count, unsigned short *buffer) {
    lba += FAT16_PARTITION_LBA_OFFSET;
    if (!ata_wait_ready()) return 0;

    outb(ATA_DRIVE_HEAD, (unsigned char)(0xE0 | ((lba >> 24) & 0x0F))); /* LBA mode, master */
    outb(ATA_SECCOUNT, count);
    outb(ATA_LBA_LOW,  (unsigned char)(lba & 0xFF));
    outb(ATA_LBA_MID,  (unsigned char)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HIGH, (unsigned char)((lba >> 16) & 0xFF));
    outb(ATA_COMMAND, ATA_CMD_READ_SECTORS);

    for (unsigned char s = 0; s < count; s++) {
        if (!ata_wait_ready()) return 0;
        if (!ata_wait_drq()) return 0;

        for (int i = 0; i < 256; i++) { /* 256 words = 512 bytes = one sector */
            buffer[s * 256 + i] = inw(ATA_DATA);
        }
    }
    return 1;
}

int ata_write_sectors(unsigned int lba, unsigned char count, const unsigned short *buffer) {
    lba += FAT16_PARTITION_LBA_OFFSET;
    if (!ata_wait_ready()) return 0;

    outb(ATA_DRIVE_HEAD, (unsigned char)(0xE0 | ((lba >> 24) & 0x0F)));
    outb(ATA_SECCOUNT, count);
    outb(ATA_LBA_LOW,  (unsigned char)(lba & 0xFF));
    outb(ATA_LBA_MID,  (unsigned char)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HIGH, (unsigned char)((lba >> 16) & 0xFF));
    outb(ATA_COMMAND, ATA_CMD_WRITE_SECTORS);

    for (unsigned char s = 0; s < count; s++) {
        if (!ata_wait_ready()) return 0;
        if (!ata_wait_drq()) return 0;
        for (int i = 0; i < 256; i++) {
            outw(ATA_DATA, buffer[s * 256 + i]);
        }
    }
    return 1;
}
