#include "interact.h"
#include "noc_os.h"
#include "string.h"
#include "printk.h"
#include "fs.h"

#define IL_RING_SIZE 65536  /* 64 KiB in-RAM training corpus (M5 spec) */
#define IL_FILE_MAX 4096    /* persisted checkpoint; FS files cap at 5 KiB */
#define IL_CAP_MAX 1024     /* captured output per command */

static char il_ring[IL_RING_SIZE];
static usize il_len;        /* bytes stored, always < IL_RING_SIZE (sentinel) */
static usize il_records;

static char il_cap[IL_CAP_MAX];
static usize il_cap_len;
static bool il_cap_on;

/* Append one newline-terminated record, dropping whole oldest records until
   it fits. Keeps one spare byte so a NUL sentinel is always safe. */
static void il_append(const char *s, usize n)
{
    if (n == 0)
        return;
    if (n + 1 >= IL_RING_SIZE)
        n = IL_RING_SIZE - 2;
    while (il_len + n + 1 > IL_RING_SIZE - 1) {
        usize drop = 0;
        while (drop < il_len && il_ring[drop] != '\n')
            drop++;
        if (drop < il_len)
            drop++;
        if (drop == 0)
            drop = il_len;
        memmove(il_ring, il_ring + drop, il_len - drop);
        il_len -= drop;
        if (il_records > 0)
            il_records--;
    }
    memcpy(il_ring + il_len, s, n);
    il_len += n;
    il_ring[il_len++] = '\n';
    il_records++;
}

/* Append a record with the payload escaped so multi-line output stays one
   record: backslash, newline, CR and tab become two-char escapes, other
   control bytes are dropped. */
static void il_append_esc(const char *tag, const char *payload)
{
    char tmp[IL_CAP_MAX * 2 + 64];
    usize n = 0;
    while (*tag && n < sizeof(tmp) - 2)
        tmp[n++] = *tag++;
    for (usize i = 0; payload[i] && n < sizeof(tmp) - 2; i++) {
        char c = payload[i];
        switch (c) {
        case '\\': tmp[n++] = '\\'; tmp[n++] = '\\'; break;
        case '\n': tmp[n++] = '\\'; tmp[n++] = 'n'; break;
        case '\r': tmp[n++] = '\\'; tmp[n++] = 'r'; break;
        case '\t': tmp[n++] = '\\'; tmp[n++] = 't'; break;
        default:
            if ((u8)c >= 0x20 && (u8)c < 0x7f)
                tmp[n++] = c;
            break; /* drop other control bytes */
        }
    }
    il_append(tmp, n);
}

void il_begin_capture(void)
{
    il_cap_len = 0;
    il_cap_on = true;
}

void il_capture_putc(char c)
{
    if (!il_cap_on)
        return;
    if (il_cap_len < IL_CAP_MAX - 1)
        il_cap[il_cap_len++] = c;
}

void il_end_capture(const char *cmd)
{
    il_cap_on = false;
    il_cap[il_cap_len] = '\0';
    while (il_cap_len > 0 &&
           (il_cap[il_cap_len - 1] == '\n' || il_cap[il_cap_len - 1] == '\r'))
        il_cap[--il_cap_len] = '\0';

    /* One tick marker per REPL command. The payload is a fixed token, NOT
       the wall-clock tick value: a byte-language model must train on a
       deterministic corpus, and wall-clock digits are pure noise that make
       the generated output depend on timing. */
    static const char rec[] = "[TICK]";
    il_append(rec, sizeof(rec) - 1);

    il_append_esc("[CMD] ", cmd);

    if (il_cap_len) {
        /* noc_report() prefixes errors with "NOC: " whether the compile
           failed or a runtime error occurred. */
        const char *p = il_cap;
        bool is_err = strncmp(p, "NOC: ", 5) == 0;
        if (is_err)
            p += 5;
        il_append_esc(is_err ? "[ERR] " : "[OUT] ", p);
    }
}

void il_save(void)
{
    usize size = il_len < IL_FILE_MAX ? il_len : IL_FILE_MAX;
    usize off = il_len - size;
    if (il_len > IL_FILE_MAX) {
        /* align the checkpoint to a record boundary inside the window */
        usize i = off;
        while (i < il_len && il_ring[i] != '\n')
            i++;
        if (i < il_len) {
            off = i + 1;
            size = il_len - off;
        }
    }
    if (size == 0) {
        noc_os_puts("log: nothing to save\n");
        return;
    }
    int ino = fs_lookup("interact.log");
    if (ino < 0)
        ino = fs_create("interact.log", FS_INODE_REG);
    if (ino < 0) {
        noc_os_puts("log: save failed (create)\n");
        return;
    }
    if (fs_write_file((u32)ino, il_ring + off, size) != 0) {
        noc_os_puts("log: save failed (write)\n");
        return;
    }
    fs_sync_bitmap();
    char buf[80];
    sprintk(buf, sizeof(buf), "log: saved interact.log (%u bytes)\n",
            (unsigned)size);
    noc_os_puts(buf);
}

void il_load(void)
{
    int ino = fs_lookup("interact.log");
    if (ino < 0)
        return;
    struct fs_inode st;
    if (fs_stat((u32)ino, &st) != 0)
        return;
    if (st.size == 0 || st.size > IL_FILE_MAX)
        return;

    il_len = 0;
    il_records = 0;
    il_cap_on = false;
    il_len = (usize)fs_read_file((u32)ino, il_ring, st.size);
    for (usize i = 0; i < il_len; i++)
        if (il_ring[i] == '\n')
            il_records++;
    printk("log: restored %u bytes, %u records\n", (unsigned)il_len,
           (unsigned)il_records);
}

void il_dump(void)
{
    char buf[96];
    sprintk(buf, sizeof(buf), "log: %u records, %u bytes of %u\n",
            (unsigned)il_records, (unsigned)il_len, (unsigned)IL_RING_SIZE);
    noc_os_puts(buf);
    if (il_len > 0) {
        char save = il_ring[il_len];
        il_ring[il_len] = '\0';
        noc_os_puts(il_ring);
        il_ring[il_len] = save;
    }
}

void il_clear(void)
{
    il_len = 0;
    il_records = 0;
}

/* Kernel process events appended straight to the ring, independent of the
   per-command output capture. */
void il_event_spawn(u32 pid, const char *name)
{
    char rec[64];
    usize n = sprintk(rec, sizeof(rec), "[SPAWN] %u", (unsigned)pid);
    if (name && n < sizeof(rec) - 1)
        rec[n++] = ' ';
    if (name) {
        for (usize i = 0; name[i] && n < sizeof(rec) - 1; i++)
            rec[n++] = name[i];
    }
    il_append(rec, n);
}

void il_event_exit(u32 pid, i64 code)
{
    char rec[48];
    sprintk(rec, sizeof(rec), "[EXIT] %u %lld", (unsigned)pid,
            (long long)code);
    il_append(rec, strlen(rec));
}

void il_stats(char *buf, usize cap)
{
    sprintk(buf, cap, "log: %u records, %u bytes of %u\n",
            (unsigned)il_records, (unsigned)il_len, (unsigned)IL_RING_SIZE);
}

usize il_len_bytes(void)
{
    return il_len;
}

/* The ring is shift-compacted on eviction (memmove), so the tail window is
   always contiguous in il_ring. Copy the most recent `cap` bytes into dst. */
usize il_copy_tail(char *dst, usize cap)
{
    if (!dst || cap == 0 || il_len == 0)
        return 0;
    if (cap > il_len)
        cap = il_len;
    memcpy(dst, il_ring + il_len - cap, cap);
    return cap;
}
