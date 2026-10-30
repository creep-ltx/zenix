/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* preview - the third column. Directories list, text and ANSI show
 * their first screen, everything else gets a card saying what it is.
 * Computed only when the keyboard is idle (main's loop calls
 * pv_compute when no message is waiting), so holding j down over a
 * floppy never reads a file per row. */
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdlib.h>
#include <string.h>
#include "ahiru.h"
#include "fs/arcfs.h"
#include "core/arc.h"

enum { PV_NONE, PV_DIR, PV_TEXT, PV_ANSI, PV_HEX, PV_CARD };

#define PBUFSZ 16384
#define CARDMAX 64
#define CARDW 120

DList *prv;
int pvdirty = 1;
static int pvmode = PV_NONE;
static char pvpath[PATHMAX];
static unsigned char *pbuf;
static long pblen;
static char card[CARDMAX][CARDW];
static int cardrole[CARDMAX];
static int ncard;
static int wantpic;             /* loadfile: a picture - worth a thumbnail */
static unsigned char *pthumb;   /* the thumbnail (src/thumb.c), RGB */
static int ptw, pth;
static unsigned char *acells, *aattrs;  /* the ANSI preview, rendered once */
static int aw, arows, aok;              /* its size; aok: it is the file's */

extern long recallsel(DList *l);

/* the thumbnail goes (and thumb.c's dithered copy of it with it) */
static void nothumb(void)
{
    free(pthumb);
    pthumb = NULL;
    thumb_drop();
}

int pv_init(void)
{
    pbuf = malloc(PBUFSZ + 16);
    prv = dl_new();
    return pbuf && prv;
}

void pv_free(void)
{
    nothumb();
    thumb_exit();
    free(acells);
    free(aattrs);
    acells = aattrs = NULL;
    aok = 0;
    free(pbuf);
    dl_free(prv);
}

void pv_invalidate(void)
{
    pvpath[0] = 0;
    pvdirty = 1;
}

/* prv already holds the listing of path (going up hands it over) */
void pv_adopt(const char *path)
{
    strcpy(pvpath, path);
    pvmode = prv->ok ? PV_DIR : PV_NONE;
    dl_clamp(prv, listh);
    pvdirty = 1;
}

static void cardadd(int role, const char *s)
{
    if (ncard >= CARDMAX) return;
    strncpy(card[ncard], s, CARDW - 1);
    card[ncard][CARDW - 1] = 0;
    cardrole[ncard] = role;
    ncard++;
}

static void cardf(int role, const char *f, long a, long b, long c)
{
    char s[CARDW + 40];
    fmt(s, f, a, b, c);
    cardadd(role, s);
}

/* ---- the cards ------------------------------------------------------ */

static void card_exec(BPTR fh, long size)
{
    long n = pblen, i, chunk = 0;
    char ver[CARDW];
    ver[0] = 0;
    cardadd(RO_EXEC, "AmigaDOS executable");
    if (n >= 20)
        cardf(RO_TEXT, "%ld hunks", (long)(be32(pbuf + 16) - be32(pbuf + 12) + 1), 0, 0);
    /* $VER: usually sits in the first data hunk; read on up to 512K */
    for (;;) {
        for (i = 0; i + 5 < n; i++) {
            if (pbuf[i] == '$' && memcmp(pbuf + i, "$VER:", 5) == 0) {
                long j = i + 5, k = 0;
                while (j < n && pbuf[j] == ' ') j++;
                while (j < n && k < CARDW - 1 && pbuf[j] >= 32 && pbuf[j] != 127)
                    ver[k++] = (char)pbuf[j++];
                ver[k] = 0;
                break;
            }
        }
        if (ver[0] || ++chunk >= 32 || n < PBUFSZ) break;
        Seek(fh, -16, OFFSET_CURRENT);  /* a tag split across chunks */
        n = Read(fh, pbuf, PBUFSZ);
        if (n <= 16) break;
    }
    cardadd(RO_TEXT, "");
    if (ver[0]) cardadd(RO_HI, ver);
    else cardadd(RO_DIM, "no version string");
    (void)size;
    pblen = 0;                  /* pbuf no longer holds the head */
}

