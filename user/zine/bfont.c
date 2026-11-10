/* SPDX-License-Identifier: GPL-2.0-only */
/* ZINE's diskfont.library (docs/d29-zine.md 16.13): OpenDiskFont and AvailFonts on the AmigaOS bitmap fonts in
 * FONTS: (the program's own FONTS folder, or where it assigned FONTS:), which `zine` fills from Zenix's
 * /usr/share/fonts/amiga before the hand-over. A NAME.font contents file and its NAME/SIZE font file are checked by
 * the desktop's reader (display/server/amifont.c: every length, offset and relocation; a damaged or hostile file is
 * refused with its reason) before the font file is loaded as AmigaOS loads it (LoadSeg) and its TextFont put on
 * graphics' list. Written from the RKRM (Libraries, "Graphics Library and Text"), the autodocs and the NDK 3.2's
 * include and .fd files; never from AmigaOS code. */
#include "blob.h"
#include "zvol.h"
#include "../../display/server/amifont.h"

#define FPF_DISKFONT 0x02
#define FPF_DESIGNED 0x40
#define NFNAME 160                      /* a font's path */

static u32 len(const char *s)
{
    u32 n = 0;
    while (s[n])
        n++;
    return n;
}
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* the name's last part ("PROGDIR:fonts/xen.font" -> "xen.font") */
static const char *base(const char *n)
{
    const char *b = n;
    for (const char *c = n; *c; c++)
        if (*c == ':' || *c == '/')
            b = c + 1;
    return b;
}

/* a/b joined into o (room NFNAME): 1, or 0 when it does not fit */
static int join(char *o, const char *a, u32 an, const char *b)
{
    u32 bn = len(b);
    if (an + bn + 1 > NFNAME)
        return 0;
    zmemcpy(o, a, an);
    zmemcpy(o + an, b, bn + 1);
    return 1;
}

/* the nearest size in af_contents' list ("8 11 13") to want, 0 if none */
static u32 nearest(const char *sizes, u32 want)
{
    u32 best = 0, bd = 0xFFFF;
    for (const char *p = sizes; *p;) {
        u32 v = 0;
        while (*p >= '0' && *p <= '9')
            v = v * 10 + (u32)(*p++ - '0');
        while (*p == ' ')
            p++;
        u32 d = v > want ? v - want : want - v;
        if (v && d < bd)
            bd = d, best = v;
    }
    return best;
}

static void say_num(u32 v)
{
    char b[12], *p = b + sizeof b;
    *--p = 0;
    do
        *--p = (char)('0' + v % 10);
    while (v /= 10);
    zb_say(p);
}

/* "ZINE: NAME needs xen.font size 8: ..." */
static void say_font(const char *what, const char *name, u32 size, const char *rest)
{
    zb_say("ZINE: ");
    zb_say(Z.blk->name);
    zb_say(what);
    zb_say(name);
    zb_say(" size ");
    say_num(size);
    zb_say(rest);
}

/* OpenDiskFont(textAttr a0) (-30): a font in memory of that name and size (OpenFont's), else NAME.font from FONTS:
 * (a name with a path from there, else from FONTS: by its last part); the size asked, or the nearest the contents
 * file lists unless FPF_DESIGNED is asked (AmigaOS would scale a bitmap; ZINE gives the nearest, and says so) */
