#ifndef NOOS_TRAIN_H
#define NOOS_TRAIN_H

#include "types.h"

/* M5 idle-retrain loop: a byte-bigram model (8-bit weights, 64 KiB) trained
   from the interaction log. kbd_readc polls train_poll_idle() while waiting
   for input, so the model retrains whenever the system has been idle past a
   threshold AND enough new bytes accumulated. Train; forces a pass; the
   threshold is configurable via TrainIdle so the harness can prove the
   auto-retrain fires without waiting 30 s. */

void train_init(void);
void train_note_input(void);
void train_poll_idle(void);
u32  train_run(const char *why, usize max_bytes, usize max_passes);
void train_stats(char *buf, usize cap);
u32  train_set_idle_secs(u32 secs);
u32  train_idle_secs(void);

#endif
