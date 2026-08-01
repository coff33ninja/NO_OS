#ifndef NOOS_PREDICT_H
#define NOOS_PREDICT_H

#include "types.h"

#define CMDHIST_MAX 256
#define CMD_LEN     128

void cmdhist_add(const char *cmd);
void cmdhist_clear(void);
usize cmdhist_count(void);
const char *cmdhist_entry(usize back);
const char *cmdhist_predict(void);

#endif
