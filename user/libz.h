#pragma once
#include "usys.h"
#include <stdarg.h>
typedef unsigned long size_t;
u32 strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, u32 n);
char *strcpy(char *d, const char *s);
char *strchr(const char *s, int c);
char *strstr(const char *h, const char *n);
void *memcpy(void *d, const void *s, unsigned long n);
void *memset(void *d, int c, unsigned long n);
void *memmove(void *d, const void *s, unsigned long n);
void *malloc(u32 n);
void free(void *p);
int fprintf(int fd, const char *fmt, ...);
int printf(const char *fmt, ...);
int atoi(const char *s);
