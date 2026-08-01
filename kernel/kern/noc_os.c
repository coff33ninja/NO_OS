#include "noc_os.h"
#include "interact.h"
#include "heap.h"
#include "pit.h"
#include "kbd.h"
#include "serial.h"
#include "vga.h"
#include "sched.h"

void *noc_os_alloc(usize n)
{
    return kmalloc(n);
}

void noc_os_free(void *p)
{
    kfree(p);
}

void noc_os_putc(char c)
{
    il_capture_putc(c);
    if (c == '\n')
        serial_putc('\r');
    serial_putc(c);
    vga_putc(c);
}

void noc_os_puts(const char *s)
{
    while (*s)
        noc_os_putc(*s++);
}

u64 noc_os_ticks(void)
{
    return (u64)pit_ticks();
}

void noc_os_sleep(u64 ms)
{
    pit_sleep((usize)ms);
}

int noc_os_kbd_poll(void)
{
    return kbd_getc();
}

int noc_os_kbd_peek(void)
{
    return kbd_peekc();
}

int noc_os_kbd_wait(void)
{
    return kbd_readc();
}

void noc_os_exit(int code)
{
    (void)code;
    for (;;)
        __asm__ volatile("hlt");
}

u64 noc_os_model_budget(u64 kb)
{
    task_t *t = sched_current();
    if (kb > 65536)
        kb = 65536;
    t->model_budget_kb = (u32)kb;
    return kb;
}

u64 noc_os_model_commit(u64 pages)
{
    task_t *t = sched_current();
    u64 need = pages * 4; /* 4 KiB per weight page */
    if ((u64)t->model_weights_kb + need > (u64)t->model_budget_kb)
        return (u64)-1;
    t->model_weights_kb += (u32)need;
    return 0;
}
