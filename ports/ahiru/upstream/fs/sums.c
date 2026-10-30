/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* sums - checksums of files, see sums.h. Folders on disks only. */
#include <string.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "sums.h"
#include "ops.h"
#include "os.h"
#include "fe.h"
#include "../core/core.h"
#include "../core/util.h"
#include "../core/arc.h"
#include "../core/sums.h"

#define SBUF 32768
#define LINEW (NAMEMAX + 60)
#define MAXLIST 500             /* lines a check shows (all are counted) */

/* MD5 and CRC-32 of one file; 0 when it cannot be read (or Esc) */
static int filesums(const char *path, UBYTE *buf, unsigned char md[16], ULONG *crc, int bytes)
{
    struct md5 m;
    BPTR fh;
    LONG n;
    if (!(fh = Open((STRPTR)path, MODE_OLDFILE))) return 0;
    md5_init(&m);
    *crc = 0;
    while ((n = Read(fh, buf, SBUF)) > 0) {
        md5_add(&m, buf, n);
        *crc = arc_crc32(*crc, buf, n);
        if (bytes) progadd(n);
        if (checkabort()) {
            n = -1;
            break;
        }
    }
    Close(fh);
    if (n < 0) return 0;
    md5_end(&m, md);
    return 1;
}

static int sumkind(const char *name)
{
    int l = strlen(name);
    if (l > 4 && !nocase(name + l - 4, ".md5")) return SUM_MD5;
    if (l > 4 && !nocase(name + l - 4, ".sfv")) return SUM_SFV;
    return 0;
}

/* ---- check a .md5 / .sfv ------------------------------------------------ */

static int checkfile(struct place *p, int cur, int kind, UBYTE *buf)
{
    char path[CPATH], fp[CPATH], q[100];
    char *text = NULL, *lines = NULL, *s, *e;
    const char **items = NULL;
    LONG len;
    BPTR fh;
    int n = 0, ok = 0, bad = 0, miss = 0, wrong = 0, shown = 0;
    if (!buildfull(path, p->path, p->ent[cur].name)) return 0;
    if (p->ent[cur].size > 256000) {
        showmsg("that checksum file is too big to read");
        return 0;
    }
    len = (LONG)p->ent[cur].size;
    if (!(text = xalloc(len + 1))) {
        showmsg("not enough memory");
        goto out;
    }
    if (!(fh = Open((STRPTR)path, MODE_OLDFILE))) {
        faultmsg("cannot read it");
        goto out;
    }
    len = Read(fh, text, len);
    Close(fh);
    if (len < 0) len = 0;
    text[len] = 0;
    /* the bar counts lines: the files' sizes are not known yet */
    for (e = text, len = 1; *e; e++) len += *e == '\n';
    /* room to show as many lines as there are (up to MAXLIST), no more */
    lines = xalloc((len < MAXLIST ? len : MAXLIST) * LINEW);
    items = xalloc((len < MAXLIST ? len : MAXLIST) * sizeof(char *));
    if (!lines || !items) {
        showmsg("not enough memory");
        goto out;
    }
    progshow((ULONG)len);
    cancelok = 1;
    abortop = 0;
    for (s = text; *s && !abortop; s = e) {
        char *name, *want, have[33], *l;
        unsigned char md[16];
        ULONG crc;
        int r;
        for (e = s; *e && *e != '\n'; e++) ;
        if (*e) *e++ = 0;
        if ((r = sum_parse(s, kind, &name, &want)) == 0) continue;
        n++;
        l = shown < MAXLIST && shown < len ? lines + shown * LINEW : NULL;
        if (r < 0) {
            wrong++;
            if (l) sfmt(l, LINEW, "?        line %ld not understood", (long)n);
        } else if (!buildfull(fp, p->path, name) || os_pathtype(fp) != 1) {
            miss++;
            if (l) sfmt(l, LINEW, "MISSING  %s", name);
        } else if (!filesums(fp, buf, md, &crc, 0)) {
            if (abortop) break;
            bad++;
            if (l) sfmt(l, LINEW, "UNREAD   %s", name);
        } else {
            if (kind == SUM_MD5) sum_hex(md, 16, have);
            else sfmt(have, sizeof(have), "%08lx", crc);
            if (!strcmp(have, want)) {
                ok++;
                l = NULL;           /* the good ones are only counted */
            } else {
                bad++;
                if (l) sfmt(l, LINEW, "BAD      %s", name);
            }
        }
        if (l) items[shown++] = l;
        progadd(1);
    }
    cancelok = 0;
    progoff();
    if (abortop) {
        abortop = 0;
        showmsg("checking stopped");
        goto out;
    }
    if (!n) {
        showmsg("no checksums in that file");
        goto out;
    }
    if (!shown) {
        sfmt(q, sizeof(q), "all %d files are as they should be", ok);
        showmsg(q);
        goto out;
    }
    sfmt(q, sizeof(q), "%d ok, %d bad, %d missing%s", ok, bad, miss, wrong ? ", some lines not understood" : "");
    asklist(q, items, shown);
    showmsg(q);
out:
    xfree(text);
    xfree(lines);
    xfree(items);
    return 1;
}

