#include "train.h"
#include "interact.h"
#include "noc_os.h"
#include "pit.h"
#include "printk.h"
#include "string.h"

/* Byte-bigram model: w[prev<<8|next] is the 8-bit weight for the byte
   transition prev->next (65536 u8 = 64 KiB, matching the M5 "8-bit weights"
   target). Training is count accumulation with capping plus periodic halving
   when a row saturates -- the integer analogue of SGD learning-rate decay.
   Loss is integer fixed-point cross-entropy in millibits per byte, computed
   with a 16-entry log2 table (error < ~0.5%). */

#define TRAIN_BATCH   4096 /* contiguous batch cap from the log (M5 spec) */
#define TRAIN_MIN     4    /* don't bother training on fewer bytes */
#define IDLE_NEW_MIN  64   /* new log bytes that justify an idle retrain */

static u8   w[65536];
static u32  train_passes;     /* gradient steps accumulated, lifetime */
static u32  train_bytes;      /* bytes trained on, lifetime */
static u32  last_loss_mbit;   /* millibits/byte of the most recent pass */
static usize train_watermark; /* il_len_bytes() at the last training */
static u32  idle_secs = 30;   /* idle trigger threshold (M5 spec: 30 s) */
static u64  last_input_tick;  /* pit tick of the most recent key */
static char batch[TRAIN_BATCH];

/* log2(n) * 1000 for n >= 1. The mantissa is normalized to [256,512); the
   16-entry table is the left-endpoint log2(1+k/16), which is exact at every
   power of two and within ~46 mbit elsewhere. */
static const u32 log2t[16] = {
    0, 88, 170, 248, 322, 392, 459, 524,
    585, 644, 700, 755, 807, 858, 907, 954,
};

static u32 log2_mbit(u32 n)
{
    u32 b = 0, m = n;
    while (m >>= 1)            /* b = floor(log2(n)) */
        b++;
    if (b >= 8)                /* normalize mantissa to [256,512) */
        m = n >> (b - 8);
    else
        m = n << (8 - b);
    return b * 1000 + log2t[(m - 256) >> 4];
}

void train_init(void)
{
    last_input_tick = pit_ticks();
    train_watermark = il_len_bytes();
}

void train_note_input(void)
{
    last_input_tick = pit_ticks();
}

u32 train_set_idle_secs(u32 secs)
{
    if (secs < 1)
        secs = 1;
    idle_secs = secs;
    return secs;
}

u32 train_idle_secs(void)
{
    return idle_secs;
}

/* One training cycle: pull a contiguous batch from the tail of the
   interaction log, run 1-4 accumulation passes over it, then score the
   updated model with fixed-point cross-entropy. Prints a report only when
   it actually trained; returns the number of bytes trained (0 = nothing). */
u32 train_run(const char *why, usize max_bytes, usize max_passes)
{
    usize cap = max_bytes ? (max_bytes < TRAIN_BATCH ? max_bytes
                                                      : TRAIN_BATCH)
                          : TRAIN_BATCH;
    usize nb = il_copy_tail(batch, cap);
    train_watermark = il_len_bytes();
    if (nb < TRAIN_MIN)
        return 0;

    usize passes = max_passes ? max_passes : (nb / 1024) + 1;
    if (passes > 4)
        passes = 4;

    for (usize p = 0; p < passes; p++) {
        u32 sat = 0;
        for (usize i = 0; i + 1 < nb; i++) {
            u32 idx = ((u8)batch[i] << 8) | (u8)batch[i + 1];
            if (w[idx] < 255) {
                w[idx]++;
                if (w[idx] == 255)
                    sat++;
            }
        }
        if (sat > 64) {
            for (usize i = 0; i < 65536; i++)
                w[i] >>= 1;   /* learning-rate decay */
        }
    }

    u32 rowsum[256];
    for (u32 r = 0; r < 256; r++) {
        u32 s = 0;
        for (u32 c = 0; c < 256; c++)
            s += w[r * 256 + c];
        rowsum[r] = s;
    }

    u64 bits = 0;
    for (usize i = 0; i + 1 < nb; i++) {
        u32 idx = ((u8)batch[i] << 8) | (u8)batch[i + 1];
        u32 cnt = w[idx];
        u32 rt  = rowsum[(u8)batch[i]];
        bits += cnt ? (u64)(log2_mbit(rt) - log2_mbit(cnt))
                    : (u64)log2_mbit(rt);
    }
    last_loss_mbit = (u32)(bits / (nb - 1));

    train_passes += (u32)passes;
    train_bytes   += (u32)nb;

    /* loss has no zero-pad support in sprintk, so the two fraction digits
       are emitted individually (frac is millibits, 0..999). */
    char buf[96];
    sprintk(buf, sizeof(buf),
            "train: %s (%u passes, %u bytes, loss=%u.%u%u bits/byte)\n",
            why, (unsigned)passes, (unsigned)nb,
            (unsigned)(last_loss_mbit / 1000),
            (unsigned)((last_loss_mbit % 1000) / 100),
            (unsigned)(((last_loss_mbit % 1000) / 10) % 10));
    noc_os_puts(buf);
    return (u32)nb;
}

/* Auto-retrain trigger, polled from kbd_readc's input-wait loop: fire only
   after the idle threshold AND enough new log bytes have accumulated. */
void train_poll_idle(void)
{
    if (last_input_tick == 0)
        return;
    if ((u64)pit_ticks() - last_input_tick < (u64)idle_secs * 100)
        return;
    if (il_len_bytes() < train_watermark + IDLE_NEW_MIN)
        return;
    train_run("idle", TRAIN_BATCH, 0);
}

void train_stats(char *buf, usize cap)
{
    sprintk(buf, cap,
            "model: trained %u passes, %u bytes, loss=%u.%u%u bits/byte, "
            "idle=%u s\n",
            (unsigned)train_passes, (unsigned)train_bytes,
            (unsigned)(last_loss_mbit / 1000),
            (unsigned)((last_loss_mbit % 1000) / 100),
            (unsigned)(((last_loss_mbit % 1000) / 10) % 10),
            (unsigned)idle_secs);
}

/* Forget all learned transitions and reset the lifetime counters. Used by
   the harness to build a clean, controlled corpus for generation tests. */
void train_reset(void)
{
    for (usize i = 0; i < 65536; i++)
        w[i] = 0;
    train_passes = 0;
    train_bytes = 0;
    last_loss_mbit = 0;
}

/* Greedy next-byte generation from the trained bigram table: at each step
   pick the max-weight follower of the previous byte (ties -> lowest byte
   value, so generation is deterministic), stopping at the cap or when no
   transition is known. Returns the number of bytes written; the output is
   the continuation only, the seed is not echoed. */
usize train_generate(char *out, usize cap, const char *seed, usize seedlen)
{
    if (cap == 0 || seedlen == 0)
        return 0;
    u8 prev = (u8)seed[seedlen - 1];
    usize o = 0;
    while (o < cap) {
        u8 next = 0;
        u32 best = 0;
        const u8 *row = &w[(usize)prev * 256];
        for (u32 c = 0; c < 256; c++) {
            if (row[c] > best) {
                best = row[c];
                next = (u8)c;
            }
        }
        if (best == 0)
            break;
        out[o++] = next;
        prev = next;
    }
    return o;
}

const u8 *train_weights(void)
{
    return w;
}
