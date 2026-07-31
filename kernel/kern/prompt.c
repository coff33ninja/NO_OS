#include "prompt.h"
#include "types.h"
#include "kbd.h"
#include "printk.h"
#include "string.h"
#include "pmm.h"
#include "heap.h"

#define LINE_MAX 128

static usize getline(char *buf, usize cap)
{
    usize i = 0;
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
        if (i < cap - 1) {
            buf[i++] = (char)c;
            printk("%c", c);
        }
    }
    buf[i] = '\0';
    return i;
}

static void cmd_version(void)
{
    printk("kernel version: NO_OS v0.1 (milestone M1)\n");
}

static void cmd_meminfo(void)
{
    u64 total = pmm_total_frames();
    u64 avail = pmm_avail_frames();
    u64 free_mib = avail * FRAME_SIZE / 0x100000;
    u64 tot_mib  = total * FRAME_SIZE / 0x100000;
    printk("mem: %u MiB free of %u MiB total (%u frames)\n",
           (unsigned)free_mib, (unsigned)tot_mib, (unsigned)avail);
    printk("heap: %u bytes used in %u blocks\n",
           (unsigned)heap_used_bytes(), (unsigned)heap_blocks());
}

static void cmd_help(void)
{
    printk("commands: help, version, meminfo, echo <text>, fault, reboot\n");
}

static void cmd_fault(void)
{
    printk("triggering deliberate #UD (invalid opcode)\n");
    __asm__ volatile("ud2");
}

static void cmd_reboot(void)
{
    printk("rebooting...\n");
    __asm__ volatile("cli");
    /* keyboard controller reset */
    __asm__ volatile("outb %%al, $0x64" :: "a"((u8)0xFE));
    for (;;)
        __asm__ volatile("hlt");
}

static void cmd_echo(const char *args)
{
    if (*args)
        printk("%s\n", args);
}

static void cmd_dispatch(const char *line)
{
    prompt_handle(line);
}

bool prompt_handle(const char *line)
{
    const char *cmd = line;
    const char *args = line;
    while (*args && *args != ' ')
        args++;
    usize clen = (usize)(args - cmd);
    while (*args == ' ')
        args++;

    if (clen == 0) {
        return true;
    } else if (clen == 4 && memcmp(cmd, "help", 4) == 0) {
        cmd_help();
        return true;
    } else if (clen == 7 && memcmp(cmd, "version", 7) == 0) {
        cmd_version();
        return true;
    } else if (clen == 7 && memcmp(cmd, "meminfo", 7) == 0) {
        cmd_meminfo();
        return true;
    } else if (clen == 4 && memcmp(cmd, "echo", 4) == 0) {
        cmd_echo(args);
        return true;
    } else if (clen == 5 && memcmp(cmd, "fault", 5) == 0) {
        cmd_fault();
        return true;
    } else if (clen == 6 && memcmp(cmd, "reboot", 6) == 0) {
        cmd_reboot();
        return true;
    } else {
        printk("unknown command '%s' (try 'help')\n", line);
        return false;
    }
}

void prompt_main(void)
{
    char line[LINE_MAX];
    printk("no/os kernel prompt. type 'help'.\n");
    for (;;) {
        printk("no/os> ");
        getline(line, sizeof(line));
        cmd_dispatch(line);
    }
}
