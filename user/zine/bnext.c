/* SPDX-License-Identifier: GPL-2.0-only */
/* ZINE's blob: the calls the Top 100 productions asked for next (docs/d29-zine.md 16.13, the third round of
 * docs/zine-top100-findings.md): exec FindSemaphore; graphics GetVPModeID, ModeNotAvailable, BltClear, GetSprite,
 * FreeSprite, Draw, PolyDraw; intuition QueryOverscan, WBenchToFront/Back, DisplayAlert, ActivateWindow,
 * AllocRemember/FreeRemember, DrawBorder; dos DateStamp. Written from the RKRM Includes and Autodocs and the NDK 3.2's
 * autodocs, include and .fd files (names, numbers and documented behaviour); never from AmigaOS code. */
#include "blob.h"
#include "zvol.h"

/* ---- exec ---- */

/* FindSemaphore(name a1) (-594): a public semaphore by its name (exec's SemaphoreList; AddSemaphore is not ZINE's,
 * so it finds one only when the program put it there itself) */
u32 c_FindSemaphore(const char *name)
{
    return name ? c_FindName((struct zlist *)(ZEXEC + ZO_ExecBase_SemaphoreList), name) : 0;
}

/* SumKickData (-612): the KickMem/KickTag lists' checksum: ZINE has none of them, 0 */
u32 c_SumKickData(void) { return 0; }

/* ---- graphics ---- */

/* the display ID a ViewPort's mode bits stand for (graphics/modeid.h): the PAL or NTSC monitor and the mode keys */
u32 zb_mode_id(u32 modes)
{
    return (Z.pal ? 0x00021000u : 0x00011000u) | (modes & (0x8000 | 0x0800 | 0x0400 | 0x0080 | 0x0020 | 0x0004));
}

/* GetVPModeID(vp a0) (-792, V36): the ViewPort's display ID; a V39 ColorMap (cm_Type 2) holds it in cm_VPModeID
 * (offset 36), which ZINE's screens set; else from vp_Modes. INVALID_ID for no ViewPort. */
u32 c_GetVPModeID(const u8 *vp)
{
    if (!vp || !c_TypeOfMem((u32)vp))
        return ~0u;
    const u8 *cm = *(const u8 *const *)(vp + 4);
    if (cm && c_TypeOfMem((u32)cm) && cm[1] == 2)
        return *(const u32 *)(cm + 36);
    return zb_mode_id(*(const u16 *)(vp + 32));
}

/* ModeNotAvailable(id d0) (-798, V36): 0 for the native PAL and NTSC modes (any monitor ZINE shows); other monitors
 * (DblPAL, Multiscan, a graphics card's) DI_AVAIL_NOMONITOR (2); INVALID_ID DI_AVAIL_NOCHIPS (1) */
u32 c_ModeNotAvailable(u32 id)
{
    if (id == ~0u)
        return 1;
    u32 mon = id & 0xFFFF1000u;
    return mon == 0 || mon == 0x00011000u || mon == 0x00021000u ? 0 : 2;
}

/* the display database (graphics/displayinfo.h, NDK 3.2; 16.17): FindDisplayInfo(id d0) (-726) a handle for a
 * native PAL or NTSC mode (0 for another, as ModeNotAvailable says), NextDisplayInfo(id d0) (-732) the native modes
 * of the machine's video standard in turn (lores, hires, super-hires, each also laced; INVALID_ID at the end),
 * GetDisplayInfoData(handle a0, buf a1, size d0, tagID d1, id d2) (-756) the record as AmigaOS fills it for these
 * chips: DTAG_DISP (properties, ticks per pixel, the colours' bits), DTAG_DIMS (depth, sizes, the nominal
 * rectangle), DTAG_MNTR (rows and colour clocks of a frame: 312/313 x 227 PAL, 262/263 x 227 NTSC), DTAG_NAME ("PAL:
 * LowRes" ...). The bytes given (at most size), 0 for another tag. Elude's 14bit sound driver asks DTAG_MNTR of the
 * front screen's mode. */
static u32 dhandle[8];
static u32 dnext;
u32 c_FindDisplayInfo(u32 id)
{
    if (c_ModeNotAvailable(id))
        return 0;
    dhandle[dnext & 7] = id;
    return (u32)&dhandle[dnext++ & 7];
}

