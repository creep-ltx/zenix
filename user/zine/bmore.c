/* SPDX-License-Identifier: GPL-2.0-only */
/* ZINE's blob: the calls the Top 100 productions asked for (docs/d29-zine.md 16.12; docs/zine-top100-findings.md):
 * exec's debug serial port, IO requests and public ports; dos IsInteractive, FRead/FWrite, GetProgramName,
 * GetArgStr, ReadArgs/FreeArgs, AllocDosObject; graphics' ROM font and text (OpenFont, SetFont, Text...), SetChipRev,
 * GetBitMapAttr, BestModeIDA; utility.library. Written from the RKRM Includes and Autodocs (2.04) and the NDK 3.2's
 * include and .fd files (names and numbers only); never from AmigaOS code. */
#include "blob.h"
#include "zfont.h"

static u32 len0(const char *s)
{
    u32 n = 0;
    while (s && s[n])
        n++;
    return n;
}


/* Latin-1's letters as AmigaDOS compares them (zvol.c's rule): ASCII, and 0xE0-0xFE against 0xC0-0xDE but 0xF7 */
static u32 up1(u32 c)
{
    c &= 0xFF;
    if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7))
        return c - 32;
    return c;
}
static u32 low1(u32 c)
{
    c &= 0xFF;
    if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7))
        return c + 32;
    return c;
}

/* ---- exec ---- */

/* RawIOInit, RawMayGetChar, RawPutChar (-504, -510, -516): exec's debug serial port. The port stays at the speed
 * Zenix set (zine_args serper, put back on the way out anyway): what the other end of Zenix's serial line reads. */
#define SERDATR CUSTOM(0x018)
#define SERDAT  CUSTOM(0x030)
void c_RawIOInit(void) {}
u32 c_RawMayGetChar(void)
{
    u16 d = SERDATR;
    if (!(d & 0x4000))                  /* RBF: nothing received */
        return (u32)-1;
    INTREQ = 0x0800;
    return d & 0xFF;
}
void c_RawPutChar(u32 c)
{
    c &= 0xFF;
    if (!c)
        return;
    u8 t0 = CIAA(CIA_TODLO);            /* at most two frames for the transmit buffer (CLAUDE.md: a timed wait) */
    while (!(SERDATR & 0x2000))
        if ((u8)(CIAA(CIA_TODLO) - t0) > 2)
            return;
    SERDAT = (u16)(0x100 | c);
}

/* CreateIORequest(port a0, size d0) (-654), DeleteIORequest(a0) (-660): a cleared request of that size, its reply
 * port and length set (mn_ReplyPort 14, mn_Length 18) */
u32 c_CreateIORequest(u32 port, u32 size)
{
    if (!port || size < 20)
        return 0;
    u8 *io = (u8 *)zx_AllocMem(size, MEMF_PUBLIC | MEMF_CLEAR);
    if (!io)
        return 0;
    io[8] = 7;                          /* NT_REPLYMSG: not in progress */
    *(u32 *)(io + 14) = port;
    *(u16 *)(io + 18) = (u16)size;
    return (u32)io;
}
void c_DeleteIORequest(u8 *io)
{
    if (io)
        zx_FreeMem((u32)io, *(u16 *)(io + 18));
}

/* AddPort(a1) (-354), RemPort(a1) (-360), FindPort(a1 name) (-390): the public ports, ExecBase's PortList (the port's
 * message list made new, as the autodoc says AddPort does) */
#define MP_MsgList 20
void c_AddPort(u8 *mp)
{
    if (!mp)
        return;
    mp[8] = NT_MSGPORT;
    struct zlist *l = (struct zlist *)(mp + MP_MsgList);
    l->head = (struct znode *)&l->tail;
    l->tail = 0;
    l->tailpred = (struct znode *)&l->head;
    l->type = NT_MESSAGE;
    c_Enqueue((struct zlist *)(ZEXEC + ZO_ExecBase_PortList), (struct znode *)mp);
}
void c_RemPort(u8 *mp) { if (mp) c_Remove((struct znode *)mp); }
u32 c_FindPort(const char *name) { return name ? c_FindName((struct zlist *)(ZEXEC + ZO_ExecBase_PortList), name) : 0; }

/* ---- dos ---- */
#define DOSTRUE ((u32)-1)
#define TASKP(o) (*(volatile u32 *)(Z.proc + (o)))

/* IsInteractive(d1) (-216): the console's own handles (Input(), Output(): what the user sees) are; files, NIL: and a
 * command's redirections are not */
u32 c_IsInteractive(u32 fh) { return fh && (fh == Z.in_fh || fh == Z.out_fh) ? DOSTRUE : 0; }

/* FRead/FWrite(fh d1, buf d2, blocklen d3, blocks d4) (-324, -330): whole blocks, the number done */
static u32 blocks(u32 fh, u32 buf, u32 bl, u32 n, int wr)
{
    if (!bl || !n || n > 0x7FFFFFFFu / bl)
        return 0;
    u32 r = wr ? c_Write(fh, (const char *)buf, (s32)(bl * n)) : c_Read(fh, (char *)buf, (s32)(bl * n));
    return r == (u32)-1 ? 0 : r / bl;
}
u32 c_FRead(u32 fh, u32 buf, u32 bl, u32 n) { return blocks(fh, buf, bl, n, 0); }
u32 c_FWrite(u32 fh, u32 buf, u32 bl, u32 n) { return blocks(fh, buf, bl, n, 1); }

/* GetProgramName(buf d1, len d2) (-576): the CLI's cli_CommandName (a BSTR); too long: cut, DOSFALSE and
 * ERROR_LINE_TOO_LONG */
u32 c_GetProgramName(char *buf, s32 len)
{
    if (len <= 0)
        return 0;
    u32 cli = TASKP(ZO_Process_pr_CLI) << 2;
    const u8 *b = cli ? (const u8 *)(*(u32 *)(cli + ZO_CommandLineInterface_cli_CommandName) << 2) : 0;
    u32 n = b ? b[0] : 0, k = n < (u32)len - 1 ? n : (u32)len - 1;
    if (b)
        zmemcpy(buf, b + 1, k);
    buf[k] = 0;
    if (!cli) {
        Z.ioerr = 212;                  /* ERROR_OBJECT_WRONG_TYPE */
        return 0;
    }
    if (k < n) {
        Z.ioerr = 120;                  /* ERROR_LINE_TOO_LONG */
        return 0;
    }
    return DOSTRUE;
}

