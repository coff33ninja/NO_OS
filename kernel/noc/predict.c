/* M5: next-command predictor. The REPL feeds every typed command into a
   small circular history; cmdhist_predict builds an on-demand bigram model
   (most recent command is the context) with a unigram fallback, so a bare
   Predict; returns the command most likely to follow what was just typed. */

#include "predict.h"
#include "string.h"

static char  hist[CMDHIST_MAX][CMD_LEN];
static usize hist_count;   /* number of live entries (<= CMDHIST_MAX) */
static usize hist_wr;      /* next ring write index */

/* Meta builtins that query or reset the model are not user workflow commands;
   logging them would make Predict side-effecting (the just-typed Predict;
   would become the prediction context) and ClearHist/Hist would pollute the
   training stream. */
static bool is_meta(const char *cmd)
{
    usize n = strlen(cmd);
    if (n > 0 && cmd[n - 1] == ';')
        n--;
    if (n != 7 && n != 4 && n != 9)
        return false;
    static const char *metas[] = { "predict", "hist", "clearhist" };
    for (usize m = 0; m < 3; m++) {
        if (strlen(metas[m]) != n)
            continue;
        bool eq = true;
        for (usize i = 0; i < n; i++) {
            char c = cmd[i];
            if (c >= 'A' && c <= 'Z')
                c = (char)(c - 'A' + 'a');
            if (c != metas[m][i]) {
                eq = false;
                break;
            }
        }
        if (eq)
            return true;
    }
    return false;
}

typedef struct {
    const char *cmd;
    usize count;
    usize recency;         /* index of the last occurrence in the stream */
} cand_t;

static cand_t cands[CMDHIST_MAX];

void cmdhist_add(const char *cmd)
{
    if (!cmd || !*cmd || is_meta(cmd))
        return;
    usize n = 0;
    while (cmd[n] && n < CMD_LEN - 1) {
        hist[hist_wr][n] = cmd[n];
        n++;
    }
    hist[hist_wr][n] = '\0';
    hist_wr = (hist_wr + 1) % CMDHIST_MAX;
    if (hist_count < CMDHIST_MAX)
        hist_count++;
}

void cmdhist_clear(void)
{
    hist_count = 0;
    hist_wr = 0;
}

/* Index of the entry written i positions before the most recent one. */
static usize hist_idx(usize back)
{
    return (hist_wr + CMDHIST_MAX - back) % CMDHIST_MAX;
}

usize cmdhist_count(void)
{
    return hist_count;
}

const char *cmdhist_entry(usize back)
{
    if (back < 1 || back > hist_count)
        return NULL;
    return hist[hist_idx(back)];
}

static const char *pick_best(usize ncand)
{
    if (ncand == 0)
        return NULL;
    usize best = 0;
    for (usize k = 1; k < ncand; k++) {
        if (cands[k].count > cands[best].count ||
            (cands[k].count == cands[best].count &&
             cands[k].recency > cands[best].recency))
            best = k;
    }
    return cands[best].cmd;
}

const char *cmdhist_predict(void)
{
    if (hist_count == 0)
        return NULL;

    const char *last = hist[hist_idx(1)];
    usize ncand = 0;

    /* Bigram pass: followers of the most recent command. */
    for (usize i = 1; i < hist_count; i++) {
        if (strcmp(hist[hist_idx(i + 1)], last) != 0)
            continue;
        const char *next = hist[hist_idx(i)];
        usize k = 0;
        for (; k < ncand; k++)
            if (strcmp(cands[k].cmd, next) == 0)
                break;
        if (k == ncand) {
            cands[k].cmd = next;
            cands[k].count = 0;
            cands[k].recency = 0;
            ncand++;
        }
        cands[k].count++;
        cands[k].recency = hist_count - i;
    }
    const char *p = pick_best(ncand);
    if (p)
        return p;

    /* Unigram fallback: most frequent command overall. */
    ncand = 0;
    for (usize i = 1; i <= hist_count; i++) {
        const char *cmd = hist[hist_idx(i)];
        usize k = 0;
        for (; k < ncand; k++)
            if (strcmp(cands[k].cmd, cmd) == 0)
                break;
        if (k == ncand) {
            cands[k].cmd = cmd;
            cands[k].count = 0;
            cands[k].recency = 0;
            ncand++;
        }
        cands[k].count++;
        cands[k].recency = hist_count - i + 1;
    }
    return pick_best(ncand);
}