u32 c_OpenDiskFont(const u8 *ta)
{
    const char *n = ta && c_TypeOfMem((u32)ta) ? *(const char *const *)(ta + ZO_TextAttr_ta_Name) : 0;
    if (!n || !c_TypeOfMem((u32)n) || len(n) >= NFNAME - 8)
        return 0;
    u32 want = *(const u16 *)(ta + ZO_TextAttr_ta_YSize), designed = ta[ZO_TextAttr_ta_Flags] & FPF_DESIGNED;
    const char *bn = base(n);
    u8 tab[ZO_TextAttr_SIZEOF];         /* the memory fonts are named by the last part */
    zmemcpy(tab, ta, sizeof tab);
    *(const char **)tab = bn;
    u32 tf = c_OpenFont(tab);
    if (tf && *(u16 *)(tf + ZO_TextFont_tf_YSize) == want)
        return tf;
    if (tf)
        c_CloseFont((u8 *)tf);
    char path[NFNAME], file[NFNAME], sizes[64];
    const u8 *d;
    u32 sz, dl;
    if (bn != n && !zb_file_bytes(n, &d, &sz)) {
        zmemcpy(path, n, len(n) + 1);
        dl = (u32)(bn - n);
    } else {
        join(path, "FONTS:", 6, bn);
        dl = 6;
        if (zb_file_bytes(path, &d, &sz)) {
            say_font(" needs ", bn, want, ": copy it into /usr/share/fonts/amiga (the .font file and its folder, "
                     "as in FONTS:)\n");
            return 0;
        }
    }
    const char *why = af_contents(d, sz, (int)want, file, sizeof file, sizes, sizeof sizes);
    u32 got = want;
    if (why && !designed && (got = nearest(sizes, want)) != 0 && got != want) {
        why = af_contents(d, sz, (int)got, file, sizeof file, 0, 0);
        if (!why) {
            say_font(": ", bn, want, " is not there (");
            zb_say(sizes);
            zb_say(" are): size ");
            say_num(got);
            zb_say(" given instead\n");
        }
    }
    if (why) {
        int nosize = 1;
        for (const char *a = why, *b = "no such size"; nosize && (*a || *b); a++, b++)
            nosize = *a == *b;
        if (nosize) {
            say_font(" needs ", bn, want, ": ");
            zb_say(path);
            zb_say(" has sizes ");
            zb_say(sizes);
            zb_say(" only\n");
        } else {
            say_font(": the font ", bn, want, " cannot be used: ");
            zb_say(path);
            zb_say(" is damaged (");
            zb_say(why);
            zb_say(")\n");
        }
        return 0;
    }
    char fp[NFNAME];
    const u8 *fd;
    u32 fsz;
    if (!join(fp, path, dl, file) || zb_file_bytes(fp, &fd, &fsz)) {
        say_font(" needs ", bn, got, ": its file ");
        zb_say(file);
        zb_say(" is not in FONTS: (copy the .font file and its whole folder into /usr/share/fonts/amiga)\n");
        return 0;
    }
    struct af_font *chk = (struct af_font *)zx_AllocMem(sizeof *chk, MEMF_PUBLIC);   /* the reader's result: 2 KB */
    if (!chk)
        return 0;
    why = af_load(fd, fsz, chk, 0);
    zx_FreeMem((u32)chk, sizeof *chk);
    if (why) {
        say_font(": the font ", bn, got, " cannot be used: ");
        zb_say(fp);
        zb_say(" is damaged (");
        zb_say(why);
        zb_say(")\n");
        return 0;
    }
    u32 seg = c_LoadSeg(fp, 0, 0);
    if (!seg)
        return 0;
    u8 *h = (u8 *)(seg << 2) + 4;       /* the hunk, as af_load checked it: the DiskFontHeader at 4 */
    u8 *t = h + 58;
    char *dn = (char *)h + 26;          /* dfh_Name (32 bytes): the font's name, as AmigaOS names a disk font */
    u32 k = len(bn) < 31 ? len(bn) : 31;
    zmemcpy(dn, bn, k);
    dn[k] = 0;
    *(char **)(t + 10) = dn;            /* tf_Message.mn_Node.ln_Name */
    t[8] = 12;                          /* NT_FONT */
    t[ZO_TextFont_tf_Style + 1] |= FPF_DISKFONT;
    *(u16 *)(t + ZO_TextFont_tf_Accessors) = 1;
    *(u32 *)(h + 22) = seg;             /* dfh_Segment */
    zb_font_add(t);
    return (u32)t;
}