/* GetArgStr (-534): the arguments the program was started with */
u32 c_GetArgStr(void) { return TASKP(ZO_Process_pr_Arguments); }

/* AllocDosObject(type d1, tags d2) (-228), FreeDosObject(type d1, ptr d2) (-234): a FileInfoBlock (DOS_FIB 2, 260
 * bytes, longword aligned) or an RDArgs (DOS_RDARGS 5, 32 bytes), cleared; the others ZINE has not (0,
 * ERROR_BAD_NUMBER as an unknown type) */
#define DOS_FIB 2
#define DOS_RDARGS 5
u32 c_AllocDosObject(u32 type, u32 tags)
{
    (void)tags;
    u32 n = type == DOS_FIB ? 260 : type == DOS_RDARGS ? 32 : 0;
    if (!n) {
        Z.ioerr = 115;
        return 0;
    }
    u32 p = c_AllocVec(n, MEMF_PUBLIC | MEMF_CLEAR);
    if (!p)
        Z.ioerr = 103;                  /* ERROR_NO_FREE_STORE */
    return p;
}
void c_FreeDosObject(u32 type, u32 p) { (void)type; if (p) c_FreeVec(p); }

/* ReadArgs(template d1, array d2, rdargs d3) (-798) and FreeArgs(d1) (-858), as the autodoc describes them: the
 * template's items are separated by commas, each one or more names (NAME=ALIAS) and modifiers /A (required), /K (only
 * with its keyword), /S (a switch: the keyword alone), /T (a toggle: the keyword then YES/NO/ON/OFF), /N (a number:
 * the array gets a pointer to a LONG), /M (many: a NULL-ended array of strings), /F (the rest of the line). An
 * argument given as KEY VALUE or KEY=VALUE fills its item; the others fill the items not yet filled that take
 * positional arguments, in order (a /M item takes all of them, giving back the last ones to /A items after it). Quotes
 * keep blanks, *" *N *E ** inside them stand for ", a newline, ESC and *. The source: the RDArgs' CSource if it has a
 * buffer, else the program's arguments. The array is not cleared: items not given keep the caller's defaults. One
 * block (AllocVec) holds the strings, numbers and lists; its address in RDA_DAList (the field AmigaOS keeps private
 * for this); an RDArgs ReadArgs made itself is freed with it. Errors in IoErr: 116 a required argument missing,
 * 118 too many arguments, 115 a bad number, 114 a keyword without its value, 103 no memory. ---- */
#define RA_MAX 32
#define RA_A 1
#define RA_K 2
#define RA_S 4
#define RA_N 8
#define RA_M 16
#define RA_F 32
#define RA_T 64
struct ra_item { const char *name; u32 nlen; u8 f, set; u16 nm; u32 val; u32 first; };   /* name: all its aliases */

/* the template's items: 0 and *n, or 1 when it is longer than RA_MAX items */
static int ra_template(const char *t, struct ra_item *it, u32 *n)
{
    u32 k = 0;
    while (*t) {
        if (k == RA_MAX)
            return 1;
        struct ra_item *x = &it[k++];
        zmemset(x, 0, sizeof *x);
        x->name = t;
        while (*t && *t != '/' && *t != ',')
            t++;
        x->nlen = (u32)(t - x->name);
        while (*t == '/') {
            u32 c = up1((u8)t[1]);
            x->f |= c == 'A' ? RA_A : c == 'K' ? RA_K : c == 'S' ? RA_S : c == 'N' ? RA_N : c == 'M' ? RA_M :
                    c == 'F' ? RA_F : c == 'T' ? RA_T : 0;
            t += t[1] ? 2 : 1;
        }
        while (*t && *t != ',')
            t++;
        if (*t == ',')
            t++;
    }
    *n = k;
    return 0;
}

/* is word (n bytes) one of item x's names (without case)? */
static int ra_is(const struct ra_item *x, const char *w, u32 n)
{
    const char *p = x->name, *e = x->name + x->nlen;
    while (p < e) {
        const char *q = p;
        while (q < e && *q != '=')
            q++;
        if ((u32)(q - p) == n) {
            u32 i = 0;
            while (i < n && up1((u8)p[i]) == up1((u8)w[i]))
                i++;
            if (i == n)
                return 1;
        }
        p = q < e ? q + 1 : q;
    }
    return 0;
}

struct ra_src { const u8 *s; u32 n, pos; char *out; u32 used, cap; };

/* the next argument into the block: its string (0-ended) or 0 at the line's end; *quoted when it was in quotes;
 * stop_eq: an unquoted '=' ends it (KEY=VALUE) */
static char *ra_next(struct ra_src *r, int *quoted, int stop_eq)
{
    while (r->pos < r->n && (r->s[r->pos] == ' ' || r->s[r->pos] == '\t' || r->s[r->pos] == '='))
        r->pos++;
    if (r->pos >= r->n || r->s[r->pos] == '\n' || r->s[r->pos] == 0)
        return 0;
    char *o = r->out + r->used;
    u32 k = 0;
    *quoted = r->s[r->pos] == '"';
    if (*quoted) {
        r->pos++;
        while (r->pos < r->n && r->s[r->pos] != '"' && r->s[r->pos] != '\n' && r->s[r->pos]) {
            u8 c = r->s[r->pos++];
            if (c == '*' && r->pos < r->n) {
                u8 d = (u8)up1(r->s[r->pos++]);
                c = d == 'N' ? '\n' : d == 'E' ? 0x1B : d;
            }
            if (r->used + k + 1 < r->cap)
                o[k++] = (char)c;
        }
        if (r->pos < r->n && r->s[r->pos] == '"')
            r->pos++;
    } else
        while (r->pos < r->n && r->s[r->pos] != ' ' && r->s[r->pos] != '\t' && r->s[r->pos] != '\n' &&
               r->s[r->pos] && !(stop_eq && r->s[r->pos] == '=')) {
            if (r->used + k + 1 < r->cap)
                o[k++] = (char)r->s[r->pos];
            r->pos++;
        }
    o[k] = 0;
    r->used += k + 1;
    return o;
}

