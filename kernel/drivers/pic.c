#include "pic.h"
#include "io.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define ICW1_INIT  0x11
#define ICW4_8086  0x01

void pic_init(void)
{
    outb(PIC1_CMD, ICW1_INIT);
    outb(PIC2_CMD, ICW1_INIT);

    outb(PIC1_DATA, 0x20);  /* master: vectors 0x20..0x27 */
    outb(PIC2_DATA, 0x28);  /* slave:  vectors 0x28..0x2F */

    outb(PIC1_DATA, 4);     /* slave connected on IRQ2 */
    outb(PIC2_DATA, 2);

    outb(PIC1_DATA, ICW4_8086);
    outb(PIC2_DATA, ICW4_8086);

    outb(PIC1_DATA, 0xFF);  /* mask everything */
    outb(PIC2_DATA, 0xFF);
}

void pic_eoi(u8 irq)
{
    if (irq >= 8)
        outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}

void pic_mask(u8 irq, bool masked)
{
    u16 port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    u8 bit   = (u8)(1 << (irq & 7));
    u8 mask  = inb(port);
    if (masked)
        mask |= bit;
    else
        mask &= (u8)~bit;
    outb(port, mask);
}
