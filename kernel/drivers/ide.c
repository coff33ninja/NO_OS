#include "ide.h"
#include "io.h"
#include "printk.h"
#include "string.h"

static bool present;
static u64  drive_sectors; /* LBA28 capacity from IDENTIFY (0 = unknown) */

static void ide_wait_bsy(void)
{
    while (inb(ATA_PRIMARY_BASE + 7) & ATA_SR_BSY)
        ;
}

static void ide_wait_drq(void)
{
    for (;;) {
        u8 st = inb(ATA_PRIMARY_BASE + 7);
        if (st & ATA_SR_DRQ)
            return;
        if (st & ATA_SR_ERR) {
            printk("ide: ERR during transfer\n");
            return;
        }
        if (!(st & ATA_SR_BSY) && !(st & ATA_SR_DRQ)) {
            /* PIO transfer finished or errored */
            return;
        }
    }
}

static void ide_wait_ready(void)
{
    for (;;) {
        u8 st = inb(ATA_PRIMARY_BASE + 7);
        if (!(st & ATA_SR_BSY))
            return;
    }
}

int ide_read_sectors(u32 lba, usize count, void *buf)
{
    u8 *p = (u8 *)buf;

    for (usize i = 0; i < count; i++) {
        ide_wait_bsy();
        outb(ATA_PRIMARY_BASE + 6, 0xE0 | (u8)((lba >> 24) & 0x0F));
        outb(ATA_PRIMARY_BASE + 1, 0);
        outb(ATA_PRIMARY_BASE + 2, 1);
        outb(ATA_PRIMARY_BASE + 3, (u8)lba);
        outb(ATA_PRIMARY_BASE + 4, (u8)(lba >> 8));
        outb(ATA_PRIMARY_BASE + 5, (u8)(lba >> 16));
        outb(ATA_PRIMARY_BASE + 7, ATA_CMD_READ);
        io_wait();

        ide_wait_bsy();
        ide_wait_drq();
        for (int j = 0; j < SECTOR_SIZE / 2; j++)
            ((u16 *)p)[j] = inw(ATA_PRIMARY_BASE + 0);
        p += SECTOR_SIZE;
        lba++;
    }
    return 0;
}

int ide_write_sectors(u32 lba, usize count, const void *buf)
{
    const u8 *p = (const u8 *)buf;

    for (usize i = 0; i < count; i++) {
        ide_wait_bsy();
        outb(ATA_PRIMARY_BASE + 6, 0xE0 | (u8)((lba >> 24) & 0x0F));
        outb(ATA_PRIMARY_BASE + 1, 0);
        outb(ATA_PRIMARY_BASE + 2, 1);
        outb(ATA_PRIMARY_BASE + 3, (u8)lba);
        outb(ATA_PRIMARY_BASE + 4, (u8)(lba >> 8));
        outb(ATA_PRIMARY_BASE + 5, (u8)(lba >> 16));
        outb(ATA_PRIMARY_BASE + 7, ATA_CMD_WRITE);
        io_wait();

        ide_wait_bsy();
        ide_wait_drq();
        for (int j = 0; j < SECTOR_SIZE / 2; j++)
            outw(ATA_PRIMARY_BASE + 0, ((const u16 *)p)[j]);
        p += SECTOR_SIZE;
        lba++;

        /* Flush to guarantee the data hits the disk. */
        ide_wait_bsy();
        outb(ATA_PRIMARY_BASE + 7, ATA_CMD_FLUSH);
        io_wait();
        ide_wait_ready();
    }
    return 0;
}

int ide_init(void)
{
    /* Probe: select drive 0 and issue IDENTIFY. A missing drive leaves the
       controller in a state we can distinguish from a present one. */
    outb(ATA_PRIMARY_BASE + 6, 0xE0); /* drive 0, LBA mode */
    io_wait();
    outb(ATA_PRIMARY_BASE + 7, ATA_CMD_IDENTIFY);
    io_wait();

    u8 st = inb(ATA_PRIMARY_BASE + 7);
    if (!(st & ATA_SR_ERR) || (st & ATA_SR_DRQ)) {
        /* Either DRQ set (real drive answering) or no ERR (emulated). */
        present = true;
        drive_sectors = 0;
        /* Read the 256-word IDENTIFY payload so the disk geometry is real,
           not a hardcoded image size. Word 60-61 = LBA28 sector count. */
        u32 spin = 0;
        while ((st = inb(ATA_PRIMARY_BASE + 7)) & ATA_SR_BSY) {
            if (++spin > 1000000)
                break;
        }
        if (st & ATA_SR_DRQ) {
            u16 id[256];
            for (int i = 0; i < 256; i++)
                id[i] = inw(ATA_PRIMARY_BASE + 0);
            drive_sectors = ((u64)id[61] << 16) | id[60];
        }
        printk("ide: primary master present");
        if (drive_sectors)
            printk(" (%u MiB)", (unsigned)(drive_sectors / 2048));
        printk("\n");
        return 1;
    }

    printk("ide: no drive on primary master\n");
    return 0;
}

u64 ide_drive_sectors(void)
{
    return drive_sectors;
}

bool ide_present(void)
{
    return present;
}
