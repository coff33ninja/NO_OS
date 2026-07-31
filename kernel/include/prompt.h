#ifndef NOOS_PROMPT_H
#define NOOS_PROMPT_H

#include "types.h"

void prompt_main(void);

/* Handle one legacy admin command line. Returns true when recognized. */
bool prompt_handle(const char *line);

#endif