static void card_iff(void)
{
    char type[5], s[CARDW];
    long o = 12, w = 0, h = 0, planes = 0, camg = 0, rate = 0, len8 = 0;
    memcpy(type, pbuf + 8, 4);
    type[4] = 0;
    while (o + 8 <= pblen) {
        unsigned long sz = be32(pbuf + o + 4);
        if (memcmp(pbuf + o, "BMHD", 4) == 0 && o + 8 + 9 <= pblen) {
            w = be16(pbuf + o + 8);
            h = be16(pbuf + o + 10);
            planes = pbuf[o + 16];
        } else if (memcmp(pbuf + o, "CAMG", 4) == 0 && o + 12 <= pblen) {
            camg = (long)be32(pbuf + o + 8);
        } else if (memcmp(pbuf + o, "VHDR", 4) == 0 && o + 20 <= pblen) {
            len8 = (long)be32(pbuf + o + 8);
            rate = be16(pbuf + o + 20);
        } else if (memcmp(pbuf + o, "BODY", 4) == 0) {
            break;
        }
        if (sz > (unsigned long)(pblen - o - 8)) break;     /* past the end, or wrapping round */
        o += 8 + (long)((sz + 1) & ~1UL);
    }
    if (strcmp(type, "ILBM") == 0) {
        cardadd(RO_IMG, "IFF ILBM picture");
        cardadd(RO_TEXT, "");
        if (w) {
            const char *mode = (camg & 0x800) ? "HAM" : (camg & 0x80) ? "Extra Half-Brite" :
                               planes == 24 ? "24-bit" : "";
            fmt(s, "%ld x %ld, %ld plane%s", w, h, planes, planes == 1 ? "" : "s");
            cardadd(RO_TEXT, s);
            if (*mode) cardadd(RO_TEXT, mode);
            else if (planes <= 8) cardf(RO_TEXT, "%ld colours", 1L << planes, 0, 0);
        }
    } else if (strcmp(type, "8SVX") == 0) {
        cardadd(RO_AUD, "IFF 8SVX sound");
        cardadd(RO_TEXT, "");
        if (rate) {
            cardf(RO_TEXT, "%ld Hz", rate, 0, 0);
            if (len8) cardf(RO_TEXT, "%ld.%ld seconds", len8 / rate, (len8 % rate) * 10 / rate, 0);
        }
    } else {
        fmt(s, "IFF %s", type);
        cardadd(RO_IMG, s);
    }
}

static void card_mod(void)
{
    char s[CARDW], nm[23];
    int i, k, pats = 0;
    memcpy(nm, pbuf, 20);
    nm[20] = 0;
    cardadd(RO_AUD, "ProTracker module");
    if (nm[0]) cardadd(RO_HI, nm);
    for (i = 0; i < 128; i++)
        if (pbuf[952 + i] > pats) pats = pbuf[952 + i];
    fmt(s, "%d positions, %d patterns, %.4s", pbuf[950], pats + 1, (char *)pbuf + 1080);
    cardadd(RO_TEXT, s);
    cardadd(RO_TEXT, "");
    for (i = 0; i < 31; i++) {
        const unsigned char *p = pbuf + 20 + i * 30;
        long len = (long)be16(p + 22) * 2;
        for (k = 0; k < 22; k++) nm[k] = (char)(p[k] >= 32 ? p[k] : ' ');
        nm[22] = 0;
        for (k = 21; k >= 0 && nm[k] == ' '; k--) nm[k] = 0;
        if (nm[0] == 0 && len == 0) continue;
        fmt(s, "%2d %-22s %6ld", i + 1, nm, len);
        cardadd(len ? RO_TEXT : RO_DIM, s);
    }
}