/* the rest of the line (a /F item), blanks at both ends off */
static char *ra_rest(struct ra_src *r)
{
    while (r->pos < r->n && (r->s[r->pos] == ' ' || r->s[r->pos] == '\t'))
        r->pos++;
    u32 e = r->pos;
    while (e < r->n && r->s[e] != '\n' && r->s[e])
        e++;
    u32 b = r->pos;
    r->pos = e;
    while (e > b && (r->s[e - 1] == ' ' || r->s[e - 1] == '\t'))
        e--;
    if (e > b && r->s[b] == '"' && r->s[e - 1] == '"' && e - b >= 2)
        b++, e--;
    char *o = r->out + r->used;
    u32 k = e - b < r->cap - r->used - 1 ? e - b : r->cap - r->used - 1;
    zmemcpy(o, r->s + b, k);
    o[k] = 0;
    r->used += k + 1;
    return o;
}

static int ra_num(const char *s, s32 *v)
{
    int neg = *s == '-';
    if (*s == '-' || *s == '+')
        s++;
    if (!*s)
        return 0;
    u32 x = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9')
            return 0;
        x = x * 10 + (u32)(*s - '0');
    }
    *v = neg ? -(s32)x : (s32)x;
    return 1;
}

static int ra_eq(const char *a, const char *b)
{
    while (*a && up1((u8)*a) == up1((u8)*b))
        a++, b++;
    return !*a && !*b;
}

#define RDA_DAList 12
#define RDA_Flags 28
#define RA_OURS 0x80000000u             /* in the block's first longword: ReadArgs made the RDArgs itself */

u32 c_ReadArgs(const char *tmpl, u32 *array, u8 *rda)
{
    struct ra_item it[RA_MAX];
    u32 n;
    if (!tmpl || !array || ra_template(tmpl, it, &n)) {
        Z.ioerr = 114;
        return 0;
    }
    const u8 *src;
    u32 slen, spos = 0;
    if (rda && *(u32 *)rda) {           /* RDA_Source: CS_Buffer, CS_Length, CS_CurChr */
        src = *(const u8 **)rda;
        slen = *(u32 *)(rda + 4);
        spos = *(u32 *)(rda + 8);
        if (spos > slen)
            spos = slen;
    } else {
        src = (const u8 *)TASKP(ZO_Process_pr_Arguments);
        slen = len0((const char *)src);
    }
    /* the block: a header (whether the RDArgs is ours, the block's size), the strings (at most the line's bytes
     * twice, every string 0-ended), the numbers and the lists' pointers */
    u32 cap = (slen - spos) * 2 + 2 * RA_MAX + 16;
    u32 lists = ((slen - spos) / 2 + 2) * 4 + 4 * RA_MAX;
    u32 total = 8 + cap + 4 * RA_MAX + lists + 4;
    u8 *blk = (u8 *)c_AllocVec(total, MEMF_PUBLIC | MEMF_CLEAR);
    int ours = !rda;
    if (ours)
        rda = (u8 *)c_AllocVec(32, MEMF_PUBLIC | MEMF_CLEAR);
    if (!blk || !rda) {
        if (blk)
            c_FreeVec((u32)blk);
        if (ours && rda)
            c_FreeVec((u32)rda);
        Z.ioerr = 103;
        return 0;
    }
    *(u32 *)blk = ours ? RA_OURS : 0;
    struct ra_src r = { src, slen, spos, (char *)blk + 8, 0, cap };
    s32 *nums = (s32 *)(blk + 8 + ((cap + 3) & ~3u));
    u32 *lst = (u32 *)((u8 *)nums + 4 * RA_MAX), nl = 0;
    u32 err = 0, mitem = n;             /* the /M item that takes positional arguments, if any */
    u32 pos[RA_MAX], npos = 0;          /* the positional values in order (string pointers) */
    for (;;) {
        int q;
        u32 save = r.pos;
        char *w = ra_next(&r, &q, 1);
        if (!w)
            break;
        u32 k = n;
        if (!q)
            for (u32 i = 0; i < n; i++)
                if (ra_is(&it[i], w, len0(w))) {
                    k = i;
                    break;
                }
        if (k < n) {                    /* a keyword */
            struct ra_item *x = &it[k];
            if (x->f & RA_S) {
                x->set = 1, x->val = DOSTRUE;
                continue;
            }
            if (x->f & RA_F) {
                x->set = 1, x->val = (u32)ra_rest(&r);
                continue;
            }
            char *v = ra_next(&r, &q, 0);
            if (!v) {
                err = 114;
                break;
            }
            if (x->f & RA_T) {
                x->set = 1;
                x->val = ra_eq(v, "YES") || ra_eq(v, "ON") ? DOSTRUE : 0;
                continue;
            }
            if (x->f & RA_M) {          /* a keyword /M: this one and what follows go to it */
                if (!x->set)
                    x->first = nl, x->set = 1;
                if (nl < lists / 4 - 1)
                    lst[nl++] = (u32)v, x->nm++;
                continue;
            }
            x->set = 1, x->val = (u32)v;
            continue;
        }
        (void)save;
        if (npos == RA_MAX) {
            err = 118;
            break;
        }
        pos[npos++] = (u32)w;
    }
    /* the positional values: to the items that take them (not /S, /T, /K), in order; a /M item takes the rest,
     * minus what the /A items after it need */
    u32 p = 0;
    for (u32 i = 0; i < n && !err; i++) {
        struct ra_item *x = &it[i];
        if (x->set || (x->f & (RA_S | RA_T | RA_K)))
            continue;
        if (x->f & RA_F) {              /* the rest of the line was taken word by word: joined by one blank */
            if (p < npos) {
                char *o = r.out + r.used, *d = o;
                for (; p < npos; p++) {
                    const char *s = (const char *)pos[p];
                    while (*s && r.used + (u32)(d - o) + 2 < r.cap)
                        *d++ = *s++;
                    if (p + 1 < npos)
                        *d++ = ' ';
                }
                *d = 0;
                r.used += (u32)(d - o) + 1;
                x->set = 1, x->val = (u32)o;
            }
            continue;
        }
        if (x->f & RA_M) {
            u32 need = 0;
            for (u32 j = i + 1; j < n; j++)
                if ((it[j].f & RA_A) && !it[j].set && !(it[j].f & (RA_S | RA_T | RA_K)))
                    need++;
            if (!x->set)
                x->first = nl, x->set = 1;
            while (p < npos && npos - p > need && nl < lists / 4 - 1)
                lst[nl++] = pos[p++], x->nm++;
            mitem = i;
            continue;
        }
        if (p < npos)
            x->set = 1, x->val = pos[p++];
    }
    if (!err && p < npos)
        err = 118;
    (void)mitem;
    for (u32 i = 0; i < n && !err; i++) {
        struct ra_item *x = &it[i];
        if ((x->f & RA_A) && (!x->set || ((x->f & RA_M) && !x->nm)))
            err = 116;
        if (!err && x->set && (x->f & RA_N) && !(x->f & RA_M)) {
            if (!ra_num((const char *)x->val, &nums[i]))
                err = 115;
            else
                x->val = (u32)&nums[i];
        }
    }
    if (err) {
        c_FreeVec((u32)blk);
        if (ours)
            c_FreeVec((u32)rda);
        Z.ioerr = err;
        return 0;
    }
    /* the /M lists: each its own NULL-ended run (a list is built in place: the items' entries are consecutive) */
    u32 *ml = lst + nl + 1;             /* after the shared run, a 0 between */
    for (u32 i = 0; i < n; i++) {
        struct ra_item *x = &it[i];
        if (!x->set)
            continue;
        if (x->f & RA_M) {
            u32 *start = ml;
            for (u32 j = 0; j < x->nm && (u8 *)(ml + 1) <= blk + total - 4; j++)
                *ml++ = lst[x->first + j];
            *ml++ = 0;
            array[i] = (u32)start;
        } else
            array[i] = x->val;
    }
    *(u32 *)(rda + RDA_DAList) = (u32)blk;
    if (rda && !ours)                   /* CS_CurChr: where the parse stopped */
        if (*(u32 *)rda)
            *(u32 *)(rda + 8) = r.pos;
    Z.ioerr = 0;
    return (u32)rda;
}

