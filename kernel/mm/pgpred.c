/* M5: page-access predictor, the learning half of the future page/swap
   prefetcher. The page-fault dispatcher feeds every faulting address into a
   circular PFN history; pgpred_predict builds an on-demand bigram model
   (most recent fault is the context) with a unigram fallback, exactly like
   the NOC command predictor. When demand paging / swap arrive, this is the
   model that decides which pages to prefetch. */

#include "pgpred.h"

#define PGHIST_MAX 256

static u64   pfns[PGHIST_MAX];
static usize count;
static usize wr;

typedef struct {
    u64   pfn;
    usize cnt;
    usize rec;   /* stream index of the last occurrence (larger = newer) */
} cand_t;

static cand_t cands[PGHIST_MAX];

void pgpred_fault(u64 vaddr)
{
    pfns[wr] = vaddr >> 12;
    wr = (wr + 1) % PGHIST_MAX;
    if (count < PGHIST_MAX)
        count++;
}

void pgpred_clear(void)
{
    count = 0;
    wr = 0;
}

usize pgpred_count(void)
{
    return count;
}

/* PFN written i positions before the most recent one. */
u64 pgpred_entry(usize back)
{
    if (back < 1 || back > count)
        return 0;
    return pfns[(wr + PGHIST_MAX - back) % PGHIST_MAX];
}

static u64 pick_best(usize ncand)
{
    if (ncand == 0)
        return 0;
    usize best = 0;
    for (usize k = 1; k < ncand; k++) {
        if (cands[k].cnt > cands[best].cnt ||
            (cands[k].cnt == cands[best].cnt &&
             cands[k].rec > cands[best].rec))
            best = k;
    }
    return cands[best].pfn;
}

u64 pgpred_predict(void)
{
    if (count == 0)
        return 0;

    u64 last = pgpred_entry(1);
    usize ncand = 0;

    /* Bigram pass: followers of the most recent faulting page. */
    for (usize i = 1; i < count; i++) {
        if (pgpred_entry(i + 1) != last)
            continue;
        u64 next = pgpred_entry(i);
        usize k = 0;
        for (; k < ncand; k++)
            if (cands[k].pfn == next)
                break;
        if (k == ncand) {
            cands[k].pfn = next;
            cands[k].cnt = 0;
            cands[k].rec = 0;
            ncand++;
        }
        cands[k].cnt++;
        cands[k].rec = count - i;
    }
    u64 p = pick_best(ncand);
    if (p)
        return p;

    /* Unigram fallback: most frequently faulted page. */
    ncand = 0;
    for (usize i = 1; i <= count; i++) {
        u64 pfn = pgpred_entry(i);
        usize k = 0;
        for (; k < ncand; k++)
            if (cands[k].pfn == pfn)
                break;
        if (k == ncand) {
            cands[k].pfn = pfn;
            cands[k].cnt = 0;
            cands[k].rec = 0;
            ncand++;
        }
        cands[k].cnt++;
        cands[k].rec = count - i + 1;
    }
    return pick_best(ncand);
}