u32 c_NextDisplayInfo(u32 last)
{
    static const u16 keys[] = { 0x0000, 0x0004, 0x8000, 0x8004, 0x8020, 0x8024 };
    u32 mon = Z.pal ? 0x00021000u : 0x00011000u;
    u32 i = 0;
    if (last != ~0u)
        while (i < 6 && (mon | keys[i]) != last)
            i++;
    i = last == ~0u ? 0 : i + 1;
    return i < 6 ? mon | keys[i] : ~0u;
}

u32 c_GetDisplayInfoData(const u32 *h, u8 *buf, u32 size, u32 tag, u32 id)
{
    if (h && c_TypeOfMem((u32)h))
        id = *h;
    if (!buf || !c_TypeOfMem((u32)buf) || !size || c_ModeNotAvailable(id))
        return 0;
    u8 d[96];
    zmemset(d, 0, sizeof d);
    u32 len;
    int pal = (id & 0xFFFF1000u) == 0x00021000u || ((id & 0xFFFF1000u) == 0 && Z.pal);
    int lace = (id & 4) != 0, hi = (id & 0x8000) != 0, shi = (id & 0x8020) == 0x8020;
    u16 w = (u16)(shi ? 1280 : hi ? 640 : 320), h2 = (u16)((pal ? 256 : 200) << lace);
    switch (tag) {
    case 0x80000000u:                   /* DTAG_DISP: dis_SIZEOF 56 */
        len = 56;
        *(u32 *)(d + 18) = (u32)(lace | (id & 0x0800 ? 8 : 0) | (id & 0x0080 ? 0x1000 : 0) | (id & 0x0400 ? 2 : 0) |
                                 (pal ? 0x20 : 0) | 0x40 | 0x200 | 0x800 | (Z.aga ? 0x00010000u | 0x2000 : 0) |
                                 (Z.ecs ? 0x10 : 0) | 0x00100000u);
        *(u16 *)(d + 22) = (u16)(shi ? 11 : hi ? 22 : 44);     /* dis_Resolution: ticks per pixel x, y */
        *(u16 *)(d + 24) = (u16)(lace ? 22 : 44);
        *(u16 *)(d + 26) = (u16)(shi ? 35 : hi ? 70 : 140);    /* dis_PixelSpeed, ns */
        *(u16 *)(d + 28) = 8;           /* dis_NumStdSprites */
        *(u16 *)(d + 30) = Z.aga ? 0xFFFF : 4096;                /* dis_PaletteRange */
        *(u16 *)(d + 32) = 44, *(u16 *)(d + 34) = 44;           /* dis_SpriteResolution */
        d[40] = d[41] = d[42] = (u8)(Z.aga ? 8 : 4);            /* RedBits, GreenBits, BlueBits */
        break;
    case 0x80001000u:                   /* DTAG_DIMS: dim_SIZEOF 88 */
        len = 88;
        *(u16 *)(d + 16) = (u16)(Z.aga ? 8 : hi ? 4 : 6);       /* dim_MaxDepth */
        *(u16 *)(d + 18) = 16, *(u16 *)(d + 20) = 1;
        *(u16 *)(d + 22) = 16368, *(u16 *)(d + 24) = 16384;
        for (int r = 0; r < 5; r++) {   /* Nominal, MaxOScan, VideoOScan, TxtOScan, StdOScan: 0,0 - w-1,h-1 */
            *(s16 *)(d + 26 + 8 * r + 4) = (s16)(w - 1);
            *(s16 *)(d + 26 + 8 * r + 6) = (s16)(h2 - 1);
        }
        break;
    case 0x80002000u:                   /* DTAG_MNTR: mtr_SIZEOF 96 */
        len = 96;
        *(u16 *)(d + 20) = 129, *(u16 *)(d + 22) = (u16)(pal ? 44 : 44);    /* mtr_ViewPosition */
        *(u16 *)(d + 24) = 22, *(u16 *)(d + 26) = 22;           /* mtr_ViewResolution */
        *(u16 *)(d + 36) = (u16)(pal ? 312 : 262);              /* mtr_TotalRows */
        *(u16 *)(d + 38) = 227;         /* mtr_TotalColorClocks */
        *(u16 *)(d + 40) = (u16)(pal ? 29 : 21);                /* mtr_MinRow */
        *(u16 *)(d + 76) = 129, *(u16 *)(d + 78) = 44;          /* mtr_DefaultViewPosition */
        *(u32 *)(d + 80) = id;          /* mtr_PreferredModeID */
        break;
    case 0x80003000u: {                 /* DTAG_NAME: nif_SIZEOF 16 + 32 + 8 */
        len = 56;
        const char *a = pal ? "PAL:" : "NTSC:", *b = shi ? "SuperHighRes" : hi ? "HighRes" : "LowRes";
        u32 k = 16;
        while (*a)
            d[k++] = (u8)*a++;
        while (*b)
            d[k++] = (u8)*b++;
        if (lace)
            for (const char *c = " Laced"; *c; c++)
                d[k++] = (u8)*c;
        break;
    }
    default:
        return 0;
    }
    *(u32 *)d = tag;                    /* the QueryHeader: StructID, DisplayID, SkipID (TAG_SKIP), Length (8 bytes) */
    *(u32 *)(d + 4) = id;
    *(u32 *)(d + 8) = 3;
    *(u32 *)(d + 12) = (len - 16 + 7) / 8;
    u32 n = size < len ? size : len;
    if (!c_TypeOfMem((u32)buf + n - 1))
        return 0;
    zmemcpy(buf, d, n);
    return n;
}

