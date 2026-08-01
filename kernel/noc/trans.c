#include "trans.h"
#include "interact.h"
#include "noc_os.h"
#include "pmm.h"
#include "heap.h"
#include "string.h"
#include "printk.h"
#include "model.h"

/* M5 micro-transformer (integer fixed-point, no float): byte-level,
   decoder-only, causal multi-head self-attention + FFN + layer norm,
   trained by SGD with full backprop on the interaction log. Everything is
   memory-driven: the config is derived from total RAM so the same binary
   runs a spec-sized model on a 64 MiB test image and a much larger one on
   multi-GB hardware, degrading gracefully when the budget won't fit.

   Numeric scheme:
     weights            int8
     activations        Q8.8 int16 (raw = value*256)
     layer-norm g,b     int8 Q4.4  (raw = value*16; init g=16 -> 1.0, b=0)
     matmuls            int32 accumulators, result >> 8
     attention scores   int64 accum, Q8.8 via >>8, temperature >>3
     exp                Q12 table built at init (4096*(4080/4096)^k)
     log2               millibit table (same scheme as the bigram model)
     inverse-root       (1<<24)/isqrt(var_raw) then >>16

   The blob is header(32) + weights; offsets are computed from the config,
   never hardcoded. trans_weights() is the only source of truth for the
   demand-paged model window. */

#define TRANS_BATCH 4096
#define TRANS_MIN   4

/* ------------------------------------------------------------------ */
/* config + RAM-derived budgets                                        */
/* ------------------------------------------------------------------ */

static trans_cfg_t  cfg;
static u32          wbudget_kb; /* weight RAM budget (total_ram/8)   */
static u32          acap_kb;    /* activation/workspace cap (ram/4)  */
static u32          acap_override_kb;
static u64          train_passes;   /* lifetime SGD steps            */
static u64          train_bytes;    /* lifetime bytes trained        */
static u32          last_loss_mbit;
static usize        train_watermark;
static char         batch[TRANS_BATCH];

/* workspace (allocated lazily on first train, sized by cfg) */
static i16 *h0, *logits, *dl, *fwd_lay, *bwd_lay, *scr, *gup;
static i32 *wgrad;          /* weight gradient, weights_bytes/1 i32  */
static i32 *embgrad;        /* VOCAB*D */
static u8  *wblob;          /* header + weights                     */
static usize wbytes;        /* blob size (header + weights)         */
static usize wgrad_bytes;
static void free_workspace(void);

/* log2(n)*1000 for n>=1 (identical scheme to kernel/noc/train.c). */
static const u32 log2t[16] = {
    0, 88, 170, 248, 322, 392, 459, 524,
    585, 644, 700, 755, 807, 858, 907, 954,
};

static u32 log2_mbit(u32 n)
{
    u32 b = 0, m = n;
    while (m >>= 1)
        b++;
    if (b >= 8)
        m = n >> (b - 8);
    else
        m = n << (8 - b);
    return b * 1000 + log2t[(m - 256) >> 4];
}

/* e^(-k/256) * 4096 for k in [0,EXP_MAX]; built at init via the exact
   recurrence e^-(k+1)d = e^(-kd) * 4080/4096 (4080/4096 ~= e^-1/256). */
#define EXP_MAX 2048
static u32 expm_tab[EXP_MAX + 1];

static void expm_init(void)
{
    u32 v = 4096;
    expm_tab[0] = v;
    for (u32 k = 1; k <= EXP_MAX; k++) {
        v = (u32)(((u64)v * 4080) >> 12);
        if (v < 1)
            v = 1;
        expm_tab[k] = v;
    }
}

/* deterministic init RNG (fixed seed -> reproducible training) */
static u64 tr_rng = 0x5EED12345678ULL;

static u64 lcg(void)
{
    tr_rng = tr_rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return tr_rng >> 33;
}

static i8 rndi8(void)
{
    return (i8)((int)(lcg() % 65) - 32);
}

u32 trans_weight_budget_kb(void) { return wbudget_kb; }
u32 trans_activ_cap_kb(void)
{
    return acap_override_kb ? acap_override_kb : acap_kb;
}

/* ------------------------------------------------------------------ */
/* layout (offsets computed from cfg, never hardcoded)                 */
/* ------------------------------------------------------------------ */