static void card_lha(BPTR fh, long size)
{
    unsigned char hb[512];
    char name[CARDW], s[CARDW + 20], sz[8];
    long pos = 0, count = 0, total = 0;
    int first;
    cardadd(RO_ARCH, "LhA archive");
    first = ncard;
    cardadd(RO_TEXT, "");
    cardadd(RO_TEXT, "");
    while (pos < size) {
        unsigned long pk, og, nx;
        long n;
        if (Seek(fh, pos, OFFSET_BEGINNING) < 0) break;
        n = Read(fh, hb, sizeof(hb));
        if (!lha_header(hb, n, name, sizeof(name), &pk, &og, &nx) || nx == 0) break;
        count++;
        total += (long)og;
        fmtsize(sz, og);
        fmt(s, "%5s %s", sz, name);
        cardadd(name[0] && name[strlen(name) - 1] == '/' ? RO_DIR : RO_TEXT, s);
        pos += (long)nx;
        if (count > 2000) break;
    }
    fmtsize(sz, total);
    fmt(card[first], "%ld members, %s unpacked", count, sz);
    pblen = 0;
}

static void card_adf(BPTR fh, long size)
{
    unsigned char boot[4], root[512];
    char s[CARDW], nm[32];
    long rb = size == 1802240 ? 1760 : 880;
    static const char *const fs[] = { "OFS", "FFS", "OFS intl", "FFS intl",
                                      "OFS dircache", "FFS dircache" };
    cardadd(RO_ARCH, size == 1802240 ? "ADF disk image (HD)" : "ADF disk image (DD)");
    cardadd(RO_TEXT, "");
    Seek(fh, 0, OFFSET_BEGINNING);
    if (Read(fh, boot, 4) == 4) {
        if (memcmp(boot, "DOS", 3) == 0 && boot[3] < 6) {
            fmt(s, "AmigaDOS, %s", fs[boot[3]]);
            cardadd(RO_TEXT, s);
            Seek(fh, rb * 512, OFFSET_BEGINNING);
            if (Read(fh, root, 512) == 512) {
                int l = root[432];
                if (l > 30) l = 30;
                memcpy(nm, root + 433, l);
                nm[l] = 0;
                fmt(s, "volume \"%s\"", nm);
                cardadd(RO_HI, s);
            }
        } else {
            cardadd(RO_TEXT, "not a DOS disk (a game or a demo?)");
        }
    }
    pblen = 0;
}

/* lzx and zip (and lha the same way): the members, through the core's
 * readers (fs/arcfs.c's arcfs_listfile) */
static long acount, atotal;
static int afirst;

static int amember(void *ctx, const struct arcent *e)
{
    char s[CARDW + 20], sz[8];
    (void)ctx;
    acount++;
    atotal += (long)e->size;
    if (ncard < CARDMAX) {
        fmtsize(sz, e->size);
        fmt(s, "%5s %.100s%s", e->isdir ? "" : sz, e->name, e->isdir ? "/" : "");
        cardadd(e->isdir ? RO_DIR : RO_TEXT, s);
    }
    return acount < 5000;
}

static void card_arc(const char *path, const char *title, int kind)
{
    char sz[8];
    cardadd(RO_ARCH, title);
    afirst = ncard;
    cardadd(RO_TEXT, "");
    cardadd(RO_TEXT, "");
    acount = atotal = 0;
    arcfs_listfile(path, kind, amember, NULL);
    fmtsize(sz, (unsigned long)atotal);
    fmt(card[afirst], "%ld members, %s unpacked", acount, sz);
    pblen = 0;
}

static void loadfile(const char *path)
{
    BPTR fh;
    long size = 0, ty;
    APTR old = quietreq();
    ncard = 0;
    pblen = 0;
    aok = 0;                    /* a new file: the ANSI is rendered again */
    fh = Open((STRPTR)path, MODE_OLDFILE);
    loudreq(old);
    if (!fh) {
        cardadd(RO_ERR, "cannot open this file");
        pvmode = PV_CARD;
        return;
    }
    Seek(fh, 0, OFFSET_END);
    size = Seek(fh, 0, OFFSET_BEGINNING);
    pblen = Read(fh, pbuf, PBUFSZ);
    if (pblen < 0) pblen = 0;
    ty = sniffmem(pbuf, pblen);
    pvmode = PV_CARD;
    if (size == 901120 || size == 1802240) {
        if (nocase(extof(path), "adf") == 0) ty = -1;
    }
    switch (ty) {
    case -1: card_adf(fh, size); break;
    case TY_EMPTY: cardadd(RO_DIM, "empty file"); break;
    case TY_EXEC: card_exec(fh, size); break;
    case TY_IFF:
        card_iff();
        wantpic = memcmp(pbuf + 8, "ILBM", 4) == 0 || memcmp(pbuf + 8, "PBM ", 4) == 0;
        break;
    case TY_MOD: card_mod(); break;
    case TY_LHA: card_lha(fh, size); break;
    case TY_LZX: card_arc(path, "LZX archive", AK_LZX); break;
    case TY_ZIP: card_arc(path, "Zip archive", AK_ZIP); break;
    case TY_DMS: cardadd(RO_ARCH, "DMS disk archive"); break;
    case TY_ANSI: pvmode = PV_ANSI; break;
    case TY_TEXT: pvmode = PV_TEXT; break;
    default:
        pvmode = PV_HEX;
        /* a picture of another kind: datatypes may read it */
        if (kindof(path) == K_IMG) wantpic = 1;
        break;
    }
    Close(fh);
}

