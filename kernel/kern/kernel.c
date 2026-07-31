#include "types.h"
#include "vga.h"
#include "serial.h"
#include "printk.h"
#include "string.h"
#include "gdt.h"
#include "tss.h"
#include "idt.h"
#include "pic.h"
#include "pit.h"
#include "kbd.h"
#include "pmm.h"
#include "heap.h"
#include "sched.h"
#include "noc.h"

static void heap_test(void)
{
    u8 *a = kmalloc(32);
    u8 *b = kmalloc(256);
    u8 *c = kmalloc(4096);
    if (!a || !b || !c) {
        printk("heap test: FAIL (allocation failed)\n");
        return;
    }

    memset(a, 0xAA, 32);
    memset(b, 0xBB, 256);
    memset(c, 0xCC, 4096);

    bool bad = false;
    for (usize i = 0; i < 32; i++)
        bad |= a[i] != 0xAA;
    for (usize i = 0; i < 256; i++)
        bad |= b[i] != 0xBB;
    for (usize i = 0; i < 4096; i++)
        bad |= c[i] != 0xCC;

    u64 used0 = heap_used_bytes();
    kfree(b);
    kfree(a);
    u64 used1 = heap_used_bytes();
    kfree(c);

    u8 *d = kmalloc(64);
    if (!d)
        bad = true;
    memset(d, 0xDD, 64);
    for (usize i = 0; i < 64; i++)
        bad |= d[i] != 0xDD;
    kfree(d);

    if (!bad && used1 < used0)
        printk("heap test: ok (%u allocs, %u blocks, %u bytes used)\n",
               (unsigned)4, (unsigned)heap_blocks(), (unsigned)heap_used_bytes());
    else
        printk("heap test: FAIL\n");
}

static void kbd_echo_test(void)
{
    char line[16];
    usize i = 0;

    printk("keyboard echo test: type 'ok' then Enter\n");
    for (;;) {
        int c = kbd_readc();
        if (c == '\n') {
            printk("\n");
            break;
        }
        if (c == '\b') {
            if (i > 0) {
                i--;
                printk("\b \b");
            }
            continue;
        }
        if (i < sizeof(line) - 1)
            line[i++] = (char)c;
        printk("%c", c);
    }
    line[i] = '\0';

    if (strcmp(line, "ok") == 0)
        printk("boot-test-ok\n");
    else
        printk("KEY-TEST-FAIL\n");
}

void kmain(u32 mb_info)
{
    serial_init();
    vga_init();

    gdt_init();
    tss_init();
    idt_init();
    pic_init();
    pit_init();
    kbd_init();

    printk("NO_OS v0.1\n");
    printk("multiboot info: 0x%x\n", mb_info);
    printk("GDT + IDT installed\n");
    printk("PIT timer at %u Hz\n", 100);

    pmm_init(mb_info);
    heap_test();

    sched_init();

    noc_init();
    noc_selftest();

    __asm__ volatile("sti");

    u64 t0 = pit_ticks();
    pit_sleep(100);
    printk("timer: 100 ms sleep, ticks delta=%u\n",
           (unsigned)(pit_ticks() - t0));

    kbd_echo_test();

    noc_repl();

    sched_idle();
}