void c_FreeArgs(u8 *rda)
{
    if (!rda)
        return;
    u8 *blk = *(u8 **)(rda + RDA_DAList);
    *(u32 *)(rda + RDA_DAList) = 0;
    int ours = blk && (*(u32 *)blk & RA_OURS);
    if (blk)
        c_FreeVec((u32)blk);
    if (ours)
        c_FreeVec((u32)rda);
}

/* ---- graphics: the ROM font and text ----
 * topaz 8, ZINE's stand-in for the ROM's (its glyphs are Zenix's own console font's, user/zine/zfont.h: the ROM's
 * topaz is Kickstart's and not ZINE's to copy). A TextFont (graphics/text.h, amigaofs.h's offsets) whose CharData is
 * the glyphs side by side, one byte each, ZF_N of them: ZF_LO to ZF_HI and the box after them for every other
 * character, as a ROM font draws a character it lacks. OpenFont finds it by name (any size: the nearest ZINE has);
 * every other name answers 0, said once, as AmigaOS answers for a font not in memory (diskfont.library is not ZINE's).
 * Text draws with the CPU into the RastPort's BitMap: FgPen and BgPen, JAM1, JAM2, COMPLEMENT and INVERSVID, the
 * RastPort's Mask, clipped to the BitMap (no layers: ZINE has none). */
#define RP_BitMap 4
#define RP_Mask 24
#define RP_FgPen 25
#define RP_BgPen 26
#define RP_DrawMode 28
#define RP_cp_x 36
#define RP_cp_y 38
#define RP_Font 52
#define RP_TxHeight 58
#define RP_TxWidth 60
#define RP_TxBaseline 62
#define RP_TxSpacing 64
#define TF_(f) ZO_TextFont_##f
static u8 *topaz;                       /* the TextFont (in the ZINE block) */
static const char topaz_name[] = "topaz.font";

void zb_font_init(u32 (*take)(u32))
{
    u8 *tf = (u8 *)take(ZO_TextFont_SIZEOF + ZF_N * 4 + ZF_ROWS * ZF_N);
    u32 *loc = (u32 *)(tf + ZO_TextFont_SIZEOF);
    u8 *data = (u8 *)(loc + ZF_N);
    for (u32 i = 0; i < ZF_N; i++)
        loc[i] = (i * 8) << 16 | 8;
    for (u32 r = 0; r < ZF_ROWS; r++)
        for (u32 i = 0; i < ZF_N; i++)
            data[r * ZF_N + i] = zfont[i][r];
    tf[8] = 12;                         /* ln_Type NT_FONT */
    *(const char **)(tf + 10) = topaz_name;
    *(u16 *)(tf + 18) = ZO_TextFont_SIZEOF;  /* mn_Length */
    *(u16 *)(tf + TF_(tf_YSize)) = ZF_ROWS;
    tf[TF_(tf_Style)] = 0;
    tf[TF_(tf_Style) + 1] = 0x01 | 0x40;    /* tf_Flags: FPF_ROMFONT, FPF_DESIGNED */
    *(u16 *)(tf + TF_(tf_XSize)) = 8;
    *(u16 *)(tf + TF_(tf_Baseline)) = ZF_BASE;
    *(u16 *)(tf + TF_(tf_BoldSmear)) = 1;
    tf[TF_(tf_LoChar)] = ZF_LO;
    tf[TF_(tf_HiChar)] = ZF_HI;
    *(u32 *)(tf + TF_(tf_CharData)) = (u32)data;
    *(u16 *)(tf + TF_(tf_Modulo)) = ZF_N;
    *(u32 *)(tf + TF_(tf_CharLoc)) = (u32)loc;
    topaz = tf;
    u32 gb = Z.lib[1];
    *(u32 *)(gb + ZO_GfxBase_DefaultFont) = (u32)tf;
    struct zlist *fl = (struct zlist *)(gb + ZO_GfxBase_TextFonts);
    fl->head = (struct znode *)&fl->tail;
    fl->tail = 0;
    fl->tailpred = (struct znode *)&fl->head;
    fl->type = 12;
    c_AddTail(fl, (struct znode *)tf);
}

