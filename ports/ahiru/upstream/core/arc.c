/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* arc - see arc.h: the shared bits of the readers */
#include "arc.h"

const char *arc_errstr(int e)
{
    switch (e) {
    case ARC_OK: return "ok";
    case ARC_ECORRUPT: return "the archive is damaged";
    case ARC_ECRC: return "checksum error - the data is damaged";
    case ARC_EUNSUP: return "a packing method CFile cannot read";
    case ARC_ENOMEM: return "not enough memory";
    case ARC_EREAD: return "the archive could not be read";
    case ARC_ESTOP: return "stopped";
    default: return "unknown error";
    }
}

static unsigned long *t32;
static unsigned short *t16;

/* the tables go back at the end (AllocMem on the Amiga: a leak would
 * last until the next reboot) */
void arc_freetables(void)
{
    if (t32) arc_free(t32);
    if (t16) arc_free(t16);
    t32 = 0;
    t16 = 0;
}

unsigned long arc_crc32(unsigned long crc, const void *buf, long n)
{
    const unsigned char *p = buf;
    if (!t32) {
        unsigned long c;
        int i, k;
        if (!(t32 = arc_alloc(256 * sizeof(unsigned long)))) return 0;
        for (i = 0; i < 256; i++) {
            c = (unsigned long)i;
            for (k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320UL ^ (c >> 1) : c >> 1;
            t32[i] = c;
        }
    }
    crc = ~crc & 0xFFFFFFFFUL;
    while (n-- > 0) crc = t32[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc & 0xFFFFFFFFUL;
}

int arc_crcready(void)
{
    arc_crc32(0, "", 0);
    return t32 != 0;
}

unsigned int arc_crc16(unsigned int crc, const void *buf, long n)
{
    const unsigned char *p = buf;
    if (!t16) {
        unsigned int c;
        int i, k;
        if (!(t16 = arc_alloc(256 * sizeof(unsigned short)))) return 0;
        for (i = 0; i < 256; i++) {
            c = (unsigned int)i;
            for (k = 0; k < 8; k++) c = c & 1 ? 0xA001 ^ (c >> 1) : c >> 1;
            t16[i] = (unsigned short)c;
        }
    }
    while (n-- > 0) crc = t16[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc & 0xFFFF;
}

void arc_datestamp(int y, int mo, int d, int h, int mi, int s, long *days, long *mins, long *ticks)
{
    static const short cum[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    long dd;
    int yy;
    /* a broken or crafted date (hour 31, minute 63) gives no date, not
     * a datestamp AmigaDOS would refuse or show oddly */
    if (y < 1978 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 ||
        s > 59) {
        *days = *mins = *ticks = 0;
        return;
    }
    dd = 0;
    for (yy = 1978; yy < y; yy++) dd += (yy & 3) == 0 ? 366 : 365;
    dd += cum[mo - 1] + d - 1;
    if (mo > 2 && (y & 3) == 0) dd++;
    *days = dd;
    *mins = h * 60L + mi;
    *ticks = s * 50L;
}
