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
const char *zwhy(const char *dflt);   /* "permission denied" if the last failed call was refused, else dflt */

/* more of the C library */
char *strdup(const char *s);
char *strcat(char *d, const char *s);
char *strncpy(char *d, const char *s, u32 n);
char *strrchr(const char *s, int c);
int memcmp(const void *a, const void *b, unsigned long n);
long strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
void qsort(void *base, u32 n, u32 size, int (*cmp)(const void *, const void *));
int snprintf(char *buf, u32 max, const char *fmt, ...);
int isdigit(int c); int isalpha(int c); int isalnum(int c); int isspace(int c);
int isupper(int c); int islower(int c); int toupper(int c); int tolower(int c);
char *getenv(const char *name);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
extern char **environ;

/* buffered line reader */
struct rd { int fd; int pos, len, eof; char buf[2048]; };
void rd_init(struct rd *r, int fd);
/* next line into *line (malloc'd, grown as needed, without the newline); returns its length, or -1 at the end */
int rd_getline(struct rd *r, char **line, u32 *cap);
int rd_getc(struct rd *r);

/* buffered standard output: oputs/oprintf collect, oflush writes; _zexit (called when main returns) flushes */
void oputs(const char *s);
void oputn(const char *s, u32 n);
void oputc(int c);
int oprintf(const char *fmt, ...);
void oflush(void);
void _zexit(int status) __attribute__((noreturn));
