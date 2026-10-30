/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* icon - an icon's tooltypes as lines of text, and back. See icon.h. */
#include <string.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "icon.h"
#include "place.h"
#include "ops.h"
#include "os.h"
#include "../core/util.h"

/* ---- tooltypes by hand (T) ------------------------------------------
 * The .info is rewritten byte by byte: prefix + a fresh tooltype block
 * + the untouched suffix (images, position, any 3.5+ colour appendix
 * survive exactly; a save with no change writes an identical file).
 * PutDiskObject is never used, so icon.library is not needed either.
 * On-disk DiskObject: 78-byte header (magic $E310), DrawerData (56)
 * when do_DrawerData is set, the two Images when their pointers are,
 * the default tool when set, then the tooltypes block when set. */

static ULONG rdl(const UBYTE *b, LONG o)
{
    return ((ULONG)b[o] << 24) | ((ULONG)b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3];
}

static void wrl(UBYTE *b, LONG o, ULONG v)
{
    b[o] = (UBYTE)(v >> 24);
    b[o + 1] = (UBYTE)(v >> 16);
    b[o + 2] = (UBYTE)(v >> 8);
    b[o + 3] = (UBYTE)v;
}

/* past one on-disk Image at off, or -1 when the numbers do not add up */
static LONG skipimage(const UBYTE *b, LONG size, LONG off)
{
    LONG w, h, d;
    if (off + 20 > size) return -1;
    w = (b[off + 4] << 8) | b[off + 5];
    h = (b[off + 6] << 8) | b[off + 7];
    d = (b[off + 8] << 8) | b[off + 9];
    if (w < 1 || h < 1 || d < 1 || w > 4096 || h > 4096 || d > 8) return -1;
    off += 20 + ((w + 15) >> 4) * 2 * h * d;
    return off > size ? -1 : off;
}

/* res[0] = where the tooltype block starts, res[1] = where the suffix
 * after it begins (equal when there is none). FALSE = leave it alone. */
static int ttlocate(const UBYTE *b, LONG size, LONG *res)
{
    LONG off = 78, n, nent, j, l;
    if (size < 78 || b[0] != 0xE3 || b[1] != 0x10) return 0;
    if (rdl(b, 66)) off += 56;
    if (off > size) return 0;
    if (rdl(b, 22) && (off = skipimage(b, size, off)) < 0) return 0;
    if (rdl(b, 26) && (off = skipimage(b, size, off)) < 0) return 0;
    if (rdl(b, 50)) {
        if (off + 4 > size) return 0;
        l = (LONG)rdl(b, off);
        if (l < 0 || l > size - off - 4) return 0;
        off += 4 + l;
    }
    res[0] = off;
    if (rdl(b, 54)) {
        if (off + 4 > size) return 0;
        n = (LONG)rdl(b, off);
        nent = (n >> 2) - 1;
        if (nent < 0 || nent > 4096) return 0;
        off += 4;
        for (j = 0; j < nent; j++) {
            if (off + 4 > size) return 0;
            l = (LONG)rdl(b, off);
            if (l < 0 || l > size - off - 4) return 0;    /* no wrap */
            off += 4 + l;
        }
    }
    res[1] = off;
    return 1;
}

/* the icon file whole, and where its tooltypes are; NULL = unreadable */
static UBYTE *readicon(const char *icon, LONG *size, LONG *res)
{
    struct FileInfoBlock *fib;
    BPTR l, fh;
    LONG isz = -1, n;
    UBYTE *ib = NULL;
    if ((fib = AllocMem(sizeof(*fib), MEMF_PUBLIC | MEMF_CLEAR))) {
        if ((l = Lock((STRPTR)icon, ACCESS_READ))) {
            if (Examine(l, fib)) isz = fib->fib_Size;
            UnLock(l);
        }
        FreeMem(fib, sizeof(*fib));
    }
    if (isz <= 0 || !(ib = xalloc(isz)) || !(fh = Open((STRPTR)icon, MODE_OLDFILE))) {
        xfree(ib);
        return NULL;
    }
    n = Read(fh, ib, isz);
    Close(fh);
    if (n != isz || !ttlocate(ib, isz, res)) {
        /* a layout the walker cannot prove: never guess at pixels */
        xfree(ib);
        return NULL;
    }
    *size = isz;
    return ib;
}

