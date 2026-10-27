/* SPDX-License-Identifier: GPL-2.0-only */
/* Stand-ins for libz, only so the size test links (tools/host/crypto-size/entry.c). */
typedef unsigned long size_t;
void *memcpy(void *d, const void *s, size_t n) { char *a = d; const char *b = s; while (n--) *a++ = *b++; return d; }
void *memmove(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
void *memset(void *d, int c, size_t n) { char *a = d; while (n--) *a++ = (char)c; return d; }
int memcmp(const void *a, const void *b, size_t n) { (void)a; (void)b; (void)n; return 0; }
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
char *strchr(const char *s, int c) { (void)c; return (char *)s; }
char *strcpy(char *d, const char *s) { (void)s; return d; }
int strcmp(const char *a, const char *b) { (void)b; return *a; }
void *calloc(size_t a, size_t b) { (void)a; (void)b; return 0; }
void free(void *p) { (void)p; }
long time(long *t) { (void)t; return 0; }
void size_entry(void);
volatile unsigned char b[600];
void _start(void) { size_entry(); for (;;) ; }