/* ---- compute + draw ------------------------------------------------- */

void pv_compute(void)
{
    char full[PATHMAX];
    long f;
    pvdirty = 0;
    if (!showpv || c2w <= 0) return;
    if (cur->n == 0) {
        pvmode = PV_NONE;
        pvpath[0] = 0;
        pv_draw();
        return;
    }
    selpath(full);
    if (strcmp(full, pvpath) == 0) {
        pv_draw();
        return;
    }
    strcpy(pvpath, full);
    f = dflags(cur, cur->sel);
    if (f & F_DIR) {
        dl_read(prv, full, RD_HIDE | RD_SORT);
        prv->sel = recallsel(prv);
        prv->top = 0;
        dl_clamp(prv, listh);
        pvmode = prv->ok ? PV_DIR : PV_CARD;
        if (!prv->ok) {
            ncard = 0;
            cardadd(RO_ERR, (f & F_VOL) ? "not available" : "cannot read this directory");
        }
    } else if (cur->pl.arc) {
        /* a member: only a picture is unpacked for its preview (to T:,
         * when the keyboard is idle, stopped by any key) */
        char sz[12], line[CARDW], tmp[PATHMAX];
        const char *nm = DNAME(cur, cur->sel);
        unsigned long size = (unsigned long)DSIZE(cur, cur->sel);
        int pic = kindof(nm) == K_IMG && size > 0 && size < 4000000UL && !job_busy(), got;
        nothumb();
        ncard = 0;
        pvmode = PV_CARD;
        cardadd(pic ? RO_IMG : RO_ARCH, pic ? "picture, in the archive" : "in the archive");
        fmtsize(sz, size);
        fmt(line, "%s", sz);
        cardadd(RO_TEXT, line);
        if (!pic) {
            cardadd(RO_TEXT, "");
            cardadd(RO_DIM, "i views it, E edits it");
            cardadd(RO_DIM, "yy and pp copy it out");
        } else {
            pv_draw();
            peekstop = 0;
            pvpeek = 1;
            got = arcfs_temp(&cur->pl, nm, tmp);
            pvpeek = 0;
            if (peekstop) {
                /* a key came while it unpacked: the keyboard wins */
                if (got) DeleteFile((STRPTR)tmp);
                pvpath[0] = 0;
                pvdirty = 1;
                return;
            }
            if (got) {
                int stop, sw, sh;
                pthumb = thumb_make(tmp, (c2w - 2) * cw, (listh - ncard - 4) * ch, &ptw, &pth, &sw, &sh, &stop);
                DeleteFile((STRPTR)tmp);
                if (stop) {
                    pvpath[0] = 0;
                    pvdirty = 1;
                    return;
                }
                if (pthumb) cardf(RO_TEXT, "%ld x %ld", (long)sw, (long)sh, 0);
            }
        }
    } else {
        nothumb();
        wantpic = 0;
        loadfile(full);
        if (wantpic) {
            /* the picture itself, under its card: the room left there */
            int stop, sw, sh, rw = (c2w - 2) * cw, rh = (listh - (pvmode == PV_CARD ? ncard : 0) - 4) * ch;
            pv_draw();                  /* the card first: the picture takes a moment */
            pthumb = thumb_make(full, rw, rh, &ptw, &pth, &sw, &sh, &stop);
            if (stop) {
                /* a key came: it wins; the picture is made again when idle */
                pvpath[0] = 0;
                pvdirty = 1;
                return;
            }
            if (pthumb && pvmode != PV_CARD) {
                /* read by a datatype: its kind from the name, its size */
                char t[40];
                const char *e = extof(full);
                int k;
                ncard = 0;
                for (k = 0; e[k] && k < 8; k++) t[k] = (char)(e[k] >= 'a' && e[k] <= 'z' ? e[k] - 32 : e[k]);
                t[k] = 0;
                fmt(t + k, "%spicture", k ? " " : "");
                cardadd(RO_IMG, t);
                cardf(RO_TEXT, "%ld x %ld", (long)sw, (long)sh, 0);
                pvmode = PV_CARD;
            }
        }
    }
    pv_draw();
}

