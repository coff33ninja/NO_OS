#ifndef NOOS_IDE_H
#define NOOS_IDE_H

#include "types.h"

#define SECTOR_SIZE 512

/* ATA status register bits */
#define ATA_SR_BSY  0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

/* ATA commands (28-bit LBA, PIO) */
#define ATA_CMD_READ    0x20
#define ATA_CMD_WRITE   0x30
#define ATA_CMD_IDENTIFY 0xEC
#define ATA_CMD_FLUSH   0xE7

/* Primary channel registers */
#define ATA_PRIMARY_BASE 0x1F0
#define ATA_PRIMARY_CTRL 0x3F6

int  ide_init(void);
int  ide_read_sectors(u32 lba, usize count, void *buf);
int  ide_write_sectors(u32 lba, usize count, const void *buf);
bool ide_present(void);

/* Total capacity in 512-byte sectors, from the IDENTIFY payload read at
   ide_init(). 0 when the drive reported no geometry (or is absent). */
u64  ide_drive_sectors(void);

#endif