static usize lay_size(const trans_cfg_t *c)
{
    usize D = c->d_model, F = c->d_ff;
    return 4 * D * D + 2 * D * F + 4 * D;
}

static usize weights_bytes_of(const trans_cfg_t *c)
{
    usize D = c->d_model, V = c->vocab;
    return TRANS_HDR + 2 * V * D + (usize)c->n_layers * lay_size(c) + 2 * D;
}

static usize lay_off(const trans_cfg_t *c, usize l)
{
    return TRANS_HDR + (usize)c->vocab * c->d_model +
           l * lay_size(c);
}

/* offsets of the per-layer tensors relative to lay_off() */
#define O_WQ   0
#define O_WK   (cfg.d_model * cfg.d_model)
#define O_WV   (2 * O_WK)
#define O_WO   (3 * O_WK)
#define O_LN1G (4 * O_WK)
#define O_LN1B (O_LN1G + cfg.d_model)
#define O_WUP  (O_LN1B + cfg.d_model)
#define O_WDN  (O_WUP + cfg.d_model * cfg.d_ff)
#define O_LN2G (O_WDN + cfg.d_ff * cfg.d_model)
#define O_LN2B (O_LN2G + cfg.d_model)
#define LAYSZ  (O_LN2B + cfg.d_model)

static usize o_finln_g(void) { return TRANS_HDR + cfg.vocab * cfg.d_model +
                               cfg.n_layers * LAYSZ; }
static usize o_finln_b(void) { return o_finln_g() + cfg.d_model; }
static usize o_head(void)    { return o_finln_b() + cfg.d_model; }

usize trans_embed_row(u32 b)
{
    return TRANS_HDR + (usize)b * cfg.d_model;
}

/* ------------------------------------------------------------------ */
/* fixed-point helpers                                                 */
/* ------------------------------------------------------------------ */

static int clz32(u32 v)
{
    int n = 0;
    if (!(v & 0xffff0000u)) { n += 16; v <<= 16; }
    if (!(v & 0xff000000u)) { n += 8;  v <<= 8;  }
    if (!(v & 0xf0000000u)) { n += 4;  v <<= 4;  }
    if (!(v & 0xc0000000u)) { n += 2;  v <<= 2;  }
    if (!(v & 0x80000000u)) { n += 1; }
    return n;
}

/* (1<<24)/sqrt(n) >> 16: Q8.8 reciprocal root, used by LayerNorm. */
static i32 rsqrt24(u32 n)
{
    if (n < 1)
        n = 1;
    int shift = (31 - clz32(n)) & ~1;
    u32 m = n << (28 - shift);          /* 28-bit mantissa, even shift */
    u32 r = 0, bit = 1u << 14;
    while (bit) {
        u32 t = r + bit;
        if (t * t <= m) {
            m -= t * t;
            r = t + bit;
        }
        bit >>= 1;
    }
    return (i32)((u64)r * (1u << (18 - shift / 2)) >> 8);
}

/* LayerNorm over len D (Q8.8 data). g/b are int8 Q4.4. */
static void layernorm(i16 *x, usize D, const i8 *g, const i8 *b)
{
    i64 sum = 0, sq = 0;
    for (usize i = 0; i < D; i++) {
        i32 v = x[i];
        sum += v;
        sq += (i64)v * v;
    }
    i64 mean = sum / (i64)D;
    i64 var = (sq / (i64)D) - mean * mean;
    i32 r = rsqrt24((u32)((var > 0) ? var : 1));
    for (usize i = 0; i < D; i++) {
        i32 v = ((i32)x[i] - (i32)mean) * r;
        i32 gg = (i32)(i8)g[i];
        i32 bb = (i32)(i8)b[i];
        v = (v * gg) >> 4;              /* gain Q4.4 */
        v += (bb << 8);                 /* bias Q4.4 -> Q8.8 */
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        x[i] = (i16)v;
    }
}

/* GELU(x)*256 in Q12, x in Q8.8, via x*sigmoid(1.702x). */
static u32 gelu_tab[4097];

