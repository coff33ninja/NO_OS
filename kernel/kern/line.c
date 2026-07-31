#include "types.h"
#include "kbd.h"
#include "printk.h"
#include "serial.h"
#include "vga.h"
#include "string.h"

#define HIST_MAX 16
#define HIST_LEN 256

static char history[HIST_MAX][HIST_LEN];
static usize hist_count;
static usize hist_pos; /* recall slot; hist_count means no recall in progress */

static void hist_add(const char *line)
{
    if (!line || !*line)
        return;
    if (hist_count > 0 && strcmp(history[hist_count - 1], line) == 0)
        return;
    if (hist_count >= HIST_MAX) {
        for (usize i = 1; i < hist_count; i++)
            memcpy(history[i - 1], history[i], HIST_LEN);
        hist_count = HIST_MAX - 1;
    }
    strcpy(history[hist_count], line);
    hist_count++;
}

static void putc2(char c)
{
    serial_putc(c);
    vga_putc(c);
}

static void out2(const char *s)
{
    serial_write(s);
    vga_write(s);
}

/* Redraw the editing line on both serial and VGA. `\r` resets the column on
   both, so reprinting prompt+buffer re-renders the full line; the final
   reprint of buf[0..cur] leaves the cursor right after the current position. */
static void line_redraw(const char *prompt, const char *buf, usize len,
                        usize cur, usize *prev_len)
{
    printk("\r%s", prompt);
    out2(buf);
    for (usize i = len; i < *prev_len; i++)
        out2(" ");
    printk("\r%s", prompt);
    for (usize i = 0; i < cur; i++)
        putc2(buf[i]);
    *prev_len = len;
}

usize line_read(char *buf, usize cap, const char *prompt)
{
    if (!buf || cap == 0)
        return 0;

    usize len = 0, cur = 0, prev_len = 0;
    hist_pos = hist_count;
    line_redraw(prompt, buf, len, cur, &prev_len);

    for (;;) {
        int c = kbd_readc();
        switch (c) {
        case '\n':
            printk("\n");
            buf[len] = '\0';
            hist_add(buf);
            return len;

        case '\b':
            if (cur > 0) {
                memmove(buf + cur - 1, buf + cur, len - cur);
                len--;
                cur--;
                hist_pos = hist_count;
                line_redraw(prompt, buf, len, cur, &prev_len);
            }
            break;

        case KBD_LEFT:
            if (cur > 0) {
                cur--;
                line_redraw(prompt, buf, len, cur, &prev_len);
            }
            break;

        case KBD_RIGHT:
            if (cur < len) {
                cur++;
                line_redraw(prompt, buf, len, cur, &prev_len);
            }
            break;

        case KBD_HOME:
            if (cur != 0) {
                cur = 0;
                line_redraw(prompt, buf, len, cur, &prev_len);
            }
            break;

        case KBD_END:
            if (cur != len) {
                cur = len;
                line_redraw(prompt, buf, len, cur, &prev_len);
            }
            break;

        case KBD_DEL:
            if (cur < len) {
                memmove(buf + cur, buf + cur + 1, len - cur - 1);
                len--;
                hist_pos = hist_count;
                line_redraw(prompt, buf, len, cur, &prev_len);
            }
            break;

        case KBD_UP:
            if (hist_count > 0 && hist_pos > 0) {
                hist_pos--;
                strcpy(buf, history[hist_pos]);
                len = strlen(buf);
                cur = len;
                line_redraw(prompt, buf, len, cur, &prev_len);
            }
            break;

        case KBD_DOWN:
            if (hist_pos < hist_count) {
                hist_pos++;
                if (hist_pos < hist_count) {
                    strcpy(buf, history[hist_pos]);
                    len = strlen(buf);
                } else {
                    len = 0;
                }
                cur = len;
                line_redraw(prompt, buf, len, cur, &prev_len);
            }
            break;

        case 0x03:  /* Ctrl+C */
        case 0x1B:  /* Esc */
            len = cur = 0;
            buf[0] = '\0';
            hist_pos = hist_count;
            printk("^C\n");
            return 0;

        default:
            if ((u8)c >= 0x20 && (u8)c != 0x7F && len + 1 < cap) {
                memmove(buf + cur + 1, buf + cur, len - cur);
                buf[cur] = (char)c;
                len++;
                cur++;
                hist_pos = hist_count;
                line_redraw(prompt, buf, len, cur, &prev_len);
            }
            break;
        }
    }
}