static int name_eq(const char *a, const char *b)
{
    while (*a && up1((u8)*a) == up1((u8)*b))
        a++, b++;
    return !*a && !*b;
}

u32 c_OpenFont(const u8 *ta)
{
    static u8 said;
    const char *n = ta ? *(const char *const *)(ta + ZO_TextAttr_ta_Name) : 0;
    if (!n || !c_TypeOfMem((u32)n))
        return 0;
    if (name_eq(n, topaz_name)) {
        (*(u16 *)(topaz + TF_(tf_Accessors)))++;
        return (u32)topaz;
    }
    if (said < 4) {
        said++;
        zb_say_text("ZINE: no font \"", (u32)n, "\" (ZINE has topaz 8 only; fonts on disk need diskfont.library)\n");
    }
    return 0;
}

void c_CloseFont(u8 *tf)
{
    if (tf && *(u16 *)(tf + TF_(tf_Accessors)))
        (*(u16 *)(tf + TF_(tf_Accessors)))--;
}

void c_SetFont(u8 *rp, u8 *tf)
{
    if (!rp || !tf)
        return;
    *(u32 *)(rp + RP_Font) = (u32)tf;
    *(u16 *)(rp + RP_TxHeight) = *(u16 *)(tf + TF_(tf_YSize));
    *(u16 *)(rp + RP_TxWidth) = *(u16 *)(tf + TF_(tf_XSize));
    *(u16 *)(rp + RP_TxBaseline) = *(u16 *)(tf + TF_(tf_Baseline));
    *(u16 *)(rp + RP_TxSpacing) = 0;
}

void zb_rp_font(u8 *rp) { if (topaz) c_SetFont(rp, topaz); }

/* AskFont(rp a1, textAttr a0) (-474): the RastPort's font as a TextAttr */
void c_AskFont(u8 *rp, u8 *ta)
{
    u8 *tf = rp ? *(u8 **)(rp + RP_Font) : 0;
    if (!ta || !tf)
        return;
    *(u32 *)(ta + ZO_TextAttr_ta_Name) = *(u32 *)(tf + 10);
    *(u16 *)(ta + ZO_TextAttr_ta_YSize) = *(u16 *)(tf + TF_(tf_YSize));
    ta[ZO_TextAttr_ta_Style] = tf[TF_(tf_Style)];
    ta[ZO_TextAttr_ta_Flags] = tf[TF_(tf_Style) + 1];
}

/* TextLength(rp a1, string a0, count d0) (-54): the font is fixed-width */
u32 c_TextLength(u8 *rp, const char *s, u32 n)
{
    (void)s;
    u8 *tf = rp ? *(u8 **)(rp + RP_Font) : 0;
    return n * (tf ? *(u16 *)(tf + TF_(tf_XSize)) : 8u);
}

void c_Move(u8 *rp, u32 x, u32 y)
{
    if (rp)
        *(s16 *)(rp + RP_cp_x) = (s16)x, *(s16 *)(rp + RP_cp_y) = (s16)y;
}
void c_SetAPen(u8 *rp, u32 p) { if (rp) rp[RP_FgPen] = (u8)p; }
void c_SetBPen(u8 *rp, u32 p) { if (rp) rp[RP_BgPen] = (u8)p; }
void c_SetDrMd(u8 *rp, u32 m) { if (rp) rp[RP_DrawMode] = (u8)m; }

/* one byte-aligned or not run of 8 pixels into every plane: bits the glyph row's (set: FgPen), the others BgPen
 * (JAM2), left alone (JAM1), or every set pixel complemented (COMPLEMENT); INVERSVID swaps set and clear first */
#define JAM2 1
#define COMPLEMENT 2
#define INVERSVID 4
static void put8(u8 *bm, s32 x, s32 y, u32 bits, u32 nbits, u8 fg, u8 bg, u8 mode, u8 mask)
{
    u32 bpr = *(u16 *)(bm + ZO_BitMap_BytesPerRow), rows = *(u16 *)(bm + ZO_BitMap_Rows);
    u32 depth = bm[ZO_BitMap_Depth];
    if (y < 0 || (u32)y >= rows)
        return;
    if (mode & INVERSVID)
        bits = ~bits & ((1u << nbits) - 1) << (8 - nbits);
    for (u32 px = 0; px < nbits; px++) {
        s32 xx = x + (s32)px;
        if (xx < 0 || (u32)xx >= bpr * 8)
            continue;
        int on = (bits >> (7 - px)) & 1;
        if (!on && !(mode & JAM2))
            continue;
        u32 o = (u32)y * bpr + (u32)xx / 8;
        u8 m = (u8)(0x80 >> (xx & 7));
        for (u32 pl = 0; pl < depth && pl < 8; pl++) {
            if (!(mask & (1u << pl)))
                continue;
            u8 *p = *(u8 **)(bm + ZO_BitMap_Planes + 4 * pl);
            if (!p || p == (u8 *)-1)
                continue;
            if (mode & COMPLEMENT) {
                if (on)
                    p[o] ^= m;
                continue;
            }
            u8 col = on ? fg : bg;
            if (col & (1u << pl))
                p[o] |= m;
            else
                p[o] &= (u8)~m;
        }
    }
}