static void gelu_init(void)
{
    for (int k = 0; k <= 4096; k++) {
        i64 x = (i64)(k - 2048);        /* Q8.8 raw */
        i64 sx = x * 4356 / 4096;       /* 1.702*x */
        i64 idx = sx / 256 + 1024;      /* exp-table index in [0,2048] */
        if (idx < 0) idx = 0;
        if (idx > 2048) idx = 2048;
        i64 e = expm_tab[(u32)idx];     /* e^(-sx/256)*4096 (Q12) */
        i64 sig = (e * 4096) / (e + 4096); /* sigmoid(sx) Q12 */
        i64 g = (x * sig) >> 4;         /* x*Q8.8 * sig Q12 -> Q12 */
        if (g < 0) g = 0;
        if (g > 65535) g = 65535;
        gelu_tab[k] = (u32)g;
    }
}

/* ------------------------------------------------------------------ */
/* forward pass                                                        */
/* ------------------------------------------------------------------ */

/* Per-layer activation block size in i16 words: Q,K,V (3*C*D) then
   attention scores (H*C*C) then attention output pre-WO (C*D). */
static usize laybuf_words(const trans_cfg_t *c)
{
    usize C = c->ctx, D = c->d_model, H = c->n_heads;
    return 3 * C * D + H * C * C + C * D;
}

/* Full forward pass over `C` tokens: writes logits (C*V Q8.8) and returns a
   pointer to the last position's logits row. h0/fwd_lay/scr/logits must be
   allocated (alloc_workspace). */
