/* SPDX-License-Identifier: GPL-2.0-only */
/* Reading the kernel log from where it is held (audit 2026-10-26 F10): once the 8 KB ring has wrapped, an offset
 * below its oldest character reads from that character, so a reader that starts at 0 and adds what it got would read
 * the oldest chunk again. So: ask SYS_klog (KLOG_OLDEST) where the log starts, read from there, and ask again before
 * each piece (the log goes on meanwhile; what fell out of the ring is skipped, never repeated). Plain C with the read
 * call given: dmesg uses it with sys_klog, tools/serial-test.c with the kernel's klog_read. A kernel without
 * KLOG_OLDEST answers 0 to it (an offset of 1 GB: nothing), which reads as before. */
#ifndef KLOGREAD_H
#define KLOGREAD_H
#define KLOG_PREV   0x80000000u
#define KLOG_OLDEST 0x40000000u

/* every character of the log (flag: 0 or KLOG_PREV) to out, buf (cap bytes) the piece; returns the count */
static inline unsigned klog_dump(int (*rd)(unsigned off, void *b, unsigned n), unsigned flag,
                                 void (*out)(const char *s, unsigned n), char *buf, unsigned cap)
{
    unsigned off = 0, done = 0;
    for (;;) {
        unsigned oldest = (unsigned)rd(flag | KLOG_OLDEST, buf, 1);
        if (oldest < KLOG_OLDEST && off < oldest)
            off = oldest;
        int n = rd(flag | off, buf, cap);
        if (n <= 0)
            break;
        out(buf, (unsigned)n);
        off += (unsigned)n;
        done += (unsigned)n;
    }
    return done;
}
#endif
