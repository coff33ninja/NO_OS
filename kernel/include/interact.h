#ifndef NOOS_INTERACT_H
#define NOOS_INTERACT_H

#include "types.h"

/* Persistent interaction log (M5: training corpus). The REPL wraps every
   command in a capture, then appends [TICK]/[CMD]/[OUT]/[ERR] records to a
   64 KiB in-memory ring. LogSave persists a checkpoint to interact.log so a
   reboot restores the corpus. Kernel-only: fed from noc_os_putc. */

void il_begin_capture(void);
void il_capture_putc(char c);
void il_end_capture(const char *cmd);
void il_event_spawn(u32 pid, const char *name);
void il_event_exit(u32 pid, i64 code);
void il_save(void);
void il_load(void);
void il_dump(void);
void il_clear(void);
void il_stats(char *buf, usize cap);

#endif