static i16 *trans_forward(const u8 *toks, usize C, i16 *logits)
{
    usize D = cfg.d_model, F = cfg.d_ff, H = cfg.n_heads, L = cfg.n_layers;
    usize D2 = D * 2; /* per-token width doubled: Q->V block stride below */
    const usize per = laybuf_words(&cfg);
    const i8 *emb = (const i8 *)wblob + TRANS_HDR;

    for (usize t = 0; t < C; t++) {
        const i8 *r = emb + (usize)toks[t] * D;
        for (usize j = 0; j < D; j++)
            h0[t * D + j] = (i16)((i32)r[j] * 16); /* embed Q4.4 -> Q8.8 */
    }

    for (usize l = 0; l < L; l++) {
        usize lo = lay_off(&cfg, l);
        const i8 *wq = (const i8 *)wblob + lo + O_WQ;
        const i8 *wk = (const i8 *)wblob + lo + O_WK;
        const i8 *wv = (const i8 *)wblob + lo + O_WV;
        const i8 *wo = (const i8 *)wblob + lo + O_WO;
        const i8 *g1 = (const i8 *)wblob + lo + O_LN1G;
        const i8 *b1 = (const i8 *)wblob + lo + O_LN1B;
        const i8 *g2 = (const i8 *)wblob + lo + O_LN2G;
        const i8 *b2 = (const i8 *)wblob + lo + O_LN2B;
        const i8 *wu = (const i8 *)wblob + lo + O_WUP;
        const i8 *wd = (const i8 *)wblob + lo + O_WDN;

        i16 *lay = fwd_lay + l * per;
        i16 *Q = lay, *K = lay + C * D, *V = lay + C * D2;
        i16 *S = V + C * D;
        i16 *A = S + H * C * C;
        i16 *h  = h0 + l * (C * D);
        i16 *h1 = h0 + (l + 1) * (C * D);

        /* pre-LN: copy residual to scr, normalize in place, project */
        for (usize t = 0; t < C * D; t++)
            scr[t] = h[t];
        for (usize t = 0; t < C; t++)
            layernorm(scr + t * D, D, g1, b1);

        for (usize t = 0; t < C; t++) {
            const i16 *xin = scr + t * D;
            for (usize j = 0; j < D; j++) {
                i32 a = 0;
                for (usize k = 0; k < D; k++)
                    a += (i32)wq[k * D + j] * (i32)xin[k];
                Q[t * D + j] = (i16)(a >> 8);
                a = 0;
                for (usize k = 0; k < D; k++)
                    a += (i32)wk[k * D + j] * (i32)xin[k];
                K[t * D + j] = (i16)(a >> 8);
                a = 0;
                for (usize k = 0; k < D; k++)
                    a += (i32)wv[k * D + j] * (i32)xin[k];
                V[t * D + j] = (i16)(a >> 8);
            }
        }

        /* attention scores + softmax + weighted V, per head */
        for (usize hd = 0; hd < H; hd++) {
            usize hs = hd * C * C;
            for (usize i = 0; i < C; i++) {
                i32 mx = -32768;
                for (usize j = 0; j < C; j++) {
                    i32 s;
                    if (j > i) {
                        s = -32768; /* causal mask */
                    } else {
                        i64 acc = 0;
                        for (usize k = 0; k < D / H; k++) {
                            acc += (i64)Q[i * D + hd * (D / H) + k] *
                                   (i64)K[j * D + hd * (D / H) + k];
                        }
                        s = (i32)((acc >> 8) >> 3); /* /sqrt(dh), Q8.8 */
                        if (s > 32767) s = 32767;
                    }
                    S[hs + i * C + j] = (i16)s;
                    if (s > mx) mx = s;
                }
                i64 esum = 0;
                for (usize j = 0; j < C; j++) {
                    i32 s = S[hs + i * C + j] - mx;
                    i32 e;
                    if (s <= -2048)
                        e = 0;
                    else if (s >= 0)
                        e = 4096;
                    else
                        e = (i32)expm_tab[(u32)(-s)];
                    S[hs + i * C + j] = (i16)e; /* overwrite scores w/ exp */
                    esum += e;
                }
                if (esum < 1)
                    esum = 1;
                for (usize j = 0; j < C; j++)
                    S[hs + i * C + j] = (i16)(((u32)S[hs + i * C + j] << 12) /
                                              (u32)esum); /* softmax Q12 */
                for (usize o = 0; o < D / H; o++) {
                    i64 acc = 0;
                    for (usize j = 0; j < C; j++)
                        acc += (i64)S[hs + i * C + j] *
                               (i64)V[j * D + hd * (D / H) + o];
                    A[i * D + hd * (D / H) + o] = (i16)(acc >> 12);
                }
            }
        }

        /* WO project attention out, add residual */
        for (usize t = 0; t < C; t++) {
            i32 acc;
            for (usize j = 0; j < D; j++) {
                acc = 0;
                for (usize k = 0; k < D; k++)
                    acc += (i32)wo[k * D + j] * (i32)A[t * D + k];
                i32 v = (acc >> 8) + (i32)h[t * D + j];
                if (v > 32767) v = 32767;
                if (v < -32768) v = -32768;
                h1[t * D + j] = (i16)v;
            }
        }

        /* FFN sub-block: h1 <- h1 + Wd*GELU(Wu*LN2(h1)) */
        for (usize t = 0; t < C * D; t++)
            scr[t] = h1[t];
        for (usize t = 0; t < C; t++)
            layernorm(scr + t * D, D, g2, b2);
        for (usize t = 0; t < C; t++) {
            i16 *s = scr + t * D;
            for (usize j = 0; j < F; j++) {
                i32 a = 0;
                for (usize k = 0; k < D; k++)
                    a += (i32)wu[k * F + j] * (i32)s[k];
                a >>= 8;
                if (a > 2047) a = 2047;
                if (a < -2048) a = -2048;
                gup[j] = (i16)((i32)gelu_tab[a + 2048] >> 4); /* Q8.8 */
            }
            for (usize j = 0; j < D; j++) {
                i32 a = 0;
                for (usize k = 0; k < F; k++)
                    a += (i32)wd[k * D + j] * (i32)gup[k];
                i32 v = (a >> 8) + (i32)h1[t * D + j];
                if (v > 32767) v = 32767;
                if (v < -32768) v = -32768;
                h1[t * D + j] = (i16)v;
            }
        }
    }

    /* final LN + output head. h0 block L is spare scratch (L+1 blocks). */
    i16 *fin = h0 + L * (C * D);
    for (usize t = 0; t < C * D; t++)
        fin[t] = h0[(L - 1) * (C * D) + t];
    for (usize t = 0; t < C; t++)
        layernorm(fin + t * D, D, (const i8 *)wblob + o_finln_g(),
                  (const i8 *)wblob + o_finln_b());
    const i8 *head = (const i8 *)wblob + o_head();
    for (usize t = 0; t < C; t++) {
        for (usize j = 0; j < TRANS_VOCAB; j++) {
            i32 a = 0;
            for (usize k = 0; k < D; k++)
                a += (i32)head[k * TRANS_VOCAB + j] * (i32)fin[t * D + k];
            i32 v = a >> 8;
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            logits[t * TRANS_VOCAB + j] = (i16)v;
        }
    }
    return logits + (C - 1) * TRANS_VOCAB;
}