int tt_export(const char *icon, const char *file)
{
    BPTR fh;
    LONG isz, res[2], o, j;
    UBYTE *ib = readicon(icon, &isz, res);
    if (!ib) {
        sfmt(opmsg, sizeof(opmsg), "cannot read that icon file");
        return 0;
    }
    if (!(fh = Open((STRPTR)file, MODE_NEWFILE))) {
        xfree(ib);
        sfmt(opmsg, sizeof(opmsg), "cannot stage the tooltypes");
        return 0;
    }
    if (rdl(ib, 54)) {
        LONG nent = ((LONG)rdl(ib, res[0]) >> 2) - 1;
        o = res[0] + 4;
        for (j = 0; j < nent; j++) {
            LONG ln = (LONG)rdl(ib, o);
            if (ln > 1) Write(fh, ib + o + 4, ln - 1);
            Write(fh, "\n", 1);
            o += 4 + ln;
        }
    }
    Close(fh);
    xfree(ib);
    return 1;
}

int tt_import(const char *icon, const char *file)
{
    struct FileInfoBlock *fib;
    BPTR l, fh;
    LONG isz, res[2], o, j, ntt = 0, nsz, tsz = -1, tl;
    UBYTE *ib, *nb = NULL, *tb = NULL;
    char ntmp[CPATH];
    int swap, ok = 0;
    if (!(ib = readicon(icon, &isz, res))) {
        sfmt(opmsg, sizeof(opmsg), "cannot read that icon file");
        return 0;
    }
    if ((fib = AllocMem(sizeof(*fib), MEMF_PUBLIC | MEMF_CLEAR))) {
        if ((l = Lock((STRPTR)file, ACCESS_READ))) {
            if (Examine(l, fib)) tsz = fib->fib_Size;
            UnLock(l);
        }
        FreeMem(fib, sizeof(*fib));
    }
    sfmt(opmsg, sizeof(opmsg), "tooltypes not saved - the icon is unchanged");
    if (tsz >= 0 && (tb = xalloc(tsz + 1)) && (fh = Open((STRPTR)file, MODE_OLDFILE))) {
        tl = Read(fh, tb, tsz);
        Close(fh);
        if (tl < 0) tl = 0;
        /* count the non-empty lines and the new size */
        nsz = res[0] + (isz - res[1]);
        for (o = 0; o < tl; o = j + 1) {
            for (j = o; j < tl && tb[j] != 10; j++)
                ;
            if (j > o) ntt++, nsz += 4 + (j - o) + 1;
        }
        if (ntt) nsz += 4;
        if ((nb = xalloc(nsz))) {
            LONG w = res[0];
            memcpy(nb, ib, res[0]);
            if (ntt) {
                /* the header's present flag flips only when it must:
                 * an icon that had tooltypes keeps its bytes there */
                if (!rdl(nb, 54)) wrl(nb, 54, 1);
                wrl(nb, w, (ntt + 1) * 4);
                w += 4;
                for (o = 0; o < tl; o = j + 1) {
                    for (j = o; j < tl && tb[j] != 10; j++)
                        ;
                    if (j > o) {
                        wrl(nb, w, j - o + 1);
                        w += 4;
                        memcpy(nb + w, tb + o, j - o);
                        w += j - o;
                        nb[w++] = 0;
                    }
                }
            } else {
                wrl(nb, 54, 0);
            }
            if (isz - res[1] > 0) memcpy(nb + w, ib + res[1], isz - res[1]);
            /* beside the icon, then swapped in: a failed write leaves the
             * old icon whole */
            if ((fh = safeopen(icon, ntmp, &swap))) {
                ok = safeclose(fh, icon, ntmp, swap, Write(fh, nb, nsz) == nsz, 1);
                if (!ok) sfmt(opmsg, sizeof(opmsg), "icon write failed - the icon is unchanged");
            } else {
                char fb[80];
                os_fault(IoErr(), fb, sizeof(fb));
                sfmt(opmsg, sizeof(opmsg), "cannot write the icon: %s", fb);
            }
            xfree(nb);
        }
    }
    xfree(tb);
    xfree(ib);
    return ok;
}