u32 c_Text(u8 *rp, const u8 *s, u32 n)
{
    if (!rp || !s)
        return 0;
    u8 *bm = *(u8 **)(rp + RP_BitMap);
    u8 *tf = *(u8 **)(rp + RP_Font) ? *(u8 **)(rp + RP_Font) : topaz;
    if (!tf)
        return 0;
    s32 x = *(s16 *)(rp + RP_cp_x), y = *(s16 *)(rp + RP_cp_y) - *(s16 *)(tf + TF_(tf_Baseline));
    u32 h = *(u16 *)(tf + TF_(tf_YSize)), mod = *(u16 *)(tf + TF_(tf_Modulo)), xs = *(u16 *)(tf + TF_(tf_XSize));
    const u8 *cd = *(const u8 **)(tf + TF_(tf_CharData));
    const u32 *loc = *(const u32 **)(tf + TF_(tf_CharLoc));
    u8 lo = tf[TF_(tf_LoChar)], hi = tf[TF_(tf_HiChar)];
    for (u32 i = 0; i < n; i++, x += (s32)xs) {
        if (!bm || !cd || !loc)
            continue;
        u32 c = s[i] >= lo && s[i] <= hi ? (u32)(s[i] - lo) : (u32)(hi - lo + 1);
        u32 l = loc[c], bit = l >> 16, w = l & 0xFFFF;
        if (w > 8)
            w = 8;
        for (u32 r = 0; r < h; r++) {
            const u8 *row = cd + r * mod;
            u32 b = 0;
            for (u32 k = 0; k < w; k++)     /* the glyph's bits from the strip, any bit offset */
                b |= ((row[(bit + k) >> 3] >> (7 - ((bit + k) & 7))) & 1u) << (7 - k);
            put8(bm, x, y + (s32)r, b, w ? w : 8, rp[RP_FgPen], rp[RP_BgPen], rp[RP_DrawMode], rp[RP_Mask]);
        }
    }
    *(s16 *)(rp + RP_cp_x) = (s16)x;
    return 0;
}

/* RectFill(rp a1, xmin d0, ymin d1, xmax d2, ymax d3) (-306) and SetRast(rp a1, pen d0) (-234): FgPen's colour (SetRast
 * the pen given) into the BitMap's planes the Mask allows, clipped; COMPLEMENT flips them */
static void fill(u8 *rp, s32 x0, s32 y0, s32 x1, s32 y1, u8 pen, u8 mode)
{
    u8 *bm = rp ? *(u8 **)(rp + RP_BitMap) : 0;
    if (!bm)
        return;
    s32 w = *(u16 *)(bm + ZO_BitMap_BytesPerRow) * 8, h = *(u16 *)(bm + ZO_BitMap_Rows);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= w) x1 = w - 1;
    if (y1 >= h) y1 = h - 1;
    for (s32 y = y0; y <= y1; y++)
        for (s32 x = x0; x <= x1; x += 8) {
            s32 n = x1 - x + 1 < 8 ? x1 - x + 1 : 8;
            put8(bm, x, y, 0xFF, (u32)n, pen, pen, mode & COMPLEMENT, rp[RP_Mask]);
        }
}
void c_RectFill(u8 *rp, s32 x0, s32 y0, s32 x1, s32 y1)
{
    if (rp && x0 <= x1 && y0 <= y1)
        fill(rp, x0, y0, x1, y1, rp[RP_FgPen], rp[RP_DrawMode]);
}
void c_SetRast(u8 *rp, u32 pen)
{
    u8 *bm = rp ? *(u8 **)(rp + RP_BitMap) : 0;
    if (!bm)
        return;
    u32 bpr = *(u16 *)(bm + ZO_BitMap_BytesPerRow), rows = *(u16 *)(bm + ZO_BitMap_Rows);
    for (u32 pl = 0; pl < bm[ZO_BitMap_Depth] && pl < 8; pl++) {
        u8 *p = *(u8 **)(bm + ZO_BitMap_Planes + 4 * pl);
        if (p && p != (u8 *)-1)
            zmemset(p, pen & (1u << pl) ? 0xFF : 0, bpr * rows);
    }
}

/* SetChipRev(want d0) (-888, V39): the chips are what they are; the answer the revision bits they have
 * (gb_ChipRevBits0) */
u32 c_SetChipRev(u32 want)
{
    (void)want;
    return *(u8 *)(Z.lib[1] + ZO_GfxBase_ChipRevBits0);
}

/* GetBitMapAttr(bm a0, attr d1) (-960, V39; graphics/gfx.h: BMA_HEIGHT 0, BMA_DEPTH 4, BMA_WIDTH 8, BMA_FLAGS 12;
 * BMF_DISPLAYABLE 2, BMF_INTERLEAVED 4, BMF_STANDARD 8). Interleaved: the planes one plane row apart. */
u32 c_GetBitMapAttr(const u8 *bm, u32 attr)
{
    if (!bm)
        return 0;
    u32 bpr = *(const u16 *)(bm + ZO_BitMap_BytesPerRow), d = bm[ZO_BitMap_Depth];
    u32 p0 = *(const u32 *)(bm + ZO_BitMap_Planes), p1 = d > 1 ? *(const u32 *)(bm + ZO_BitMap_Planes + 4) : 0;
    int il = d > 1 && !(bpr % d) && p1 - p0 == bpr / d;
    switch (attr) {
    case 0:
        return *(const u16 *)(bm + ZO_BitMap_Rows);
    case 4:
        return d;
    case 8:
        return (il ? bpr / d : bpr) * 8;
    case 12:
        return 8 | (c_TypeOfMem(p0) & MEMF_CHIP ? 2u : 0) | (il ? 4u : 0);
    }
    return 0;
}

/* BestModeIDA(tags a0) (-1050, V39; graphics/modeid.h): the native mode for the size and depth asked: the machine's
 * PAL or NTSC monitor (or the one asked), HIRES from 640 wide, LACE when taller than a frame, HAM or EHB when
 * DIPF_IS_HAM or DIPF_IS_EXTRAHALFBRITE must be had; INVALID_ID for more planes than the chips show */