/* workspace (h0/fwd_lay/logits/scr) lazily allocated for forward+train */
static bool alloc_workspace(void)
{
    usize C = cfg.ctx, D = cfg.d_model, L = cfg.n_layers, F = cfg.d_ff;
    usize need_h0  = (L + 1) * C * D;   /* L layer residuals + fin scratch */
    usize need_lay = L * laybuf_words(&cfg);
    usize need_log = C * TRANS_VOCAB;
    usize need_scr = C * D;
    usize need_gup = F;
    if (h0 && fwd_lay && logits && scr && gup)
        return true;
    usize sz = need_h0 + need_lay + need_log + need_scr + need_gup;
    i16 *p = (i16 *)kmalloc(sz * sizeof(i16));
    if (!p)
        return false;
    if (!h0) {
        h0 = p; p += need_h0;
    }
    if (!fwd_lay) {
        fwd_lay = p; p += need_lay;
    }
    if (!logits) {
        logits = p; p += need_log;
    }
    if (!scr) {
        scr = p; p += need_scr;
    }
    if (!gup) {
        gup = p; p += need_gup;
    }
    return true;
}

/* Greedy argmax generation: run the forward over a short rolling window of
   the seed, then continue by appending the highest-probability next byte.
   Stops at the output cap or when the argmax would repeat the current byte /
   fall back to the unknown token. Returns the number of continuation bytes
   written (0 = nothing to do). The window is intentionally small so each
   step is a bounded O(W^2) attention pass, keeping generation inside the
   per-burst CPU budget. */
#define TRANS_GEN_WIN 32
usize trans_generate(char *out, usize cap, const char *seed, usize seedlen)
{
    usize W = TRANS_GEN_WIN;
    if (!alloc_workspace())
        return 0;
    if (cap == 0)
        return 0;

    u8 toks[64];
    usize n = seedlen < W ? seedlen : W;
    for (usize i = 0; i < n; i++)
        toks[i] = (u8)seed[i];
    if (n == 0) {
        toks[0] = 0; /* deterministic bootstrap */
        n = 1;
    }

    usize outlen = 0;
    for (;;) {
        i16 *last = trans_forward(toks, n, logits);
        i32 best = -32768;
        i32 arg = -1;
        for (u32 b = 0; b < TRANS_VOCAB; b++) {
            i32 l = last[b];
            if (l > best) {          /* lowest byte wins ties */
                best = l;
                arg = (i32)b;
            }
        }
        if (arg < 0)
            break;
        u8 nb = (u8)arg;
        if (nb == toks[n - 1])
            break; /* argmax repeats the current byte: stop */
        if (outlen >= cap)
            break;
        out[outlen++] = (char)nb;

        /* shift the rolling window and append the new byte */
        if (n >= W) {
            for (usize i = 1; i < W; i++)
                toks[i - 1] = toks[i];
            toks[W - 1] = nb;
        } else {
            toks[n++] = nb;
        }
    }
    return outlen;
}



/* ------------------------------------------------------------------ */
/* workspace / degradation                                             */
/* ------------------------------------------------------------------ */

/* Release all lazily-allocated training buffers (fwd/bwd activations,
   gradients, blob-adjacent state). Called on reconfigure and reset; the
   next trans_train re-allocates them sized to the effective config. */
static void free_workspace(void)
{
    if (h0)      { kfree(h0);      h0 = NULL; }
    if (logits)  { kfree(logits);  logits = NULL; }
    if (dl)      { kfree(dl);      dl = NULL; }
    if (fwd_lay) { kfree(fwd_lay); fwd_lay = NULL; }
    if (bwd_lay) { kfree(bwd_lay); bwd_lay = NULL; }
    if (scr)     { kfree(scr);     scr = NULL; }
    if (gup)     { kfree(gup);     gup = NULL; }
    if (embgrad) { kfree(embgrad); embgrad = NULL; }
    if (wgrad)   { kfree(wgrad);   wgrad = NULL; }
    wgrad_bytes = 0;
}

/* Training working set in KB for a given config: forward activations
   (h0 + per-layer Q/K/V/S/A + logits + LN scratch + FFN gup), the same
   again for the backprop buffers, weight/embedding gradients, and the
   weight blob. Mirrors what alloc_workspace + the train pass allocate so
   graceful degradation tracks the true footprint. */
