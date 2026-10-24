/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* say(): printf for the servers built into the kernel image, with only %s, %d and %u and no widths. libz's printf
 * costs about 3 KB of every image that uses it, and the RAM floppy carries the kernel with its images (DESIGN D11,
 * docs/d11-service-manager.md section 15). A line longer than 150 characters is cut. */
#ifdef ZHOST
#define say printf
#else
static void say(const char *f, ...)
{
    char buf[160], *o = buf, *end = buf + sizeof buf - 12;
    va_list ap;
    va_start(ap, f);
    for (; *f && o < end; f++) {
        if (*f != '%') { *o++ = *f; continue; }
        if (*++f == 's') {
            for (const char *s = va_arg(ap, const char *); *s && o < end; ) *o++ = *s++;
            continue;
        }
        u32 v = va_arg(ap, u32);
        char t[10];
        int n = 0;
        if (*f == 'd' && (int)v < 0) { *o++ = '-'; v = -v; }
        do t[n++] = (char)('0' + v % 10); while ((v /= 10) && n < 10);
        while (n) *o++ = t[--n];
    }
    va_end(ap);
    sys_write(1, buf, (u32)(o - buf));
}
#endif