/* BltClear(memBlock a1, bytecount d0, flags d1) (-300): bytes cleared; flags bit 1: d0 is rows << 16 | bytes a row.
 * Done by the processor after the blitter is idle (the autodoc's "wait" bit 0 is then always kept). Only the
 * program's own memory is cleared. */
void c_BltClear(u8 *mem, u32 count, u32 flags)
{
    u32 n = flags & 2 ? (count >> 16) * (count & 0xFFFF) : count;
    zb_waitblit();
    if (!mem || !n || !c_TypeOfMem((u32)mem) || !c_TypeOfMem((u32)mem + n - 1))
        return;
    zmemset(mem, 0, n);
}

/* GetSprite(sprite a0, num d0) (-408), FreeSprite(num d0) (-414): hardware sprites 1-7 handed out (0 is the mouse
 * pointer's, as intuition keeps it; ZINE shows no pointer, so a program asking for 0 by number gets it too);
 * ss_num (offset 10) set. -1 when the one asked for (or every one) is taken. */
static u8 sprites_taken;
s32 c_GetSprite(u8 *ss, s32 num)
{
    if (num < -1 || num > 7)
        return -1;
    s32 lo = num < 0 ? 1 : num, hi = num < 0 ? 7 : num;
    for (s32 i = lo; i <= hi; i++)
        if (!(sprites_taken & (1u << i))) {
            sprites_taken |= (u8)(1u << i);
            if (ss && c_TypeOfMem((u32)ss))
                *(s16 *)(ss + 10) = (s16)i;
            return i;
        }
    return -1;
}
void c_FreeSprite(s32 num)
{
    if (num >= 0 && num <= 7)
        sprites_taken &= (u8)~(1u << num);
}

/* lines into a RastPort's BitMap (Draw -246, PolyDraw -336; intuition's DrawBorder): every pixel the FgPen in the
 * planes the Mask allows (COMPLEMENT flips them), clipped to the BitMap; the pen ends at the line's end */
#define RP_BitMap 4
#define RP_Mask 24
#define RP_FgPen 25
#define RP_DrawMode 28
#define RP_cp_x 36
#define RP_cp_y 38
static void pixel(u8 *bm, s32 x, s32 y, u8 pen, u8 mode, u8 mask)
{
    u32 bpr = *(u16 *)(bm + ZO_BitMap_BytesPerRow), rows = *(u16 *)(bm + ZO_BitMap_Rows);
    if (x < 0 || y < 0 || (u32)x >= bpr * 8 || (u32)y >= rows)
        return;
    u32 o = (u32)y * bpr + (u32)x / 8;
    u8 m = (u8)(0x80 >> (x & 7));
    for (u32 pl = 0; pl < bm[ZO_BitMap_Depth] && pl < 8; pl++) {
        u8 *p = *(u8 **)(bm + ZO_BitMap_Planes + 4 * pl);
        if (!(mask & (1u << pl)) || !p || p == (u8 *)-1)
            continue;
        if (mode & 2)                   /* COMPLEMENT */
            p[o] ^= m;
        else if (pen & (1u << pl))
            p[o] |= m;
        else
            p[o] &= (u8)~m;
    }
}
static void line(u8 *rp, s32 x0, s32 y0, s32 x1, s32 y1, u8 pen, u8 mode)
{
    u8 *bm = *(u8 **)(rp + RP_BitMap);
    if (bm && c_TypeOfMem((u32)bm)) {
        s32 dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y0 - y1 : y1 - y0;
        s32 sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
        for (u32 n = 0; n < 4096; n++) {    /* no line on a BitMap is longer */
            pixel(bm, x0, y0, pen, mode, rp[RP_Mask]);
            if (x0 == x1 && y0 == y1)
                break;
            s32 e2 = 2 * err;
            if (e2 >= dy)
                err += dy, x0 += sx;
            if (e2 <= dx)
                err += dx, y0 += sy;
        }
    }
    *(s16 *)(rp + RP_cp_x) = (s16)x1, *(s16 *)(rp + RP_cp_y) = (s16)y1;
}
void c_Draw(u8 *rp, s32 x, s32 y)
{
    if (rp)
        line(rp, *(s16 *)(rp + RP_cp_x), *(s16 *)(rp + RP_cp_y), (s16)x, (s16)y, rp[RP_FgPen], rp[RP_DrawMode]);
}
void c_PolyDraw(u8 *rp, s32 n, const s16 *xy)
{
    for (s32 i = 0; rp && xy && i < n && i < 4096; i++)
        c_Draw(rp, xy[2 * i], xy[2 * i + 1]);
}

