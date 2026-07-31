#include "kbd.h"
#include "io.h"
#include "isr.h"
#include "pic.h"
#include "sched.h"

#define KBD_DATA 0x60
#define BUF_SIZE 256

static u8  buf[BUF_SIZE];
static volatile u16 head, tail;
static bool shift, caps, ext, ctrl;

static const char kb_normal[128] = {
    0, 27,
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=',
    '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']',
    '\n', 0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\',
    'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
    0, '*', 0, ' ',
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

static const char kb_shift[128] = {
    0, 27,
    '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+',
    '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}',
    '\n', 0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|',
    'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
    0, '*', 0, ' ',
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

static void kbd_push(char c)
{
    u16 next = (u16)((head + 1) % BUF_SIZE);
    if (next == tail)
        return; /* full; drop */
    buf[head] = (u8)c;
    head = next;
}

static void kbd_handler(struct regs *r)
{
    (void)r;
    u8 sc = inb(KBD_DATA);

    if (ext) {
        ext = false;
        if (sc == 0x1D)      ctrl = true;              /* E0 right ctrl */
        else if (sc == 0x9D) ctrl = false;
        else if (sc == 0x2A) shift = true;             /* E0 left shift */
        else if (sc == 0x36) shift = true;             /* E0 right shift */
        else if (sc == 0x48) kbd_push(KBD_UP);
        else if (sc == 0x50) kbd_push(KBD_DOWN);
        else if (sc == 0x4B) kbd_push(KBD_LEFT);
        else if (sc == 0x4D) kbd_push(KBD_RIGHT);
        else if (sc == 0x47) kbd_push(KBD_HOME);
        else if (sc == 0x4F) kbd_push(KBD_END);
        else if (sc == 0x53) kbd_push(KBD_DEL);
        pic_eoi(1);
        return;
    }

    if (sc == 0xE0) {
        ext = true;
        pic_eoi(1);
        return;
    }

    if (sc == 0x2A || sc == 0x36) {     /* shift press */
        shift = true;
        pic_eoi(1);
        return;
    }
    if (sc == 0xAA || sc == 0xB6) {     /* shift release */
        shift = false;
        pic_eoi(1);
        return;
    }
    if (sc == 0x1D) {                   /* ctrl press */
        ctrl = true;
        pic_eoi(1);
        return;
    }
    if (sc == 0x9D) {                   /* ctrl release */
        ctrl = false;
        pic_eoi(1);
        return;
    }
    if (sc == 0x3A) {                   /* caps lock */
        caps = !caps;
        pic_eoi(1);
        return;
    }

    if (sc & 0x80) {                    /* key release */
        pic_eoi(1);
        return;
    }

    char c = (shift ^ caps) ? kb_shift[sc & 0x7F] : kb_normal[sc & 0x7F];
    if (ctrl && c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 1);        /* Ctrl+A..Z -> control codes 1..26 */
    if (c)
        kbd_push(c);
    sched_on_key();
    pic_eoi(1);
}

void kbd_init(void)
{
    head = tail = 0;
    shift = caps = ext = false;
    isr_register(33, kbd_handler);
    pic_mask(1, false);
}

bool kbd_avail(void)
{
    return head != tail;
}

int kbd_getc(void)
{
    if (head == tail)
        return -1;
    char c = (char)buf[tail];
    tail = (u16)((tail + 1) % BUF_SIZE);
    return (int)(u8)c;
}

int kbd_peekc(void)
{
    if (head == tail)
        return -1;
    return (int)(u8)buf[tail];
}

int kbd_readc(void)
{
    int c;
    while ((c = kbd_getc()) < 0) {
        sched_yield_to_user(); /* let user tasks run while the shell waits */
        __asm__ volatile("sti; hlt");
    }
    return c;
}