static u32 working_kb(const trans_cfg_t *c)
{
    u64 C = c->ctx, D = c->d_model, F = c->d_ff, L = c->n_layers, V = c->vocab;
    u64 laybuf = 3 * C * D + (u64)c->n_heads * C * C + C * D; /* words */
    u64 fwd = (L + 1) * C * D     /* h0 (+ fin scratch block) */
            + L * laybuf          /* per-layer Q/K/V/S/A */
            + C * V               /* logits */
            + C * D               /* LN scratch */
            + F;                  /* FFN gup */
    u64 bwd = L * laybuf          /* per-layer backward buffers */
            + C * V               /* dlogits */
            + C * D;              /* dl */
    u64 bytes = (fwd + bwd) * 2   /* i16 */
              + 4 * (u64)weights_bytes_of(c)  /* weight grads (int32) */
              + 4 * V * D         /* embedding grads (int32) */
              + (u64)weights_bytes_of(c);     /* the weight blob itself */
    return (u32)((bytes + 1023) / 1024);
}

/* Graceful degradation: shrink ctx, then d_ff, then layers, then d_model
   until the working set fits the activation cap (weights must also fit the
   weight budget). Mutates c; returns true if a usable config was reached. */
static bool degrade(trans_cfg_t *c)
{
    for (;;) {
        u32 wk = working_kb(c);
        u64 wb = (u64)weights_bytes_of(c);
        if (wk <= trans_activ_cap_kb() &&
            wb / 1024 <= (u64)wbudget_kb)
            return true;
        if (c->ctx > TRANS_CTX_MIN) {
            c->ctx = (c->ctx > TRANS_CTX_MIN) ? c->ctx / 2 : TRANS_CTX_MIN;
            continue;
        }
        if (c->d_ff > c->d_model) {
            c->d_ff /= 2;
            continue;
        }
        if (c->n_layers > TRANS_LAYERS_MIN) {
            c->n_layers /= 2;
            continue;
        }
        if (c->d_model > TRANS_D_MIN) {
            c->d_model /= 2;
            c->d_ff = 4 * c->d_model;
            c->n_heads = (c->d_model >= 8) ? 8 : c->d_model;
            continue;
        }
        return false; /* even the minimum does not fit */
    }
}

/* Default config for the available RAM: start at the M5 spec, then grow
   deterministically while the budgets allow (grow ctx, then d_ff, then
   layers, then d_model). On a 64 MiB image the spec default is the fixed
   point; on larger machines the model scales up with the RAM. */
static void autotune(trans_cfg_t *c)
{
    *c = cfg; /* start from whatever trans_init picked */
    int grown = 1;
    while (grown) {
        grown = 0;
        trans_cfg_t t = *c;
        t.ctx = (u16)((t.ctx * 2 < TRANS_CTX_MAX) ? t.ctx * 2
                                                  : TRANS_CTX_MAX);
        if (working_kb(&t) <= trans_activ_cap_kb() &&
            (u64)weights_bytes_of(&t) / 1024 <= wbudget_kb) {
            *c = t;
            grown = 1;
            continue;
        }
        t = *c;
        t.d_ff = (u16)((t.d_ff * 2 <= 16 * t.d_model) ? t.d_ff * 2
                                                      : 16 * t.d_model);
        if (working_kb(&t) <= trans_activ_cap_kb() &&
            (u64)weights_bytes_of(&t) / 1024 <= wbudget_kb) {
            *c = t;
            grown = 1;
            continue;
        }
        t = *c;
        t.n_layers = (u16)((t.n_layers * 2 <= TRANS_LAYERS_MAX)
                               ? t.n_layers * 2 : TRANS_LAYERS_MAX);
        if (working_kb(&t) <= trans_activ_cap_kb() &&
            (u64)weights_bytes_of(&t) / 1024 <= wbudget_kb) {
            *c = t;
            grown = 1;
            continue;
        }
        t = *c;
        t.d_model = (u16)((t.d_model * 2 <= TRANS_D_MAX) ? t.d_model * 2
                                                         : TRANS_D_MAX);
        t.d_ff = 4 * t.d_model;
        t.n_heads = (t.d_model >= 8) ? 8 : t.d_model;
        if (working_kb(&t) <= trans_activ_cap_kb() &&
            (u64)weights_bytes_of(&t) / 1024 <= wbudget_kb) {
            *c = t;
            grown = 1;
        }
    }
}

