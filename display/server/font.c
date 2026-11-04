/* SPDX-License-Identifier: GPL-2.0-only */
/* displayd's font (DESIGN D38 5): the console's own, held once in the server - Topaz 8 as the kernel took it from the
 * ROM (CON_GRAB copies it: never shipped) for Latin-1, and the Unicode supplement /usr/share/consolefonts/
 * zenix-8x8.zcf (ZCF1, DESIGN D28 7, the format in docs/frozen-interfaces.md) for the rest. A code point with no glyph
 * draws the box, as the console. Its checks are the kernel's (confont.c): a damaged file is not used. */
#include "dpyd.h"

static u8 topaz[224 * 8];
static u8 *zcf;                         /* the file (checked), or 0 */
static u32 zglyphs;
static const u8 box[8] = { 0x00, 0x7E, 0x42, 0x42, 0x42, 0x42, 0x7E, 0x00 };

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static u16 be16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }

static int zcf_ok(const u8 *d, u32 n)
{
    if (n < 32 + 512 || memcmp(d, "ZCF1", 4) || be16(d + 4) != 32 || be16(d + 6) || d[8] != 8 || d[9] != 8)
        return 0;
    u32 g = be16(d + 10), pages = be16(d + 12);
    if (g < 3 || g > 4096 || pages > 255 || be32(d + 16) != n || n != 32 + 512 + 256 * pages + 8 * g)
        return 0;
    for (int i = 0; i < 512; i++)
        if (d[32 + i] > pages)
            return 0;
    for (u32 i = 0; i < pages * 128; i++) {
        u16 e = be16(d + 32 + 512 + 2 * i);
        if (e && !(e >= 32 && e <= 126) && !(e >= 160 && e <= 255) && !(e >= 256 && e < 256 + g))
            return 0;
    }
    zglyphs = g;
    return 1;
}

void font_init(const u8 *t)
{
    memcpy(topaz, t, sizeof topaz);
    int fd = sys_open("/usr/share/consolefonts/zenix-8x8.zcf", O_RDONLY);
    if (fd < 0)
        return;
    struct ustat st;
    if (sys_fstat(fd, &st) == 0 && st.size <= 98848 && (zcf = malloc(st.size))) {
        u32 got = 0;
        while (got < st.size) {
            int r = sys_read(fd, zcf + got, st.size - got);
            if (r <= 0)
                break;
            got += (u32)r;
        }
        if (got != st.size || !zcf_ok(zcf, st.size)) {
            dprintf(2, "displayd: /usr/share/consolefonts/zenix-8x8.zcf is damaged: not used\n");
            free(zcf);
            zcf = 0;
        }
    }
    sys_close(fd);
}

const u8 *font_glyph(u32 cp)
{
    if (cp >= 32 && cp < 256 && !(cp >= 0x7F && cp < 0xA0))
        return topaz + (cp - 32) * 8;
    if (!zcf || cp > 0xFFFF)
        return zcf ? zcf + 32 + 512 + 256 * be16(zcf + 12) : box;
    u8 page = zcf[32 + (cp >> 7)];
    if (!page)
        return zcf + 32 + 512 + 256 * be16(zcf + 12);   /* glyph 0: the font's box */
    u16 e = be16(zcf + 32 + 512 + 256 * (u32)(page - 1) + 2 * (cp & 127));
    if (!e)
        return zcf + 32 + 512 + 256 * be16(zcf + 12);
    if (e < 256)
        return topaz + (e - 32) * 8;
    return zcf + 32 + 512 + 256 * be16(zcf + 12) + 8 * (u32)(e - 256);
}
