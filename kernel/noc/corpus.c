#include "corpus.h"
#include "fs.h"
#include "noc.h"
#include "noc_os.h"
#include "pit.h"
#include "printk.h"
#include "sched.h"
#include "string.h"

/* Versioned, rollback-safe corpus of model-generated NOC programs.
   The flat FS caps files at 5 KiB and sprintk has no zero-pad support,
   so version numbers are formatted by hand and every file stays well
   under the cap (header + one command line). */

#define CORP_HDR_LINES 5      /* @@ GENERATED/PROMPT/SCORE/PARENT/SEQ */
#define CORP_BUF_MAX 4096
#define CORP_NAME_MAX 16

/* Format a zero-padded 4-digit version into a 12-char file name
   "corp%04u.noc". */
static void corp_name(char *out, u32 seq)
{
    memcpy(out, "corp", 4);
    for (int i = 3; i >= 0; i--) {
        out[4 + i] = (char)('0' + (seq % 10));
        seq /= 10;
    }
    memcpy(out + 8, ".noc", 4);
    out[12] = '\0';
}

/* Read a decimal number from the file `name`; 0 when missing. */
static u32 corp_read_seq(const char *name)
{
    int ino = fs_lookup(name);
    if (ino < 0)
        return 0;
    struct fs_inode st;
    if (fs_stat((u32)ino, &st) != 0 || st.size == 0 || st.size > 32)
        return 0;
    char buf[32];
    usize n = (usize)fs_read_file((u32)ino, buf, st.size);
    u32 v = 0;
    for (usize i = 0; i < n && buf[i] >= '0' && buf[i] <= '9'; i++)
        v = v * 10 + (u32)(buf[i] - '0');
    return v;
}

static void corp_write_seq(u32 seq)
{
    char buf[16];
    usize n = 0;
    u32 v = seq;
    if (v == 0)
        buf[n++] = '0';
    while (v && n < sizeof(buf) - 1) {
        buf[n++] = (char)('0' + v % 10);
        v /= 10;
    }
    for (usize i = 0; i < n / 2; i++) {
        char t = buf[i];
        buf[i] = buf[n - 1 - i];
        buf[n - 1 - i] = t;
    }
    int ino = fs_lookup("corpus.seq");
    if (ino < 0)
        ino = fs_create("corpus.seq", FS_INODE_REG);
    if (ino < 0)
        return;
    fs_write_file((u32)ino, buf, n);
    fs_sync_bitmap();
}

/* Build the versioned file content: 5 metadata lines + the draft source. */
static usize corp_build(char *out, usize cap, u32 seq, u32 parent,
                        const char *seed, const char *prog)
{
    usize o = 0;
    u64 now = pit_ticks();
    o += sprintk(out + o, cap - o, "@@ GENERATED: %llu\n",
                 (unsigned long long)now);
    o += sprintk(out + o, cap - o, "@@ PROMPT: %s\n", seed);
    o += sprintk(out + o, cap - o, "@@ SCORE: 1.0\n");
    o += sprintk(out + o, cap - o, "@@ PARENT: %u\n", (unsigned)parent);
    o += sprintk(out + o, cap - o, "@@ SEQ: %u\n", (unsigned)seq);
    for (usize i = 0; prog[i] && o + 1 < cap; i++)
        out[o++] = prog[i];
    out[o] = '\0';
    return o;
}

static int corp_write(const char *name, const char *content, usize n)
{
    int ino = fs_lookup(name);
    if (ino < 0)
        ino = fs_create(name, FS_INODE_REG);
    if (ino < 0)
        return -1;
    if (fs_write_file((u32)ino, content, n) != 0)
        return -1;
    fs_sync_bitmap();
    return 0;
}

void corpus_commit(const char *prog, const char *seed)
{
    if (!prog || !*prog)
        return;

    u32 seq = corp_read_seq("corpus.seq");
    if (seq == 0)
        seq = 1;
    u32 parent = seq > 1 ? seq - 1 : 0;

    char content[CORP_BUF_MAX];
    usize n = corp_build(content, sizeof(content), seq, parent, seed, prog);
    if (n == 0)
        return;

    char name[CORP_NAME_MAX];
    corp_name(name, seq);
    if (corp_write(name, content, n) != 0)
        return;
    if (corp_write("last_known_good.noc", content, n) != 0)
        return;
    corp_write_seq(seq + 1);

    char buf[96];
    sprintk(buf, sizeof(buf), "corpus: saved %s (seq %u)\n",
            name, (unsigned)seq);
    noc_os_puts(buf);
}

void corpus_info(void)
{
    u32 seq = corp_read_seq("corpus.seq");
    if (seq == 0)
        seq = 1;
    u32 versions = seq > 1 ? seq - 1 : 0;

    int ino = fs_lookup("last_known_good.noc");
    bool has_lkg = ino >= 0;

    char buf[96];
    sprintk(buf, sizeof(buf), "corpus: versions=%u next=%u lkg=%s\n",
            (unsigned)versions, (unsigned)seq, has_lkg ? "yes" : "none");
    noc_os_puts(buf);
}

void corpus_rollback(void)
{
    int ino = fs_lookup("last_known_good.noc");
    if (ino < 0) {
        noc_os_puts("corpus: rollback unavailable\n");
        return;
    }
    struct fs_inode st;
    if (fs_stat((u32)ino, &st) != 0 || st.size == 0 || st.size >= CORP_BUF_MAX) {
        noc_os_puts("corpus: rollback unavailable\n");
        return;
    }
    char buf[CORP_BUF_MAX];
    usize n = (usize)fs_read_file((u32)ino, buf, st.size);
    buf[n] = '\0';

    /* Strip the CORP_HDR_LINES metadata lines to recover the source. */
    usize line = 0, off = 0;
    while (off < n && line < CORP_HDR_LINES) {
        if (buf[off] == '\n')
            line++;
        off++;
    }
    if (line < CORP_HDR_LINES || off >= n) {
        noc_os_puts("corpus: rollback unavailable (no source)\n");
        return;
    }
    buf[n] = '\0';

    if (!noc_check_syntax(buf + off)) {
        noc_os_puts("corpus: rollback unavailable (invalid)\n");
        return;
    }
    i64 pid = sched_spawn(buf + off, "corpus");
    char msg[64];
    sprintk(msg, sizeof(msg), "corpus: rollback spawned pid %d\n", (int)pid);
    noc_os_puts(msg);
}
