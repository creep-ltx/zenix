/* SPDX-License-Identifier: GPL-2.0-only */
/* Stand-ins for libz and netd's OS layer, only so the size test links (tools/host/net-size/entry.c). */
#include "../../../user/libz.h"
void *memcpy(void *d, const void *s, unsigned long n) { char *a = d; const char *b = s; while (n--) *a++ = *b++; return d; }
void *memmove(void *d, const void *s, unsigned long n) { return memcpy(d, s, n); }
void *memset(void *d, int c, unsigned long n) { char *a = d; while (n--) *a++ = (char)c; return d; }
int memcmp(const void *a, const void *b, unsigned long n) { (void)a; (void)b; (void)n; return 0; }
u32 strlen(const char *s) { u32 n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) { (void)b; return *a; }
int strncmp(const char *a, const char *b, u32 n) { (void)a; (void)b; (void)n; return 0; }
char *strcpy(char *d, const char *s) { (void)s; return d; }
char *strncpy(char *d, const char *s, u32 n) { (void)s; (void)n; return d; }
char *strcat(char *d, const char *s) { (void)s; return d; }
char *strchr(const char *s, int c) { (void)c; return (char *)s; }
void *malloc(u32 n) { (void)n; return 0; }
void free(void *p) { (void)p; }
void *netd_heap_alloc(unsigned long n) { (void)n; return 0; }
void netd_heap_free(void *p) { (void)p; }
int vsnprintf(char *buf, u32 max, const char *fmt, va_list ap) { (void)buf; (void)max; (void)fmt; (void)ap; return 0; }
int netd_os_serial_open(const char *dev, unsigned long baud) { (void)dev; (void)baud; return 0; }
long netd_os_read(int h, void *buf, unsigned long n) { (void)h; (void)buf; (void)n; return 0; }
long netd_os_write(int h, const void *buf, unsigned long n) { (void)h; (void)buf; return (long)n; }
void netd_os_close(int h) { (void)h; }
void *netd_os_alloc(unsigned long n) { (void)n; return 0; }
void netd_os_free(void *p) { (void)p; }
void netd_os_log(const char *line) { (void)line; }
void netd_os_fatal(void) { for (;;) ; }
unsigned long netd_os_now_ms(void) { return 0; }
unsigned long netd_os_clock4us(void) { return 0; }
