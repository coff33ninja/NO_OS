#ifndef NOOS_STRING_H
#define NOOS_STRING_H

#include "types.h"

void *memset(void *dst, int c, u64 n);
void *memcpy(void *dst, const void *src, u64 n);
void *memmove(void *dst, const void *src, u64 n);
int   memcmp(const void *a, const void *b, u64 n);
u64   strlen(const char *s);
int   strcmp(const char *a, const char *b);
int   strncmp(const char *a, const char *b, u64 n);
char *strcpy(char *dst, const char *src);

#endif
