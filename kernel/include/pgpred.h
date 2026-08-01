#ifndef NOOS_PGPRED_H
#define NOOS_PGPRED_H

#include "types.h"

void pgpred_fault(u64 vaddr);
void pgpred_clear(void);
usize pgpred_count(void);
u64 pgpred_entry(usize back);
u64 pgpred_predict(void);

#endif