/* ------------------------------------------------------------------ */
/* blob lifecycle                                                      */
/* ------------------------------------------------------------------ */

static void hdr_write(void)
{
    u8 *b = wblob;
    b[0] = TRANS_MAGIC0; b[1] = TRANS_MAGIC1; b[2] = 'R'; b[3] = 'N';
    b[4] = (u8)(cfg.d_model & 0xff);   b[5]  = (u8)(cfg.d_model >> 8);
    b[6] = (u8)(cfg.n_layers & 0xff);  b[7]  = (u8)(cfg.n_layers >> 8);
    b[8] = (u8)(cfg.ctx & 0xff);       b[9]  = (u8)(cfg.ctx >> 8);
    b[10] = (u8)(cfg.d_ff & 0xff);     b[11] = (u8)(cfg.d_ff >> 8);
    b[12] = (u8)(cfg.vocab & 0xff);    b[13] = (u8)(cfg.vocab >> 8);
    for (usize i = 14; i < TRANS_HDR; i++)
        b[i] = 0;
}

static void weights_rndinit(void)
{
    usize D = cfg.d_model, V = cfg.vocab;
    i8 *wb = (i8 *)wblob;
    usize n = weights_bytes_of(&cfg);
    for (usize l = 0; l < cfg.n_layers; l++) {
        usize o = lay_off(&cfg, l);
        usize mats = 4 * D * D + 2 * D * cfg.d_ff;
        for (usize i = 0; i < mats; i++)
            wb[o + i] = rndi8();
        for (usize i = 0; i < D; i++) {
            wb[o + O_LN1G + i] = 16; /* gamma = 1.0 (Q4.4) */
            wb[o + O_LN2G + i] = 16;
            wb[o + O_LN1B + i] = 0;  /* bias   = 0.0 */
            wb[o + O_LN2B + i] = 0;
        }
    }
    for (usize i = 0; i < V * D; i++) {
        wb[TRANS_HDR + i] = rndi8(); /* embedding */
        wb[n - V * D + i] = rndi8(); /* head: last V*D bytes of blob */
    }
    for (usize i = 0; i < D; i++) {
        wb[o_finln_g() + i] = 16;
        wb[o_finln_b() + i] = 0;
    }
}

static void alloc_blob(void)
{
    usize n = weights_bytes_of(&cfg);
    if (wblob && wbytes >= n && wbytes <= n * 2) {
        memset(wblob, 0, n);
        wbytes = n;
        hdr_write();
        weights_rndinit();
        return;
    }
    if (wblob)
        kfree(wblob);
    wblob = kmalloc(n);
    wbytes = n;
    memset(wblob, 0, n);
    hdr_write();
    weights_rndinit();
}

/* ------------------------------------------------------------------ */
/* init / config / reset                                               */
/* ------------------------------------------------------------------ */

void trans_init(void)
{
    u64 total_kb = pmm_total_bytes() / 1024;
    wbudget_kb = (u32)((total_kb / 8 < 8192) ? 8192 : total_kb / 8);
    if (wbudget_kb > (1 << 20))
        wbudget_kb = (1 << 20);
    acap_kb = (u32)((total_kb / 4 < 4096) ? 4096 : total_kb / 4);
    if (acap_kb > (1 << 20))
        acap_kb = (1 << 20);

    expm_init();
    gelu_init();

    cfg.d_model  = 64;   /* M5 spec */
    cfg.n_heads  = 8;
    cfg.d_ff     = 256;
    cfg.n_layers = 8;
    cfg.ctx      = 256;
    cfg.vocab    = TRANS_VOCAB;

    if (LAYSZ != lay_size(&cfg)) {
        printk("*** PANIC: trans LAYSZ %u != lay_size %u "
               "(layout macros out of sync)\n",
               (u32)LAYSZ, (u32)lay_size(&cfg));
        __asm__ volatile("cli; hlt; 1: jmp 1b");
    }

    autotune(&cfg);
    degrade(&cfg); /* grow may overshoot; shrink back to a fitting model */
    alloc_blob();
    model_window_setup();
    train_watermark = il_len_bytes();
    last_loss_mbit = 0;
}