/* ---- intuition ---- */

/* QueryOverscan(displayID a0, rect a1, oScanType d0) (-474, V36): the overscan rectangles ZINE's display has, in the
 * mode's own pixels: OSCAN_TEXT (1) 320 x 256 (NTSC 200) lowres pixels at 0,0; OSCAN_STANDARD (2) 352 x 283 (NTSC
 * 241) from -16, -14 (NTSC -10); OSCAN_MAX (3) and OSCAN_VIDEO (4) 368 x 283 (241) from -24, -14 (-10). Hires twice
 * as wide, superhires four times, interlace twice as tall. ZINE's own choice of values (the user's Overscan
 * preferences decide them under AmigaOS); TRUE, or FALSE for INVALID_ID or an unknown type. */
u32 c_QueryOverscan(u32 id, s16 *r, s32 type)
{
    if (id == ~0u || !r || !c_TypeOfMem((u32)r) || type < 1 || type > 4)
        return 0;
    s32 w = 320, h = Z.pal ? 256 : 200, x = 0, y = 0;
    if (type == 2)
        w = 352, h = Z.pal ? 283 : 241, x = -16, y = Z.pal ? -14 : -10;
    else if (type > 2)
        w = 368, h = Z.pal ? 283 : 241, x = -24, y = Z.pal ? -14 : -10;
    s32 sx = (id & 0x8020) == 0x8020 ? 4 : id & 0x8000 ? 2 : 1, sy = id & 4 ? 2 : 1;
    r[0] = (s16)(x * sx), r[1] = (s16)(y * sy);
    r[2] = (s16)((x + w) * sx - 1), r[3] = (s16)((y + h) * sy - 1);
    return 1;
}

/* WBenchToFront (-342), WBenchToBack (-336): FALSE, there is no Workbench screen (as when it is closed) */
u32 c_WBenchTo(void) { return 0; }

/* ActivateWindow(win a0) (-450): the window gets the keys and the mouse (binput.c, 16.17) */
void c_ActivateWindow(u8 *w) { if (w) zi_activate(w); }

/* DisplayAlert(alertNumber d0, string a0, height d1) (-90): the alert's text in the program's output (each part: a
 * 16-bit x, an 8-bit y, the text, a continuation byte), answered as if the right mouse button was pressed: FALSE, as
 * a DEADEND_ALERT and a RECOVERY_ALERT that could not be shown both answer */
u32 c_DisplayAlert(u32 num, const u8 *s, u32 h)
{
    (void)h;
    zb_say("ZINE: the program shows an alert:");
    for (int part = 0; s && c_TypeOfMem((u32)s) && part < 16; part++) {
        s += 3;
        zb_say_text(" ", (u32)s, "");
        u32 n = 0;
        while (s[n] && n < 200)
            n++;
        if (s[n])
            break;
        s += n + 1;
        if (!*s++)
            break;
    }
    zb_say(num & 0x80000000u ? " (a dead-end alert)\n" : " (answered with the right mouse button: FALSE)\n");
    return 0;
}

/* AllocRemember(rememberKey a0, size d0, flags d1) (-396), FreeRemember(rememberKey a0, reallyForget d0) (-408): the
 * memory and a Remember node (NextRemember 0, RememberSize 4, Memory 8) put first on the key's list; FreeRemember
 * frees every node, and the memory too when reallyForget */
