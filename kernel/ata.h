#ifndef ATA_H
#define ATA_H

int ata_identify(void);
int ata_read_sectors(unsigned int lba, unsigned char count, unsigned short *buffer);
int ata_write_sectors(unsigned int lba, unsigned char count, const unsigned short *buffer);

#endif
