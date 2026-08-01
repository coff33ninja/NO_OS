#include "noc_os.h"

#define SYS_EXIT     0
#define SYS_PUTC     1
#define SYS_PUTS     2
#define SYS_SLEEP    3
#define SYS_TICKS    4
#define SYS_KBD_POLL 5
#define SYS_KBD_WAIT 6
#define SYS_ALLOC    7
#define SYS_FREE     8
#define SYS_YIELD    9
#define SYS_KBD_PEEK 10
#define SYS_MODEL_BUDGET 11
#define SYS_MODEL_COMMIT 12

/* int 0x80 preserves every GPR except rax (the return value). Arg 1 in rdi. */
static u64 do_sys(u64 num, u64 arg)
{
    u64 ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(num), "D"(arg)
                     : "memory");
    return ret;
}

void *noc_os_alloc(usize n)
{
    return (void *)do_sys(SYS_ALLOC, n);
}

void noc_os_free(void *p)
{
    do_sys(SYS_FREE, (u64)p);
}

void noc_os_putc(char c)
{
    do_sys(SYS_PUTC, (u64)(u8)c);
}

void noc_os_puts(const char *s)
{
    do_sys(SYS_PUTS, (u64)s);
}

u64 noc_os_ticks(void)
{
    return do_sys(SYS_TICKS, 0);
}

void noc_os_sleep(u64 ms)
{
    do_sys(SYS_SLEEP, ms);
}

int noc_os_kbd_poll(void)
{
    return (int)(i64)do_sys(SYS_KBD_POLL, 0);
}

int noc_os_kbd_peek(void)
{
    return (int)(i64)do_sys(SYS_KBD_PEEK, 0);
}

int noc_os_kbd_wait(void)
{
    return (int)(i64)do_sys(SYS_KBD_WAIT, 0);
}

void noc_os_exit(int code)
{
    do_sys(SYS_EXIT, (u64)code);
    for (;;)
        __asm__ volatile("hlt");
}

u64 noc_os_model_budget(u64 kb)
{
    return do_sys(SYS_MODEL_BUDGET, kb);
}

u64 noc_os_model_commit(u64 pages)
{
    return do_sys(SYS_MODEL_COMMIT, pages);
}