/* ---- make them ----------------------------------------------------------- */

static int makesums(struct place *p, int cur, UBYTE *buf)
{
    char fp[CPATH], q[100], save[NAMEMAX + 4];
    char *lines = NULL;
    const char **items = NULL;
    int *idx = NULL, n = 0, nm = 0, i, done = 0;
    ULONG total = 0;
    for (i = 0; i < p->n; i++) nm += p->ent[i].mark != 0;
    /* the files first, then memory for just those */
    for (i = 0; i < p->n; i++)
        if ((nm ? p->ent[i].mark : i == cur) && p->ent[i].kind == KD_FILE) n++;
    if (!n) {
        showmsg("checksums are made of files - mark some, or stand on one");
        goto out;
    }
    lines = xalloc(n * LINEW);
    items = xalloc(n * sizeof(char *));
    idx = xalloc(n * sizeof(int));
    if (!lines || !items || !idx) {
        showmsg("not enough memory");
        goto out;
    }
    n = 0;
    for (i = 0; i < p->n; i++) {
        if (nm ? !p->ent[i].mark : i != cur) continue;
        if (p->ent[i].kind != KD_FILE) continue;
        idx[n++] = i;
        total += p->ent[i].size;
    }
    progshow(total);
    cancelok = 1;
    abortop = 0;
    for (i = 0; i < n && !abortop; i++) {
        unsigned char md[16];
        ULONG crc;
        char hx[33];
        char *l = lines + i * LINEW;
        if (!buildfull(fp, p->path, p->ent[idx[i]].name) || !filesums(fp, buf, md, &crc, 1)) {
            if (abortop) break;
            /* dashes: no hex digit starts it, so saving leaves it out */
            sfmt(l, LINEW, "--- cannot be read -------------  --------  %s", p->ent[idx[i]].name);
        } else {
            sum_hex(md, 16, hx);
            sfmt(l, LINEW, "%s  %08lx  %s", hx, crc, p->ent[idx[i]].name);
            done++;
        }
        items[i] = l;
    }
    cancelok = 0;
    progoff();
    if (abortop) {
        abortop = 0;
        showmsg("stopped");
        goto out;
    }
    sfmt(q, sizeof(q), "MD5 and CRC-32 - Return saves them as a .md5 file, Esc only shows");
    if (!asklist(q, items, n) || !done) {
        askdone();
        goto out;
    }
    if (n == 1) sfmt(save, sizeof(save), "%.100s.md5", p->ent[idx[0]].name);
    else sfmt(save, sizeof(save), "checksums.md5");
    if (!lineinput("save as: ", save, sizeof(save) - 1, 1) || !save[0]) {
        askdone();
        goto out;
    }
    if (!buildfull(fp, p->path, save)) goto out;
    if (os_pathtype(fp) != 0) {
        sfmt(q, sizeof(q), "%.40s is there - replace it? (y)es (n)o", save);
        if ((askkey(q) | 32) != 'y') {
            askdone();
            goto out;
        }
    }
    {
        /* written beside and swapped in whole: the old file is never
         * emptied first (ops.h) */
        char tmp[CPATH];
        int swap = os_pathtype(fp) == 1;
        BPTR fh;
        int w = 1;
        if (swap) sidename(tmp, fp, 's');
        else strcpy(tmp, fp);
        fh = Open((STRPTR)tmp, MODE_NEWFILE);
        if (!fh) {
            faultmsg("cannot write it");
            goto out;
        }
        for (i = 0; i < n && w; i++) {
            char line[NAMEMAX + 40];
            int l;
            if (lines[i * LINEW] == '-') continue;      /* could not be read */
            /* md5sum's line: the hash, two spaces, the name */
            sfmt(line, sizeof(line), "%.32s  %s\n", lines + i * LINEW, p->ent[idx[i]].name);
            l = strlen(line);
            w = Write(fh, line, l) == l;
        }
        if (!Close(fh) && v_dos >= 36) w = 0;
        if (w && swap && !swapin(tmp, fp, 1)) w = 0;
        if (!w) {
            if (swap) DeleteFile((STRPTR)tmp);
            faultmsg("writing it failed");
        }
        else {
            placechanged(p, save, 0);
            sfmt(q, sizeof(q), "saved %.60s - the same key on it checks the files", save);
            showmsg(q);
        }
    }
out:
    xfree(lines);
    xfree(items);
    xfree(idx);
    return 1;
}

int fs_checksums(struct place *p, int cur)
{
    UBYTE *buf;
    int nm = 0, i, kind, r;
    if (pl_involume(p) || pl_inarc(p) || p->fail || !p->n) {
        showmsg("checksums work in a folder on a disk");
        return 0;
    }
    if (!(buf = AllocMem(SBUF, MEMF_ANY))) {
        showmsg("not enough memory");
        return 0;
    }
    for (i = 0; i < p->n; i++) nm += p->ent[i].mark != 0;
    kind = !nm && cur >= 0 && cur < p->n && p->ent[cur].kind == KD_FILE ? sumkind(p->ent[cur].name) : 0;
    r = kind ? checkfile(p, cur, kind, buf) : makesums(p, cur, buf);
    FreeMem(buf, SBUF);
    return r;
}