u32 c_AllocRemember(u32 *key, u32 size, u32 flags)
{
    if (!key || !size)
        return 0;
    u32 *r = (u32 *)zx_AllocMem(12, MEMF_PUBLIC);
    if (!r)
        return 0;
    u32 m = zx_AllocMem(size, flags);
    if (!m) {
        zx_FreeMem((u32)r, 12);
        return 0;
    }
    r[0] = *key, r[1] = size, r[2] = m;
    *key = (u32)r;
    return m;
}
void c_FreeRemember(u32 *key, u32 forget)
{
    if (!key)
        return;
    for (u32 n = 0; *key && n < 100000; n++) {
        u32 *r = (u32 *)*key;
        *key = r[0];
        if (forget && r[2])
            zx_FreeMem(r[2], r[1]);
        zx_FreeMem((u32)r, 12);
    }
}

/* DrawBorder(rp a0, border a1, left d0, top d1) (-108): each Border (LeftEdge 0, TopEdge 2, FrontPen 4, DrawMode 6,
 * Count 7, XY 8, NextBorder 12) drawn as Count - 1 lines through its XY pairs, offset by left/top and its own edges */
void c_DrawBorder(u8 *rp, const u8 *b, s32 left, s32 top)
{
    if (!rp || !c_TypeOfMem((u32)rp))
        return;
    for (u32 k = 0; b && c_TypeOfMem((u32)b) && k < 256; k++, b = *(const u8 *const *)(b + 12)) {
        const s16 *xy = *(const s16 *const *)(b + 8);
        s32 x0 = (s16)left + *(const s16 *)b, y0 = (s16)top + *(const s16 *)(b + 2);
        if (!xy || !c_TypeOfMem((u32)xy))
            continue;
        for (u32 i = 1; i < b[7]; i++)
            line(rp, x0 + xy[2 * i - 2], y0 + xy[2 * i - 1], x0 + xy[2 * i], y0 + xy[2 * i + 1], b[4], b[6]);
    }
}

/* ---- dos ---- */

/* DateStamp(ds d1) (-192): the time now as AmigaDOS counts it (days since 1978-01-01, minutes, 1/50 s): Zenix's
 * clock at the hand-over (the volume says it) and the frames since */
u32 c_DateStamp(u32 *ds)
{
    if (!ds)
        return 0;
    u32 now = ((const struct zv_hdr *)Z.a.vol)->now, base = now > 252460800u ? now - 252460800u : 0;
    u32 t = zb_ticks50();
    u32 secs = base % 86400u + t / 50;
    ds[0] = base / 86400u + secs / 86400u;
    ds[1] = secs % 86400u / 60;
    ds[2] = secs % 60 * 50 + t % 50;
    return (u32)ds;
}

/* FilePart(path d1) (-870): the part after the last ':' or '/'; PathPart(path d1) (-876): where that part starts (the
 * '/' before it dropped, a ':' kept); AddPart(dir d1, name d2, size d3) (-882): name joined to dir in place with a
 * '/' as needed (a name with a ':' replaces it; leading '/' go up one each), FALSE when it does not fit */
u32 c_FilePart(const char *p)
{
    const char *f = p;
    for (const char *c = p; c && *c; c++)
        if (*c == ':' || *c == '/')
            f = c + 1;
    return (u32)f;
}
u32 c_PathPart(const char *p)
{
    const char *f = (const char *)c_FilePart(p);
    if (f > p && f[-1] == '/')
        f--;
    return (u32)f;
}
u32 c_AddPart(char *dir, const char *name, u32 size)
{
    if (!dir || !name)
        return 0;
    u32 dl = 0, nl = 0;
    while (dir[dl])
        dl++;
    while (name[nl])
        nl++;
    for (u32 i = 0; i < nl; i++)
        if (name[i] == ':') {           /* an absolute name: it replaces dir */
            if (nl + 1 > size)
                return 0;
            zmemcpy(dir, name, nl + 1);
            return (u32)-1;
        }
    while (*name == '/') {              /* each leading '/': the dir's last part goes */
        name++, nl--;
        u32 k = dl;
        while (k && dir[k - 1] != '/' && dir[k - 1] != ':')
            k--;
        if (k && dir[k - 1] == '/' && k == dl)
            k--;                        /* "a/b/" counts as "a/b" */
        dl = k && dir[k - 1] == '/' ? k - 1 : k;
    }
    int sep = dl && dir[dl - 1] != ':' && dir[dl - 1] != '/' && nl;
    if (dl + (u32)sep + nl + 1 > size)
        return 0;
    if (sep)
        dir[dl++] = '/';
    zmemcpy(dir + dl, name, nl);
    dir[dl + nl] = 0;
    return (u32)-1;
}