/* AvailFonts(buffer a0, bufBytes d0, flags d1) (-36): AFF_MEMORY the fonts in memory, AFF_DISK every size each
 * FONTS:NAME.font lists (style and flags 0 but FPF_DISKFONT); AvailFonts (10 bytes) or, with AFF_TAGGED, TAvailFonts
 * (14, no tags) after the 2-byte count, the names after them; 0, or the bytes more it needs */
#define AFF_MEMORY 1
#define AFF_DISK 2
#define AFF_TAGGED 0x10000
struct av { u8 *buf; u32 size, n, esz, names, need; };
static void av_add(struct av *a, u16 type, const char *name, u32 nl, u32 ysize, u8 style, u8 flags)
{
    a->need += a->esz + nl + 1;
    a->n++;
    if (a->need > a->size)
        return;
    a->names -= nl + 1;                 /* names from the buffer's end down */
    zmemcpy(a->buf + a->names, name, nl);
    a->buf[a->names + nl] = 0;
    u8 *e = a->buf + 2 + (a->n - 1) * a->esz;
    *(u16 *)e = type;
    *(u32 *)(e + 2) = (u32)(a->buf + a->names);
    *(u16 *)(e + 6) = (u16)ysize;
    e[8] = style, e[9] = flags;
    if (a->esz == 14)
        *(u32 *)(e + 10) = 0;
}
u32 c_AvailFonts(u8 *buf, s32 size, u32 flags)
{
    if (!buf || size < 2 || !c_TypeOfMem((u32)buf) || !c_TypeOfMem((u32)buf + (u32)size - 1))
        return size < 2 ? 2 : 0;
    struct av a = { buf, (u32)size, 0, flags & AFF_TAGGED ? 14u : 10u, (u32)size, 2 };
    if (flags & AFF_MEMORY) {
        struct zlist *fl = (struct zlist *)(Z.lib[1] + ZO_GfxBase_TextFonts);
        for (struct znode *f = fl->head; f && f->succ; f = f->succ)
            if (f->name)
                av_add(&a, AFF_MEMORY, f->name, len(f->name), *(u16 *)((u8 *)f + ZO_TextFont_tf_YSize),
                       ((u8 *)f)[ZO_TextFont_tf_Style], ((u8 *)f)[ZO_TextFont_tf_Style + 1]);
    }
    u32 dir;
    if ((flags & AFF_DISK) && zb_dir_ent("FONTS:", &dir)) {
        const void *vol = (const void *)Z.a.vol;
        for (u32 e = zv_next(vol, dir, 0), k = 0; e && k < 500; e = zv_next(vol, dir, e), k++) {
            const struct zv_ent *en = zv_ent(vol, e);
            if (en->dir || en->namelen < 6 || lower(en->name[en->namelen - 5]) != '.' ||
                lower(en->name[en->namelen - 4]) != 'f' || lower(en->name[en->namelen - 1]) != 't')
                continue;
            char sizes[64], file[8];
            af_contents((const u8 *)vol + en->off, en->size, -1, file, sizeof file, sizes, sizeof sizes);
            for (const char *p = sizes; *p;) {
                u32 v = 0;
                while (*p >= '0' && *p <= '9')
                    v = v * 10 + (u32)(*p++ - '0');
                while (*p == ' ')
                    p++;
                if (v)
                    av_add(&a, AFF_DISK, en->name, en->namelen, v, 0, FPF_DISKFONT);
            }
        }
    }
    if (a.need > a.size)
        return a.need - a.size;
    *(u16 *)buf = (u16)a.n;
    return 0;
}

/* ---- the library: built at the start (bmore.c's zb_more_init), the fifth .fd function on stopping it ---- */
#define F(x) extern void zs_##x(void)
F(OpenDiskFont); F(AvailFonts); F(rts);
static const u16 df_lvo[] = { 30, 36, 6, 12, 18, 24 };
static void (*const df_fn[])(void) = { zs_OpenDiskFont, zs_AvailFonts, zs_rts, zs_rts, zs_rts, zs_rts };
const u16 *const zb_df_lvo = df_lvo;
void (*const *const zb_df_fn)(void) = df_fn;
const u16 zb_df_n = sizeof df_lvo / 2;