#define BIDTAG(n) (0x80000000u + (n))
#define DIPF_IS_LACE 0x00000001u
#define DIPF_IS_HAM 0x00000008u
#define DIPF_IS_EXTRAHALFBRITE 0x00001000u
u32 c_BestModeIDA(const u32 *t)
{
    u32 w = 320, h = Z.pal ? 256 : 200, depth = 1, must = 0, mon = Z.pal ? 0x00021000u : 0x00011000u, src = ~0u;
    for (; t && t[0]; t += 2) {
        if (t[0] == 2) {                /* TAG_MORE */
            if (!t[1])
                break;
            t = (const u32 *)t[1] - 2;
            continue;
        }
        if (t[0] == 1)                  /* TAG_IGNORE */
            continue;
        if (t[0] == 3) {                /* TAG_SKIP */
            t += 2 * t[1];
            continue;
        }
        switch (t[0]) {
        case BIDTAG(1): must = t[1]; break;
        case BIDTAG(4): case BIDTAG(6): w = t[1]; break;
        case BIDTAG(5): case BIDTAG(7): h = t[1]; break;
        case BIDTAG(8): depth = t[1]; break;
        case BIDTAG(9): mon = t[1] & 0xFFFF1000u; break;
        case BIDTAG(10): src = t[1]; break;
        case BIDTAG(3):                 /* a ViewPort: its Modes (offset 32) */
            if (t[1])
                src = *(const u16 *)(t[1] + 32);
            break;
        }
    }
    u32 id = src != ~0u ? (src & 0x8804u) : 0;
    if (w >= 640)
        id |= 0x8000;                   /* HIRES_KEY */
    if (h > (Z.pal ? 256u : 200u) || (must & DIPF_IS_LACE))
        id |= 0x0004;                   /* LACE */
    if (must & DIPF_IS_HAM)
        id |= 0x0800;
    if (must & DIPF_IS_EXTRAHALFBRITE)
        id |= 0x0080;
    u32 most = Z.aga ? 8 : (id & 0x8000) ? 4 : 6;
    if (depth > most)
        return ~0u;                     /* INVALID_ID */
    return mon | id;
}

/* ---- utility.library (the NDK 3.2's utility/tagitem.h, date.h, hooks.h; utility_lib.fd) ---- */
#define TAG_DONE 0
#define TAG_IGNORE 1
#define TAG_MORE 2
#define TAG_SKIP 3
#define TAG_USER 0x80000000u

/* NextTagItem(tagListPtr a0) (-48): the next tag that is not a system one, following TAG_MORE */
u32 c_NextTagItem(u32 **pp)
{
    if (!pp)
        return 0;
    u32 *t = *pp;
    while (t) {
        switch (t[0]) {
        case TAG_DONE:
            *pp = 0;
            return 0;
        case TAG_IGNORE:
            t += 2;
            continue;
        case TAG_MORE:
            t = (u32 *)t[1];
            continue;
        case TAG_SKIP:
            t += 2 + 2 * t[1];
            continue;
        }
        *pp = t + 2;
        return (u32)t;
    }
    *pp = 0;
    return 0;
}
u32 c_FindTagItem(u32 tag, u32 *list)
{
    u32 *t;
    while ((t = (u32 *)c_NextTagItem(&list)))
        if (t[0] == tag)
            return (u32)t;
    return 0;
}
u32 c_GetTagData(u32 tag, u32 def, u32 *list)
{
    u32 *t = (u32 *)c_FindTagItem(tag, list);
    return t ? t[1] : def;
}
u32 c_TagInArray(u32 tag, const u32 *a)
{
    for (; a && *a != TAG_DONE; a++)
        if (*a == tag)
            return 1;
    return 0;
}
/* PackBoolTags(flags d0, tags a0, boolmap a1) (-42): each boolmap tag found in tags sets (data true) or clears its bits */
u32 c_PackBoolTags(u32 flags, u32 *tags, u32 *map)
{
    u32 *t;
    while ((t = (u32 *)c_NextTagItem(&tags))) {
        u32 *m = (u32 *)c_FindTagItem(t[0], map);
        if (m)
            flags = t[1] ? flags | m[1] : flags & ~m[1];
    }
    return flags;
}
u32 c_AllocateTagItems(u32 n) { return n ? c_AllocVec(n * 8, MEMF_PUBLIC | MEMF_CLEAR) : 0; }
void c_FreeTagItems(u32 p) { if (p) c_FreeVec(p); }
u32 c_CloneTagItems(u32 *list)
{
    u32 n = 0, *t, *l = list;
    while (c_NextTagItem(&l))
        n++;
    u32 *c = (u32 *)c_AllocateTagItems(n + 1);
    if (!c)
        return 0;
    u32 *d = c;
    while ((t = (u32 *)c_NextTagItem(&list)))
        d[0] = t[0], d[1] = t[1], d += 2;
    d[0] = TAG_DONE;
    return (u32)c;
}

/* the 32- and 64-bit multiplies (the divides are in blobs.S: two answers) */
typedef unsigned long long u64;
u64 c_UMult64(u32 a, u32 b) { return (u64)a * b; }
u64 c_SMult64(u32 a, u32 b) { return (u64)((long long)(s32)a * (s32)b); }

u32 c_Stricmp(const u8 *a, const u8 *b)
{
    for (;; a++, b++) {
        s32 d = (s32)low1(*a) - (s32)low1(*b);
        if (d || !*a)
            return (u32)(d < 0 ? -1 : d > 0);
    }
}
u32 c_Strnicmp(const u8 *a, const u8 *b, s32 n)
{
    for (; n > 0; a++, b++, n--) {
        s32 d = (s32)low1(*a) - (s32)low1(*b);
        if (d || !*a)
            return (u32)(d < 0 ? -1 : d > 0);
    }
    return 0;
}
u32 c_ToUpper(u32 c) { return up1(c); }
u32 c_ToLower(u32 c) { return low1(c); }

/* Amiga2Date(seconds d0, ClockData a0) (-120), Date2Amiga(a0) (-126), CheckDate(a0) (-132): seconds since 1978-01-01
 * (a Sunday); ClockData sec, min, hour, mday, month (1-12), year, wday (0 Sunday) as UWORDs */