void pv_draw(void)
{
    int r, x = c2x, w = c2w, rows = listh;
    if (!showpv || w <= 0) return;
    switch (pvmode) {
    case PV_DIR:
        for (r = 0; r < rows; r++) {
            long i = prv->top + r;
            if (i < prv->n) {
                int role;
                entrytext(prv, i, w, w >= 24, &role);
                if (i == prv->sel)
                    put(x, r + 1, (char *)rowbuf, w, 0, rolepen[role]);
                else
                    put(x, r + 1, (char *)rowbuf, w, rolepen[role], 0);
            } else
                clearrow(r + 1, x, w);
        }
        if (prv->n == 0) putrole(x + 1, 1, "empty", 5, RO_DIM);
        break;
    case PV_TEXT:
        {
            static char u8[4 * MAXCOLS + 8];
            long o = 0;
            int isu8 = u8_is(pbuf, pblen);      /* a UTF-8 file: its letters, wide ones in two columns */
            for (r = 0; r < rows; r++) {
                if (o < pblen && isu8) {
                    int n;
                    o = u8_row(pbuf, pblen, o, w, 0, u8, &n, rowbuf);
                    put8(x, r + 1, u8, n, rowbuf, w, rolepen[RO_TEXT], 0);
                } else if (o < pblen) {
                    o = text_row(pbuf, pblen, o, rowbuf, w, 0);
                    put(x, r + 1, (char *)rowbuf, w, rolepen[RO_TEXT], 0);
                } else
                    clearrow(r + 1, x, w);
            }
        }
        break;
    case PV_ANSI:
        /* rendered once per file and size, then only drawn */
        if (!aok || aw != w || arows != rows) {
            free(acells);
            free(aattrs);
            acells = malloc(w * rows);
            aattrs = malloc(w * rows);
            aok = 0;
            if (acells && aattrs) {
                ansi_render(pbuf, pblen, 0, ANSI_DEFATTR, acells, aattrs, w, rows, 80);
                aw = w;
                arows = rows;
                aok = 1;
            }
        }
        if (aok)
            for (r = 0; r < rows; r++)
                ui_attrrow(x, r + 1, acells + r * w, aattrs + r * w, w);
        break;
    case PV_HEX:
        {
            int bpr = hex_bpr(w);
            for (r = 0; r < rows; r++) {
                long o = (long)r * bpr;
                if (o < pblen) {
                    hex_row(pbuf, pblen, o, bpr, rowbuf, w);
                    put(x, r + 1, (char *)rowbuf, w, rolepen[RO_DIM], 0);
                } else
                    clearrow(r + 1, x, w);
            }
        }
        break;
    case PV_CARD:
        for (r = 0; r < rows; r++) {
            clearrow(r + 1, x, w);
            if (r < ncard) {
                int l = (int)strlen(card[r]);
                if (l > w - 1) l = w - 1;
                putrole(x + 1, r + 1, card[r], l, cardrole[r]);
            }
        }
        if (pthumb) {
            int px, py;
            cellxy(x + 1, ncard + 2, &px, &py);
            thumb_show(pthumb, ptw, pth, px, py);
        }
        break;
    default:
        for (r = 0; r < rows; r++) clearrow(r + 1, x, w);
    }
}
