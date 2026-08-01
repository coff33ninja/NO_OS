#ifndef NOOS_CORPUS_H
#define NOOS_CORPUS_H

#include "types.h"

/* M5 self-evolution corpus: every successfully spawned model draft is
   committed to a versioned, rollback-safe file set on the flat FS:
     corp%04u.noc        one file per generation, metadata header + source
     last_known_good.noc the most recent successful generation (rollback pt)
     corpus.seq          persisted next sequence number
   A rejected draft never writes a file, so a failed experiment cannot
   corrupt the corpus. Rollback re-runs the last known good generation. */

void corpus_commit(const char *prog, const char *seed);
void corpus_info(void);
void corpus_rollback(void);

#endif