static int leap(u32 y) { return (y % 4 == 0 && y % 100) || y % 400 == 0; }
static u32 mdays(u32 y, u32 m)
{
    static const u8 d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return d[m - 1] + (m == 2 && leap(y));
}
void c_Amiga2Date(u32 s, u16 *cd)
{
    u32 days = s / 86400, r = s % 86400;
    cd[0] = (u16)(r % 60), cd[1] = (u16)(r / 60 % 60), cd[2] = (u16)(r / 3600);
    cd[6] = (u16)(days % 7);
    u32 y = 1978;
    while (days >= 365u + leap(y))
        days -= 365u + leap(y), y++;
    u32 m = 1;
    while (days >= mdays(y, m))
        days -= mdays(y, m), m++;
    cd[3] = (u16)(days + 1), cd[4] = (u16)m, cd[5] = (u16)y;
}
u32 c_CheckDate(const u16 *cd)
{
    if (!cd || cd[0] > 59 || cd[1] > 59 || cd[2] > 23 || cd[4] < 1 || cd[4] > 12 || cd[5] < 1978 || cd[5] > 2113 ||
        cd[3] < 1 || cd[3] > mdays(cd[5], cd[4]))
        return 0;
    u32 days = 0;
    for (u32 y = 1978; y < cd[5]; y++)
        days += 365u + leap(y);
    for (u32 m = 1; m < cd[4]; m++)
        days += mdays(cd[5], m);
    days += cd[3] - 1u;
    return days * 86400u + cd[2] * 3600u + cd[1] * 60u + cd[0];
}
u32 c_Date2Amiga(const u16 *cd) { return c_CheckDate(cd); }
u32 c_GetUniqueID(void)
{
    static u32 id;
    return ++id;
}

/* ---- the libraries beyond the four (utility, the math ones in bmath.c): built at the start in the ZINE block,
 * found by OpenLibrary by name (zb_more_open), named in zine's stop line by their number (kernel/zine.h's lib) ---- */
#define F(x) extern void zs_##x(void)
F(FindTagItem); F(GetTagData); F(PackBoolTags); F(NextTagItem); F(AllocateTagItems); F(CloneTagItems);
F(FreeTagItems); F(TagInArray); F(CallHookPkt); F(Amiga2Date); F(Date2Amiga); F(CheckDate); F(SMult32); F(UMult32);
F(SDivMod32); F(UDivMod32); F(Stricmp); F(Strnicmp); F(ToUpper); F(ToLower); F(SMult64); F(UMult64);
F(GetUniqueID); F(rts);
static const u16 ut_lvo[] = { 30, 36, 42, 48, 66, 72, 78, 90, 102, 120, 126, 132, 138, 144, 150, 156, 162, 168, 174,
                              180, 198, 204, 270, 6, 12, 18, 24 };
static void (*const ut_fn[])(void) = { zs_FindTagItem, zs_GetTagData, zs_PackBoolTags, zs_NextTagItem,
    zs_AllocateTagItems, zs_CloneTagItems, zs_FreeTagItems, zs_TagInArray, zs_CallHookPkt, zs_Amiga2Date,
    zs_Date2Amiga, zs_CheckDate, zs_SMult32, zs_UMult32, zs_SDivMod32, zs_UDivMod32, zs_Stricmp, zs_Strnicmp,
    zs_ToUpper, zs_ToLower, zs_SMult64, zs_UMult64, zs_GetUniqueID, zs_rts, zs_rts, zs_rts, zs_rts };
_Static_assert(sizeof ut_lvo / 2 == sizeof ut_fn / sizeof ut_fn[0], "utility table");

/* the math libraries' tables (bmath.c) */
struct zb_mlib { const char *name; u16 slots; const u16 *lvo; void (*const *fn)(void); u16 n; };
extern const struct zb_mlib zb_mlibs[6];

u32 zb_xlib[ZB_NXLIB];                  /* the bases, by number - 4 (kernel/zine.h ZINE_LIB_ numbers) */
static const char *const xname[ZB_NXLIB] = { "utility.library", "mathffp.library", "mathtrans.library",
    "mathieeedoubbas.library", "mathieeedoubtrans.library", "mathieeesingbas.library", "mathieeesingtrans.library" };

static u32 xlib(u32 (*take)(u32), const char *name, u16 slots, const u16 *lvo, void (*const *fn)(void), u16 n)
{
    static const char id[] = "ZINE 40.1 (Zenix's small AmigaOS)\r\n";
    u32 base = take(slots * 6u + 64) + slots * 6u;
    zb_jumptable(base, slots, lvo, fn, n);
    struct zlib *l = (struct zlib *)base;
    l->node.type = NT_LIBRARY;
    l->node.name = (char *)name;
    l->negsize = (u16)(slots * 6);
    l->possize = 64;
    l->version = 40;
    l->revision = 1;
    l->idstring = (char *)id;
    return base;
}

void zb_more_init(u32 (*take)(u32))
{
    zb_xlib[0] = xlib(take, xname[0], 45, ut_lvo, ut_fn, sizeof ut_lvo / 2);
    for (int i = 0; i < 6; i++)
        zb_xlib[1 + i] = xlib(take, xname[1 + i], zb_mlibs[i].slots, zb_mlibs[i].lvo, zb_mlibs[i].fn, zb_mlibs[i].n);
    zb_font_init(take);
}

u32 zb_more_open(const char *name, u32 version)
{
    for (int i = 0; i < ZB_NXLIB; i++)
        if (name_eq(xname[i], name) && zb_xlib[i]) {
            struct zlib *l = (struct zlib *)zb_xlib[i];
            if (version > l->version)
                return 0;
            l->opencnt++;
            return (u32)l;
        }
    return 0;
}

int zb_more_close(u32 lib)
{
    for (int i = 0; i < ZB_NXLIB; i++)
        if (lib && lib == zb_xlib[i]) {
            struct zlib *l = (struct zlib *)lib;
            if (l->opencnt)
                l->opencnt--;
            return 1;
        }
    return 0;
}