/* Fault(code d1, header d2, buffer d3, len d4) (-468): "header: text" for the code (the commonest ones' texts, else
 * "error N"), its length; PrintFault(code d1, header d2) (-474): that line to Output() */
static const struct { u16 code; const char *t; } faults[] = {
    { 103, "not enough memory available" }, { 120, "argument line invalid or too long" },
    { 202, "object is in use" }, { 205, "object not found" }, { 212, "object is not of required type" },
    { 213, "disk not validated" }, { 214, "disk is write-protected" }, { 218, "device (or volume) is not mounted" },
    { 221, "disk is full" }, { 232, "no more entries in directory" } };
u32 c_Fault(u32 code, const char *hdr, char *buf, s32 len)
{
    if (!buf || len < 1)
        return 0;
    char t[80];
    u32 n = 0;
    for (const char *h = hdr && c_TypeOfMem((u32)hdr) ? hdr : ""; *h && n < 40; h++)
        t[n++] = *h;
    if (n)
        t[n++] = ':', t[n++] = ' ';
    const char *m = 0;
    for (u32 i = 0; i < sizeof faults / sizeof faults[0]; i++)
        if (faults[i].code == code)
            m = faults[i].t;
    if (m)
        while (*m && n < sizeof t - 1)
            t[n++] = *m++;
    else {
        const char *e = "error ";
        while (*e)
            t[n++] = *e++;
        char d[12], *p = d + sizeof d;
        u32 v = code;
        do
            *--p = (char)('0' + v % 10);
        while ((v /= 10) && p > d);
        while (p < d + sizeof d)
            t[n++] = *p++;
    }
    if (n > (u32)len - 1)
        n = (u32)len - 1;
    zmemcpy(buf, t, n);
    buf[n] = 0;
    return n;
}
u32 c_PrintFault(u32 code, const char *hdr)
{
    char t[84];
    u32 n = c_Fault(code, hdr, t, 82);
    t[n++] = '\n';
    c_Write(c_Output(), t, (s32)n);
    return (u32)-1;
}

/* WaitBOVP(vp a0) (-402): until the beam is below the ViewPort (its DHeight lines from the display's top, ZINE's
 * screens start at line $2C), at most a frame */
void c_WaitBOVP(const u8 *vp)
{
    u32 end = 0x2C + (vp && c_TypeOfMem((u32)vp) ? *(const u16 *)(vp + 26) : 256);
    for (u32 it = 0; it < 4000000u; it++) {
        u32 l = ((u32)(VPOSR & 1) << 8) | (VHPOSR >> 8);
        if (l >= end || l < 0x10)
            return;
    }
}

/* WritePixel(rp a1, x d0, y d1) (-324): the FgPen's colour (a pixel outside the BitMap: -1); ReadPixel (-318): the
 * pen at x, y (-1 outside) */
s32 c_WritePixel(u8 *rp, s32 x, s32 y)
{
    u8 *bm = rp && c_TypeOfMem((u32)rp) ? *(u8 **)(rp + RP_BitMap) : 0;
    if (!bm || !c_TypeOfMem((u32)bm) || x < 0 || y < 0 || (u32)x >= *(u16 *)(bm + ZO_BitMap_BytesPerRow) * 8u ||
        (u32)y >= *(u16 *)(bm + ZO_BitMap_Rows))
        return -1;
    pixel(bm, x, y, rp[RP_FgPen], rp[RP_DrawMode], rp[RP_Mask]);
    return 0;
}
s32 c_ReadPixel(u8 *rp, s32 x, s32 y)
{
    u8 *bm = rp && c_TypeOfMem((u32)rp) ? *(u8 **)(rp + RP_BitMap) : 0;
    if (!bm || !c_TypeOfMem((u32)bm) || x < 0 || y < 0 || (u32)x >= *(u16 *)(bm + ZO_BitMap_BytesPerRow) * 8u ||
        (u32)y >= *(u16 *)(bm + ZO_BitMap_Rows))
        return -1;
    u32 o = (u32)y * *(u16 *)(bm + ZO_BitMap_BytesPerRow) + (u32)x / 8, pen = 0;
    for (u32 pl = 0; pl < bm[ZO_BitMap_Depth] && pl < 8; pl++) {
        u8 *p = *(u8 **)(bm + ZO_BitMap_Planes + 4 * pl);
        if (p && p != (u8 *)-1 && (p[o] & (0x80 >> (x & 7))))
            pen |= 1u << pl;
    }
    return (s32)pen;
}
