#include "pit.h"
#include "io.h"
#include "isr.h"
#include "pic.h"

#define PIT_CMD  0x43
#define PIT_CH0  0x40
#define PIT_BASE 1193182
#define PIT_HZ   100
#define PIT_DIV  (PIT_BASE / PIT_HZ)

static volatile u64 ticks;

static void pit_handler(struct regs *r)
{
    (void)r;
    ticks++;
    pic_eoi(0);
}

void pit_init(void)
{
    outb(PIT_CMD, 0x36); /* channel 0, lobyte/hibyte, rate generator */
    outb(PIT_CH0, PIT_DIV & 0xFF);
    outb(PIT_CH0, (PIT_DIV >> 8) & 0xFF);

    isr_register(32, pit_handler);
    pic_mask(0, false);
}

usize pit_ticks(void)
{
    return (usize)ticks;
}

void pit_sleep(usize ms)
{
    u64 target = ticks + (u64)ms * PIT_HZ / 1000;
    while ((u64)ticks < target) {
        __asm__ volatile("sti; hlt");
    }
}
