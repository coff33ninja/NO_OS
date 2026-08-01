#ifndef NOOS_TRANS_H
#define NOOS_TRANS_H

#include "types.h"

/* M5 micro-transformer: a byte-level, decoder-only, causal transformer
   (multi-head self-attention + FFN + layer norm, integer fixed-point) that
   learns next-byte prediction from the interaction log. The model is fully
   memory-driven, not size-fixed: every dimension lives in trans_cfg_t and is
   chosen at runtime so the same kernel scales from a 64 MiB test image up to
   multi-GB bare-metal machines. All weights are int8, activations Q8.8
   int16, matmuls accumulate in int32/int64; there is no floating point.

   The weight blob is allocated dynamically to exactly weights_bytes(cfg) and
   is the source of truth for the demand-paged read-only model window (see
   model.c). It begins with a 32-byte config header so a ring-3 process can
   introspect the live model through the window without any syscall. */

#define TRANS_MAGIC0 0x4E /* 'N' */
#define TRANS_MAGIC1 0x54 /* 'T' */
#define TRANS_HDR    32   /* bytes of config header before the weights */

/* Config bounds (generous; budgets derived from total RAM decide the actual
   values via trans_autotune / graceful degradation). */
#define TRANS_D_MIN      16
#define TRANS_D_MAX      1024
#define TRANS_HEADS_MIN  1
#define TRANS_HEADS_MAX  32
#define TRANS_LAYERS_MIN 1
#define TRANS_LAYERS_MAX 32
#define TRANS_CTX_MIN    16
#define TRANS_CTX_MAX    8192
#define TRANS_VOCAB      256

typedef struct {
    u16 d_model;   /* model/embedding dim (spec: 64)          */
    u16 n_heads;   /* attention heads; head_dim = d/n_heads   */
    u16 d_ff;      /* feed-forward inner dim (spec: 4*d_model) */
    u16 n_layers;  /* transformer blocks (spec: 4-8)          */
    u16 ctx;       /* context length in bytes (spec: 256-512) */
    u16 vocab;     /* 256 (raw bytes)                         */
} trans_cfg_t;

/* Kernel RAM budgets, derived from pmm_total_bytes() at boot: weight budget
   and activation cap are each total_ram/8 (8 MB on a 64 MiB image -> the
   spec defaults; GB-scale on big machines). The harness can shrink the
   activation cap with TransMem to prove graceful degradation. */
u32 trans_weight_budget_kb(void);
u32 trans_activ_cap_kb(void);

void trans_init(void);

/* Current effective config (post-degradation). */
const trans_cfg_t *trans_cfg(void);

/* Reconfigure: requested (layers, ctx); d_model/d_ff/heads are derived from
   available RAM by trans_autotune (they are not user-settable directly).
   Validates against the RAM budgets and degrades (ctx first, then d_ff,
   then layers, then d_model) so the effective model always fits. Returns the
   effective cfg; prints a report when degradation occurred. */
const trans_cfg_t *trans_config(u32 layers, u32 ctx);

/* Activation-cap override in KB (TransMem): raises/lowers the cap used for
   degradation and autotune. 0 restores the RAM-derived default. Returns the
   new cap. */
u32 trans_set_act_cap(u32 kb);

/* Forget learned weights and re-initialize deterministically (fixed LCG
   seed), so retraining is reproducible. */
void trans_reset(void);

/* Config + lifetime statistics, one line for TransInfo. */
void trans_info(char *buf, usize cap);

/* The weight blob (header + weights), its size, and how many 4 KiB window
   pages it spans. */
const u8 *trans_weights(void);
u64 trans_weights_bytes(void);
u32 trans_weight_pages(void);

/* Byte offset of the token embedding row for byte `b` inside the blob. */
usize trans_embed_row(u32 b);

/* Generation (greedy argmax, deterministic): continue the seed with the
   trained model, stopping at cap or when the argmax repeats/unknown. Writes
   the continuation only. */
usize trans_generate(char *out, usize cap, const char *seed, usize seedlen);

/* One SGD training pass over the tail of the interaction log (cross-entropy,
   full fixed-point backprop). Reports loss like the bigram and returns the
   number of bytes trained (0 = nothing to do). */
u32 trans_train(const char *why, usize max_bytes, usize max_passes);

/* Report in-corpus top-1 next-byte accuracy (percent) plus average loss. */
u32 trans_eval(void);

#endif