void trans_reset(void)
{
    tr_rng = 0x5EED12345678ULL;
    alloc_blob(); /* re-randomizes deterministically */
    train_passes = 0;
    train_bytes = 0;
    last_loss_mbit = 0;
    model_invalidate_all();
}

const trans_cfg_t *trans_cfg(void) { return &cfg; }

const trans_cfg_t *trans_config(u32 layers, u32 ctx)
{
    trans_cfg_t t = cfg;
    if (layers >= TRANS_LAYERS_MIN && layers <= TRANS_LAYERS_MAX)
        t.n_layers = (u16)layers;
    if (ctx >= TRANS_CTX_MIN && ctx <= TRANS_CTX_MAX)
        t.ctx = (u16)ctx;
    bool degraded = (t.n_layers != cfg.n_layers) ||
                    (t.ctx != cfg.ctx) || (t.d_ff != cfg.d_ff) ||
                    (t.d_model != cfg.d_model);
    if (!degraded)
        return &cfg; /* no requested change: keep the current model */
    if (!degrade(&t))
        t = cfg; /* cannot degrade to fit: keep the current model */
    if (t.n_layers == cfg.n_layers && t.ctx == cfg.ctx &&
        t.d_ff == cfg.d_ff && t.d_model == cfg.d_model)
        return &cfg; /* requested change degraded back to the current model */
    {
        cfg = t;
        alloc_blob();
        free_workspace();
        model_invalidate_all();
        model_window_setup();
        char b[96];
        sprintk(b, sizeof(b),
                "trans: effective L=%u ctx=%u d=%u ff=%u heads=%u\n",
                (unsigned)cfg.n_layers, (unsigned)cfg.ctx,
                (unsigned)cfg.d_model, (unsigned)cfg.d_ff,
                (unsigned)cfg.n_heads);
        noc_os_puts(b);
    }
    return &cfg;
}

u32 trans_set_act_cap(u32 kb)
{
    if (kb == 0) {
        acap_override_kb = 0;
    } else {
        if (kb < 64)
            kb = 64;
        if (kb > (1 << 20))
            kb = (1 << 20);
        acap_override_kb = kb;
    }
    /* re-derive so the current model still fits the new cap */
    trans_cfg_t t = cfg;
    if (!degrade(&t))
        t = cfg;
    if (t.n_layers != cfg.n_layers || t.ctx != cfg.ctx ||
        t.d_ff != cfg.d_ff || t.d_model != cfg.d_model) {
        cfg = t;
        alloc_blob();
        free_workspace();
        model_invalidate_all();
        model_window_setup();
        char b[96];
        sprintk(b, sizeof(b),
                "trans: effective L=%u ctx=%u d=%u ff=%u heads=%u\n",
                (unsigned)cfg.n_layers, (unsigned)cfg.ctx,
                (unsigned)cfg.d_model, (unsigned)cfg.d_ff,
                (unsigned)cfg.n_heads);
        noc_os_puts(b);
    }
    return trans_activ_cap_kb();
}

void trans_info(char *buf, usize cap)
{
    sprintk(buf, cap,
            "trans: model L=%u ctx=%u d=%u ff=%u heads=%u "
            "pages=%u weights=%u KB acap=%u KB wbudget=%u KB "
            "trained=%u passes %u bytes loss=%u.%u%u bits/byte\n",
            (unsigned)cfg.n_layers, (unsigned)cfg.ctx,
            (unsigned)cfg.d_model, (unsigned)cfg.d_ff,
            (unsigned)cfg.n_heads, (unsigned)trans_weight_pages(),
            (unsigned)(wbytes / 1024), (unsigned)trans_activ_cap_kb(),
            (unsigned)wbudget_kb,
            (unsigned)train_passes, (unsigned)train_bytes,
            (unsigned)(last_loss_mbit / 1000),
            (unsigned)((last_loss_mbit % 1000) / 100),
            (unsigned)(((last_loss_mbit % 1000) / 10) % 10));
}

const u8 *trans_weights(void) { return wblob; }

u64 trans_weights_bytes(void) { return wbytes; }

u32 trans_weight_pages(void)
{
    return (u32)((wbytes + 4095) / 4096);
}
