/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* kind - what a file is as an archive or disk image, and the memory the
 * format readers use. Small and always there: CFile loads the rest of
 * the archive code only when an archive is used (src/modload.c), and
 * asking what a file is must not load it. */
#include <string.h>
#include <dos/dos.h>
#include <proto/dos.h>
#include "arcfs.h"
#include "os.h"
#include "../core/core.h"
#include "../core/util.h"

void *arc_alloc(unsigned long n) { return xalloc(n); }
void arc_free(void *p) { xfree(p); }

int arcfs_kind(const char *path)
{
    unsigned char b[512];
    BPTR fh = Open((STRPTR)path, MODE_OLDFILE);
    LONG n;
    int k = AK_NONE;
    if (!fh) return AK_NONE;
    n = Read(fh, b, sizeof(b));
    if (n >= 4 && b[0] == 'D' && b[1] == 'O' && b[2] == 'S' && b[3] < 8) k = AK_ADF;
    else if (n >= 4 && b[0] == 'R' && b[1] == 'D' && b[2] == 'S' && b[3] == 'K') k = AK_ADF;
    else if (n > 0) {
        switch (sniffmem(b, n)) {
        case TY_LHA: k = AK_LHA; break;
        case TY_LZX: k = AK_LZX; break;
        case TY_ZIP: k = AK_ZIP; break;
        case TY_DMS: k = AK_DMS; break;
        }
    }
    /* a CD image: the descriptor set from sector 16 */
    if (k == AK_NONE && n == sizeof(b) && Seek(fh, 32769, OFFSET_BEGINNING) >= 0 && Read(fh, b, 5) == 5 &&
        memcmp(b, "CD001", 5) == 0)
        k = AK_ISO;
    Close(fh);
    return k;
}

/* an entry inside archive pane p that looks like an archive or image by
 * its name (opening one inside another is not offered) */
int arcfs_kind_rel(struct place *p, const char *name)
{
    static const char *ext[] = { ".lha", ".lzh", ".lzx", ".zip", ".adf", ".iso", ".dms" };
    int l = strlen(name), i;
    (void)p;
    for (i = 0; i < 7; i++)
        if (l > 4 && nocase(name + l - 4, ext[i]) == 0) return i + 1;
    return AK_NONE;
}

