/* SPDX-License-Identifier: GPL-2.0-only */
/* zas: Zenix's assembler (DESIGN D52 section 6, docs/d52-self-hosting.md; docs/man/zas.txt). It reads 68k assembly
 * in GNU as's syntax (the m68k "MIT" flavour gcc writes and the repository's .S files use: %-prefixed registers,
 * move.l and movel, (d,%an,%xn.l*4) and %an@(d,%xn:l:4)) and writes an ELF32 m68k relocatable object that zld (and
 * GNU ld) link. Instructions of the 68000-68060, the 68881/68882 and 68040/68060 FPU, the 68030/68040/68060 MMU and
 * cache instructions; branch and operand relaxation as gas does it; sections, symbols, local labels, expressions,
 * macros, .rept/.irp, .if, .include. The object is the one GNU as 2.45 writes, byte for byte, for the corpus the
 * build feeds it (tools/zas-host-test.sh): gas's choices (which instruction form, which relocation against which
 * symbol, the order of symbols, relocations and strings) are copied rule for rule and named where they are coded.
 * Debugging information is not made: .loc, .cfi_* and the .debug_* sections are read and dropped.
 *
 * How it works: the source is read whole and assembled in passes. Every pass walks all statements and lays out the
 * sections; an instruction whose size depends on a distance (a branch, a PC-relative operand) keeps its size in a
 * table that only ever grows, so the passes settle. The last pass writes the bytes and the relocations. Standard C
 * and POSIX only: the same file builds for the PC (the host test) and for Zenix (a C library program). No floating
 * point (a ZEX2 program cannot use libgcc's soft float): .double and #0r constants are converted with integers. */
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t i32;
typedef int64_t i64;
typedef uint64_t u64;

/* ---- messages (without printf: its size) ---- */
static char obuf[256];
static int olen, nerr, final, pass;
static const char *srcname = "-", *outname = "a.out";
static int srcline;

static void oflush(int fd)
{
    if (olen && write(fd, obuf, olen) < 0)
        olen = 0;
    olen = 0;
}
static void oc(int fd, char c)
{
    if (olen == (int)sizeof obuf)
        oflush(fd);
    obuf[olen++] = c;
}
static void os(int fd, const char *s)
{
    for (; *s; s++)
        oc(fd, (u8)*s < 32 && *s != '\t' ? '?' : *s);
}
static void onum(int fd, u32 v, int base)
{
    char t[12];
    int n = 0;
    do
        t[n++] = "0123456789abcdef"[v % base];
    while ((v /= base) != 0);
    while (n)
        oc(fd, t[--n]);
}
/* %s, %d, %x (0x...) */
static void vsay(int fd, const char *f, va_list ap)
{
    for (; *f; f++) {
        if (*f != '%') {
            oc(fd, *f);
            continue;
        }
        f++;
        if (*f == 's') {
            const char *s = va_arg(ap, const char *);
            os(fd, s ? s : "?");
        } else if (*f == 'd') {
            i32 v = va_arg(ap, i32);
            if (v < 0)
                oc(fd, '-');
            onum(fd, v < 0 ? 0u - (u32)v : (u32)v, 10);
        } else {
            oc(fd, '0');
            oc(fd, 'x');
            onum(fd, va_arg(ap, u32), 16);
        }
    }
}
static void where(void)
{
    os(2, "zas: ");
    if (srcline) {
        os(2, srcname);
        oc(2, ':');
        onum(2, srcline, 10);
        os(2, ": ");
    }
}
/* an error in the source: reported in the last pass only (the earlier ones may not know every symbol yet) */
static void err(const char *f, ...)
{
    va_list ap;
    if (!final)
        return;
    nerr++;
    where();
    va_start(ap, f);
    vsay(2, f, ap);
    va_end(ap);
    oc(2, '\n');
    oflush(2);
}
static void warn(const char *f, ...)
{
    va_list ap;
    if (!final)
        return;
    where();
    os(2, "warning: ");
    va_start(ap, f);
    vsay(2, f, ap);
    va_end(ap);
    oc(2, '\n');
    oflush(2);
}
static int started;      /* assembling: a failure leaves no object */
static void die(const char *f, ...)
{
    va_list ap;
    if (started)
        unlink(outname);
    where();
    va_start(ap, f);
    vsay(2, f, ap);
    va_end(ap);
    oc(2, '\n');
    oflush(2);
    exit(1);
}
static void *xalloc(size_t n)
{
    void *p = calloc(1, n ? n : 1);
    if (!p)
        die("out of memory");
    return p;
}
static void *xgrow(void *p, u32 *cap, u32 need, u32 esz)
{
    u32 c = *cap;
    if (need <= c)
        return p;
    c = c ? c : 16;
    while (c < need) {
        if (c > 0x3fffffff / esz)
            die("out of memory");
        c *= 2;
    }
    p = realloc(p, (size_t)c * esz);
    if (!p)
        die("out of memory");
    memset((char *)p + (size_t)*cap * esz, 0, (size_t)(c - *cap) * esz);
    *cap = c;
    return p;
}
static char *xstrndup(const char *s, int n)
{
    char *p = xalloc(n + 1);
    memcpy(p, s, n);
    return p;
}

/* ---- characters ---- */
static int isid0(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '.' || c == '$'; }
static int isdig(int c) { return c >= '0' && c <= '9'; }
static int isid(int c) { return isid0(c) || isdig(c); }
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static char *skipws(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}
static int ideq(const char *a, int n, const char *b)    /* a[0..n) == b, case-insensitive */
{
    int i;
    for (i = 0; i < n; i++)
        if (lower(a[i]) != b[i])
            return 0;
    return b[n] == 0;
}

/* ---- the CPU (gas's -m options) ---- */
#define C00 1       /* 68000 */
#define C10 2
#define C20 4
#define C30 8
#define C40 16
#define C60 32
#define CFPU 64     /* 68881/68882, or the 68040/68060's own */
#define CMMU 128    /* 68851, or the 68030/68040/68060's own */
static int cpu = C20 | CFPU | CMMU, cpu0, pcrel_opt, regopt, verbose;
#define UP20 (C20 | C30 | C40 | C60)
#define UP10 (C10 | UP20)

/* ---- sections ---- */
struct rel {
    u32 off;
    i32 add;
    struct sym *s;
    u8 type;
};
struct sect {
    char *name;
    u32 type, flags, entsize, align;
    u32 loc;            /* this pass */
    u8 *buf;            /* the last pass's contents */
    u32 cap;
    struct rel *rel;    /* the last pass's relocations: those of fixed instructions, then those of relaxed ones */
    u32 nrel, crel;
    struct rel *drel;
    u32 ndrel, cdrel;
    struct sym *ssym;
    int frag;           /* gas's frag number: grows at every relaxable instruction and alignment */
    int region;         /* gas's relaxation region: grows at every alignment */
    u32 fragloc;        /* where the current frag began */
    u32 fixseq;         /* fixed bytes put (instructions, data; not alignment padding), counted */
    int idx, ridx, nameoff, rnameoff;
    u32 off, roff;
    u8 drop;            /* .debug_*: read and dropped */
};
#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_RELA 4
#define SHT_NOTE 7
#define SHT_NOBITS 8
#define SHT_INIT_ARRAY 14
#define SHT_FINI_ARRAY 15
#define SHT_PREINIT_ARRAY 16
#define SHF_WRITE 1
#define SHF_ALLOC 2
#define SHF_EXEC 4
#define SHF_MERGE 0x10
#define SHF_STRINGS 0x20
#define SHF_INFO_LINK 0x40
#define SHF_GNU_RETAIN 0x200000
static struct sect **sects;
static u32 nsect, csect;
static struct sect *cur;
static struct sect *prevsec, *secstack[16], *prevstack[16];
static int secsp;

/* ---- symbols ---- */
#define SEC_ABS (-1)
#define SEC_UNDEF (-2)
#define F_L 1           /* .L and numbered local labels: not in the symbol table */
#define F_USED 2        /* referenced */
#define F_KNOWN 4       /* a .set whose value was a constant where it was set */
#define F_KEEP 8        /* named by a relocation (a .L symbol in a merged section, a GOT entry) */
#define F_SEC 16        /* a section's own symbol */
#define F_EQU 32        /* set by .set/= (may be set again) */
#define F_LOCAL 64      /* .local */
struct sym {
    struct sym *hnext, *next;
    char *name;
    struct sym *base;   /* .set X, Y+4 with Y undefined: X stands for Y */
    i64 val;
    u32 size;
    int sec, defpass, frag, region, idx, nameoff, sizepass, typepass;
    u32 fixseq;         /* its section's fixseq where it was defined */
    i64 fval;           /* its first value in the pass (a .set may set it again) */
    int fsec;
    u8 atstart;         /* defined where a frag begins */
    struct sym *eq;     /* .set X, Y: Y (X takes Y's type and size, as gas copies them) */
    u8 bind, type, other, flags;
};
#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STB_WEAK 2
static struct sym *htab[1024], *symfirst, *symlast;
static char *filesym;

static u32 hash(const char *s, int n)
{
    u32 h = 5381;
    while (n--)
        h = h * 33 + (u8)*s++;
    return h & 1023;
}
static struct sym *lookup(const char *s, int n, int make)
{
    u32 h = hash(s, n);
    struct sym *y;
    for (y = htab[h]; y; y = y->hnext)
        if (!strncmp(y->name, s, n) && !y->name[n])
            return y;
    if (!make)
        return 0;
    y = xalloc(sizeof *y);
    y->name = xstrndup(s, n);
    y->sec = SEC_UNDEF;
    y->hnext = htab[h];
    htab[h] = y;
    if (n >= 2 && s[0] == '.' && s[1] == 'L')
        y->flags |= F_L;
    if (symlast)
        symlast->next = y;
    else
        symfirst = y;
    symlast = y;
    return y;
}

static struct sect *newsect(const char *name, u32 type, u32 flags, u32 entsize)
{
    struct sect *s = xalloc(sizeof *s);
    s->name = xstrndup(name, strlen(name));
    s->type = type;
    s->flags = flags;
    s->entsize = entsize;
    s->align = 1;
    s->drop = !strncmp(name, ".debug", 6) || !strncmp(name, ".gnu.debuglto", 13);
    sects = xgrow(sects, &csect, nsect + 1, sizeof *sects);
    sects[nsect++] = s;
    /* the section symbol, where gas makes it: at the section's creation (gas puts it in the symbol chain then) */
    if (!s->drop) {
        struct sym *y = xalloc(sizeof *y);
        y->name = s->name;
        y->flags = F_SEC;
        y->sec = nsect - 1;
        y->type = 3;
        if (symlast)
            symlast->next = y;
        else
            symfirst = y;
        symlast = y;
        s->ssym = y;
    }
    return s;
}
static int secidx(struct sect *s)
{
    u32 i;
    for (i = 0; i < nsect; i++)
        if (sects[i] == s)
            return i;
    return 0;
}

/* ---- the output of the last pass ---- */
static int inpad, fragnext;
static void putbytes(const void *p, u32 n)
{
    struct sect *s = cur;
    if (n > 0x40000000 - s->loc || s->loc > 0x40000000)
        die("section %s larger than 1 GB", s->name);
    if (!inpad)
        s->fixseq++;
    if (final && !s->drop) {
        if (s->type == SHT_NOBITS) {
            u32 i;
            for (i = 0; i < n; i++)
                if (((const u8 *)p)[i]) {
                    err("non-zero data in %s", s->name);
                    break;
                }
        } else {
            s->buf = xgrow(s->buf, &s->cap, s->loc + n, 1);
            memcpy(s->buf + s->loc, p, n);
        }
    }
    s->loc += n;
}
static void putfill(u32 n, int c)
{
    u8 b[64];
    memset(b, c, sizeof b);
    while (n) {
        u32 k = n > sizeof b ? sizeof b : n;
        putbytes(b, k);
        n -= k;
    }
}
static void put16(u32 v)
{
    u8 b[2] = {v >> 8, v};
    putbytes(b, 2);
}
static void put32(u32 v)
{
    u8 b[4] = {v >> 24, v >> 16, v >> 8, v};
    putbytes(b, 4);
}

/* ---- relaxation: one grow-only state per relaxable item, in the order the items come. As gas does it: pass 1
 * finds the symbols; pass 2 is gas's first estimate (an item whose target is elsewhere takes its long form, the
 * others their shortest); from pass 3 on, items grow while a target is out of reach ---- */
static u8 *rst;
static u32 nrst, crst, rpos, *rloc, crloc;
static int changed;
static i64 stretch;
static int nogrow;
/* before a relaxable item: how far it moved since the last pass (gas's "stretch": a forward target in this section
 * is taken to move as much) */
static void relax_at(u32 loc)
{
    stretch = rpos < nrst ? (i64)loc - rloc[rpos] : 0;
    rloc = xgrow(rloc, &crloc, rpos + 1, sizeof *rloc);
    rloc[rpos] = loc;
}
static int relax(int need)
{
    if (nogrow) {
        nogrow = 0;
        need = 0;
    }
    if (rpos >= nrst) {
        rst = xgrow(rst, &crst, rpos + 1, 1);
        nrst = rpos + 1;
    }
    if (need > rst[rpos]) {
        if (final)
            die("internal error: a size changed in the last pass");
        rst[rpos] = need;
        changed = 1;
    }
    return rst[rpos++];
}

/* ---- expressions (gas's: 64-bit arithmetic, -1 for true) ---- */
struct val {
    i64 v;              /* the value: a number, or an offset in section sec */
    struct sym *s;      /* the one symbol added (for its relocation), or 0 */
    i64 add;            /* the value relative to s */
    int sec;            /* SEC_ABS, SEC_UNDEF (relative to the undefined symbol s), or a section */
    u8 known;           /* a constant where it stands (gas's O_constant at parse time) */
    u8 pcrel;           /* X - . : PC-relative */
    u8 got;             /* @GOT etc: 1 GOT, 2 GOTOFF?, 3 PLT */
    u8 gotw;            /* @GOT.w / @GOT.b (1 = .b, 2 = .w) */
    u8 bad;
    u8 here;            /* every label in it defined in this pass already */
    int frag;           /* the frag of its label (for differences gas can fold where they stand) */
    i64 pcoff;          /* with pcrel: the offset subtracted */
};
static char *ep;        /* the expression parser's position */

/* a symbol's value; a forward reference uses what the last pass found */
static int dbgscan;      /* reading debugging data: its references do not count as uses */
static void symval(struct val *v, struct sym *y)
{
    memset(v, 0, sizeof *v);
    if (!dbgscan)
        y->flags |= F_USED;
    if (y->base) {      /* an alias of an undefined symbol (a chain of them, followed without recursion) */
        i64 a = 0;
        int depth = 0;
        while (y->base && depth++ < 100) {
            a += y->val;
            y = y->base;
            if (!dbgscan)
                y->flags |= F_USED;
        }
        if (y->base) {
            err("aliases nested too deep");
            v->bad = 1;
            return;
        }
        symval(v, y);
        v->v += a;
        v->add += a;
        v->known = 0;
        return;
    }
    if (y->sec == SEC_ABS) {
        v->sec = SEC_ABS;
        v->v = y->val;
        v->known = y->defpass == pass && (y->flags & F_KNOWN);
        return;
    }
    v->s = y;
    v->sec = y->sec;
    v->v = y->val;
    v->add = 0;
    v->frag = y->frag;
    v->here = y->defpass == pass;
    if (y->sec == SEC_UNDEF)
        v->v = 0;
}

/* numbered local labels (1: ... 1b, 1f): instance k of label N is the symbol "\1N\2k" */
static struct sym *numlabel(const char *d, int n, int ahead, int define)
{
    char b[48];
    struct sym *c;
    i64 k;
    if (n > 20)
        n = 20;
    b[0] = 1;
    memcpy(b + 1, d, n);
    c = lookup(b, n + 1, 1);    /* its count of instances in this pass */
    c->flags |= F_L;
    if (c->defpass != pass) {
        c->defpass = pass;
        c->val = 0;
    }
    if (define)
        c->val++;
    k = c->val + (ahead ? 1 : 0);
    b[n + 1] = 2;
    n += 2;
    do
        b[n++] = '0' + k % 10;
    while ((k /= 10) != 0);
    c = lookup(b, n, 1);
    c->flags |= F_L;
    return c;
}

static int parse_num(char **s, i64 *out)
{
    char *p = *s;
    u64 v = 0;
    int base = 10, any = 0;
    if (*p == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    } else if (*p == '0' && (p[1] == 'b' || p[1] == 'B') && (p[2] == '0' || p[2] == '1')) {
        base = 2;
        p += 2;
    } else if (*p == '0')
        base = 8;
    for (;; p++) {
        int c = lower(*p), d;
        if (isdig(c))
            d = c - '0';
        else if (c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        else
            break;
        if (d >= base)
            break;
        v = v * base + d;
        any = 1;
    }
    if (!any && base != 8)
        return 0;
    *s = p;
    *out = (i64)v;
    return 1;
}

static int escape(char **s)
{
    char *p = *s;
    int c = *p++;
    if (c != '\\') {
        *s = p;
        return (u8)c;
    }
    c = *p;
    if (!c) {           /* a backslash at the line's end */
        *s = p;
        return '\\';
    }
    p++;
    switch (c) {
    case 'n': c = 10; break;
    case 't': c = 9; break;
    case 'r': c = 13; break;
    case 'b': c = 8; break;
    case 'f': c = 12; break;
    case 'v': c = 11; break;
    case 'a': c = 7; break;
    case 'e': c = 27; break;
    case 'x': case 'X':
        for (c = 0; isdig(*p) || (lower(*p) >= 'a' && lower(*p) <= 'f'); p++)
            c = (c * 16 + (isdig(*p) ? *p - '0' : lower(*p) - 'a' + 10)) & 0xff;
        break;
    default:
        if (c >= '0' && c <= '7') {
            int n = 1;
            c -= '0';
            while (n < 3 && *p >= '0' && *p <= '7') {
                c = c * 8 + *p++ - '0';
                n++;
            }
        }
    }
    *s = p;
    return c & 0xff;
}

static int isreg_at(const char *s);
static void expr_level(struct val *r, int lev);

static void setabs(struct val *r, i64 v, int known)
{
    memset(r, 0, sizeof *r);
    r->sec = SEC_ABS;
    r->v = v;
    r->known = known;
}

static int edepth;       /* the parser's nesting: a 16 KB stack, no MMU (about 300 bytes a level on the 68k) */
static void primary(struct val *r)
{
    char *p = skipws(ep);
    memset(r, 0, sizeof *r);
    r->sec = SEC_ABS;
    r->known = 1;
    if (edepth >= 24) {
        err("expression nested too deep (at most 24)");
        r->bad = 1;
        ep = p + strlen(p);
        return;
    }
    if (*p == '(') {
        ep = p + 1;
        edepth++;
        expr_level(r, 0);
        edepth--;
        p = skipws(ep);
        if (*p != ')') {
            r->bad = 1;
            ep = p;
            return;
        }
        ep = p + 1;
        return;
    }
    if (*p == '-' || *p == '~' || *p == '!' || *p == '+') {
        int op = *p;
        ep = p + 1;
        edepth++;
        primary(r);
        edepth--;
        if (op == '+')
            return;
        if (r->sec != SEC_ABS) {
            r->bad = 1;
            return;
        }
        r->v = op == '-' ? -r->v : op == '~' ? ~r->v : !r->v;
        return;
    }
    if (*p == '\'') {
        p++;
        r->v = escape(&p);
        if (*p == '\'')
            p++;
        ep = p;
        return;
    }
    if (isdig(*p)) {
        char *q = p;
        while (isdig(*q))
            q++;
        if ((*q == 'b' || *q == 'f') && !isid(q[1])) {    /* 1b, 1f */
            struct sym *y = numlabel(p, q - p, *q == 'f', 0);
            symval(r, y);
            ep = q + 1;
            return;
        }
        if (!parse_num(&p, &r->v)) {
            r->bad = 1;
            ep = p + 1;
            return;
        }
        ep = p;
        return;
    }
    if (*p == '.' && !isid(p[1])) {
        r->sec = secidx(cur);
        r->v = cur->loc;
        r->known = 0;
        r->here = 1;
        r->frag = cur->frag;
        ep = p + 1;
        return;
    }
    if (isid0(*p) && !(regopt && isreg_at(p))) {
        char *q = p;
        struct sym *y;
        while (isid(*q))
            q++;
        /* a size after a symbol (sym.w, sym.l): not part of its name (gas's m68k operand rule) */
        if (q - p > 2 && q[-2] == '.' && strchr("wWlLbB", q[-1]))
            q -= 2;
        y = lookup(p, q - p, 1);
        symval(r, y);
        if (*q == '@') {    /* sym@GOT, sym@GOT.w, sym@GOT.b */
            if (!strncmp(q + 1, "GOT", 3) && !isid(q[4]) ) {
                r->got = 1;
                q += 4;
            } else if (!strncmp(q + 1, "PLTPC", 5) && !isid(q[6])) {
                r->got = 3;
                q += 6;
            } else if (!strncmp(q + 1, "GOT.w", 5) || !strncmp(q + 1, "GOT.b", 5)) {
                r->got = 1;
                r->gotw = q[5] == 'b' ? 1 : 2;
                q += 6;
            } else
                r->bad = 1;
            r->known = 0;
        }
        ep = q;
        return;
    }
    r->bad = 1;
    ep = p;
}

/* the binary operators by level: 0 ||, 1 &&, 2 comparisons, 3 + -, 4 & ^ !, 5 * / % << >> */
static int binop(char *p, int *len)
{
    *len = 2;
    if (p[0] == '|' && p[1] == '|') return 0;
    if (p[0] == '&' && p[1] == '&') return 1;
    if ((p[0] == '=' && p[1] == '=') || (p[0] == '!' && p[1] == '=') || (p[0] == '<' && p[1] == '>') ||
        (p[0] == '<' && p[1] == '=') || (p[0] == '>' && p[1] == '='))
        return 2;
    if (p[0] == '<' && p[1] == '<') return 5;
    if (p[0] == '>' && p[1] == '>') return 5;
    *len = 1;
    if (*p == '<' || *p == '>' || (*p == '=' && p[1] != '=')) return 2;
    if (*p == '+' || *p == '-') return 3;
    if (*p == '&' || *p == '^' || *p == '!') return 4;
    if (*p == '*' || *p == '/' || *p == '%') return 5;
    return -1;
}

static void combine(struct val *a, struct val *b, char *op, int len)
{
    int c = op[0], c2 = len > 1 ? op[1] : 0;
    i64 x = a->v, y = b->v;
    if (a->bad || b->bad) {
        a->bad = 1;
        return;
    }
    if (c == '+') {
        if (a->sec == SEC_ABS) {
            int k = a->known;
            i64 v = a->v;
            *a = *b;
            a->v += v;
            a->add += v;
            a->known = k && b->known;
            return;
        }
        if (b->sec == SEC_ABS) {
            a->v += y;
            a->add += y;
            a->known = a->known && b->known;
            return;
        }
        a->bad = 1;
        return;
    }
    if (c == '-') {
        if (b->sec == SEC_ABS) {
            a->v -= y;
            a->add -= y;
            a->known = a->known && b->known;
            return;
        }
        if (a->sec >= 0 && a->sec == b->sec && !a->pcrel && !b->pcrel && !a->got && !b->got &&
            !(a->s && a->s->bind == STB_WEAK) && !(b->s && b->s->bind == STB_WEAK)) {
            int k = a->here && b->here && a->frag == b->frag;
            setabs(a, x - y, k);
            return;
        }
        if (b->sec >= 0 && b->sec == secidx(cur) && !b->pcrel && !a->pcrel && !a->got && a->sec != SEC_ABS) {
            /* X - . : a PC-relative value */
            a->pcrel = 1;
            a->pcoff = y;
            a->known = 0;
            return;
        }
        a->bad = 1;
        return;
    }
    if (a->sec != SEC_ABS || b->sec != SEC_ABS) {
        a->bad = 1;
        return;
    }
    a->known = a->known && b->known;
    switch (c) {
    case '*': x *= y; break;
    case '/':
    case '%':
        if (!y) {
            if (final)
                err("division by zero");
            x = 0;
        } else if (c == '/')
            x /= y;
        else
            x %= y;
        break;
    case '<':
        if (c2 == '<') x = y < 0 || y > 63 ? 0 : (i64)((u64)x << y);
        else if (c2 == '=') x = x <= y ? -1 : 0;
        else if (c2 == '>') x = x != y ? -1 : 0;
        else x = x < y ? -1 : 0;
        break;
    case '>':
        if (c2 == '>') x = y < 0 || y > 63 ? 0 : x >> y;
        else if (c2 == '=') x = x >= y ? -1 : 0;
        else x = x > y ? -1 : 0;
        break;
    case '=': x = x == y ? -1 : 0; break;
    case '!': x = c2 == '=' ? (x != y ? -1 : 0) : (x | ~y); break;
    case '&': x = c2 == '&' ? (x && y) : (x & y); break;
    case '^': x ^= y; break;
    case '|': x = x || y; break;
    }
    a->v = x;
}

static void expr_level(struct val *r, int lev)
{
    primary(r);
    for (;;) {
        char *p = skipws(ep);
        int len, l = binop(p, &len);
        struct val b;
        if (l < lev)
            break;
        ep = p + len;
        expr_level(&b, l + 1);
        combine(r, &b, p, len);
    }
}

/* parse an expression at *s; r->bad when it is none */
static void getexpr(char **s, struct val *r)
{
    ep = *s;
    expr_level(r, 0);
    *s = skipws(ep);
}
/* an absolute expression, known where it stands (.if, .rept, .org's counts...) */
static i64 absexpr(char **s, int *ok)
{
    struct val v;
    getexpr(s, &v);
    if (v.bad || v.sec != SEC_ABS) {
        if (ok)
            *ok = 0;
        return 0;
    }
    if (ok)
        *ok = 1;
    return v.v;
}

/* ---- registers ---- */
#define R_PC 16
#define R_ZPC 17
#define R_ZA 18         /* 18-25: za0-za7 (suppressed address registers) */
#define R_ZD 26         /* 26-33: zd0-zd7 */
#define R_CCR 34
#define R_SR 35
#define R_USP 36
#define R_FP 40         /* 40-47: fp0-fp7 */
#define R_FPCR 48
#define R_FPSR 49
#define R_FPIAR 50
#define R_CREG 56       /* control registers, in the order of creg[] */
#define R_CACHE 80      /* nc dc ic bc */
#define R_CRP 84
#define R_TT0 85
#define R_TT1 86
#define R_PSR 87
static const char *const rnames[] = {
    "sfc", "dfc", "cacr", "tc", "itt0", "itt1", "dtt0", "dtt1", "buscr", "vbr", "caar", "msp", "isp", "mmusr",
    "urp", "srp", "pcr", "iacr0", "iacr1", "dacr0", "dacr1", 0};
static const u16 creg[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 0x801, 0x802, 0x803, 0x804, 0x805, 0x806, 0x807, 0x808, 4, 5,
                           6, 7};
/* the CPUs that have each (movec) */
static const u8 cregcpu[] = {UP10, UP10, UP20, C40 | C60, C40 | C60, C40 | C60, C40 | C60, C40 | C60, C60, UP10,
                             C20 | C30, C20 | C30 | C40, C20 | C30 | C40, C40, C40 | C60, C40 | C60, C60, C40 | C60,
                             C40 | C60, C40 | C60, C40 | C60};
#define CR_TC (R_CREG + 3)
#define CR_MMUSR (R_CREG + 13)
#define CR_SRP (R_CREG + 15)
#define CR_SFC R_CREG
#define CR_DFC (R_CREG + 1)

/* the register named at s ("%d0", or "d0" with --register-prefix-optional): its number + 1 and its length, or 0 */
static int regat(const char *s, int *len)
{
    const char *p = s, *q;
    int n, i;
    char b[8];
    if (*p == '%')
        p++;
    else if (!regopt)
        return 0;
    for (q = p; isid(*q) && *q != '.' && *q != '$'; q++)
        ;
    n = q - p;
    if (n < 2 || n > 6)
        return 0;
    for (i = 0; i < n; i++)
        b[i] = lower(p[i]);
    b[n] = 0;
    *len = q - s;
    if (n == 2 && (b[0] == 'd' || b[0] == 'a') && b[1] >= '0' && b[1] <= '7')
        return (b[0] == 'a' ? 8 : 0) + b[1] - '0' + 1;
    if (n == 3 && b[0] == 'z' && (b[1] == 'a' || b[1] == 'd') && b[2] >= '0' && b[2] <= '7')
        return (b[1] == 'a' ? R_ZA : R_ZD) + b[2] - '0' + 1;
    if (n == 3 && b[0] == 'f' && b[1] == 'p' && b[2] >= '0' && b[2] <= '7')
        return R_FP + b[2] - '0' + 1;
    if (!strcmp(b, "sp")) return 15 + 1;
    if (!strcmp(b, "fp")) return 14 + 1;
    if (!strcmp(b, "pc")) return R_PC + 1;
    if (!strcmp(b, "zpc")) return R_ZPC + 1;
    if (!strcmp(b, "ccr")) return R_CCR + 1;
    if (!strcmp(b, "sr")) return R_SR + 1;
    if (!strcmp(b, "usp")) return R_USP + 1;
    if (!strcmp(b, "fpcr")) return R_FPCR + 1;
    if (!strcmp(b, "fpsr")) return R_FPSR + 1;
    if (!strcmp(b, "fpiar") || !strcmp(b, "fpi")) return R_FPIAR + 1;
    if (!strcmp(b, "nc")) return R_CACHE + 1;
    if (!strcmp(b, "dc")) return R_CACHE + 2;
    if (!strcmp(b, "ic")) return R_CACHE + 3;
    if (!strcmp(b, "bc")) return R_CACHE + 4;
    if (!strcmp(b, "crp")) return R_CRP + 1;
    if (!strcmp(b, "tt0")) return R_TT0 + 1;
    if (!strcmp(b, "tt1")) return R_TT1 + 1;
    if (!strcmp(b, "psr")) return R_PSR + 1;
    for (i = 0; rnames[i]; i++)
        if (!strcmp(b, rnames[i]))
            return R_CREG + i + 1;
    return 0;
}
static int isreg_at(const char *s)
{
    int l;
    return regat(s, &l);
}

/* ---- operands ---- */
enum { O_DN, O_AN, O_IND, O_POST, O_PRE, O_DISP, O_IDX, O_ABSW, O_ABSL, O_PCDISP, O_PCIDX, O_IMM,
       O_ABS, O_REG, O_LIST, O_PAIR, O_IPAIR, O_NONE };
#define M(k) (1 << (k))
#define EA_ALL 0xfff
#define EA_DATA (EA_ALL & ~M(O_AN))
#define EA_MEM (EA_DATA & ~M(O_DN))
#define EA_CTL (M(O_IND) | M(O_DISP) | M(O_IDX) | M(O_ABSW) | M(O_ABSL) | M(O_PCDISP) | M(O_PCIDX))
#define EA_ALT (EA_ALL & ~(M(O_PCDISP) | M(O_PCIDX) | M(O_IMM)))
#define EA_DALT (EA_DATA & EA_ALT)
#define EA_MALT (EA_MEM & EA_ALT)
#define EA_CALT (EA_CTL & EA_ALT)
struct op {
    u8 k, reg, x, xl, xsc, bs, xs, mem, dsz, osz, hasd, haso, bf, bfod, bfwd, reg2, lk, mitbase;
    u16 mask;
    struct val d, o, bfo, bfw;
    char *fl;           /* a floating immediate's text */
};

static int opsize(char **s)     /* .w .l .b (or :w :l) after a value: 1 w, 2 l, 3 b; 0 none */
{
    char *p = *s;
    int c;
    if ((p[0] != '.' && p[0] != ':') || !p[1] || isid(p[2]))
        return 0;
    c = lower(p[1]);
    if (c != 'w' && c != 'l' && c != 'b')
        return 0;
    *s = p + 2;
    return c == 'w' ? 1 : c == 'l' ? 2 : 3;
}

/* an index register: %d0, %a1.l, %d2.w*4, %d3:l:2, %zd0 */
static int index_reg(char **s, struct op *o, int r)
{
    char *p = *s;
    int sz, sc = 0;
    if (r < 16)
        o->x = r;
    else if (r >= R_ZD && r < R_ZD + 8) {
        o->x = r - R_ZD;
        o->xs = 1;
    } else if (r >= R_ZA && r < R_ZA + 8) {
        o->x = r - R_ZA + 8;
        o->xs = 1;
    } else
        return 0;
    sz = opsize(&p);
    o->xl = sz != 1;    /* (gas's default index size is long) */
    if (sz == 3)
        return 0;
    if ((*p == '*' && isdig(p[1])) || (*p == ':' && isdig(p[1]))) {
        int n = p[1] - '0';
        if (isdig(p[2]))
            return 0;
        sc = n == 1 ? 0 : n == 2 ? 1 : n == 4 ? 2 : n == 8 ? 3 : -1;
        if (sc < 0)
            return 0;
        p += 2;
    }
    o->xsc = sc;
    *s = p;
    return 1;
}

/* the inside of ( ... ) or [ ... ]: displacement, base, index, nested [ ] */
static int paren_items(char **s, struct op *o, int inner, char close)
{
    char *p = skipws(*s);
    int got_base = 0, got_x = 0;
    for (;;) {
        int l, r;
        p = skipws(p);
        if (*p == '[' && !inner) {
            p++;
            if (!paren_items(&p, o, 1, ']'))
                return 0;
            o->mem = 1;
            got_base = 1;       /* the base, if any, was inside */
            if (o->x != 0xff)
                got_x = 1, o->mem = 1;
            else
                o->mem = 2;
        } else if ((r = regat(p, &l)) != 0) {
            char *q = p + l;
            r--;
            if (!got_base && !(inner && o->mitbase) && (r == R_PC || r == R_ZPC || (r >= 8 && r < 16) || (r >= R_ZA && r < R_ZA + 8)) &&
                *q != '.' && *q != ':' && *q != '*') {
                if (r == R_PC || r == R_ZPC) {
                    o->k = O_PCDISP;
                    o->bs = r == R_ZPC;
                } else if (r >= R_ZA) {
                    o->reg = r - R_ZA;
                    o->bs = 1;
                } else
                    o->reg = r - 8;
                got_base = 1;
                p = q;
            } else {
                if (got_x)
                    return 0;
                if (!index_reg(&q, o, r))
                    return 0;
                got_x = 1;
                p = q;
            }
        } else {
            struct val v;
            char *q = p;
            int sz;
            getexpr(&q, &v);
            if (v.bad)
                return 0;
            sz = opsize(&q);
            if (sz == 3)
                return 0;
            if (o->mem && !inner) {     /* the outer displacement */
                if (o->haso)
                    return 0;
                o->o = v;
                o->haso = 1;
                o->osz = sz;
            } else {
                if (o->hasd)
                    return 0;
                o->d = v;
                o->hasd = 1;
                o->dsz = sz;
            }
            p = skipws(q);
        }
        p = skipws(p);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p != close)
            return 0;
        *s = p + 1;
        if (!got_base && !inner && !o->mem)
            o->bs = 1;          /* (d,Xn): the base suppressed */
        if (inner && !got_base)
            o->bs = 1;
        return 1;
    }
}

/* a register list: %d0-%d3/%a0, %fp0-%fp2/%fp4, %fpcr/%fpsr */
static int reglist(char **s, struct op *o, int first)
{
    char *p = *s;
    int r = first, l, kind = r >= R_FP && r < R_FP + 8 ? 1 : r >= R_FPCR && r <= R_FPIAR ? 2 : r < 16 ? 0 : -1;
    u16 m = 0;
    if (kind < 0)
        return 0;
    for (;;) {
        int r2 = r;
        if (*p == '-') {
            r2 = regat(p + 1, &l);
            if (!r2--)
                return 0;
            p += 1 + l;
        }
        if (kind == 0 && r < 16 && r2 < 16 && r2 >= r) {
            for (; r <= r2; r++)
                m |= 1 << r;
        } else if (kind == 1 && r2 >= r && r2 < R_FP + 8) {
            for (; r <= r2; r++)
                m |= 1 << (r - R_FP);
        } else if (kind == 2 && r == r2 && r >= R_FPCR && r <= R_FPIAR)
            m |= r == R_FPCR ? 4 : r == R_FPSR ? 2 : 1;
        else
            return 0;
        if (*p != '/')
            break;
        r = regat(p + 1, &l);
        if (!r--)
            return 0;
        p += 1 + l;
    }
    o->k = O_LIST;
    o->mask = m;
    o->lk = kind;
    *s = p;
    return 1;
}

/* MIT syntax after %an@ or %pc@: "", +, -, (d), (d,%xn:l:4), (bd)@(od), (bd,%xn)@(od), (bd)@(od,%xn) */
static int mit(char **s, struct op *o)
{
    char *p = *s;
    if (*p == '+') {
        o->k = O_POST;
        *s = p + 1;
        return 1;
    }
    if (*p == '-') {
        o->k = O_PRE;
        *s = p + 1;
        return 1;
    }
    if (*p != '(') {
        o->k = o->k == O_PCDISP ? O_PCDISP : O_IND;
        *s = p;
        return 1;
    }
    o->mitbase = 1;
    p++;
    if (o->k != O_PCDISP)
        o->k = O_DISP;
    if (!paren_items(&p, o, 1, ')'))
        return 0;
    o->bs = 0;
    if (*p == '@' && p[1] == '(') {     /* memory indirect */
        int xin = o->x != 0xff;
        struct val d = o->d;
        u8 hd = o->hasd, dz = o->dsz;
        p += 2;
        o->hasd = 0;
        if (!paren_items(&p, o, 1, ')'))
            return 0;
        o->o = o->d;
        o->haso = o->hasd;
        o->osz = o->dsz;
        o->d = d;
        o->hasd = hd;
        o->dsz = dz;
        o->mem = xin ? 1 : 2;
        o->bs = 0;
    }
    *s = p;
    return 1;
}

static int parse_op(char *p, struct op *o)
{
    int r, l;
    memset(o, 0, sizeof *o);
    o->x = 0xff;
    p = skipws(p);
    if (*p == '#' || *p == '&') {     /* (&: the MIT syntax's immediate) */
        p = skipws(p + 1);
        o->k = O_IMM;
        if (*p == '0' && strchr("rRfFdDsSeE", p[1]) && p[1] && (isdig(p[2]) || p[2] == '-' || p[2] == '+' || p[2] == '.')) {
            o->fl = p;
            return 1;
        }
        getexpr(&p, &o->d);
        return !o->d.bad && !*p;
    }
    if (*p == '-' && p[1] == '(' && (r = regat(skipws(p + 2), &l)) != 0 && r - 1 >= 8 && r - 1 < 16) {
        char *q = skipws(skipws(p + 2) + l);
        if (*q == ')' && !*skipws(q + 1)) {
            o->k = O_PRE;
            o->reg = r - 9;
            return 1;
        }
    }
    if (*p == '(' && (r = regat(skipws(p + 1), &l)) != 0 && r - 1 < 16) {
        /* (%rn):(%rn) (cas2) */
        char *q = skipws(skipws(p + 1) + l);
        int r2;
        if (q[0] == ')' && q[1] == ':' && q[2] == '(' && (r2 = regat(skipws(q + 3), &l)) != 0 && r2 - 1 < 16) {
            q = skipws(skipws(q + 3) + l);
            if (*q == ')' && !*skipws(q + 1)) {
                o->k = O_IPAIR;
                o->reg = r - 1;
                o->reg2 = r2 - 1;
                return 1;
            }
        }
    }
    if (*p == '(') {
        /* ( ... ) with a register or [ ] inside is an addressing mode; a value alone in parentheses ((4).w) is a
         * value */
        struct op t = *o;
        char *q = p + 1;
        if (paren_items(&q, &t, 0, ')') && (!t.bs || t.x != 0xff || t.mem || t.k == O_PCDISP)) {
            *o = t;
            if (o->k != O_PCDISP)
                o->k = O_DISP;
            p = q;
            goto ea_done;
        }
        memset(o, 0, sizeof *o);
        o->x = 0xff;
    }
    if ((r = regat(p, &l)) != 0) {
        char *q = p + l;
        r--;
        if (*q == '@' && (r >= 8 && r < 16 ? 1 : r == R_PC)) {
            q++;
            if (r == R_PC)
                o->k = O_PCDISP;
            o->reg = r - 8;
            if (!mit(&q, o))
                return 0;
            if (r == R_PC && o->k != O_PCDISP)
                return 0;
            p = q;
            goto ea_done;
        }
        if (*q == '-' || *q == '/') {
            if (!reglist(&q, o, r))
                return 0;
            return !*skipws(q);
        }
        if (*q == ':' && (r < 16 || (r >= R_FP && r < R_FP + 8))) {
            int r2 = regat(q + 1, &l);
            if (r2 && (r < 16 ? r2 - 1 < 16 : r2 - 1 >= R_FP && r2 - 1 < R_FP + 8)) {
                o->k = O_PAIR;
                o->reg = r;
                o->reg2 = r2 - 1;
                return !*skipws(q + 1 + l);
            }
        }
        p = q;
        if (r < 8)
            o->k = O_DN, o->reg = r;
        else if (r < 16)
            o->k = O_AN, o->reg = r - 8;
        else
            o->k = O_REG, o->reg = r;
        goto bitfield;
    }
    /* an expression: absolute, or a displacement before ( ... ) */
    {
        struct val v;
        int sz;
        getexpr(&p, &v);
        if (v.bad)
            return 0;
        sz = opsize(&p);
        if (*p == '(') {
            o->k = O_DISP;
            o->d = v;
            o->hasd = 1;
            o->dsz = sz;
            p++;
            if (!paren_items(&p, o, 0, ')'))
                return 0;
            goto ea_done;
        }
        o->k = O_ABS;
        o->d = v;
        o->hasd = 1;
        o->dsz = sz;
        goto bitfield;
    }
ea_done:
    if (o->k == O_DISP || o->k == O_PCDISP) {
        int pc = o->k == O_PCDISP;
        if (*p == '+' && o->k == O_DISP && !o->hasd && o->x == 0xff && !o->mem && !o->bs) {
            o->k = O_POST;
            p++;
            goto bitfield;
        }
        if (o->x != 0xff || o->mem || o->bs)
            o->k = pc ? O_PCIDX : O_IDX;
        else if (!o->hasd && !pc)
            o->k = O_IND;
    }
    if (o->dsz == 3 || o->osz == 3)
        return 0;
bitfield:
    p = skipws(p);
    if (*p == '{') {
        int i;
        p = skipws(p + 1);
        for (i = 0; i < 2; i++) {
            struct val *v = i ? &o->bfw : &o->bfo;
            if ((r = regat(p, &l)) != 0 && r - 1 < 8) {
                if (i)
                    o->bfwd = 1;
                else
                    o->bfod = 1;
                v->v = r - 1;
                p += l;
            } else {
                if (*p == '#')
                    p++;
                getexpr(&p, v);
                if (v->bad || v->sec != SEC_ABS)
                    return 0;
            }
            p = skipws(p);
            if (!i && *p == '}') {      /* {#k} or {%dn}: fmove.p's k-factor */
                p = skipws(p + 1);
                o->bf = 2;
                return !*p;
            }
            if (*p != (i ? '}' : ':'))
                return 0;
            p = skipws(p + 1);
        }
        o->bf = 1;
    }
    return !*p;
}

/* ---- the instruction being built: words and fixups ---- */
struct fix {
    struct val v;
    u8 at, n, pc, defer, pcb, imm;  /* byte offset, size, PC-relative (base at byte pcb), from a relaxed item, an
                                     * immediate (2: a byte one in a word) */
};
static u16 iw[16];
static int niw;
static struct fix fx[8];
static int nfx, idefer;

static int fits8(i64 x) { return x >= -128 && x <= 127; }
static int fits16(i64 x) { return x >= -32768 && x <= 32767; }
static int dropins;      /* the instruction is refused: gas leaves it out ("statement ignored"), so no bytes */
static void refuse(const char *m)
{
    err("%s", m);
    dropins = 1;
}
static void w16(u32 v)
{
    if (niw < 16)
        iw[niw] = v;
    niw++;
}
static void addfix(struct val *v, int n, int pc, int pcb, int imm)
{
    struct fix *f;
    if (v->known && v->sec == SEC_ABS && !pc && n < 4) {
        /* a number known where it stands and too big: gas refuses the statement then (later it is only an error) */
        i64 x = v->v;
        if (imm == 2 ? (x < -128 || x > 255) : imm ? (n == 1 ? (x < -128 || x > 255) : (x < -32768 || x > 65535))
                     : (n == 1 ? !fits8(x) : !fits16(x)))
            refuse("value out of range");
    }
    if (nfx == 8)
        return;
    f = &fx[nfx++];
    f->v = *v;
    f->at = niw * 2 + (n == 1);
    f->n = n;
    f->pc = pc;
    f->pcb = pcb;
    f->imm = imm;
    f->defer = idefer;
    if (n == 4) {
        w16(0);
        w16(0);
    } else
        w16(0);
}

/* gas's rule for a relocation's symbol (write.c adjust_reloc_syms): the section's symbol and the offset, unless the
 * symbol is undefined, weak, named by a GOT relocation, or local in a merged section with an addend */
static int adjustable(struct val *v)
{
    if (!v->s)
        return 1;
    if (v->sec == SEC_UNDEF || v->s->bind == STB_WEAK || v->got)
        return 0;
    if (v->sec >= 0 && (sects[v->sec]->flags & SHF_MERGE) && v->add != 0)
        return 0;
    return 1;
}
/* the distance from the PC base to a target in this section, or 0 when it is elsewhere (another section, undefined,
 * absolute, weak: a relocation) */
static int near(struct val *v, u32 base, i64 *d)
{
    if (pass == 1 && v->s && v->sec == SEC_UNDEF && !v->s->defpass && v->got != 1 && !v->pcrel) {
        *d = 2;         /* the first pass: a forward reference is taken as near (gas starts small too) */
        return 1;
    }
    if (v->bad || v->pcrel || v->got == 1 || v->sec != secidx(cur) || (v->s && v->s->bind == STB_WEAK))
        return 0;
    *d = v->v - (i64)base;
    /* gas's relax_frag: a target not reached yet in this pass moves by the stretch too, unless an alignment lies
     * between (it may absorb it); a forward target that then seems behind leaves the size as it is */
    if (v->s && v->s->defpass != pass && stretch) {
        if (stretch < 0 || v->s->region == cur->region)
            *d += stretch;
        else if (*d < 0)
            nogrow = 1;
    }
    return 1;
}

static void addrel(struct sect *s, int defer, u32 off, int type, struct sym *y, i64 add)
{
    struct rel *r;
    if (s->drop)
        return;
    if (defer) {
        s->drel = xgrow(s->drel, &s->cdrel, s->ndrel + 1, sizeof *r);
        r = &s->drel[s->ndrel++];
    } else {
        s->rel = xgrow(s->rel, &s->crel, s->nrel + 1, sizeof *r);
        r = &s->rel[s->nrel++];
    }
    r->off = off;
    r->type = type;
    r->s = y;
    r->add = (i32)add;
}

/* a value into a field of n bytes at offset off of the current section (the bytes are in b, which the caller
 * writes): resolved, or a relocation. pc: a PC-relative field whose PC is at pcbase. imm: an immediate (signed or
 * unsigned range) */
static void fixfield(u8 *b, u32 off, int n, struct val *v, int pc, u32 pcbase, int imm, int defer)
{
    i64 x;
    int type = 0;
    if (v->bad) {
        err("bad expression");
        return;
    }
    if (v->got == 1) {
        type = n == 4 ? 10 : n == 2 ? 11 : 12;      /* R_68K_GOT32O, GOT16O, GOT8O */
        if (v->s)
            v->s->flags |= F_KEEP;
        addrel(cur, defer, off, type, v->s, v->add);
        return;
    }
    if (v->pcrel && !pc) {      /* X - . */
        int a = adjustable(v);
        if (v->sec == SEC_ABS) {
            err("bad expression");
            return;
        }
        if (!a && v->s && v->sec >= 0)
            v->s->flags |= F_KEEP;
        addrel(cur, defer, off, n == 4 ? 4 : n == 2 ? 5 : 6, a ? sects[v->sec]->ssym : v->s,
               (a ? v->v : v->add) - v->pcoff + off);
        return;
    }
    if (pc) {
        i64 d;
        if (v->sec == SEC_ABS && !v->known) {
            /* a symbol that turned out absolute: an address, PC-relative to nothing (gas: *ABS*+n) */
            addrel(cur, defer, off, n == 4 ? 4 : n == 2 ? 5 : 6, 0, v->v + off - pcbase);
            return;
        } else if (v->sec == SEC_ABS) {
            x = v->v;   /* a number: the displacement itself */
        } else if (near(v, pcbase, &d))
            x = d;
        else if (v->got == 3) {     /* sym@PLTPC: R_68K_PLT32, PLT16, PLT8 against the symbol itself */
            if (v->s)
                v->s->flags |= F_KEEP;
            addrel(cur, defer, off, n == 4 ? 13 : n == 2 ? 14 : 15, v->s, v->add + off - pcbase);
            return;
        } else {
            int a = adjustable(v);
            if (!a && v->s && v->sec >= 0)
                v->s->flags |= F_KEEP;
            addrel(cur, defer, off, n == 4 ? 4 : n == 2 ? 5 : 6, a ? sects[v->sec]->ssym : v->s,
                   (a ? v->v : v->add) + off - pcbase);
            return;
        }
        if ((n == 1 && (x < -128 || x > 127)) || (n == 2 && (x < -32768 || x > 32767)) ||
            (n == 4 && (x < -0x80000000LL || x > 0x7fffffffLL)))
            err("displacement out of range (%d)", (i32)x);
    } else if (v->got == 3) {       /* jsr f@PLTPC: gas writes R_68K_PLT32 (16, 8) against the symbol */
        if (v->s)
            v->s->flags |= F_KEEP;
        addrel(cur, defer, off, n == 4 ? 13 : n == 2 ? 14 : 15, v->s, v->add);
        return;
    } else if (v->sec != SEC_ABS) {
        int a = adjustable(v);
        if (!a && v->s && v->sec >= 0)
            v->s->flags |= F_KEEP;
        addrel(cur, defer, off, n == 4 ? 1 : n == 2 ? 2 : 3, a ? sects[v->sec]->ssym : v->s, a ? v->v : v->add);
        return;
    } else {
        x = v->v;
        if (imm == 2 ? (x < -128 || x > 255) :
            imm ? ((n == 1 && (x < -128 || x > 255)) || (n == 2 && (x < -32768 || x > 65535)))
                : ((n == 1 && (x < -128 || x > 127)) || (n == 2 && (x < -32768 || x > 32767))))
            err("value out of range (%d)", (i32)x);
    }
    if (n == 4) {
        b[0] = x >> 24;
        b[1] = x >> 16;
        b[2] = x >> 8;
        b[3] = x;
    } else if (n == 2) {
        b[0] = x >> 8;
        b[1] = x;
    } else
        b[0] = x;
}

/* write the instruction built in iw/fx at the current location */
static void emit_ins(void)
{
    u32 L = cur->loc;
    u8 b[32];
    int i;
    if (niw > 16)
        die("internal error: instruction too long");
    if (final && !cur->drop) {
        for (i = 0; i < niw; i++) {
            b[2 * i] = iw[i] >> 8;
            b[2 * i + 1] = iw[i];
        }
        for (i = 0; i < nfx; i++) {
            struct fix *f = &fx[i];
            fixfield(b + f->at, L + f->at, f->n, &f->v, f->pc, L + f->pcb, f->imm, f->defer);
        }
    }
    putbytes(b, niw * 2);
    if (fragnext) {     /* a relaxable item ended gas's frag: the next one begins here */
        cur->fragloc = cur->loc;
        fragnext = 0;
    }
    niw = nfx = 0;
}

/* sizes */
#define SZ_B 1
#define SZ_W 2
#define SZ_L 4
#define SZ_S 5      /* FPU single */
#define SZ_D 8
#define SZ_X 12
#define SZ_P 13


static void fp_imm(const char *t, int size, int at);
static int fltconv(const char *t, int kind, u8 *out);

/* an immediate of the given size into the extension words */
static void imm(struct op *o, int size)
{
    if (o->fl && (size == SZ_B || size == SZ_W || size == SZ_L)) {
        u8 b[4];
        if (!fltconv(o->fl, 4, b))
            err("bad floating-point immediate");
        setabs(&o->d, (i64)(b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]), 1);    /* (gas: its single's bits) */
        o->fl = 0;
    }
    if (o->fl) {
        fp_imm(o->fl, size, niw);
        return;
    }
    if (size == SZ_B && o->d.sec != SEC_ABS)
        addfix(&o->d, 1, 0, 0, 1);
    else if (size == SZ_B || size == SZ_W)    /* a byte immediate: its word as gas writes it (#-1: 0xffff) */
        addfix(&o->d, 2, 0, 0, size == SZ_B ? 2 : 1);
    else if (size == SZ_L || size == SZ_S)
        addfix(&o->d, 4, 0, 0, 1);
    else {
        /* a 64-bit or 96-bit immediate: an integer gives its bits, right-aligned (gas) */
        i64 x = o->d.v;
        if (o->d.sec != SEC_ABS)
            err("a %d-byte immediate must be a number", size);
        if (size == SZ_X) {
            w16(0);
            w16(0);
        }
        if (size == SZ_P) {
            err("packed immediates are not supported");
            w16(0);
            w16(0);
        }
        w16(x >> 48);
        w16(x >> 32);
        w16(x >> 16);
        w16(x);
    }
}

/* the full extension word format (68020+): base displacement and outer displacement */
static int bdsize(struct val *v, int has, int sz)
{
    if (!has || (v->known && v->v == 0 && !sz))
        return 1;
    if (sz)
        return sz + 1;
    if (v->known && fits16(v->v))
        return 2;
    return 3;
}
static void full_ext(struct op *o, int pcbase, int pc, int bdforce)
{
    int bds = bdforce ? bdforce : bdsize(&o->d, o->hasd, o->dsz);
    int ods = o->mem ? bdsize(&o->o, o->haso, o->osz) : 0;
    int is = o->x == 0xff || o->xs;
    u32 e = 0x100 | bds << 4 | (o->bs ? 0x80 : 0) | (is ? 0x40 : 0);
    if (!(cpu & UP20))
        refuse("this addressing mode needs a 68020 or higher");
    if (o->x != 0xff)
        e |= (o->x & 15) << 12 | o->xl << 11 | o->xsc << 9;
    if (o->mem)
        e |= (o->mem == 2 && !is ? 4 : 0) | ods;
    w16(e);
    if (bds == 2)
        addfix(&o->d, 2, pc, pcbase, 0);
    else if (bds == 3)
        addfix(&o->d, 4, pc, pcbase, 0);
    if (ods == 2)
        addfix(&o->o, 2, 0, 0, 0);
    else if (ods == 3)
        addfix(&o->o, 4, 0, 0, 0);
}

/* the effective address o: its 6-bit mode and register field; the extension words go into iw. size: the size of an
 * immediate; pcok: the position allows PC-relative modes (gas then turns an absolute address in this section into a
 * PC-relative one) */
static int ea(struct op *o, int size, int pcok)
{
    i64 d;
    int st;
    switch (o->k) {
    case O_DN: return o->reg;
    case O_AN: return 8 | o->reg;
    case O_IND: return 0x10 | o->reg;
    case O_POST: return 0x18 | o->reg;
    case O_PRE: return 0x20 | o->reg;
    case O_IMM:
        imm(o, size);
        return 0x3c;
    case O_DISP:
        if (o->dsz == 2 || (!o->dsz && !o->d.known && !(o->d.gotw) && (cpu & UP20))) {
            if (o->d.gotw == 1)
                err("a byte displacement here");
            full_ext(o, 0, 0, 3);
            return 0x30 | o->reg;
        }
        if (!o->dsz && o->d.known && o->d.v == 0)
            return 0x10 | o->reg;
        if (!o->dsz && o->d.known && !fits16(o->d.v) && (cpu & UP20)) {
            full_ext(o, 0, 0, 3);
            return 0x30 | o->reg;
        }
        addfix(&o->d, 2, 0, 0, 0);
        return 0x28 | o->reg;
    case O_IDX:
        if (!o->mem && !o->bs && !o->xs && !o->dsz && (o->xsc == 0 || (cpu & UP20)) &&
            (!o->hasd || (o->d.known && fits8(o->d.v)))) {
            w16((o->x & 15) << 12 | o->xl << 11 | o->xsc << 9 | ((o->hasd ? o->d.v : 0) & 0xff));
            return 0x30 | o->reg;
        }
        full_ext(o, 0, 0, 0);
        return 0x30 | o->reg;   /* (%za1: the register stays in the field) */
    case O_PCDISP:
        if (!o->hasd)
            o->d.known = 1, o->d.sec = SEC_ABS;
        if (o->dsz == 1 || (!o->dsz && o->d.known && fits16(o->d.v)) || !(cpu & UP20)) {
            addfix(&o->d, 2, 1, niw * 2, 0);
            return 0x3a;
        }
        if (o->dsz == 2 || o->d.known) {
            full_ext(o, niw * 2, 1, 3);
            return 0x3b;
        }
        /* gas's PCREL1632: (d16,%pc), or (bd.l,%pc) when the target is out of reach or elsewhere */
        relax_at(cur->loc);
        st = !near(&o->d, cur->loc + niw * 2, &d) || (pass > 2 && !fits16(d));
        stretch = 0;
        st = relax(st);
        idefer = 1;
        if (!st)
            addfix(&o->d, 2, 1, niw * 2, 0);
        else
            full_ext(o, niw * 2, 1, 3);
        idefer = 0;
        cur->frag++, fragnext = 1;
        return st ? 0x3b : 0x3a;
    case O_PCIDX:
        if (!o->mem && !o->bs && !o->xs && !o->dsz && (o->xsc == 0 || (cpu & UP20)) &&
            (!o->hasd || (o->d.known && fits8(o->d.v)))) {
            w16((o->x & 15) << 12 | o->xl << 11 | o->xsc << 9 | ((o->hasd ? o->d.v : 0) & 0xff));
            return 0x3b;
        }
        if (o->mem || o->bs || o->xs || o->dsz || !o->hasd || o->d.known || !(cpu & UP20)) {
            if (!(cpu & UP20)) {        /* 68000: (d8,%pc,%xn) only */
                if (o->mem || o->bs || o->xs || o->xsc || o->dsz)
                    refuse("this addressing mode needs a 68020 or higher");
                addfix(&o->d, 1, 1, niw * 2, 0);
                iw[niw - 1] |= (o->x & 15) << 12 | o->xl << 11;
                return 0x3b;
            }
            full_ext(o, niw * 2, 1, 0);
            return 0x3b;
        }
        /* gas's PCINDEX: (d8,%pc,%xn), then (bd.w,...), then (bd.l,...) */
        relax_at(cur->loc);
        st = !near(&o->d, cur->loc + niw * 2, &d) ? 2 : pass <= 2 || fits8(d) ? 0 : fits16(d) ? 1 : 2;
        stretch = 0;
        st = relax(st);
        idefer = 1;
        if (!st) {
            addfix(&o->d, 1, 1, niw * 2, 0);
            iw[niw - 1] |= (o->x & 15) << 12 | o->xl << 11 | o->xsc << 9;
        } else
            full_ext(o, niw * 2, 1, st + 1);
        idefer = 0;
        cur->frag++, fragnext = 1;
        return 0x3b;
    case O_ABS:
        if (o->dsz == 1 || (!o->dsz && o->d.known && fits16(o->d.v))) {
            if (o->d.gotw)
                err("a table reference needs (%%a5)");
            addfix(&o->d, 2, 0, 0, 0);
            return 0x38;
        }
        if (o->dsz || o->d.known || !pcok || pcrel_opt || o->d.got || o->d.pcrel) {
            addfix(&o->d, 4, 0, 0, 0);
            return 0x39;
        }
        /* gas's ABSTOPCREL: an address in this section within reach becomes (d16,%pc) */
        relax_at(cur->loc);
        st = !near(&o->d, cur->loc + niw * 2, &d) || (pass > 2 && !fits16(d));
        stretch = 0;
        st = relax(st);
        idefer = 1;
        if (!st)
            addfix(&o->d, 2, 1, niw * 2, 0);
        else
            addfix(&o->d, 4, 0, 0, 0);
        idefer = 0;
        cur->frag++, fragnext = 1;
        return st ? 0x39 : 0x3a;
    }
    err("operand not allowed here");
    return 0;
}

/* ---- instructions ---- */
enum {
    F_ADD, F_LOGIC, F_EOR, F_CMP, F_IMMOP, F_ADDA, F_QUICK, F_ADDX, F_MOVE, F_MOVEA, F_MOVEQ, F_MOVEM, F_MOVEP,
    F_MOVEC, F_MOVES, F_UNARY, F_TST, F_SHIFT, F_BIT, F_BF, F_EXT, F_SWAP, F_EXG, F_EA1, F_LEA, F_LINK, F_UNLK,
    F_NOARG, F_IMM16, F_TRAP, F_BKPT, F_CHK, F_CHK2, F_CAS, F_CAS2, F_MUL, F_DIV, F_DIVL, F_PACK, F_CALLM, F_RTM,
    F_MOVE16, F_CACHE, F_PFL40, F_PTEST, F_PLPA, F_LPSTOP, F_PMOVE, F_PFLUSH, F_PLOAD, F_BCC, F_JCC, F_DBCC, F_SCC,
    F_TRAPCC, F_FOP, F_FMOVE, F_FMOVEM, F_FMOVECR, F_FBCC, F_FSCC, F_FDBCC, F_FTRAPCC, F_FNOP, F_FSAVE, F_FTST,
    F_FSINCOS, F_PFLUSHA
};
#define SB 1
#define SW 2
#define SL 4
#define SS 8
#define SD 16
#define SX 32
#define SP 64
#define SN 128         /* without a size */
#define SBWL 7
#define SFP 127
struct mn {
    const char *name;
    u8 fam;
    u16 opc;
    u8 sizes, cpu;
};
#define A00 (C00 | UP10)
#define FPU (A00 | CFPU)
static const struct mn mns[] = {
    {"abcd", F_ADDX, 0xc100, SN, A00}, {"add", F_ADD, 0xd000, SN | SB | SW | SL, A00}, {"adda", F_ADDA, 0xd0c0, SN | SW | SL, A00},
    {"addi", F_IMMOP, 0x0600, SN | SB | SW | SL, A00}, {"addq", F_QUICK, 0x5000, SN | SB | SW | SL, A00},
    {"addx", F_ADDX, 0xd100, SN | SB | SW | SL, A00}, {"and", F_LOGIC, 0xc000, SN | SB | SW | SL, A00}, {"andi", F_IMMOP, 0x0200, SN | SB | SW | SL, A00},
    {"asl", F_SHIFT, 0x0100, SN | SB | SW | SL, A00}, {"asr", F_SHIFT, 0x0000, SN | SB | SW | SL, A00},
    {"bchg", F_BIT, 1, SN | SB | SL, A00}, {"bclr", F_BIT, 2, SN | SB | SL, A00}, {"bset", F_BIT, 3, SN | SB | SL, A00},
    {"btst", F_BIT, 0, SN | SB | SL, A00}, {"bfchg", F_BF, 0xeac0, SN, UP20}, {"bfclr", F_BF, 0xecc0, SN, UP20},
    {"bfexts", F_BF, 0xebc0, SN, UP20}, {"bfextu", F_BF, 0xe9c0, SN, UP20}, {"bfffo", F_BF, 0xedc0, SN, UP20},
    {"bfins", F_BF, 0xefc0, SN, UP20}, {"bfset", F_BF, 0xeec0, SN, UP20}, {"bftst", F_BF, 0xe8c0, SN, UP20},
    {"bkpt", F_BKPT, 0x4848, SN, UP10}, {"callm", F_CALLM, 0x06c0, SN, C20}, {"cas", F_CAS, 0x08c0, SN | SB | SW | SL, UP20},
    {"cas2", F_CAS2, 0x0cfc, SN | SW | SL, UP20}, {"chk", F_CHK, 0x4180, SN | SW | SL, A00},
    {"chk2", F_CHK2, 0x0800, SN | SB | SW | SL, UP20}, {"cinva", F_CACHE, 0xf418, SN, C40 | C60},
    {"cinvl", F_CACHE, 0xf408, SN, C40 | C60}, {"cinvp", F_CACHE, 0xf410, SN, C40 | C60},
    {"clr", F_UNARY, 0x4200, SN | SB | SW | SL, A00}, {"cmp", F_CMP, 0xb000, SN | SB | SW | SL, A00}, {"cmp2", F_CHK2, 0x0000, SN | SB | SW | SL, UP20},
    {"cmpa", F_ADDA, 0xb0c0, SN | SW | SL, A00}, {"cmpi", F_IMMOP, 0x0c00, SN | SB | SW | SL, A00},
    {"cmpm", F_ADDX, 0xb108, SN | SB | SW | SL, A00}, {"cpusha", F_CACHE, 0xf438, SN, C40 | C60},
    {"cpushl", F_CACHE, 0xf428, SN, C40 | C60}, {"cpushp", F_CACHE, 0xf430, SN, C40 | C60},
    {"divs", F_DIV, 0x81c0, SN | SW | SL, A00}, {"divsll", F_DIVL, 0x0800, SN, UP20},
    {"divu", F_DIV, 0x80c0, SN | SW | SL, A00}, {"divull", F_DIVL, 0x0000, SN, UP20},
    {"eor", F_EOR, 0xb100, SN | SB | SW | SL, A00}, {"eori", F_IMMOP, 0x0a00, SN | SB | SW | SL, A00}, {"exg", F_EXG, 0xc100, SN, A00},
    {"ext", F_EXT, 0x4880, SN | SW | SL, A00}, {"extb", F_EXT, 0x49c0, SW | SL, A00}, {"illegal", F_NOARG, 0x4afc, SN, A00},
    {"jmp", F_EA1, 0x4ec0, SN | SL | SS, A00}, {"jsr", F_EA1, 0x4e80, SN | SL | SS, A00}, {"lea", F_LEA, 0x41c0, SN | SL, A00},
    {"link", F_LINK, 0x4e50, SN | SW | SL, A00}, {"lpstop", F_LPSTOP, 0xf800, SN, C60},
    {"lsl", F_SHIFT, 0x0108, SN | SB | SW | SL, A00}, {"lsr", F_SHIFT, 0x0008, SN | SB | SW | SL, A00},
    {"move", F_MOVE, 0, SN | SB | SW | SL, A00}, {"mov", F_MOVE, 0, SB | SW | SL, A00}, {"move16", F_MOVE16, 0, SN, C40 | C60}, {"movea", F_MOVEA, 0, SN | SW | SL, A00},
    {"movec", F_MOVEC, 0x4e7a, SN | SL, UP10}, {"movem", F_MOVEM, 0x4880, SN | SW | SL, A00},
    {"movep", F_MOVEP, 0x0108, SN | SW | SL, A00}, {"moveq", F_MOVEQ, 0x7000, SN | SL, A00},
    {"moves", F_MOVES, 0x0e00, SN | SB | SW | SL, UP10}, {"muls", F_MUL, 0xc1c0, SN | SW | SL, A00},
    {"mulu", F_MUL, 0xc0c0, SN | SW | SL, A00}, {"nbcd", F_UNARY, 0x4800, SN | SB, A00}, {"neg", F_UNARY, 0x4400, SN | SB | SW | SL, A00},
    {"negx", F_UNARY, 0x4000, SN | SB | SW | SL, A00}, {"nop", F_NOARG, 0x4e71, SN, A00}, {"not", F_UNARY, 0x4600, SN | SB | SW | SL, A00},
    {"or", F_LOGIC, 0x8000, SN | SB | SW | SL, A00}, {"ori", F_IMMOP, 0x0000, SN | SB | SW | SL, A00}, {"pack", F_PACK, 0x8140, SN, UP20},
    {"pea", F_EA1, 0x4840, SN | SL, A00}, {"pflush", F_PFLUSH, 0xf508, SN | SS, UP20}, {"pflusha", F_PFLUSHA, 0xf518, SN, UP20},
    {"pflushan", F_PFL40, 0xf510, SN, C40 | C60}, {"pflushn", F_PFL40, 0xf500, SN, C40 | C60},
    {"pload", F_PLOAD, 0, SN, 0}, {"ploadr", F_PLOAD, 0x2200, SN, C20 | C30}, {"ploadw", F_PLOAD, 0x2000, SN, C20 | C30},
    {"plpar", F_PLPA, 0xf5c8, SN, C60}, {"plpaw", F_PLPA, 0xf588, SN, C60}, {"pmove", F_PMOVE, 0, SN, C20 | C30},
    {"pmovefd", F_PMOVE, 0x100, SN, C30}, {"ptestr", F_PTEST, 0xf568, SN, C20 | C30 | C40},
    {"ptestw", F_PTEST, 0xf548, SN, C20 | C30 | C40}, {"reset", F_NOARG, 0x4e70, SN, A00}, {"rol", F_SHIFT, 0x0118, SN | SB | SW | SL, A00},
    {"ror", F_SHIFT, 0x0018, SN | SB | SW | SL, A00}, {"roxl", F_SHIFT, 0x0110, SN | SB | SW | SL, A00},
    {"roxr", F_SHIFT, 0x0010, SN | SB | SW | SL, A00}, {"rtd", F_IMM16, 0x4e74, SN, UP10}, {"rte", F_NOARG, 0x4e73, SN, A00},
    {"rtm", F_RTM, 0x06c0, SN, C20}, {"rtr", F_NOARG, 0x4e77, SN, A00}, {"rts", F_NOARG, 0x4e75, SN, A00},
    {"sbcd", F_ADDX, 0x8100, SN | SB, A00}, {"stop", F_IMM16, 0x4e72, SN, A00}, {"sub", F_ADD, 0x9000, SN | SB | SW | SL, A00},
    {"suba", F_ADDA, 0x90c0, SN | SW | SL, A00}, {"subi", F_IMMOP, 0x0400, SN | SB | SW | SL, A00},
    {"subq", F_QUICK, 0x5100, SN | SB | SW | SL, A00}, {"subx", F_ADDX, 0x9100, SN | SB | SW | SL, A00}, {"swap", F_SWAP, 0x4840, SN | SW, A00},
    {"tas", F_UNARY, 0x4ac0, SN | SB, A00}, {"trap", F_TRAP, 0x4e40, SN, A00}, {"trapv", F_NOARG, 0x4e76, SN, A00},
    {"tst", F_TST, 0x4a00, SN | SB | SW | SL, A00}, {"unlk", F_UNLK, 0x4e58, SN, A00}, {"unpk", F_PACK, 0x8180, SN, UP20},
    /* the FPU */
    {"fabs", F_FOP, 0x18, SFP | SN, FPU}, {"facos", F_FOP, 0x1c, SFP | SN, FPU}, {"fadd", F_FOP, 0x22, SFP, FPU},
    {"fasin", F_FOP, 0x0c, SFP | SN, FPU}, {"fatan", F_FOP, 0x0a, SFP | SN, FPU}, {"fatanh", F_FOP, 0x0d, SFP | SN, FPU},
    {"fcmp", F_FOP, 0x38, SFP, FPU}, {"fcos", F_FOP, 0x1d, SFP | SN, FPU}, {"fcosh", F_FOP, 0x19, SFP | SN, FPU},
    {"fdabs", F_FOP, 0x5c, SFP | SN, C40 | C60 | CFPU}, {"fdadd", F_FOP, 0x66, SFP, C40 | C60 | CFPU}, {"fddiv", F_FOP, 0x64, SFP, C40 | C60 | CFPU},
    {"fdiv", F_FOP, 0x20, SFP, FPU}, {"fdmove", F_FOP, 0x44, SFP, C40 | C60 | CFPU}, {"fdmul", F_FOP, 0x67, SFP, C40 | C60 | CFPU},
    {"fdneg", F_FOP, 0x5e, SFP | SN, C40 | C60 | CFPU}, {"fdsqrt", F_FOP, 0x45, SFP | SN, C40 | C60 | CFPU}, {"fdsub", F_FOP, 0x6c, SFP | SN, C40 | C60 | CFPU},
    {"fetox", F_FOP, 0x10, SFP | SN, FPU}, {"fetoxm1", F_FOP, 0x08, SFP | SN, FPU}, {"fgetexp", F_FOP, 0x1e, SFP | SN, FPU},
    {"fgetman", F_FOP, 0x1f, SFP | SN, FPU}, {"fint", F_FOP, 0x01, SFP | SN, FPU}, {"fintrz", F_FOP, 0x03, SFP | SN, FPU},
    {"flog10", F_FOP, 0x15, SFP | SN, FPU}, {"flog2", F_FOP, 0x16, SFP | SN, FPU}, {"flogn", F_FOP, 0x14, SFP | SN, FPU},
    {"flognp1", F_FOP, 0x06, SFP | SN, FPU}, {"fmod", F_FOP, 0x21, SFP, FPU}, {"fmove", F_FMOVE, 0, SB | SW | SL | SS | SD | SX | SP, FPU},
    {"fmovecr", F_FMOVECR, 0, SN | SX, FPU}, {"fmovem", F_FMOVEM, 0, SN | SL | SD | SX, FPU}, {"fmul", F_FOP, 0x23, SFP, FPU},
    {"fneg", F_FOP, 0x1a, SFP | SN, FPU}, {"fnop", F_FNOP, 0, SN, FPU}, {"frem", F_FOP, 0x25, SFP, FPU},
    {"frestore", F_FSAVE, 0xf340, SN, FPU}, {"fsabs", F_FOP, 0x58, SFP | SN, C40 | C60 | CFPU}, {"fsadd", F_FOP, 0x62, SFP, C40 | C60 | CFPU},
    {"fsave", F_FSAVE, 0xf300, SN, FPU}, {"fscale", F_FOP, 0x26, SFP, FPU}, {"fsdiv", F_FOP, 0x60, SFP, C40 | C60 | CFPU},
    {"fsgldiv", F_FOP, 0x24, SFP | SN, FPU}, {"fsglmul", F_FOP, 0x27, SFP | SN, FPU}, {"fsin", F_FOP, 0x0e, SFP | SN, FPU},
    {"fsincos", F_FSINCOS, 0x30, SB | SW | SL | SS | SD | SX | SP, FPU}, {"fsinh", F_FOP, 0x02, SFP | SN, FPU}, {"fsmove", F_FOP, 0x40, SFP, C40 | C60 | CFPU},
    {"fsmul", F_FOP, 0x63, SFP, C40 | C60 | CFPU}, {"fsneg", F_FOP, 0x5a, SFP | SN, C40 | C60 | CFPU}, {"fsqrt", F_FOP, 0x04, SFP | SN, FPU},
    {"fssqrt", F_FOP, 0x41, SFP | SN, C40 | C60 | CFPU}, {"fssub", F_FOP, 0x68, SFP | SN, C40 | C60 | CFPU}, {"fsub", F_FOP, 0x28, SFP | SN, FPU},
    {"ftan", F_FOP, 0x0f, SFP | SN, FPU}, {"ftanh", F_FOP, 0x09, SFP | SN, FPU}, {"ftentox", F_FOP, 0x12, SFP | SN, FPU},
    {"ftst", F_FTST, 0x3a, SB | SW | SL | SS | SD | SX | SP, FPU}, {"ftwotox", F_FOP, 0x11, SFP | SN, FPU},
    {0, 0, 0, 0, 0}};

static const char *const ccn[] = {"t", "f", "hi", "ls", "cc", "cs", "ne", "eq", "vc", "vs", "pl", "mi", "ge", "lt",
                                  "gt", "le", "hs", "lo", 0};
static const char *const fccn[] = {"f", "eq", "ogt", "oge", "olt", "ole", "ogl", "or", "un", "ueq", "ugt", "uge",
                                   "ult", "ule", "ne", "t", "sf", "seq", "gt", "ge", "lt", "le", "gl", "gle", "ngle",
                                   "ngl", "nle", "nlt", "nge", "ngt", "sne", "st", 0};
static int cond(const char *s, int n, int fpu)
{
    const char *const *t = fpu ? fccn : ccn;
    int i;
    for (i = 0; t[i]; i++)
        if ((int)strlen(t[i]) == n && !strncmp(s, t[i], n))
            return !fpu && i >= 16 ? (i == 16 ? 4 : 5) | 0x100 : i;     /* (hs, lo: 0x100) */
    return -1;
}

static const struct mn *findmn(const char *s, int n)
{
    int i;
    for (i = 0; mns[i].name; i++)
        if ((int)strlen(mns[i].name) == n && !strncmp(mns[i].name, s, n))
            return &mns[i];
    return 0;
}
/* the conditional families: b<cc> j<cc> jb<cc> db<cc> s<cc> trap<cc> fb<cc> fj<cc> fs<cc> fdb<cc> ftrap<cc> (gas's
 * names: hs and lo with b only, and dbhs; t and f not with b and j) */
static struct mn condmn;
static const struct mn *findcond(const char *s, int n, int *cc)
{
    static const struct { const char *p; u8 fam, fpu, cpu; } pf[] = {
        {"ftrap", F_FTRAPCC, 1, FPU}, {"trap", F_TRAPCC, 0, UP20}, {"fdb", F_FDBCC, 1, FPU}, {"fb", F_FBCC, 1, FPU},
        {"fj", F_FBCC, 1, FPU}, {"fs", F_FSCC, 1, FPU}, {"jb", F_JCC, 0, A00}, {"db", F_DBCC, 0, A00},
        {"b", F_BCC, 0, A00}, {"j", F_JCC, 0, A00}, {"s", F_SCC, 0, A00}, {0, 0, 0, 0}};
    int i;
    for (i = 0; pf[i].p; i++) {
        int l = strlen(pf[i].p), c, f = pf[i].fam;
        if (n <= l || strncmp(s, pf[i].p, l))
            continue;
        if ((f == F_BCC || f == F_JCC) && n - l == 2 && !strncmp(s + l, "ra", 2))
            c = 0;
        else if ((f == F_BCC || (f == F_JCC && l == 2)) && n - l == 2 && !strncmp(s + l, "sr", 2))
            c = 1;
        else if (f == F_JCC && l == 1 && n == 4 && !strncmp(s, "jbsr", 4))
            c = 1;
        else if (f == F_DBCC && n - l == 2 && !strncmp(s + l, "ra", 2))
            c = 1;
        else {
            c = cond(s + l, n - l, pf[i].fpu);
            if (c < 0 || ((f == F_BCC || f == F_JCC) && (c & 0xff) < 2))
                continue;
            if (c & 0x100) {    /* hs, lo */
                if (!(f == F_BCC || (f == F_DBCC && c == 0x104)))
                    continue;
                c &= 0xff;
            }
        }
        *cc = c;
        condmn.name = pf[i].p;
        condmn.fam = f;
        condmn.cpu = pf[i].cpu;
        condmn.sizes = 0;
        return &condmn;
    }
    return 0;
}

static int szg;         /* the size was written (move.w %sr, ...) */
static int isea(struct op *o, int mask)
{
    int k = o->k == O_ABS ? (o->dsz == 1 ? O_ABSW : O_ABSL) : o->k;
    if (k > O_IMM)
        return 0;
    return (mask >> k) & 1;
}
static int szf(int sz) { return sz == SZ_B ? 0 : sz == SZ_W ? 1 : 2; }
static int isimm(struct op *o, i64 lo, i64 hi) { return o->k == O_IMM && !o->fl && o->d.known && o->d.v >= lo && o->d.v <= hi; }
static int isdn(struct op *o) { return o->k == O_DN; }
static int isan(struct op *o) { return o->k == O_AN; }
static int isrn(struct op *o) { return o->k == O_DN || o->k == O_AN; }
static int rn(struct op *o) { return o->k == O_AN ? o->reg + 8 : o->reg; }
static int bad(void)
{
    err("operands not valid for this instruction");
    dropins = 1;
    return 0;
}
/* a constant operand (#n, or n): its value */
static i64 cval(struct op *o, int *ok)
{
    if ((o->k != O_IMM && o->k != O_ABS) || o->fl || o->d.sec != SEC_ABS) {
        *ok = 0;
        return 0;
    }
    return o->d.v;
}
static void ea_or(struct op *o, int size, int mask)
{
    iw[0] |= ea(o, size, (mask & M(O_PCDISP)) != 0);
}

static int fpfmt(int sz)
{
    switch (sz) {
    case SZ_L: return 0;
    case SZ_S: return 1;
    case SZ_X: return 2;
    case SZ_P: return 3;
    case SZ_W: return 4;
    case SZ_D: return 5;
    case SZ_B: return 6;
    }
    return 2;
}
static int isfp(struct op *o) { return o->k == O_REG && o->reg >= R_FP && o->reg < R_FP + 8; }
static int fpn(struct op *o) { return o->reg - R_FP; }

/* a branch to v from the instruction at the current location: gas's sizes. kind 0: b<cc>/bra/bsr (unsized: a
 * word); 1: j<cc>/jra/jbsr (relaxed: byte, word, long); cc: the condition (0 bra, 1 bsr) */
static void branch(struct op *o, int cc, int sz, int relaxed)
{
    i64 d;
    int isnear, st;
    u32 opc = 0x6000 | cc << 8;
    if (o->k != O_ABS || o->dsz) {
        bad();
        return;
    }
    if (relaxed)
        relax_at(cur->loc);
    isnear = near(&o->d, cur->loc + 2, &d);
    stretch = 0;
    if (!relaxed) {
        if (sz == SZ_B || sz == 's') {
            if (!isnear && o->d.sec != SEC_ABS)
                err("a short branch to a symbol elsewhere");    /* (gas refuses it too: no 8-bit branch relocation) */
            else if (isnear && (d == 0 || d == -1 || !fits8(d)))
                err("branch out of range (%d)", (i32)d);
            w16(opc);
            fx[nfx].v = o->d;
            nfx++;
            fx[nfx - 1].at = 1;
            fx[nfx - 1].n = 1;
            fx[nfx - 1].pc = 1;
            fx[nfx - 1].pcb = 2;
            fx[nfx - 1].imm = 0;
            fx[nfx - 1].defer = 0;
            return;
        }
        if (sz == SZ_L && !(cpu & UP20)) {
            refuse("long branches need a 68020 or higher");
            return;
        }
        w16(opc | (sz == SZ_L ? 0xff : 0));
        addfix(&o->d, sz == SZ_L ? 4 : 2, 1, 2, 0);
        return;
    }
    /* gas's BRANCHBWL: byte, word, then long (on a 68000: a jump) */
    if (!isnear)
        st = 2;
    else
        st = pass <= 2 || (fits8(d) && d != 0) ? 0 : fits16(d) ? 1 : 2;
    /* gas's estimate: a target that begins the next frag holding bytes (only alignment between) takes a word */
    if (isnear && !st && pass >= 2 && o->d.s && o->d.s->defpass != pass && o->d.s->atstart &&
        o->d.s->fixseq == cur->fixseq + 1)
        st = 1;
    stretch = 0;
    st = relax(st);
    cur->frag++, fragnext = 1;
    idefer = 1;
    if (st == 0) {
        w16(opc);
        fx[nfx].v = o->d;
        fx[nfx].at = 1;
        fx[nfx].n = 1;
        fx[nfx].pc = 1;
        fx[nfx].pcb = 2;
        fx[nfx].imm = 0;
        fx[nfx].defer = 1;
        nfx++;
    } else if (st == 1) {
        w16(opc);
        addfix(&o->d, 2, 1, 2, 0);
    } else if (cpu & UP20) {
        w16(opc | 0xff);
        addfix(&o->d, 4, 1, 2, 0);
    } else if (cc < 2) {        /* jmp / jsr (xxx).l */
        w16(cc ? 0x4eb9 : 0x4ef9);
        addfix(&o->d, 4, 0, 0, 0);
    } else {                    /* b!cc.s over a jmp */
        w16((0x6000 | (cc ^ 1) << 8) | 6);
        w16(0x4ef9);
        addfix(&o->d, 4, 0, 0, 0);
    }
    idefer = 0;
}

static void fpu_src(struct op *s, int sz, int op, int dst)
{
    if (isfp(s) && (sz == 0 || sz == SZ_X)) {
        w16(fpn(s) << 10 | dst << 7 | op);
        return;
    }
    if (!sz)
        sz = SZ_X;
    if (!isea(s, EA_DATA) || (s->k == O_DN && sz != SZ_B && sz != SZ_W && sz != SZ_L && sz != SZ_S)) {
        bad();
        return;
    }
    w16(0x4000 | fpfmt(sz) << 10 | dst << 7 | op);
    ea_or(s, sz, EA_DATA);
}

static void instr(const struct mn *m, int cc, int sz, struct op *o, int n)
{
    u32 opc = m->opc;
    int ok = 1;
    i64 v;
    niw = nfx = 0;
    switch (m->fam) {
    case F_NOARG:
        if (n)
            goto bad;
        w16(opc);
        return;
    case F_ADD:
    case F_LOGIC:
    case F_EOR:
    case F_CMP: {
        int isadd = m->fam == F_ADD, f = m->fam;
        if (n != 2)
            goto bad;
        if (f != F_ADD && f != F_CMP && o[1].k == O_REG && (o[1].reg == R_CCR || o[1].reg == R_SR)) {
            if (o[0].k != O_IMM || (szg && sz != (o[1].reg == R_CCR ? SZ_B : SZ_W)))
                goto bad;
            w16((f == F_EOR ? 0x0a00 : opc == 0xc000 ? 0x0200 : 0x0000) | (o[1].reg == R_CCR ? 0x3c : 0x7c));
            imm(&o[0], o[1].reg == R_CCR ? SZ_B : SZ_W);
            return;
        }
        if (isadd && isimm(&o[0], 1, 8) && isea(&o[1], EA_ALT) && !(isan(&o[1]) && sz == SZ_B && opc == 0xd000)) {
            w16((opc == 0xd000 ? 0x5000 : 0x5100) | (o[0].d.v & 7) << 9 | szf(sz) << 6);
            ea_or(&o[1], sz, EA_ALT);
            return;
        }
        if (f == F_CMP && o[0].k == O_POST && o[1].k == O_POST) {
            w16(0xb108 | o[1].reg << 9 | szf(sz) << 6 | o[0].reg);
            return;
        }
        if ((isadd || f == F_CMP) && isan(&o[1])) {
            if (sz == SZ_B)
                goto bad;
            w16((opc | 0xc0 | (sz == SZ_L) << 8) | o[1].reg << 9);
            ea_or(&o[0], sz, EA_ALL);
            return;
        }
        if (o[0].k == O_IMM) {
            int mask = f == F_CMP ? ((cpu & UP20) ? EA_DATA & ~M(O_IMM) : EA_DALT) : EA_DALT;
            if (!isea(&o[1], mask))
                goto bad;
            w16((f == F_ADD ? (opc == 0xd000 ? 0x0600 : 0x0400) : f == F_LOGIC ? (opc == 0xc000 ? 0x0200 : 0)
                 : f == F_EOR ? 0x0a00 : 0x0c00) | szf(sz) << 6);
            imm(&o[0], sz);
            ea_or(&o[1], sz, mask);
            return;
        }
        if (f != F_EOR && isdn(&o[1])) {
            int mask = f == F_LOGIC ? EA_DATA : EA_ALL;
            if (!isea(&o[0], mask) || (isan(&o[0]) && sz == SZ_B))
                goto bad;
            w16(opc | o[1].reg << 9 | szf(sz) << 6);
            ea_or(&o[0], sz, mask);
            return;
        }
        if (f != F_CMP && isdn(&o[0])) {
            int mask = f == F_EOR ? EA_DALT : EA_MALT;
            if (!isea(&o[1], mask))
                goto bad;
            w16((f == F_EOR ? opc : opc | 0x100) | o[0].reg << 9 | szf(sz) << 6);
            ea_or(&o[1], sz, mask);
            return;
        }
        goto bad;
    }
    case F_IMMOP: {
        int mask = opc == 0x0c00 ? ((cpu & UP20) ? EA_DATA & ~M(O_IMM) : EA_DALT) : EA_DALT;
        if (n != 2 || o[0].k != O_IMM)
            goto bad;
        if (o[1].k == O_REG && (o[1].reg == R_CCR || o[1].reg == R_SR) &&
            (opc == 0x0200 || opc == 0 || opc == 0x0a00)) {
            if (szg && sz != (o[1].reg == R_CCR ? SZ_B : SZ_W))
                goto bad;
            w16(opc | (o[1].reg == R_CCR ? 0x3c : 0x7c));
            imm(&o[0], o[1].reg == R_CCR ? SZ_B : SZ_W);
            return;
        }
        if (!isea(&o[1], mask))
            goto bad;
        w16(opc | szf(sz) << 6);
        imm(&o[0], sz);
        ea_or(&o[1], sz, mask);
        return;
    }
    case F_ADDA:
        if (n != 2 || !isan(&o[1]) || sz == SZ_B)
            goto bad;
        w16(opc | (sz == SZ_L) << 8 | o[1].reg << 9);
        ea_or(&o[0], sz, EA_ALL);
        return;
    case F_QUICK:
        if (n != 2 || o[0].k != O_IMM || !isea(&o[1], EA_ALT) || (isan(&o[1]) && sz == SZ_B && opc == 0x5000))
            goto bad;
        v = cval(&o[0], &ok);
        if (!ok || v < 1 || v > 8)
            goto bad;
        w16(opc | (v & 7) << 9 | szf(sz) << 6);
        ea_or(&o[1], sz, EA_ALT);
        return;
    case F_ADDX:
        if (n != 2)
            goto bad;
        if (opc == 0xb108) {    /* cmpm */
            if (o[0].k != O_POST || o[1].k != O_POST)
                goto bad;
            w16(opc | o[1].reg << 9 | szf(sz) << 6 | o[0].reg);
            return;
        }
        if (isdn(&o[0]) && isdn(&o[1]))
            w16(opc | o[1].reg << 9 | (opc & 0x4000 ? 0 : 0) | szf(sz) << 6 | o[0].reg);
        else if (o[0].k == O_PRE && o[1].k == O_PRE)
            w16(opc | 8 | o[1].reg << 9 | szf(sz) << 6 | o[0].reg);
        else
            goto bad;
        if (opc == 0xc100 || opc == 0x8100)     /* abcd, sbcd: no size bits */
            iw[0] &= ~0xc0;
        return;
    case F_MOVE:
        if (n != 2)
            goto bad;
        if (((o[0].k == O_REG && (o[0].reg == R_CCR || o[0].reg == R_SR)) ||
             (o[1].k == O_REG && (o[1].reg == R_CCR || o[1].reg == R_SR))) && szg && sz != SZ_W)
            goto bad;
        if (((o[0].k == O_REG && o[0].reg == R_USP) || (o[1].k == O_REG && o[1].reg == R_USP)) && szg && sz != SZ_L)
            goto bad;
        if (o[1].k == O_REG && o[1].reg == R_CCR) {
            if (!isea(&o[0], EA_DATA))
                goto bad;
            w16(0x44c0);
            ea_or(&o[0], SZ_W, EA_DATA);
            return;
        }
        if (o[1].k == O_REG && o[1].reg == R_SR) {
            if (!isea(&o[0], EA_DATA))
                goto bad;
            w16(0x46c0);
            ea_or(&o[0], SZ_W, EA_DATA);
            return;
        }
        if (o[0].k == O_REG && (o[0].reg == R_SR || o[0].reg == R_CCR)) {
            if (!isea(&o[1], EA_DALT) || (o[0].reg == R_CCR && !(cpu & UP10)))
                goto bad;
            w16(o[0].reg == R_SR ? 0x40c0 : 0x42c0);
            ea_or(&o[1], SZ_W, EA_DALT);
            return;
        }
        if (o[0].k == O_REG && o[0].reg == R_USP && isan(&o[1])) {
            w16(0x4e68 | o[1].reg);
            return;
        }
        if (o[1].k == O_REG && o[1].reg == R_USP && isan(&o[0])) {
            w16(0x4e60 | o[0].reg);
            return;
        }
        if (isan(&o[1])) {
    movea:
            if (sz == SZ_B || !isea(&o[0], EA_ALL))
                goto bad;
            w16((sz == SZ_L ? 0x2040 : 0x3040) | o[1].reg << 9);
            ea_or(&o[0], sz, EA_ALL);
            return;
        }
        if (sz == SZ_L && isdn(&o[1]) && o[0].k == O_IMM && !o[0].fl && o[0].d.known && o[0].d.v >= -0x80000000LL &&
            o[0].d.v <= 0xffffffffLL && (i32)o[0].d.v >= -128 && (i32)o[0].d.v <= 127) {   /* (32 bits, as gas) */
            w16(0x7000 | o[1].reg << 9 | (o[0].d.v & 0xff));
            return;
        }
        if (!isea(&o[0], EA_ALL) || !isea(&o[1], EA_DALT) || (isan(&o[0]) && sz == SZ_B))
            goto bad;
        w16((sz == SZ_B ? 0x1000 : sz == SZ_W ? 0x3000 : 0x2000));
        iw[0] |= ea(&o[0], sz, 1);
        {
            int d = ea(&o[1], sz, 0);
            iw[0] |= (d & 7) << 9 | (d >> 3) << 6;
        }
        return;
    case F_MOVEA:
        if (n != 2 || !isan(&o[1]))
            goto bad;
        goto movea;
    case F_MOVEQ:
        if (n != 2 || !isdn(&o[1]) || o[0].k != O_IMM)
            goto bad;
        w16(0x7000 | o[1].reg << 9);
        if (o[0].d.sec == SEC_ABS) {
            v = o[0].d.v;
            if (v < -0x80000000LL || v > 0xffffffffLL || (i32)v < -128 || (i32)v > 127)
                refuse("value out of range");
            iw[0] |= v & 0xff;
        } else
            err("moveq needs a number");
        return;
    case F_MOVEM: {
        int tomem, mask, ri, r;
        struct op *l, *e;
        if (n != 2)
            goto bad;
        tomem = o[0].k == O_LIST || isrn(&o[0]) || (o[0].k == O_IMM && o[1].k != O_LIST && !isrn(&o[1]));
        l = tomem ? &o[0] : &o[1];
        e = tomem ? &o[1] : &o[0];
        if (l->k == O_LIST && l->lk == 0)
            mask = l->mask;
        else if (isrn(l))
            mask = 1 << rn(l);
        else if (l->k == O_IMM && l->d.sec == SEC_ABS && l->d.v >= -32768 && l->d.v <= 0xffff)
            mask = l->d.v & 0xffff;
        else
            goto bad;
        if (tomem ? !(isea(e, EA_CALT) || e->k == O_PRE) : !(isea(e, EA_CTL) || e->k == O_POST))
            goto bad;
        if (e->k == O_PRE && l->k != O_IMM) {
            for (r = ri = 0; ri < 16; ri++)
                if (mask & 1 << ri)
                    r |= 1 << (15 - ri);
            mask = r;
        }
        w16((tomem ? 0x4880 : 0x4c80) | (sz == SZ_L) << 6);
        w16(mask);
        ea_or(e, sz, tomem ? EA_CALT : EA_CTL);
        return;
    }
    case F_MOVEP: {
        struct op *mem;
        int tomem;
        if (n != 2)
            goto bad;
        tomem = isdn(&o[0]);
        mem = tomem ? &o[1] : &o[0];
        if (!isdn(tomem ? &o[0] : &o[1]) || mem->k != O_DISP || mem->dsz == 2)
            goto bad;
        w16(opc | (tomem ? o[0].reg : o[1].reg) << 9 | tomem << 7 | (sz == SZ_L) << 6 | mem->reg);
        addfix(&mem->d, 2, 0, 0, 0);
        return;
    }
    case F_MOVEC: {
        struct op *c, *r;
        if (n != 2)
            goto bad;
        c = isrn(&o[0]) ? &o[1] : &o[0];
        r = c == &o[0] ? &o[1] : &o[0];
        if (!isrn(r))
            goto bad;
        if (c->k == O_REG && c->reg == R_USP)
            v = 0x800;
        else if (c->k == O_REG && c->reg >= R_CREG && c->reg < R_CREG + 21 && (cregcpu[c->reg - R_CREG] & cpu))
            v = creg[c->reg - R_CREG];
        else if (c->k == O_IMM && c->d.sec == SEC_ABS)
            v = c->d.v >= 0 && c->d.v <= 0xfff ? c->d.v : 0;   /* a control register by its number (gas: 0 if not) */
        else
            goto bad;
        w16(c == &o[0] ? 0x4e7a : 0x4e7b);
        w16(rn(r) << 12 | v);
        return;
    }
    case F_MOVES: {
        int tomem = isrn(&o[0]);
        struct op *e = tomem ? &o[1] : &o[0];
        if (n != 2 || !isrn(tomem ? &o[0] : &o[1]) || !isea(e, EA_MALT))
            goto bad;
        w16(opc | szf(sz) << 6);
        w16(rn(tomem ? &o[0] : &o[1]) << 12 | tomem << 11);
        ea_or(e, sz, EA_MALT);
        return;
    }
    case F_UNARY: {
        int mask = EA_DALT;
        if (n != 1 || !isea(&o[0], mask))
            goto bad;
        w16(opc | (opc == 0x4800 || opc == 0x4ac0 ? 0 : szf(sz) << 6));
        ea_or(&o[0], sz, mask);
        return;
    }
    case F_TST: {
        int mask = (cpu & UP20) ? EA_ALL : EA_DALT;
        if (n != 1 || !isea(&o[0], mask) || (isan(&o[0]) && sz == SZ_B))
            goto bad;
        w16(opc | szf(sz) << 6);
        ea_or(&o[0], sz, mask);
        return;
    }
    case F_SHIFT:
        if (n == 2 && isdn(&o[1]) && isdn(&o[0])) {
            w16(0xe020 | o[0].reg << 9 | (opc & 0x100) | szf(sz) << 6 | (opc & 0x18) | o[1].reg);
            return;
        }
        if (n == 2 && isdn(&o[1]) && o[0].k == O_IMM) {
            v = cval(&o[0], &ok);
            if (!ok || v < 1 || v > 8)
                goto bad;
            w16(0xe000 | (v & 7) << 9 | (opc & 0x100) | szf(sz) << 6 | (opc & 0x18) | o[1].reg);
            return;
        }
        if (n == 1 && isea(&o[0], EA_MALT) && sz == SZ_W) {
            w16(0xe0c0 | (opc & 0x18) << 6 | (opc & 0x100));
            ea_or(&o[0], sz, EA_MALT);
            return;
        }
        goto bad;
    case F_BIT: {
        int mask = opc == 0 ? EA_DATA : EA_DALT;
        if (n != 2)
            goto bad;
        if (isdn(&o[0])) {
            if (!isea(&o[1], mask))
                goto bad;
            w16(0x0100 | o[0].reg << 9 | opc << 6);
            ea_or(&o[1], SZ_B, mask);
            return;
        }
        if (o[0].k != O_IMM || !isea(&o[1], mask & ~M(O_IMM)))
            goto bad;
        w16(0x0800 | opc << 6);
        imm(&o[0], SZ_W);
        ea_or(&o[1], SZ_B, mask & ~M(O_IMM));
        return;
    }
    case F_BF: {
        struct op *e = &o[0], *d = 0;
        int mask = (opc == 0xe8c0 || opc == 0xe9c0 || opc == 0xebc0 || opc == 0xedc0) ?
                   (EA_CTL | M(O_DN)) : (EA_CALT | M(O_DN));
        u32 x;
        if (opc == 0xefc0) {    /* bfins %dn,<ea>{o:w} */
            if (n != 2 || !isdn(&o[0]))
                goto bad;
            d = &o[0];
            e = &o[1];
        } else if (opc == 0xe9c0 || opc == 0xebc0 || opc == 0xedc0) {
            if (n != 2 || !isdn(&o[1]))
                goto bad;
            d = &o[1];
        } else if (n != 1)
            goto bad;
        if (!e->bf || !isea(e, mask))
            goto bad;
        x = (d ? d->reg << 12 : 0);
        if (e->bfod)
            x |= 0x800 | (e->bfo.v & 7) << 6;
        else if (e->bfo.v < 0 || e->bfo.v > 32)
            goto bad;
        else
            x |= (e->bfo.v & 31) << 6;
        if (e->bfwd)
            x |= 0x20 | (e->bfw.v & 7);
        else if (e->bfw.v < 0 || e->bfw.v > 32)
            goto bad;
        else
            x |= e->bfw.v & 31;
        w16(opc);
        w16(x);
        ea_or(e, SZ_L, mask);
        return;
    }
    case F_EXT:
        if (n != 1 || !isdn(&o[0]))
            goto bad;
        if (opc == 0x49c0 && sz == SZ_L) {
            if (!(cpu & UP20))
                goto bad;
            w16(0x49c0 | o[0].reg);
        }
        else
            w16((sz == SZ_L ? 0x48c0 : 0x4880) | o[0].reg);
        return;
    case F_SWAP:
        if (n != 1 || !isdn(&o[0]))
            goto bad;
        w16(opc | o[0].reg);
        return;
    case F_EXG:
        if (n != 2 || !isrn(&o[0]) || !isrn(&o[1]))
            goto bad;
        if (isdn(&o[0]) && isdn(&o[1]))
            w16(0xc140 | o[0].reg << 9 | o[1].reg);
        else if (isan(&o[0]) && isan(&o[1]))
            w16(0xc148 | o[0].reg << 9 | o[1].reg);
        else if (isdn(&o[0]))
            w16(0xc188 | o[0].reg << 9 | o[1].reg);
        else
            w16(0xc188 | o[1].reg << 9 | o[0].reg);
        return;
    case F_EA1:
        if (n != 1 || !isea(&o[0], EA_CTL))
            goto bad;
        w16(opc);
        ea_or(&o[0], SZ_L, EA_CTL);
        return;
    case F_LEA:
        if (n != 2 || !isan(&o[1]) || !isea(&o[0], EA_CTL))
            goto bad;
        w16(opc | o[1].reg << 9);
        ea_or(&o[0], SZ_L, EA_CTL);
        return;
    case F_LINK:
        if (n != 2 || !isan(&o[0]) || o[1].k != O_IMM)
            goto bad;
        if (sz == SZ_L) {
            if (!(cpu & UP20))
                goto bad;
            w16(0x4808 | o[0].reg);
            imm(&o[1], SZ_L);
        } else {
            w16(0x4e50 | o[0].reg);
            addfix(&o[1].d, 2, 0, 0, 0);
        }
        return;
    case F_UNLK:
        if (n != 1 || !isan(&o[0]))
            goto bad;
        w16(opc | o[0].reg);
        return;
    case F_IMM16:
        if (n != 1 || o[0].k != O_IMM)
            goto bad;
        w16(opc);
        imm(&o[0], SZ_W);
        return;
    case F_TRAP:
    case F_BKPT:
        if (n != 1 || o[0].k != O_IMM)
            goto bad;
        v = cval(&o[0], &ok);
        if (!ok || v < 0 || v > (m->fam == F_TRAP ? 15 : 7)) {
            if (!ok || m->fam == F_BKPT)
                goto bad;
            warn("trap number out of range: 0");     /* (as gas) */
            v = 0;
        }
        w16(opc | v);
        return;
    case F_CHK:
        if (n != 2 || !isdn(&o[1]) || !isea(&o[0], EA_DATA) || (sz == SZ_L && !(cpu & UP20)))
            goto bad;
        w16((sz == SZ_L ? 0x4100 : 0x4180) | o[1].reg << 9);
        ea_or(&o[0], sz, EA_DATA);
        return;
    case F_CHK2:
        if (n != 2 || !isrn(&o[1]) || !isea(&o[0], EA_CTL))
            goto bad;
        w16(0x00c0 | szf(sz) << 9);
        w16(rn(&o[1]) << 12 | opc);
        ea_or(&o[0], sz, EA_CTL);
        return;
    case F_CAS:
        if (n != 3 || !isdn(&o[0]) || !isdn(&o[1]) || !isea(&o[2], EA_MALT))
            goto bad;
        w16(0x08c0 | (szf(sz) + 1) << 9);
        w16(o[1].reg << 6 | o[0].reg);
        ea_or(&o[2], sz, EA_MALT);
        return;
    case F_CAS2:
        if (n != 3 || o[0].k != O_PAIR || o[1].k != O_PAIR || o[2].k != O_IPAIR || o[0].reg > 7 || o[0].reg2 > 7 ||
            o[1].reg > 7 || o[1].reg2 > 7)
            goto bad;
        w16(sz == SZ_L ? 0x0efc : 0x0cfc);
        w16(o[2].reg << 12 | o[1].reg << 6 | o[0].reg);
        w16(o[2].reg2 << 12 | o[1].reg2 << 6 | o[0].reg2);
        return;
    case F_MUL:
    case F_DIV:
        if (n != 2 || !isea(&o[0], EA_DATA))
            goto bad;
        if (sz != SZ_L) {
            if (!isdn(&o[1]))
                goto bad;
            w16(opc | o[1].reg << 9);
            ea_or(&o[0], SZ_W, EA_DATA);
            return;
        }
        if (!(cpu & UP20))
            goto bad;
        w16(m->fam == F_MUL ? 0x4c00 : 0x4c40);
        if (isdn(&o[1]))    /* mul.l: Dh 0; div.l: Dr = Dq */
            w16(o[1].reg << 12 | (opc & 0x100) << 3 | (m->fam == F_DIV ? o[1].reg : 0));
        else if (o[1].k == O_PAIR && o[1].reg < 8 && o[1].reg2 < 8)
            w16(o[1].reg2 << 12 | (opc & 0x100) << 3 | 0x400 | o[1].reg);
        else
            goto bad;
        ea_or(&o[0], SZ_L, EA_DATA);
        return;
    case F_DIVL:
        if (n != 2 || !isea(&o[0], EA_DATA))
            goto bad;
        w16(0x4c40);
        if (isdn(&o[1]))
            w16(o[1].reg << 12 | opc | o[1].reg);
        else if (o[1].k == O_PAIR && o[1].reg < 8 && o[1].reg2 < 8)
            w16(o[1].reg2 << 12 | opc | o[1].reg);
        else
            goto bad;
        ea_or(&o[0], SZ_L, EA_DATA);
        return;
    case F_PACK:
        if (n != 3 || o[2].k != O_IMM)
            goto bad;
        if (isdn(&o[0]) && isdn(&o[1]))
            w16(opc | o[1].reg << 9 | o[0].reg);
        else if (o[0].k == O_PRE && o[1].k == O_PRE)
            w16(opc | 8 | o[1].reg << 9 | o[0].reg);
        else
            goto bad;
        imm(&o[2], SZ_W);
        return;
    case F_CALLM:
        if (n != 2 || o[0].k != O_IMM || !isea(&o[1], EA_CTL))
            goto bad;
        w16(opc);
        imm(&o[0], SZ_W);
        ea_or(&o[1], SZ_L, EA_CTL);
        return;
    case F_RTM:
        if (n != 1 || !isrn(&o[0]))
            goto bad;
        w16(opc | rn(&o[0]));
        return;
    case F_MOVE16:
        if (n != 2)
            goto bad;
        if (o[0].k == O_POST && o[1].k == O_POST) {
            w16(0xf620 | o[0].reg);
            w16(0x8000 | o[1].reg << 12);
            return;
        }
        if ((o[0].k == O_POST || o[0].k == O_IND) && o[1].k == O_ABS) {
            w16(0xf600 | (o[0].k == O_IND ? 0x10 : 0) | o[0].reg);
            addfix(&o[1].d, 4, 0, 0, 0);
            return;
        }
        if (o[0].k == O_ABS && (o[1].k == O_POST || o[1].k == O_IND)) {
            w16(0xf608 | (o[1].k == O_IND ? 0x10 : 0) | o[1].reg);
            addfix(&o[0].d, 4, 0, 0, 0);
            return;
        }
        goto bad;
    case F_CACHE: {
        int c;
        if (n < 1 || o[0].k != O_REG || o[0].reg < R_CACHE || o[0].reg > R_CACHE + 3)
            goto bad;
        c = o[0].reg - R_CACHE;
        if ((opc & 0x18) == 0x18) {
            if (n != 1)
                goto bad;
            w16(opc | c << 6);
        } else {
            if (n != 2 || o[1].k != O_IND)
                goto bad;
            w16(opc | c << 6 | o[1].reg);
        }
        return;
    }
    case F_PFL40:
        if (n == 1 && o[0].k == O_AN)      /* (gas takes %an for (%an)) */
            o[0].k = O_IND;
        if (opc == 0xf510) {
            if (n)
                goto bad;
            w16(opc);
        } else {
            if (n != 1 || o[0].k != O_IND)
                goto bad;
            w16(opc | o[0].reg);
        }
        return;
    case F_PFLUSHA:
        if (n)
            goto bad;
        if (cpu & (C40 | C60))
            w16(0xf518);
        else {
            w16(0xf000);
            w16(0x2400);
        }
        return;
    case F_PLPA:
        if (n != 1 || o[0].k != O_IND)
            goto bad;
        w16(opc | o[0].reg);
        return;
    case F_LPSTOP:
        if (n != 1 || o[0].k != O_IMM)
            goto bad;
        w16(0xf800);
        w16(0x01c0);
        imm(&o[0], SZ_W);
        return;
    case F_PFLUSH:
    case F_PLOAD:
    case F_PTEST: {
        /* the 68030's forms: function code, mask, <ea> */
        int fc;
        if (n == 1 && o[0].k == O_AN && m->fam == F_PFLUSH)      /* (gas takes %an for (%an)) */
            o[0].k = O_IND;
        if (m->fam == F_PFLUSH && (cpu & (C40 | C60))) {
            if (n != 1 || o[0].k != O_IND)
                goto bad;
            w16(0xf508 | o[0].reg);
            return;
        }
        if (m->fam == F_PTEST && (cpu & (C40 | C60))) {     /* (the 68040's: ptestr (%an) only) */
            if (n != 1 || o[0].k != O_IND)
                goto bad;
            w16(opc | o[0].reg);
            return;
        }
        if (n < 2)
            goto bad;
        if (o[0].k == O_REG && o[0].reg == CR_SFC)
            fc = 0;
        else if (o[0].k == O_REG && o[0].reg == CR_DFC)
            fc = 1;
        else if (isdn(&o[0]))
            fc = 8 | o[0].reg;
        else if (o[0].k == O_IMM && o[0].d.sec == SEC_ABS) {
            fc = 0x10 | (o[0].d.v >= 0 && o[0].d.v <= 15 ? o[0].d.v : 0);
            if (o[0].d.v < 0 || o[0].d.v > 15)
                warn("function code out of range: 0");    /* (as gas) */
        } else
            goto bad;
        if (m->fam == F_PLOAD) {
            if (n != 2 || !isea(&o[1], EA_CALT))
                goto bad;
            w16(0xf000);
            w16(opc | fc);
            ea_or(&o[1], SZ_L, EA_CALT);
            return;
        }
        if (m->fam == F_PFLUSH) {
            if (o[1].k != O_IMM || o[1].d.sec != SEC_ABS || o[1].d.v < 0 || o[1].d.v > 15 || n > 3)
                goto bad;
            w16(0xf000);
            if (n == 2) {
                w16(0x3000 | o[1].d.v << 5 | fc);
                return;
            }
            if (!isea(&o[2], EA_CALT))
                goto bad;
            w16(0x3800 | o[1].d.v << 5 | fc);
            ea_or(&o[2], SZ_L, EA_CALT);
            return;
        }
        /* ptestr/ptestw fc,<ea>,#level[,%an] */
        if (n < 3 || n > 4 || !isea(&o[1], EA_CALT) || o[2].k != O_IMM || o[2].d.sec != SEC_ABS ||
            o[2].d.v < 0 || o[2].d.v > 7 || (n == 4 && !isan(&o[3])))
            goto bad;
        w16(0xf000);
        w16(0x8000 | o[2].d.v << 10 | (opc == 0xf568) << 9 | (n == 4 ? 0x100 | o[3].reg << 5 : 0) | fc);
        ea_or(&o[1], SZ_L, EA_CALT);
        return;
    }
    case F_PMOVE: {
        struct op *r, *e;
        int tomem, x;
        if (n != 2)
            goto bad;
        tomem = o[0].k == O_REG;
        r = tomem ? &o[0] : &o[1];
        e = tomem ? &o[1] : &o[0];
        if (r->k != O_REG)
            goto bad;
        switch (r->reg) {      /* (the 68851 on a 68020: tc, srp, crp, psr; the 68030 also tt0, tt1) */
        case CR_TC: x = 0x4000; break;
        case CR_SRP: x = 0x4800; break;
        case R_CRP: x = 0x4c00; break;
        case R_TT0: x = 0x0800; break;
        case R_TT1: x = 0x0c00; break;
        case R_PSR: x = 0x6000; break;
        default: goto bad;
        }
        if (((x == 0x0800 || x == 0x0c00) && !(cpu & C30)) || ((opc & 0x100) && (x == 0x6000 || tomem)))
            goto bad;
        {   /* (srp and crp are 64 bits: memory only) */
            int em = tomem ? EA_ALT : EA_ALL;
            if (x == 0x4800 || x == 0x4c00)
                em &= EA_MEM;
            if (!isea(e, em))
                goto bad;
            w16(0xf000);
            w16(x | tomem << 9 | (opc & 0x100));
            ea_or(e, SZ_L, em);
        }
        return;
    }
    case F_BCC:
    case F_JCC:
        if (n != 1)
            goto bad;
        if (m->fam == F_JCC && cc < 2 && o[0].k != O_ABS) {    /* jra (%a0): jmp; jbsr 4(%a1): jsr (gas) */
            if (!isea(&o[0], EA_CTL))
                goto bad;
            w16(cc ? 0x4e80 : 0x4ec0);
            ea_or(&o[0], SZ_L, EA_CTL);
            return;
        }
        if (m->fam == F_JCC && sz)
            goto bad;
        branch(&o[0], cc, sz, m->fam == F_JCC && !sz);
        return;
    case F_DBCC: {
        i64 d;
        int st;
        if (n != 2 || !isdn(&o[0]) || o[1].k != O_ABS)
            goto bad;
        w16(0x50c8 | cc << 8 | o[0].reg);
        if (o[1].d.sec == SEC_ABS) {
            addfix(&o[1].d, 2, 1, 2, 0);
            return;
        }
        /* gas's DBCC: dbcc %dn,.+6; bra.s .+8; bra.l target (jmp on a 68000) when out of reach */
        relax_at(cur->loc);
        st = !near(&o[1].d, cur->loc + 2, &d) || (pass > 2 && !fits16(d));
        stretch = 0;
        st = relax(st);
        cur->frag++, fragnext = 1;
        idefer = 1;
        if (!st)
            addfix(&o[1].d, 2, 1, 2, 0);
        else {
            w16(4);
            w16(0x6006);
            if (cpu & UP20) {
                w16(0x60ff);
                addfix(&o[1].d, 4, 1, niw * 2, 0);
            } else {
                w16(0x4ef9);
                addfix(&o[1].d, 4, 0, 0, 0);
            }
        }
        idefer = 0;
        return;
    }
    case F_SCC:
        if (n != 1 || !isea(&o[0], EA_DALT))
            goto bad;
        w16(0x50c0 | cc << 8);
        ea_or(&o[0], SZ_B, EA_DALT);
        return;
    case F_TRAPCC:
        if (n == 0 && !sz) {
            w16(0x50fc | cc << 8);
            return;
        }
        if (n != 1 || o[0].k != O_IMM || !sz)
            goto bad;
        if (sz == SZ_L) {
            w16(0x50fb | cc << 8);
            imm(&o[0], SZ_L);
        } else {
            w16(0x50fa | cc << 8);
            imm(&o[0], SZ_W);
        }
        return;
    /* ---- the FPU ---- */
    case F_FOP:
        if (n == 1 && isfp(&o[0]) && (!sz || sz == SZ_X) && (m->sizes & SN)) {
            w16(0xf200);
            w16(fpn(&o[0]) << 10 | fpn(&o[0]) << 7 | opc);
            return;
        }
        if (n != 2 || !isfp(&o[1]))
            goto bad;
        w16(0xf200);
        fpu_src(&o[0], sz, opc, fpn(&o[1]));
        return;
    case F_FTST:
        if (n != 1)
            goto bad;
        w16(0xf200);
        fpu_src(&o[0], sz, opc, 0);
        return;
    case F_FSINCOS:
        if (n != 2 || o[1].k != O_PAIR || o[1].reg < R_FP)
            goto bad;
        w16(0xf200);
        fpu_src(&o[0], sz, opc | (o[1].reg - R_FP), o[1].reg2 - R_FP);
        return;
    case F_FMOVE:
        if (n != 2)
            goto bad;
        if (o[1].k == O_REG && o[1].reg >= R_FPCR && o[1].reg <= R_FPIAR) {
            int mask = o[1].reg == R_FPIAR ? EA_ALL : EA_DATA;
            if ((sz && sz != SZ_L) || !isea(&o[0], mask))
                goto bad;
            w16(0xf200);
            w16(0x8000 | (o[1].reg == R_FPCR ? 4 : o[1].reg == R_FPSR ? 2 : 1) << 10);
            ea_or(&o[0], SZ_L, mask);
            return;
        }
        if (o[0].k == O_REG && o[0].reg >= R_FPCR && o[0].reg <= R_FPIAR) {
            int mask = o[0].reg == R_FPIAR ? EA_ALT : EA_DALT;
            if ((sz && sz != SZ_L) || !isea(&o[1], mask))
                goto bad;
            w16(0xf200);
            w16(0xa000 | (o[0].reg == R_FPCR ? 4 : o[0].reg == R_FPSR ? 2 : 1) << 10);
            ea_or(&o[1], SZ_L, mask);
            return;
        }
        if (isfp(&o[1])) {
            w16(0xf200);
            fpu_src(&o[0], sz, 0, fpn(&o[1]));
            return;
        }
        if (isfp(&o[0])) {     /* fmove %fpn,<ea> */
            if (!sz)
                sz = SZ_X;
            if (!isea(&o[1], EA_DALT) || (isdn(&o[1]) && sz != SZ_B && sz != SZ_W && sz != SZ_L && sz != SZ_S))
                goto bad;
            w16(0xf200);
            w16(0x6000 | fpfmt(sz) << 10 | fpn(&o[0]) << 7);
            if (sz == SZ_P) {
                if (o[1].bf != 2)
                    goto bad;       /* (gas: a packed store needs its k-factor) */
                if (o[1].bf == 2 && o[1].bfod)
                    iw[1] |= 0x1c00 | o[1].bfo.v << 4;  /* the k-factor in %dn: format 7 */
                else
                    iw[1] |= (o[1].bf == 2 ? o[1].bfo.v : 17) & 0x7f;
            } else if (o[1].bf)
                goto bad;
            ea_or(&o[1], sz, EA_DALT);
            return;
        }
        goto bad;
    case F_FMOVECR:
        if (n != 2 || o[0].k != O_IMM || !isfp(&o[1]))
            goto bad;
        v = cval(&o[0], &ok);
        if (!ok)
            goto bad;
        w16(0xf200);
        w16(0x5c00 | fpn(&o[1]) << 7 | (v & 0x7f));
        return;
    case F_FMOVEM: {
        int tomem, mode, mask, i;
        struct op *l, *e;
        if (n != 2)
            goto bad;
        tomem = o[0].k == O_LIST || (o[0].k == O_REG && (isfp(&o[0]) || (o[0].reg >= R_FPCR && o[0].reg <= R_FPIAR))) ||
                ((isdn(&o[0]) || o[0].k == O_IMM) && !(o[1].k == O_LIST || o[1].k == O_REG));
        l = tomem ? &o[0] : &o[1];
        e = tomem ? &o[1] : &o[0];
        if ((l->k == O_LIST && l->lk == 2) || (l->k == O_REG && l->reg >= R_FPCR && l->reg <= R_FPIAR)) {
            int cm = l->k == O_LIST ? l->mask : l->reg == R_FPCR ? 4 : l->reg == R_FPSR ? 2 : 1;
            int one = cm == 1 || cm == 2 || cm == 4;
            int emask = tomem ? (one ? EA_ALT : EA_MALT) : EA_ALL;    /* (gas's classes) */
            if ((sz && sz != SZ_L) || !isea(e, emask))
                goto bad;
            w16(0xf200);
            w16((tomem ? 0xa000 : 0x8000) | cm << 10);
            ea_or(e, SZ_L, emask);
            return;
        }
        if (sz && sz != SZ_X)
            goto bad;
        if (isdn(l)) {
            if (sz != SZ_X)
                goto bad;       /* (gas: a list in %dn only with .x) */
            mode = 1;
            mask = l->reg << 4;
        } else {
            if (l->k == O_LIST && l->lk == 1)
                mask = l->mask;
            else if (isfp(l))
                mask = 1 << fpn(l);
            else if (l->k == O_IMM && (l->fl || l->d.sec == SEC_ABS))
                mask = -1, mode = 0;
            else
                goto bad;
            mode = 0;
            if (mask >= 0 && e->k != O_PRE) {     /* fp0 is bit 7 except in the predecrement mode */
                int r = 0;
                for (i = 0; i < 8; i++)
                    if (mask & 1 << i)
                        r |= 0x80 >> i;
                mask = r;
            }
            if (mask < 0)
                mask = l->fl ? 0 : l->d.v & 0xff;
        }
        /* (gas takes only control alterable modes both ways: no PC-relative) */
        if (tomem ? !(isea(e, EA_CALT) || e->k == O_PRE) : !(isea(e, EA_CALT) || e->k == O_POST))
            goto bad;
        if (e->k != O_PRE)
            mode |= 2;
        w16(0xf200);
        w16(0xc000 | tomem << 13 | mode << 11 | mask);
        ea_or(e, SZ_X, EA_CALT);
        return;
    }
    case F_FBCC: {
        if (n != 1 || o[0].k != O_ABS)
            goto bad;
        if (sz == SZ_L) {
            w16(0xf2c0 | cc);
            addfix(&o[0].d, sz == SZ_L ? 4 : 2, 1, 2, 0);
            return;
        }
        if (sz)
            goto bad;
        w16(0xf280 | cc);       /* unsized: a word (gas never relaxes fb<cc>) */
        addfix(&o[0].d, 2, 1, 2, 0);
        return;
    }
    case F_FDBCC:
        if (n != 2 || !isdn(&o[0]) || o[1].k != O_ABS)
            goto bad;
        w16(0xf248 | o[0].reg);
        w16(cc);
        addfix(&o[1].d, 2, 1, 4, 0);
        return;
    case F_FSCC:
        if (n != 1 || !isea(&o[0], EA_DALT))
            goto bad;
        w16(0xf240);
        w16(cc);
        ea_or(&o[0], SZ_B, EA_DALT);
        return;
    case F_FTRAPCC:
        if (n == 0 && !sz) {
            w16(0xf27c);
            w16(cc);
            return;
        }
        if (n != 1 || o[0].k != O_IMM || !sz)
            goto bad;
        w16(sz == SZ_L ? 0xf27b : 0xf27a);
        w16(cc);
        imm(&o[0], sz == SZ_L ? SZ_L : SZ_W);
        return;
    case F_FNOP:
        if (n)
            goto bad;
        w16(0xf280);
        w16(0);
        return;
    case F_FSAVE:
        if (n != 1 || !(opc == 0xf300 ? (isea(&o[0], EA_CALT) || o[0].k == O_PRE) : (isea(&o[0], EA_CTL) || o[0].k == O_POST)))
            goto bad;
        w16(opc);
        ea_or(&o[0], SZ_L, opc == 0xf300 ? EA_CALT : EA_CTL);
        return;
    }
bad:
    niw = nfx = 0;
    bad();
}

/* ---- floating-point constants, with integers only: decimal text to IEEE single, double and the 68881's
 * extended format, correctly rounded (to nearest, ties to even) ---- */
#define BNL 600
struct bn {
    int n;
    u32 d[BNL];
};
static struct bn bnum, bden;
static void bn_mul(struct bn *a, u32 m, u32 add)
{
    u64 c = add;
    int i;
    for (i = 0; i < a->n; i++) {
        c += (u64)a->d[i] * m;
        a->d[i] = (u32)c;
        c >>= 32;
    }
    if (c) {
        if (a->n == BNL)
            die("number too large");
        a->d[a->n++] = (u32)c;
    }
}
static int bn_bits(struct bn *a)
{
    int b = 0;
    u32 t;
    if (!a->n)
        return 0;
    for (t = a->d[a->n - 1]; t; t >>= 1)
        b++;
    return (a->n - 1) * 32 + b;
}
static void bn_shl(struct bn *a, int s)
{
    int w = s / 32, b = s % 32, i;
    if (!a->n || s <= 0)
        return;
    if (a->n + w + 1 > BNL)
        die("number too large");
    a->d[a->n + w] = 0;
    for (i = a->n - 1 + w + 1; i >= w; i--) {
        u32 hi = i - w < a->n ? a->d[i - w] : 0, lo = i - w - 1 >= 0 ? a->d[i - w - 1] : 0;
        a->d[i] = b ? (hi << b | lo >> (32 - b)) : hi;
    }
    for (i = 0; i < w; i++)
        a->d[i] = 0;
    a->n += w + 1;
    while (a->n && !a->d[a->n - 1])
        a->n--;
}
static void bn_shr1(struct bn *a)
{
    int i;
    for (i = 0; i < a->n; i++)
        a->d[i] = a->d[i] >> 1 | (i + 1 < a->n ? a->d[i + 1] << 31 : 0);
    while (a->n && !a->d[a->n - 1])
        a->n--;
}
static int bn_cmp(struct bn *a, struct bn *b)
{
    int i;
    if (a->n != b->n)
        return a->n < b->n ? -1 : 1;
    for (i = a->n - 1; i >= 0; i--)
        if (a->d[i] != b->d[i])
            return a->d[i] < b->d[i] ? -1 : 1;
    return 0;
}
static void bn_sub(struct bn *a, struct bn *b)
{
    i64 c = 0;
    int i;
    for (i = 0; i < a->n; i++) {
        c += (i64)a->d[i] - (i < b->n ? b->d[i] : 0);
        a->d[i] = (u32)c;
        c = c < 0 ? -1 : 0;
    }
    while (a->n && !a->d[a->n - 1])
        a->n--;
}
static struct bn bd2, bn2;
/* q = floor(num * 2^s / den), as up to 96 bits in q[0..2] (q[0] the high word); returns whether a remainder is left */
static int bn_div(int s, u32 *q, int qbits)
{
    int i;
    bn2 = bnum;
    bd2 = bden;
    if (s > 0)
        bn_shl(&bn2, s);
    else
        bn_shl(&bd2, -s);
    bn_shl(&bd2, qbits - 1);
    q[0] = q[1] = q[2] = 0;
    for (i = qbits - 1; i >= 0; i--) {
        if (bn_cmp(&bn2, &bd2) >= 0) {
            bn_sub(&bn2, &bd2);
            q[2 - i / 32] |= 1u << (i % 32);
        }
        bn_shr1(&bd2);
    }
    return bn2.n != 0;
}
/* kind 4: single, 8: double, 12: extended; out: the big-endian bytes */
static int fltconv(const char *t, int kind, u8 *out)
{
    const char *p = t;
    int neg = 0, e = 0, any = 0, P = kind == 4 ? 24 : kind == 8 ? 53 : 64;
    int bias = kind == 4 ? 127 : kind == 8 ? 1023 : 16383, emin = kind == 12 ? -16383 : 1 - bias, emax = bias, E, s, i, rem;
    u32 q[3];
    u64 m;
    int guard, sticky;
    if (*p == '0' && p[1] && strchr("rRfFdDxXsSeEpP", p[1]))
        p += 2;
    if (*p == '-' || *p == '+')
        neg = *p++ == '-';
    bnum.n = 0;
    for (; isdig(*p); p++, any = 1)
        bn_mul(&bnum, 10, *p - '0');
    if (*p == '.')
        for (p++; isdig(*p); p++, any = 1) {
            bn_mul(&bnum, 10, *p - '0');
            e--;
        }
    if (!any)
        return 0;
    if (*p == 'e' || *p == 'E') {
        int en = 0, x = 0;
        p++;
        if (*p == '-' || *p == '+')
            en = *p++ == '-';
        if (!isdig(*p))
            return 0;
        for (; isdig(*p); p++)
            if (x < 100000)
                x = x * 10 + *p - '0';
        e += en ? -x : x;
    }
    p = skipws((char *)p);
    if (*p && *p != ',')
        return 0;
    memset(out, 0, kind);
    if (!bnum.n)
        goto sign;
    if (e > 5000) {
        E = emax + 1;
        goto inf;
    }
    if (e < -5200)
        goto sign;
    bden.n = 1;
    bden.d[0] = 1;
    for (i = 0; i < (e < 0 ? -e : e); i++)
        bn_mul(e < 0 ? &bden : &bnum, 10, 0);
    /* q = floor(value * 2^s) with its top bit at P+1: the mantissa (P bits), a guard bit and one more */
    s = (P + 2) - (bn_bits(&bnum) - bn_bits(&bden));
    rem = bn_div(s, q, P + 3);
    if (q[2 - (P + 2) / 32] & 1u << ((P + 2) % 32)) {
        s--;
        rem = bn_div(s, q, P + 3);
    }
    E = P + 1 - s;
    if (E < emin) {     /* a denormal: the mantissa's last bit at 2^(emin-P+1) */
        s = P + 1 - emin;
        rem = bn_div(s, q, P + 3);
        E = emin;
    }
    /* m: bits P+1..2 of q; guard: bit 1; sticky: bit 0 and the remainder */
    m = (u64)q[1] << 30 | q[2] >> 2 | (u64)(q[0] & 3) << 62;
    if (P < 62)
        m &= ((u64)1 << P) - 1;
    guard = q[2] >> 1 & 1;
    sticky = (q[2] & 1) || rem;
    if (guard && (sticky || (m & 1))) {     /* to nearest, a tie to even */
        m++;
        if (P < 64 && m == (u64)1 << P) {
            m >>= 1;
            E++;
        } else if (P == 64 && m == 0) {
            m = (u64)1 << 63;
            E++;
        }
    }
    if (E > emax)
        goto inf;
    {
        int normal = P == 64 ? (m >> 63) != 0 : (m >> (P - 1)) != 0;
        u32 be = normal ? E + bias : 0;
        if (kind == 4) {
            u32 w = be << 23 | (u32)(m & 0x7fffff);
            out[0] = w >> 24, out[1] = w >> 16, out[2] = w >> 8, out[3] = w;
        } else if (kind == 8) {
            u64 w = (u64)be << 52 | (m & 0xfffffffffffffULL);
            for (i = 0; i < 8; i++)
                out[i] = w >> (56 - 8 * i);
        } else {
            out[0] = be >> 8, out[1] = be;
            for (i = 0; i < 8; i++)
                out[4 + i] = m >> (56 - 8 * i);
        }
    }
    goto sign;
inf:
    memset(out, 0, kind);
    if (kind == 4)
        out[0] = 0x7f, out[1] = 0x80;
    else if (kind == 8)
        out[0] = 0x7f, out[1] = 0xf0;
    else
        out[0] = 0x7f, out[1] = 0xff;
sign:
    if (neg)
        out[0] |= 0x80;
    return 1;
}
static void fp_imm(const char *t, int size, int at)
{
    u8 b[12];
    int k = size == SZ_S ? 4 : size == SZ_D ? 8 : size == SZ_X ? 12 : 0, i;
    (void)at;
    if (!k || !fltconv(t, k, b)) {
        err("bad floating-point immediate");
        k = k ? k : 2;
        memset(b, 0, sizeof b);
    }
    for (i = 0; i < k; i += 2)
        w16(b[i] << 8 | b[i + 1]);
}

/* ---- input: files, macro and .rept expansions ---- */
struct src {
    char *name, *buf;
    u32 len;
    struct src *next;
};
static struct src *srcs, *lmnames;
static char **incdirs;
static u32 nincdir, cincdir;

static struct src *loadfile(const char *name)
{
    struct src *f;
    int fd;
    u32 n = 0, cap = 0;
    char *b = 0;
    for (f = srcs; f; f = f->next)
        if (!strcmp(f->name, name))
            return f;
    fd = strcmp(name, "-") ? open(name, O_RDONLY) : 0;
    if (fd < 0)
        return 0;
    for (;;) {
        int r;
        b = xgrow(b, &cap, n + 4097, 1);
        r = read(fd, b + n, cap - n - 1);
        if (r < 0)
            die("%s: cannot read", name);
        if (!r)
            break;
        n += r;
    }
    if (fd)
        close(fd);
    b[n] = 0;
    f = xalloc(sizeof *f);
    f->name = xstrndup(name, strlen(name));
    f->buf = b;
    f->len = n;
    f->next = srcs;
    srcs = f;
    return f;
}

struct frame {
    char *p, *end, *own;
    const char *name;
    int line, kind, cdepth;
};
static int ncst;         /* the .if stack's depth (below) */
#define MAXFR 100
static struct frame fr[MAXFR];
static int nfr;
static char *lb;
static u32 clb;

static void push(char *p, u32 n, const char *name, int kind, char *own)
{
    if (nfr == MAXFR)
        die("macros or includes nested too deep");
    fr[nfr].p = p;
    fr[nfr].end = p + n;
    fr[nfr].own = own;
    fr[nfr].name = name;
    fr[nfr].line = kind ? (nfr ? fr[nfr - 1].line : 0) : 0;
    fr[nfr].kind = kind;
    fr[nfr].cdepth = ncst;
    nfr++;
}
static void pop(void)
{
    nfr--;
    free(fr[nfr].own);
    if (nfr && fr[nfr].kind == 0) {
        srcname = fr[nfr - 1].name;
    }
}
/* the next line, or 0 at the end of the input */
static char *nextline(void)
{
    struct frame *f;
    char *e;
    u32 n;
    while (nfr && fr[nfr - 1].p >= fr[nfr - 1].end)
        pop();
    if (!nfr)
        return 0;
    f = &fr[nfr - 1];
    for (e = f->p; e < f->end && *e != '\n'; e++)
        ;
    n = e - f->p;
    lb = xgrow(lb, &clb, n + 1, 1);
    memcpy(lb, f->p, n);
    lb[n] = 0;
    if (n && lb[n - 1] == '\r')
        lb[n - 1] = 0;
    f->p = e < f->end ? e + 1 : e;
    if (f->kind == 0) {
        f->line++;
        srcname = f->name;
        srcline = f->line;
    }
    return lb;
}

/* ---- macros ---- */
struct macro {
    struct macro *next;
    char *name, *body;
    int np;
    char *pn[32], *pdef[32];
    u8 vararg;
};
static struct macro *macros;
static int macro_count;

static int dirword(char *s, const char *w)       /* the statement s (labels skipped) is directive w */
{
    int n = strlen(w);
    s = skipws(s);
    for (;;) {
        char *q = s;
        while (isid(*q))
            q++;
        if (q > s && *q == ':' && *s != '.') {
            s = skipws(q + 1);
            continue;
        }
        break;
    }
    if (strncmp(s, w, n) && !(lower(s[0]) == w[0] && ideq(s, n, w)))
        return 0;
    return !isid(s[n]);
}
/* the lines up to the matching end (.endm or .endr), copied */
static int bodybad;
static char *body(const char *open1, const char *open2, const char *open3, const char *close, u32 *len)
{
    char *b = 0, *l;
    u32 n = 0, cap = 0;
    int depth = 1, lev = nfr;
    /* (only the lines of the file or expansion it began in: an unclosed .rept must not read its caller's) */
    while (nfr >= lev && lev > 0 && fr[lev - 1].p < fr[lev - 1].end && (l = nextline()) != 0) {
        u32 k;
        if (dirword(l, open1) || (open2 && dirword(l, open2)) || (open3 && dirword(l, open3)))
            depth++;
        else if (dirword(l, close) && !--depth)
            break;
        k = strlen(l);
        b = xgrow(b, &cap, n + k + 2, 1);
        memcpy(b + n, l, k);
        n += k;
        b[n++] = '\n';
    }
    if (depth) {        /* (in every pass: then nothing is expanded, so an unclosed .rept cannot repeat itself) */
        err("%s missing", close);
        bodybad = 1;
    }
    b = xgrow(b, &cap, n + 1, 1);
    b[n] = 0;
    *len = n;
    return b;
}

/* split s at commas outside quotes and brackets; returns the count; the pieces are null-terminated in place */
static int split2(char *s, char **v, int max, int rest);
static int split(char *s, char **v, int max)
{
    return split2(s, v, max, 0);
}
static int split2(char *s, char **v, int max, int rest)
{
    int n = 0, depth = 0;
    char *p = skipws(s);
    if (!*p)
        return 0;
    v[n++] = p;
    for (; *p; p++) {
        if (*p == '"') {
            for (p++; *p && *p != '"'; p++)
                if (*p == '\\' && p[1])
                    p++;
            if (!*p)
                break;
        } else if (*p == '\'' && p[1]) {
            p++;
            if (*p == '\\' && p[1])
                p++;
            if (p[1] == '\'')
                p++;
        } else if (*p == '(' || *p == '[' || *p == '{')
            depth++;
        else if (*p == ')' || *p == ']' || *p == '}')
            depth--;
        else if (*p == ',' && depth <= 0 && !(rest && n == max)) {
            char *e = p;
            *p = 0;
            while (e > v[n - 1] && (e[-1] == ' ' || e[-1] == '\t'))
                *--e = 0;
            if (n == max) {
                err("too many values on one line (at most %d)", max);
                return -1;
            }
            v[n++] = skipws(p + 1);
        }
    }
    {
        char *e = p;
        while (e > v[n - 1] && (e[-1] == ' ' || e[-1] == '\t'))
            *--e = 0;
    }
    return n;
}

static void define_macro(char *args)
{
    struct macro *m = xalloc(sizeof *m);
    char *p = skipws(args), *q;
    u32 len;
    q = p;
    while (isid(*q))
        q++;
    if (q == p) {
        err("macro name missing");
        free(m);
        return;
    }
    m->name = xstrndup(p, q - p);
    p = skipws(q);
    if (*p == ',')
        p = skipws(p + 1);
    while (*p) {
        if (m->np == 32) {
            err("a macro takes at most 32 parameters");
            break;
        }
        q = p;
        while (isid(*q))
            q++;
        if (q == p)
            break;
        m->pn[m->np] = xstrndup(p, q - p);
        p = skipws(q);
        if (*p == ':') {        /* :req, :vararg */
            q = p + 1;
            while (isid(*q))
                q++;
            if (!strncmp(p + 1, "vararg", 6))
                m->vararg = 1;
            p = skipws(q);
        }
        if (*p == '=') {
            p = skipws(p + 1);
            q = p;
            while (*q && *q != ',' && *q != ' ' && *q != '\t')
                q++;
            m->pdef[m->np] = xstrndup(p, q - p);
            p = skipws(q);
        }
        m->np++;
        if (*p == ',')
            p = skipws(p + 1);
    }
    bodybad = 0;
    m->body = body(".macro", 0, 0, ".endm", &len);
    m->next = macros;
    macros = m;
}
static struct macro *findmacro(const char *s, int n)
{
    struct macro *m;
    for (m = macros; m; m = m->next)
        if ((int)strlen(m->name) == n && !strncmp(m->name, s, n))
            return m;
    return 0;
}
/* body with \name, \() and \@ replaced */
static char *expand(const char *b, int np, char **pn, char **val, u32 *len)
{
    char *o = 0;
    u32 n = 0, cap = 0;
    char cnt[12];
    int k = 0, c = macro_count++;
    do
        cnt[k++] = '0' + c % 10;
    while ((c /= 10) != 0);
    while (*b) {
        const char *rep = 0;
        int skip = 1, rl;
        if (*b == '\\') {
            if (b[1] == '(' && b[2] == ')') {
                b += 3;
                continue;
            }
            if (b[1] == '@') {
                int i;
                o = xgrow(o, &cap, n + k + 1, 1);
                for (i = k - 1; i >= 0; i--)
                    o[n++] = cnt[i];
                b += 2;
                continue;
            }
            if (isid0(b[1]) || isdig(b[1])) {
                const char *q = b + 1;
                int i;
                while (isid(*q))
                    q++;
                for (i = 0; i < np; i++)
                    if ((int)strlen(pn[i]) == q - b - 1 && !strncmp(pn[i], b + 1, q - b - 1)) {
                        rep = val[i] ? val[i] : "";
                        skip = q - b;
                        break;
                    }
            }
        }
        if (rep) {
            rl = strlen(rep);
            o = xgrow(o, &cap, n + rl + 1, 1);
            memcpy(o + n, rep, rl);
            n += rl;
            b += skip;
        } else {
            o = xgrow(o, &cap, n + 2, 1);
            o[n++] = *b++;
        }
    }
    o = xgrow(o, &cap, n + 1, 1);
    o[n] = 0;
    *len = n;
    return o;
}
static void call_macro(struct macro *m, char *args)
{
    char *v[34], *val[32];
    int n, i;
    u32 len;
    char *t;
    memset(val, 0, sizeof val);
    n = split2(args, v, m->vararg && m->np ? m->np : 33, m->vararg);
    if (n == 1 && m->np > 1 && !m->vararg) {    /* arguments separated by spaces */
        char *p = v[0];
        n = 0;
        while (*p && n < 33) {
            v[n++] = p;
            while (*p && *p != ' ' && *p != '\t')
                p++;
            if (*p)
                *p++ = 0;
            p = skipws(p);
        }
    }
    if (n > m->np && !(m->vararg && m->np)) {
        err("too many arguments for macro %s", m->name);
        n = m->np;
    }
    for (i = 0; i < m->np; i++)
        val[i] = m->pdef[i];
    for (i = 0; i < n && i < m->np; i++) {
        char *e = v[i], *q = e;
        int j;
        while (isid(*q))
            q++;
        if (*q == '=' && q > e)
            for (j = 0; j < m->np; j++)
                if ((int)strlen(m->pn[j]) == q - e && !strncmp(m->pn[j], e, q - e)) {
                    val[j] = q + 1;
                    goto next;
                }
        if (*e)
            val[i] = e;
next:;
    }
    t = expand(m->body, m->np, m->pn, val, &len);
    push(t, len, srcname, 1, t);
}

/* ---- conditionals ---- */
static struct {
    u8 active, done, parent;
} cst[64];
static int active(void)
{
    return !ncst || cst[ncst - 1].active;
}
static void cpush(int parent, int val)
{
    if (ncst == 64)
        die(".if nested too deep");
    cst[ncst].parent = parent;
    cst[ncst].active = parent && val;
    cst[ncst].done = !parent || val;
    ncst++;
}

/* ---- sections by name (gas's .section and bfd's special names) ---- */
static struct sect *findsect(const char *name, int n)
{
    u32 i;
    for (i = 0; i < nsect; i++)
        if ((int)strlen(sects[i]->name) == n && !strncmp(sects[i]->name, name, n))
            return sects[i];
    return 0;
}
static int prefix(const char *n, int len, const char *p)
{
    int l = strlen(p);
    return len >= l && !strncmp(n, p, l) && (len == l || n[l] == '.');
}
/* bfd's special sections (elf.c special_sections): a name's default type and flags */
static void special(const char *n, int len, u32 *type, u32 *flags)
{
    *type = SHT_PROGBITS;
    *flags = 0;
    if (prefix(n, len, ".text") || prefix(n, len, ".init") || prefix(n, len, ".fini"))
        *flags = SHF_ALLOC | SHF_EXEC;
    else if (prefix(n, len, ".data") || prefix(n, len, ".data1") || prefix(n, len, ".ctors") ||
             prefix(n, len, ".dtors"))
        *flags = SHF_ALLOC | SHF_WRITE;
    else if (prefix(n, len, ".bss")) {
        *type = SHT_NOBITS;
        *flags = SHF_ALLOC | SHF_WRITE;
    } else if (prefix(n, len, ".rodata") || prefix(n, len, ".rodata1"))
        *flags = SHF_ALLOC;
    else if (prefix(n, len, ".init_array") || prefix(n, len, ".fini_array") || prefix(n, len, ".preinit_array")) {
        *type = n[2] == 'n' ? SHT_INIT_ARRAY : n[2] == 'i' ? SHT_FINI_ARRAY : SHT_PREINIT_ARRAY;
        *flags = SHF_ALLOC | SHF_WRITE;
    } else if (prefix(n, len, ".note"))
        *type = SHT_NOTE;
}
static void setsect(struct sect *s)
{
    if (s != cur)
        prevsec = cur;
    cur = s;
}
/* .section NAME[,"FLAGS"[,@TYPE[,ENTSIZE]]] */
static void d_section(char *a, int push)
{
    char *v[6], *name;
    int n = split(a, v, 6), len, i;
    u32 type, flags;
    struct sect *s;
    if (n < 1) {
        err("section name missing");
        return;
    }
    name = v[0];
    if (*name == '"')
        name++;
    len = strlen(name);
    if (len && name[len - 1] == '"')
        len--;
    special(name, len, &type, &flags);
    if (n >= 2) {
        char *f = v[1];
        u32 sflags = flags;
        if (*f == '"')
            f++;
        flags = 0;
        for (; *f && *f != '"'; f++)
            switch (*f) {
            case 'a': flags |= SHF_ALLOC; break;
            case 'w': flags |= SHF_WRITE; break;
            case 'x': flags |= SHF_EXEC; break;
            case 'M': flags |= SHF_MERGE; break;
            case 'S': flags |= SHF_STRINGS; break;
            case 'R': flags |= SHF_GNU_RETAIN; break;
            case 'T': flags |= 0x400; break;
            default: err("section flag %s not supported", f); return;
            }
        if (!(flags & ~sflags))     /* gas: a special name's flags are added when the given ones are a subset */
            flags |= sflags;
    }
    if (n >= 3) {
        char *t = v[2];
        if (*t == '@' || *t == '%')
            t++;
        if (*t == '"')
            t++;
        if (!strncmp(t, "progbits", 8))
            type = SHT_PROGBITS;
        else if (!strncmp(t, "nobits", 6))
            type = SHT_NOBITS;
        else if (!strncmp(t, "note", 4))
            type = SHT_NOTE;
        else if (!strncmp(t, "init_array", 10))
            type = SHT_INIT_ARRAY;
        else if (!strncmp(t, "fini_array", 10))
            type = SHT_FINI_ARRAY;
        else if (!strncmp(t, "preinit_array", 13))
            type = SHT_PREINIT_ARRAY;
        else {
            err("section type %s not supported", v[2]);
            return;
        }
    }
    s = findsect(name, len);
    if (!s) {
        char *nm = xstrndup(name, len);
        u32 es = 0;
        if (flags & SHF_MERGE) {
            int ok;
            char *e = n >= 4 ? v[3] : 0;
            es = e ? (u32)absexpr(&e, &ok) : 0;
            if (!e)
                err("entity size missing");
        }
        if (type == SHT_INIT_ARRAY || type == SHT_FINI_ARRAY || type == SHT_PREINIT_ARRAY)
            es = 4;         /* (gas: an array of addresses) */
        s = newsect(nm, type, flags, es);
        free(nm);
    }
    if (push) {
        if (secsp == 16)
            die(".pushsection nested too deep");
        prevstack[secsp] = prevsec;
        secstack[secsp++] = cur;
    }
    setsect(s);
    (void)i;
}

/* ---- symbols defined ---- */
static void define(struct sym *y, int sec, i64 val, int equ, int known)
{
    if (y->flags & F_SEC) {
        err("%s is a section", y->name);
        return;
    }
    if (y->defpass == pass && !(y->flags & F_EQU) && !equ) {
        err("symbol `%s' is already defined", y->name);
        return;
    }
    if (y->defpass == pass && (y->flags & F_EQU) && !equ) {
        err("symbol `%s' is already defined", y->name);
        return;
    }
    if (y->defpass != pass) {   /* the first definition in this pass, compared with the last pass's first one */
        if (y->fsec != sec || y->fval != val)
            changed = 1;
        y->fsec = sec;
        y->fval = val;
    }
    y->sec = sec;
    y->val = val;
    y->defpass = pass;
    y->frag = sec >= 0 ? sects[sec]->frag : 0;
    y->region = sec >= 0 ? sects[sec]->region : 0;
    y->fixseq = sec >= 0 ? sects[sec]->fixseq : 0;
    y->atstart = sec >= 0 && sects[sec]->fragloc == (u32)val;
    y->base = 0;
    if (equ)
        y->flags |= F_EQU;
    if (known)
        y->flags |= F_KNOWN;
    else
        y->flags &= ~F_KNOWN;
}
static void label(char *s, int n)
{
    struct sym *y;
    if (isdig(*s))
        y = numlabel(s, n, 0, 1);
    else
        y = lookup(s, n, 1);
    define(y, secidx(cur), cur->loc, 0, 0);
}
static void d_set(struct sym *y, char *e)
{
    struct val v;
    getexpr(&e, &v);
    if (v.bad || *e) {
        err("bad expression");
        return;
    }
    if (v.sec == SEC_ABS)
        define(y, SEC_ABS, v.v, 1, v.known);
    else if (v.sec == SEC_UNDEF || v.pcrel || v.got || (v.s && v.s->bind == STB_WEAK)) {
        define(y, SEC_UNDEF, 0, 1, 0);
        if (v.s && v.s != y && !v.pcrel && !v.got) {
            y->base = v.s;
            y->val = v.add;
        } else
            err("bad .set value");
    } else
        define(y, v.sec, v.v, 1, 0);
    if (v.s && !v.pcrel && !v.got)
        y->eq = v.s;
}

static char *word(char **s, int *n)
{
    char *p = skipws(*s), *q = p;
    while (isid(*q))
        q++;
    *n = q - p;
    *s = skipws(q);
    return p;
}

/* a string literal "..." (with escapes) into buf; returns its length, -1 if none */
static int string(char **s, char *buf, int max)
{
    char *p = skipws(*s);
    int n = 0;
    if (*p != '"')
        return -1;
    p++;
    while (*p && *p != '"') {
        int c = escape(&p);
        if (n < max)
            buf[n] = c;
        n++;
    }
    if (*p != '"')
        return -1;
    *s = skipws(p + 1);
    return n;
}

static int b64(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (isdig(c)) return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static void data(char *a, int n, int pc)
{
    char *v[256];
    int k = 0, i;
    u8 b[8];
    if (cur->drop)
        return;
    while (*skipws(a)) {
        char *e;
        k = split2(a, v, 256, 1);
        if (k <= 0)
            return;
        for (i = 0; i < k; i++) {
            struct val x;
            e = v[i];
            if (i == 255 && k == 256) {     /* more than 256 values: the rest again */
                a = e;
                goto more;
            }
            getexpr(&e, &x);
            if (x.bad || *e) {
                err("bad expression");
                x.bad = 0;
                setabs(&x, 0, 1);
            }
            memset(b, 0, 8);
            if (final) {
                if (n == 8) {
                    if (x.sec != SEC_ABS)
                        err("an 8-byte value must be a number");
                    else {
                        int j;
                        for (j = 0; j < 8; j++)
                            b[j] = (u64)x.v >> (56 - 8 * j);
                    }
                } else {
                    if (x.sec == SEC_ABS && n < 4 && (x.v > (i64)(n == 1 ? 255 : 65535) || x.v < (n == 1 ? -128 : -32768)))
                        warn("value %d truncated", (i32)x.v);
                    if (x.sec == SEC_ABS && !x.pcrel)
                        x.v &= n == 4 ? 0xffffffffLL : n == 2 ? 0xffff : 0xff, x.v = n == 1 ? (int8_t)x.v : n == 2 ? (int16_t)x.v : (i32)x.v;
                    fixfield(b, cur->loc, n, &x, pc, cur->loc, 1, 0);
                }
            }
            putbytes(b, n);
        }
        return;
more:;
    }
}

static void align(u32 a, int fill, int fsize, int max)
{
    u32 pad;
    if (a & (a - 1)) {
        err("alignment not a power of 2");
        return;
    }
    if (!a)
        a = 1;
    if (a > cur->align)
        cur->align = a;
    pad = (a - (cur->loc & (a - 1))) & (a - 1);
    cur->frag++;
    cur->region++;
    if (max && (int)pad > max) {
        cur->fragloc = cur->loc;
        return;
    }
    inpad = 1;
    if (fsize == 2 && pad >= 2) {
        if (pad & 1) {
            putfill(1, 0);
            pad--;
        }
        while (pad) {
            put16(fill);
            pad -= 2;
        }
    } else if (fsize == 4 && pad >= 4) {
        if (pad & 3) {
            err("alignment padding (%d bytes) not a multiple of 4", (i32)pad);
            pad &= ~3u;
        }
        while (pad) {
            put32(fill);
            pad -= 4;
        }
    } else
        putfill(pad, fill);
    inpad = 0;
    cur->fragloc = cur->loc;
}

static struct lcomm {
    struct sym *y;
    u32 size;
} *lcomms;
static u32 nlcomm, clcomm;
static void cpu_name(char *a)
{
    int n;
    char *w = word(&a, &n);
    if (*w == 'm')
        w++, n--;
    if (n == 5 && !strncmp(w, "68000", 5)) cpu = C00;
    else if (n == 5 && !strncmp(w, "68010", 5)) cpu = C10;
    else if (n == 5 && !strncmp(w, "68020", 5)) cpu = C20 | CFPU | CMMU;
    else if (n == 5 && !strncmp(w, "68030", 5)) cpu = C30 | CFPU | CMMU;
    else if (n == 5 && !strncmp(w, "68040", 5)) cpu = C40 | CFPU | CMMU;
    else if (n == 5 && !strncmp(w, "68060", 5)) cpu = C60 | CFPU | CMMU;
    else if (n == 5 && (!strncmp(w, "68881", 5) || !strncmp(w, "68882", 5))) cpu |= CFPU;
    else if (n == 5 && !strncmp(w, "68851", 5)) cpu |= CMMU;
    else err("unknown CPU %s", w);
}

static void statement(char *s);

static int ifcond(const char *d, char *a)
{
    int ok = 1, n;
    i64 v;
    if (!strcmp(d, "ifdef") || !strcmp(d, "ifndef") || !strcmp(d, "ifnotdef")) {
        char *w = word(&a, &n);
        struct sym *y = n ? lookup(w, n, 0) : 0;
        int def = y && y->defpass == pass;
        return d[2] == 'd' ? def : !def;
    }
    if (!strcmp(d, "ifb") || !strcmp(d, "ifnb"))
        return (!*skipws(a)) == (d[2] == 'b');
    if (!strcmp(d, "ifc") || !strcmp(d, "ifnc")) {
        char *v2[2];
        int eq;
        if (split(a, v2, 2) != 2)
            return 0;
        eq = !strcmp(v2[0], v2[1]);
        return d[2] == 'c' ? eq : !eq;
    }
    v = absexpr(&a, &ok);
    if (!ok || *a)
        err("bad expression in .%s", d);
    if (!strcmp(d, "if") || !strcmp(d, "ifne")) return v != 0;
    if (!strcmp(d, "ifeq")) return v == 0;
    if (!strcmp(d, "ifgt")) return v > 0;
    if (!strcmp(d, "ifge")) return v >= 0;
    if (!strcmp(d, "iflt")) return v < 0;
    if (!strcmp(d, "ifle")) return v <= 0;
    err("unknown directive .%s", d);
    return 0;
}

static struct sym *symarg(char **a)
{
    int n;
    char *w = word(a, &n);
    if (!n) {
        err("symbol name missing");
        return 0;
    }
    return lookup(w, n, 1);
}

static void directive(char *d, int dn, char *a)
{
    char dir[24];
    int ok = 1, n, i;
    i64 v;
    char buf[256];
    if (dn >= (int)sizeof dir)
        dn = sizeof dir - 1;
    for (i = 0; i < dn; i++)
        dir[i] = lower(d[i]);
    dir[dn] = 0;
    d = dir;
#define IS(x) (!strcmp(d, x))
    if (IS("byte") || IS("dc.b"))
        data(a, 1, 0);
    else if (IS("word") || IS("short") || IS("hword") || IS("2byte") || IS("value") || IS("dc.w") || IS("dc"))
        data(a, 2, 0);
    else if (IS("long") || IS("int") || IS("4byte") || IS("dc.l"))
        data(a, 4, 0);
    else if (IS("8byte") || IS("quad"))
        data(a, 8, 0);
    else if (IS("ascii") || IS("asciz") || IS("string")) {
        int z = !IS("ascii");
        for (;;) {
            char *p = skipws(a);
            int l;
            if (!*p)
                break;
            l = string(&p, buf, sizeof buf);
            if (l < 0) {
                err("string expected");
                break;
            }
            if (l > (int)sizeof buf) {      /* long: again into a bigger buffer */
                char *big = xalloc(l + 1);
                char *q = skipws(a);
                string(&q, big, l);
                putbytes(big, l);
                free(big);
            } else
                putbytes(buf, l);
            if (z)
                putfill(1, 0);
            a = p;
            if (*a == ',')
                a++;
        }
    } else if (IS("base64")) {
        char *p = skipws(a);
        u32 acc = 0;
        int bits = 0;
        if (*p++ != '"') {
            err("string expected");
            return;
        }
        for (; *p && *p != '"'; p++) {
            int c = b64(*p);
            if (c < 0)
                continue;
            acc = acc << 6 | c;
            bits += 6;
            if (bits >= 8) {
                u8 b = acc >> (bits - 8);
                putbytes(&b, 1);
                bits -= 8;
            }
        }
    } else if (IS("float") || IS("single") || IS("double") || IS("extend") || IS("ldouble") || IS("dc.s") ||
               IS("dc.d") || IS("dc.x")) {
        int k = (IS("float") || IS("single") || IS("dc.s")) ? 4 : (IS("double") || IS("dc.d")) ? 8 : 12;
        char *v2[64];
        int c = split(a, v2, 64);
        for (i = 0; i < c; i++) {
            u8 b[12];
            if (!fltconv(v2[i], k, b))
                err("bad floating-point number %s", v2[i]);
            putbytes(b, k);
        }
    } else if (IS("zero") || IS("space") || IS("skip") || IS("ds") || IS("ds.b") || IS("ds.w") || IS("ds.l")) {
        char *v2[2];
        int c = split(a, v2, 2), mul = IS("ds.w") ? 2 : IS("ds.l") ? 4 : 1;
        i64 fill = 0;
        char *e = c > 0 ? v2[0] : "";
        v = absexpr(&e, &ok);
        if (c == 2) {
            e = v2[1];
            fill = absexpr(&e, &ok);
        }
        if (!ok || v < 0 || v > 0x10000000)
            err("bad size for .%s", d);
        else {      /* (gas: a frag of its own, no fixed bytes; the next one begins after it) */
            inpad = 1;
            putfill(v * mul, (int)fill);
            inpad = 0;
            cur->frag++;
            cur->fragloc = cur->loc;
        }
    } else if (IS("fill")) {
        char *v2[3];
        int c = split(a, v2, 3);
        i64 rep, size = 1, val = 0;
        char *e = c > 0 ? v2[0] : "";
        if (c < 1)
            ok = 0;
        rep = absexpr(&e, &ok);
        if (c > 1)
            e = v2[1], size = absexpr(&e, &ok);
        if (c > 2)
            e = v2[2], val = absexpr(&e, &ok);
        if (!ok || rep < 0 || size < 0 || size > 8 || rep > 0x10000000)
            err("bad .fill");
        else
            for (inpad = 1; rep > 0; rep--)
                for (i = 0; i < (int)size; i++) {     /* (gas: up to 4 bytes of the value, then zeros) */
                    int k = (size > 4 ? 4 : (int)size) - 1 - i;
                    u8 b = k >= 0 ? (u64)val >> (8 * k) : 0;
                    putbytes(&b, 1);
                }
        if (ok) {   /* (gas: a frag of its own, as .skip) */
            inpad = 0;
            cur->frag++;
            cur->fragloc = cur->loc;
        }
    } else if (IS("align") || IS("balign") || IS("balignw") || IS("balignl") || IS("p2align") || IS("p2alignw") ||
               IS("p2alignl") || IS("even")) {
        char *v2[3];
        int c = IS("even") ? 0 : split(a, v2, 3);
        i64 al = 2, fill = 0, max = 0;
        char *e;
        if (c > 0) {
            e = v2[0];
            al = absexpr(&e, &ok);
        }
        if (c > 1 && *v2[1]) {
            e = v2[1];
            fill = absexpr(&e, &ok);
        }
        if (c > 2) {
            e = v2[2];
            max = absexpr(&e, &ok);
        }
        if (d[0] == 'p') {
            if (al < 0 || al > 16)
                ok = 0;
            al = 1 << al;
        }
        if (!ok || al < 0 || al > 65536)
            err("bad alignment");
        else
            align((u32)al, (int)fill, d[strlen(d) - 1] == 'w' ? 2 : d[strlen(d) - 1] == 'l' && d[1] != 'l' ? 4 : 1,
                  (int)max);
    } else if (IS("org")) {
        struct val x;
        getexpr(&a, &x);
        if (x.sec == SEC_ABS)
            x.v = x.v, x.sec = secidx(cur);
        if (x.sec != secidx(cur) || x.v < cur->loc || x.v - cur->loc > 0x10000000)
            err(".org backwards or into another section");
        else {
            inpad = 1;
            putfill((u32)(x.v - cur->loc), 0);
            inpad = 0;
        }
        cur->frag++;
        cur->region++;
        cur->fragloc = cur->loc;
    } else if (IS("text") || IS("data") || IS("bss")) {
        if (*skipws(a))
            err("subsections are not supported");
        setsect(sects[d[0] == 't' ? 0 : d[0] == 'd' ? 1 : 2]);
    }
    else if (IS("section"))
        d_section(a, 0);
    else if (IS("pushsection"))
        d_section(a, 1);
    else if (IS("popsection")) {
        if (!secsp)
            err(".popsection without .pushsection");
        else
        {
            secsp--;
            cur = secstack[secsp];      /* (gas restores .previous's section too) */
            prevsec = prevstack[secsp];
        }
    } else if (IS("previous")) {
        if (prevsec)
            setsect(prevsec);
    } else if (IS("globl") || IS("global") || IS("weak") || IS("local") || IS("hidden") || IS("internal") ||
               IS("protected")) {
        char *v2[64];
        int c = split(a, v2, 64);
        for (i = 0; i < c; i++) {
            char *p = v2[i];
            struct sym *y = symarg(&p);
            if (!y)
                continue;
            if (IS("weak"))
                y->bind = STB_WEAK;
            else if (IS("local"))
                y->flags |= F_LOCAL, y->bind = STB_LOCAL;
            else if (IS("hidden"))
                y->other = 2;
            else if (IS("internal"))
                y->other = 1;
            else if (IS("protected"))
                y->other = 3;
            else if (y->bind != STB_WEAK)
                y->bind = STB_GLOBAL;
        }
    } else if (IS("type")) {
        char *v2[2];
        struct sym *y;
        char *t;
        if (split(a, v2, 2) != 2 || !(y = symarg(&v2[0])))
            return;
        t = v2[1];
        if (*t == '@' || *t == '%' || *t == '#')
            t++;
        if (*t == '"')
            t++;
        if (!strncmp(t, "function", 8) || !strncmp(t, "STT_FUNC", 8))
            y->type = 2;
        else if (!strncmp(t, "object", 6) || !strncmp(t, "STT_OBJECT", 10))
            y->type = 1;
        else if (!strncmp(t, "notype", 6) || !strncmp(t, "STT_NOTYPE", 10))
            y->type = 0;
        else if (!strncmp(t, "tls_object", 10))
            y->type = 6;
        else
            err("symbol type %s not supported", v2[1]);
    } else if (IS("size")) {
        char *v2[2];
        struct sym *y;
        if (split(a, v2, 2) != 2 || !(y = symarg(&v2[0])))
            return;
        v = absexpr(&v2[1], &ok);
        if (final && (!ok || *v2[1]))
            err("bad .size expression");
        y->size = (u32)v;
        y->sizepass = pass;
    } else if (IS("set") || IS("equ") || IS("equiv") || IS("eqv")) {
        char *v2[2];
        struct sym *y;
        if (split2(a, v2, 2, 1) != 2 || !(y = symarg(&v2[0])))
            return;
        if (IS("equiv") && y->defpass == pass)
            err("symbol `%s' is already defined", y->name);
        d_set(y, v2[1]);
    } else if (IS("lcomm")) {
        char *v2[3];
        struct sym *y;
        int c = split(a, v2, 3);
        struct sect *keep = cur;
        if (c < 2 || !(y = symarg(&v2[0])))
            return;
        v = absexpr(&v2[1], &ok);
        if (c > 2)
            err("junk at end of line");
        if (!ok || v < 0 || v > 0x10000000) {
            err("bad .lcomm size");
            return;
        }
        /* gas puts it in .bss's subsection 1: after all of .bss, at the end of the pass (run) */
        lcomms = xgrow(lcomms, &clcomm, nlcomm + 1, sizeof *lcomms);
        lcomms[nlcomm].y = y;
        lcomms[nlcomm++].size = (u32)v;
        (void)keep;
    } else if (IS("comm"))
        err("common symbols are not supported (use .bss or .lcomm)");
    else if (IS("file")) {
        char *p = skipws(a);
        if (*p == '"' && !filesym) {
            int l = string(&p, buf, sizeof buf - 1);
            if (l >= 0 && l < (int)sizeof buf)
                filesym = xstrndup(buf, l);
        }
    } else if (IS("ident")) {
        struct sect *keep = cur, *c = findsect(".comment", 8);
        char *p = a;
        int l = string(&p, buf, sizeof buf);
        if (l < 0 || l > (int)sizeof buf) {
            err("bad .ident");
            return;
        }
        if (!c)
            c = newsect(".comment", SHT_PROGBITS, SHF_MERGE | SHF_STRINGS, 1);
        cur = c;
        if (!c->loc)
            putfill(1, 0);
        putbytes(buf, l);
        putfill(1, 0);
        cur = keep;
    } else if (IS("swbeg") || IS("loc") || IS("loc_mark_labels") || !strncmp(d, "cfi_", 4) || IS("nolist") ||
               IS("list") || IS("title") || IS("sbttl") || IS("psize") || IS("eject") || IS("version") ||
               IS("gnu_attribute") || IS("addrsig") || IS("addrsig_sym"))
        ;
    else if (IS("uleb128") || IS("sleb128")) {
        char *v2[64];
        int c = split(a, v2, 64);
        for (i = 0; i < c; i++) {
            char *e = v2[i];
            u8 b;
            v = absexpr(&e, &ok);
            if (!ok)
                err("bad expression");
            do {
                b = v & 0x7f;
                v = d[0] == 'u' ? (i64)((u64)v >> 7) : v >> 7;
                if (d[0] == 'u' ? v != 0 : !((v == 0 && !(b & 0x40)) || (v == -1 && (b & 0x40))))
                    b |= 0x80;
                putbytes(&b, 1);
            } while (b & 0x80);
        }
    } else if (IS("incbin")) {
        char *p = a, *v2[3];
        int l = string(&p, buf, sizeof buf - 1), c;
        struct src *f;
        i64 skip = 0, cnt = -1;
        if (l < 0 || l >= (int)sizeof buf) {
            err("bad .incbin");
            return;
        }
        buf[l] = 0;
        c = *p == ',' ? split(p + 1, v2, 2) : 0;
        if (c > 0)
            skip = absexpr(&v2[0], &ok);
        if (c > 1)
            cnt = absexpr(&v2[1], &ok);
        f = loadfile(buf);
        for (i = 0; !f && i < (int)nincdir; i++) {
            char path[512];
            int dl = strlen(incdirs[i]);
            if (dl + l + 2 < (int)sizeof path) {
                memcpy(path, incdirs[i], dl);
                path[dl] = '/';
                memcpy(path + dl + 1, buf, l + 1);
                f = loadfile(path);
            }
        }
        if (!f) {
            err("cannot open %s", buf);
            return;
        }
        if (skip < 0 || (u64)skip > f->len || (cnt >= 0 && (u64)cnt > f->len - (u64)skip))
            err(".incbin beyond the file's end");
        else
            putbytes(f->buf + skip, cnt >= 0 ? (u32)cnt : f->len - (u32)skip);
    } else if (IS("include")) {
        char *p = a;
        int l = string(&p, buf, sizeof buf - 1);
        struct src *f;
        if (l < 0 || l >= (int)sizeof buf) {
            err("bad .include");
            return;
        }
        buf[l] = 0;
        f = loadfile(buf);
        for (i = 0; !f && i < (int)nincdir; i++) {
            char path[512];
            int dl = strlen(incdirs[i]);
            if (dl + l + 2 < (int)sizeof path) {
                memcpy(path, incdirs[i], dl);
                path[dl] = '/';
                memcpy(path + dl + 1, buf, l + 1);
                f = loadfile(path);
            }
        }
        if (!f)
            die("cannot open %s", buf);
        push(f->buf, f->len, f->name, 0, 0);
    } else if (IS("macro"))
        define_macro(a);
    else if (IS("endm") || IS("endr"))
        err(".%s without its start", d);
    else if (IS("exitm")) {
        while (nfr > 1 && fr[nfr - 1].kind == 0)
            pop();
        if (nfr > 1) {
            ncst = fr[nfr - 1].cdepth;  /* the .if's opened in the macro end with it */
            pop();
        }
    } else if (IS("purgem")) {
        struct macro **m;
        char *w = word(&a, &n);
        for (m = &macros; *m; m = &(*m)->next)
            if ((int)strlen((*m)->name) == n && !strncmp((*m)->name, w, n)) {
                *m = (*m)->next;
                break;
            }
    } else if (IS("rept") || IS("irp") || IS("irpc")) {
        u32 len, tl = 0, tc = 0;
        char *b, *t = 0;
        bodybad = 0;
        if (IS("rept")) {
            v = absexpr(&a, &ok);
            b = body(".rept", ".irp", ".irpc", ".endr", &len);
            if (!ok || v < 0 || v > 100000)
                err("bad .rept count");
            else
                for (; v > 0; v--) {
                    t = xgrow(t, &tc, tl + len + 1, 1);
                    memcpy(t + tl, b, len);
                    tl += len;
                }
        } else {
            char *v2[256], *pn[1];
            int c = split(a, v2, 256);
            b = body(".rept", ".irp", ".irpc", ".endr", &len);
            if (c < 1 || (!IS("irp") && c < 2)) {
                err("bad .%s", d);
                c = 0;
            }
            for (i = 1; c > 0 && (IS("irp") ? i < c : v2[1][i - 1] != 0); i++) {
                char one[2], *val[1];
                u32 el;
                char *x;
                pn[0] = v2[0];
                if (IS("irp"))
                    val[0] = v2[i];
                else {
                    one[0] = v2[1][i - 1];
                    one[1] = 0;
                    val[0] = one;
                }
                x = expand(b, 1, pn, val, &el);
                t = xgrow(t, &tc, tl + el + 1, 1);
                memcpy(t + tl, x, el);
                tl += el;
                free(x);
            }
            if (c == 1 && IS("irp")) {      /* no values: once, empty */
                char *val[1] = {0};
                u32 el;
                char *x;
                pn[0] = v2[0];
                x = expand(b, 1, pn, val, &el);
                t = x;
                tl = el;
            }
        }
        free(b);
        if (t && !bodybad)
            push(t, tl, srcname, 1, t);
        else
            free(t);
    } else if (IS("chip") || IS("cpu") || IS("arch"))
        cpu_name(a);
    else if (IS("err") || IS("error")) {
        char *p = a;
        int l = string(&p, buf, sizeof buf - 1);
        if (l >= 0 && l < (int)sizeof buf) {
            buf[l] = 0;
            err("%s", buf);
        } else
            err(".err encountered");
    } else if (IS("warning") || IS("print")) {
        char *p = a;
        int l = string(&p, buf, sizeof buf - 1);
        if (l >= 0 && l < (int)sizeof buf && final) {
            buf[l] = 0;
            if (IS("print")) {
                os(1, buf);
                oc(1, '\n');
                oflush(1);
            } else
                warn("%s", buf);
        }
    } else if (IS("end")) {
        while (nfr)
            pop();
    } else
        err("unknown directive .%s", d);
#undef IS
}

/* ---- statements ---- */
static void instruction(char *m, int ml, char *args)
{
    char name[24], *v[8];
    int n, i, sz = 0, cc = -1, bl = 0, ok, bit_given = 0;
    const struct mn *mn = 0;
    struct op o[6];
    /* gas looks a mnemonic up without its dot (move.l is movel, add.x is addx, move.s is moves), then as a name and
     * a size letter */
    for (i = 0; i < ml; i++)
        if (m[i] != '.') {
            if (bl == (int)sizeof name - 1) {
                err("unknown instruction");
                return;
            }
            name[bl++] = lower(m[i]);
        }
    name[bl] = 0;
    mn = findmn(name, bl);
    if (!mn)
        mn = findcond(name, bl, &cc);
    if (!mn && bl > 2 && strchr("bwlsdxp", name[bl - 1])) {
        sz = name[bl - 1];
        mn = findmn(name, bl - 1);
        if (!mn)
            mn = findcond(name, bl - 1, &cc);
    }
    if (!mn) {
        err("unknown instruction %s", name);
        return;
    }
    /* (gas: the 68040 and 68060 have their FPU whatever -mno-float says, which only takes the 68881 away) */
    if (!(mn->cpu & cpu & 63) || ((mn->cpu & CFPU) && !(cpu & (CFPU | C40 | C60)))) {
        err("%s is not available on this CPU", name);
        return;
    }
    /* the sizes it takes: its own, or its family's */
    {
        int allow = mn->sizes, bit;
        switch (mn->fam) {
        case F_BCC: allow = SN | SB | SW | SL | SS; break;
        case F_DBCC: allow = SN | SW; break;
        case F_SCC: allow = SN | SB; break;
        case F_TRAPCC: case F_FTRAPCC: allow = SN | SW | SL; break;
        case F_FBCC: allow = SN | SL; break;
        case F_JCC: case F_FSCC: case F_FDBCC: allow = SN; break;
        case F_FOP: allow = mn->sizes & ~SN; break;    /* (SN there: the one-register form) */
        }
        bit = sz == 'b' ? SB : sz == 'w' ? SW : sz == 'l' ? SL : sz == 's' ? SS : sz == 'd' ? SD : sz == 'x' ? SX :
              sz == 'p' ? SP : SN;
        ok = (allow & bit) != 0;
        bit_given = bit != SN;
        if (!ok) {
            err("%s: no such size", name);
            return;
        }
    }
    switch (sz) {
    case 'b': sz = SZ_B; break;
    case 'w': sz = SZ_W; break;
    case 'l': sz = SZ_L; break;
    case 's': sz = mn->fam == F_BCC ? SZ_B : SZ_S; break;
    case 'd': sz = SZ_D; break;
    case 'x': sz = SZ_X; break;
    case 'p': sz = SZ_P; break;
    default:    /* gas's default: a word (a long where there is none) */
        sz = 0;
        if (mn->fam != F_BCC && mn->fam != F_FMOVEM && (mn->sizes & (SB | SW | SL)))
            sz = mn->sizes & SW ? SZ_W : mn->sizes & SL ? SZ_L : SZ_B;
    }
    n = split(args, v, 8);
    if (n < 0 || n > 6) {
        err("too many operands");
        return;
    }
    for (i = 0; i < n; i++)
        if (!parse_op(v[i], &o[i])) {
            err("bad operand `%s'", v[i]);
            return;
        }
    szg = bit_given;
    dropins = 0;
    instr(mn, cc, sz, o, n);
    if (dropins)
        niw = nfx = 0;
    if (niw)
        emit_ins();
}

/* the comment and the statements of one line (a copy: a directive may read the lines after it) */
static char *lb2;
static u32 clb2;
static int incomment;    /* inside a C comment that began on an earlier line */
static void line(char *l0)
{
    char *l, *p, *start;
    int q = 0;
    u32 n = strlen(l0);
    lb2 = xgrow(lb2, &clb2, n + 1, 1);
    memcpy(lb2, l0, n + 1);
    l = p = start = lb2;
    if (incomment) {
        char *e = strstr(l, "*/");
        if (!e)
            return;
        incomment = 0;
        memset(l, ' ', e + 2 - l);
    }
    if (*skipws(l) == '#' || *l == '*') {      /* a comment line, or cpp's # LINE "FILE" */
        char *s = skipws(skipws(l) + 1);
        if (isdig(*s)) {
            int n = 0;
            while (isdig(*s))
                n = n * 10 + *s++ - '0';
            s = skipws(s);
            if (*s == '"' && nfr && fr[nfr - 1].kind == 0) {
                char nb[256];
                int k = string(&s, nb, sizeof nb - 1);
                fr[nfr - 1].line = n - 1;
                if (k >= 0 && k < (int)sizeof nb) {
                    struct src *f;
                    nb[k] = 0;
                    /* the name for messages, kept apart from the files read (an .include of it reads it) */
                    for (f = lmnames; f && strcmp(f->name, nb); f = f->next)
                        ;
                    if (!f) {
                        f = xalloc(sizeof *f);
                        f->name = xstrndup(nb, k);
                        f->buf = "";
                        f->next = lmnames;
                        lmnames = f;
                    }
                    fr[nfr - 1].name = f->name;
                }
            }
        }
        return;
    }
    for (;; p++) {
        if (!*p || *p == '|' || (*p == ';' && !q)) {
            int end = !*p || *p == '|';
            *p = 0;
            statement(start);
            if (end)
                return;
            start = p + 1;
            continue;
        }
        if (*p == '"') {
            for (p++; *p && *p != '"'; p++)
                if (*p == '\\' && p[1])
                    p++;
            if (!*p)
                p--;
        } else if (*p == '\'' && p[1]) {
            p++;
            if (*p == '\\' && p[1])
                p++;
            if (p[1] == '\'')
                p++;
        } else if (*p == '/' && p[1] == '*') {
            char *e = strstr(p + 2, "*/");
            if (!e) {
                *p = 0;
                incomment = 1;
                statement(start);
                return;
            }
            memset(p, ' ', e + 2 - p);
        }
    }
}

static void statement(char *s)
{
    char *w, *q;
    int n;
    s = skipws(s);
    if (cur->drop && active()) {
        /* a debugging section: only what leaves it or nests counts */
        char *p = s;
        if (*p == '.' && (p[1] == 'L' || isdig(p[1])))
            return;
        if (final && !strncmp(p, ".long", 5) && (p[5] == ' ' || p[5] == '\t')) {
            /* the relocations of debugging data are not made, but gas keeps a .L symbol in the symbol table
             * that one names with an addend (a merged string's address in a location list) */
            char *v[64];
            int k = split(p + 6, v, 64), i;
            for (i = 0; i < k; i++) {
                struct val x;
                char *e = v[i];
                dbgscan = 1;
                getexpr(&e, &x);
                dbgscan = 0;
                if (!x.bad && x.s && x.sec >= 0 && !adjustable(&x))
                    x.s->flags |= F_KEEP;
            }
            return;
        }
        if (*p == '.' && strchr("bulwsv", p[1]) && (!strncmp(p, ".byte", 5) || !strncmp(p, ".uleb128", 8) ||
            !strncmp(p, ".long", 5) || !strncmp(p, ".sleb128", 8) || !strncmp(p, ".string", 7) ||
            !strncmp(p, ".word", 5) || !strncmp(p, ".value", 6)))
            return;
    }
    if (!active()) {
        /* only the conditionals count */
        char *p = s;
        while (isid(*p) || *p == ':') {
            char *e = p;
            while (isid(*e))
                e++;
            if (*e != ':')
                break;
            p = skipws(e + 1);
        }
        if (*p != '.')
            return;
        w = word(&p, &n);
        if (n >= 3 && !strncmp(w, ".if", 3))
            cpush(0, 0);
        else if (n == 7 && !strncmp(w, ".elseif", 7)) {
            if (ncst && cst[ncst - 1].parent && !cst[ncst - 1].done) {
                char d[8] = "if";
                int c = ifcond(d, p);
                cst[ncst - 1].active = c;
                cst[ncst - 1].done = c;
            }
        } else if (n == 5 && !strncmp(w, ".else", 5)) {
            if (ncst) {
                cst[ncst - 1].active = cst[ncst - 1].parent && !cst[ncst - 1].done;
                cst[ncst - 1].done = 1;
            }
        } else if (n == 6 && !strncmp(w, ".endif", 6) && ncst)
            ncst--;
        return;
    }
    /* labels */
    for (;;) {
        q = s;
        if (isdig(*q)) {
            while (isdig(*q))
                q++;
            if (*q == ':') {
                label(s, q - s);
                s = skipws(q + 1);
                continue;
            }
            break;
        }
        if (!isid0(*q))
            break;
        while (isid(*q))
            q++;
        if (*q == ':' && q[1] != ':') {
            label(s, q - s);
            s = skipws(q + 1);
            continue;
        }
        {
            char *e = skipws(q);
            if (*e == '=' && e[1] != '=') {         /* sym = expr */
                d_set(lookup(s, q - s, 1), e + 1);
                return;
            }
            if (*e == '=' && e[1] == '=') {
                d_set(lookup(s, q - s, 1), e + 2);
                return;
            }
        }
        break;
    }
    if (!*s)
        return;
    q = s;
    while (*q && *q != ' ' && *q != '\t')
        q++;
    n = q - s;
    w = s;
    s = skipws(q);
    if (*w == '.') {
        if (n >= 3 && !strncmp(w, ".if", 3)) {
            char d[12];
            int k = n - 1 < 11 ? n - 1 : 11, i;
            for (i = 0; i < k; i++)
                d[i] = lower(w[1 + i]);
            d[k] = 0;
            cpush(1, ifcond(d, s));
            return;
        }
        if (n == 7 && !strncmp(w, ".elseif", 7)) {
            if (ncst)
                cst[ncst - 1].active = 0;
            return;
        }
        if (n == 5 && !strncmp(w, ".else", 5)) {
            if (ncst)
                cst[ncst - 1].active = 0, cst[ncst - 1].done = 1;
            else
                err(".else without .if");
            return;
        }
        if (n == 6 && !strncmp(w, ".endif", 6)) {
            if (ncst)
                ncst--;
            else
                err(".endif without .if");
            return;
        }
    }
    {
        struct macro *m = findmacro(w, n);
        if (m) {
            call_macro(m, s);
            return;
        }
    }
    if (*w == '.') {
        directive(w + 1, n - 1, s);
        return;
    }
    if (cur->drop)
        return;
    instruction(w, n, s);
}

/* ---- the ELF object ---- */
static u8 *out;
static u32 nout, cout;
static void ob(const void *p, u32 n)
{
    out = xgrow(out, &cout, nout + n, 1);
    memcpy(out + nout, p, n);
    nout += n;
}
static void o32(u32 v)
{
    u8 b[4] = {v >> 24, v >> 16, v >> 8, v};
    ob(b, 4);
}
static void o16(u32 v)
{
    u8 b[2] = {v >> 8, v};
    ob(b, 2);
}
static void opad(u32 a)
{
    while (nout & (a - 1))
        ob("", 1);
}

/* bfd's string table (elf-strtab.c): strings in the order added, each once; a string that ends another one is
 * placed inside it (sorted by the reversed string, merged from the end) */
struct str {
    const char *s;
    int len, off, next;
    struct str *in;
};
static struct str *strs;
static u32 nstr, cstr;
static int strhash[1024];   /* the table's strings by hash, each +1 (0 the end), chained through strs[].next */
static int add_str(const char *s)
{
    int i, l = strlen(s);
    u32 h = hash(s, l);
    if (!*s)
        return -1;
    if (!nstr)
        memset(strhash, 0, sizeof strhash);
    for (i = strhash[h]; i; i = strs[i - 1].next)
        if (!strcmp(strs[i - 1].s, s))
            return i - 1;
    strs = xgrow(strs, &cstr, nstr + 1, sizeof *strs);
    strs[nstr].s = s;
    strs[nstr].len = l;
    strs[nstr].in = 0;
    strs[nstr].next = strhash[h];
    strhash[h] = nstr + 1;
    return nstr++;
}
static int strrev(const void *a, const void *b)
{
    const struct str *A = *(struct str *const *)a, *B = *(struct str *const *)b;
    const u8 *s = (const u8 *)A->s + A->len - 1, *t = (const u8 *)B->s + B->len - 1;
    int l = A->len < B->len ? A->len : B->len;
    while (l) {
        if (*s != *t)
            return (int)*s - (int)*t;
        s--, t--, l--;
    }
    return A->len - B->len;
}
/* lay out the table: the offsets in strs[].off; returns the size */
static u32 finish_strs(void)
{
    struct str **a = xalloc((nstr + 1) * sizeof *a), *e;
    u32 i, size = 1;
    int k;
    for (i = 0; i < nstr; i++)
        a[i] = &strs[i];
    if (nstr > 1) {
        qsort(a, nstr, sizeof *a, strrev);
        e = a[nstr - 1];
        for (k = nstr - 2; k >= 0; k--) {
            struct str *c = a[k];
            if (e->len > c->len && !memcmp(e->s + e->len - c->len, c->s, c->len))
                c->in = e;
            else
                e = c;
        }
    }
    free(a);
    for (i = 0; i < nstr; i++)
        if (!strs[i].in) {
            strs[i].off = size;
            size += strs[i].len + 1;
        }
    for (i = 0; i < nstr; i++)
        if (strs[i].in)
            strs[i].off = strs[i].in->off + strs[i].in->len - strs[i].len;
    return size;
}
static void write_strs(u32 size)
{
    u8 *b = xalloc(size);
    u32 i;
    for (i = 0; i < nstr; i++)
        if (!strs[i].in)
            memcpy(b + strs[i].off, strs[i].s, strs[i].len);
    ob(b, size);
    free(b);
}

/* is the symbol in the table? (gas: section symbols, named symbols defined or used, .L symbols a relocation
 * names) */
static int symout(struct sym *y)
{
    if (y->flags & F_SEC)
        return !sects[y->sec]->drop;
    if (y->name[0] == 1)
        return 0;
    if ((y->flags & F_L) && !(y->flags & F_KEEP) && !(y->sec == SEC_UNDEF && !y->base && (y->flags & F_USED)))
        return 0;   /* (an undefined .L symbol that is used goes in, as gas has it) */
    if (y->sec >= 0 && sects[y->sec]->drop)
        return 0;
    if (y->sec == SEC_UNDEF && !y->base && !(y->flags & F_USED) && y->bind != STB_GLOBAL)
        return 0;   /* (an undefined weak symbol nothing uses: gas leaves it out too) */
    if (y->base) {  /* an alias of a symbol that stays undefined: gas writes only that one */
        struct sym *b = y;
        int d = 0;
        while (b->base && d++ < 100)
            b = b->base;
        if (b->sec == SEC_UNDEF)
            return 0;
    }
    return 1;
}
static int symglobal(struct sym *y)
{
    if (y->flags & F_SEC)
        return 0;
    if (y->bind != STB_LOCAL)
        return 1;
    return y->sec == SEC_UNDEF && !y->base && !(y->flags & F_LOCAL);
}
static void symresolve(struct sym *y, int *sec, i64 *val)
{
    int depth = 0;
    i64 v = 0;
    while (y->base && depth++ < 100) {
        v += y->val;
        y = y->base;
    }
    *sec = y->sec;
    *val = v + (y->sec == SEC_UNDEF ? 0 : y->val);
}

static int write_object(void)
{
    struct sym *y;
    u32 i, nsym = 1, nlocal, ns = 0, shnum, symidx, stridx, shstridx, symsize, strsize, shstrsize, fileidx = 0;
    int fd, *nameof;
    u32 *rcount;
    struct sect **osec;
    /* the sections in the file: each, then its relocations */
    osec = xalloc((nsect + 1) * sizeof *osec);
    rcount = xalloc((nsect + 1) * sizeof *rcount);
    shnum = 1;
    for (i = 0; i < nsect; i++) {
        struct sect *s = sects[i];
        if (s->drop)
            continue;
        osec[ns++] = s;
        s->idx = shnum++;
        if (s->nrel + s->ndrel)
            s->ridx = shnum++;
    }
    symidx = shnum++;
    stridx = shnum++;
    shstridx = shnum++;
    /* the symbols: locals in their order of creation, then globals */
    if (filesym)
        nsym++;
    for (y = symfirst; y; y = y->next)
        if (symout(y) && !symglobal(y))
            y->idx = nsym++;
    nlocal = nsym;
    for (y = symfirst; y; y = y->next)
        if (symout(y) && symglobal(y))
            y->idx = nsym++;
    /* the symbol names */
    nstr = 0;
    if (filesym)
        fileidx = add_str(filesym);
    for (i = 0; i < 2; i++)
        for (y = symfirst; y; y = y->next)
            if (symout(y) && symglobal(y) == (int)i && !(y->flags & F_SEC))
                y->nameoff = add_str(y->name);
    strsize = finish_strs();
    nameof = 0;
    /* the ELF header */
    nout = 0;
    ob("\177ELF\1\2\1\0\0\0\0\0\0\0\0\0", 16);
    o16(1);
    o16(4);
    o32(1);
    o32(0);
    o32(0);
    o32(0);     /* e_shoff, patched below */
    o32((cpu & UP20) ? 0 : 0x01000000);    /* EF_M68K_M68000 for a 68000 or 68010 (as gas) */
    o16(52);
    o16(0);
    o16(0);
    o16(40);
    o16(shnum);
    o16(shstridx);
    /* the contents */
    for (i = 0; i < ns; i++) {
        struct sect *s = osec[i];
        opad(s->align < 4 ? s->align : 4);     /* (bfd: the section's alignment, at most the file's 4) */
        s->off = nout;
        if (s->type != SHT_NOBITS) {
            if (s->loc)
                ob(s->buf, s->loc);
        }
    }
    /* the symbol table */
    opad(4);
    {
        u32 off = nout;
        o32(0), o32(0), o32(0), o32(0);
        if (filesym) {
            o32((int)fileidx >= 0 ? (u32)strs[fileidx].off : 0);
            o32(0), o32(0);
            ob("\4\0\377\361", 4);
        }
        for (i = 0; i < 2; i++)
            for (y = symfirst; y; y = y->next) {
                int sec, sh;
                i64 val;
                u8 info;
                if (!symout(y) || symglobal(y) != (int)i)
                    continue;
                symresolve(y, &sec, &val);
                if (y->eq) {    /* gas copies an equated symbol's type and size from its target */
                    if (!y->type && (y->eq->type == 1 || y->eq->type == 2))
                        y->type = y->eq->type;
                    if (!y->size)
                        y->size = y->eq->size;
                }
                if (y->flags & F_SEC) {
                    o32(0), o32(0), o32(0);
                    ob("\3\0", 2);
                    o16(sects[y->sec]->idx);
                    continue;
                }
                sh = sec == SEC_ABS ? 0xfff1 : sec == SEC_UNDEF ? 0 : sects[sec]->idx;
                info = (symglobal(y) ? (y->bind == STB_WEAK ? STB_WEAK : STB_GLOBAL) : STB_LOCAL) << 4 | y->type;
                o32(strs[y->nameoff].off);
                o32(sh == 0 ? 0 : (u32)val);
                o32(y->size);
                ob(&info, 1);
                ob(&y->other, 1);
                o16(sh);
            }
        symsize = nout - off;
        (void)off;
    }
    {
        u32 symoff = nout - symsize, stroff, shstroff, shoff, j;
        u32 *roff = xalloc((nsect + 1) * sizeof *roff), *rsize = xalloc((nsect + 1) * sizeof *rsize);
        struct str *ss;
        u32 nss;
        int *sn = xalloc((2 * nsect + 4) * sizeof *sn), *rn2 = xalloc((nsect + 1) * sizeof *rn2);
        stroff = nout;
        write_strs(strsize);
        /* the relocations */
        for (i = 0; i < ns; i++) {
            struct sect *s = osec[i];
            int pass2;
            if (!(s->nrel + s->ndrel))
                continue;
            opad(4);
            roff[i] = nout;
            for (pass2 = 0; pass2 < 2; pass2++) {
                struct rel *r = pass2 ? s->drel : s->rel;
                u32 nr = pass2 ? s->ndrel : s->nrel;
                for (j = 0; j < nr; j++) {
                    o32(r[j].off);
                    o32((r[j].s ? r[j].s->idx : 0) << 8 | r[j].type);
                    o32((u32)r[j].add);
                }
            }
            rsize[i] = nout - roff[i];
        }
        /* the section names */
        ss = strs;
        nss = nstr;
        strs = 0;
        nstr = cstr = 0;
        add_str(".symtab");
        add_str(".strtab");
        add_str(".shstrtab");
        for (i = 0; i < ns; i++) {
            char *rname;
            sn[i] = add_str(osec[i]->name);
            if (osec[i]->nrel + osec[i]->ndrel) {
                rname = xalloc(strlen(osec[i]->name) + 6);
                memcpy(rname, ".rela", 5);
                strcpy(rname + 5, osec[i]->name);
                rn2[i] = add_str(rname);
            }
        }
        shstrsize = finish_strs();
        shstroff = nout;
        write_strs(shstrsize);
        opad(4);
        shoff = nout;
        /* the section headers */
        for (j = 0; j < 10; j++)
            o32(0);
        for (i = 0; i < ns; i++) {
            struct sect *s = osec[i];
            o32(sn[i] >= 0 ? strs[sn[i]].off : 0);
            o32(s->type);
            o32(s->flags);
            o32(0);
            o32(s->off);
            o32(s->loc);
            o32(0);
            o32(0);
            o32(s->align);
            o32(s->entsize);
            if (s->nrel + s->ndrel) {
                o32(strs[rn2[i]].off);
                o32(SHT_RELA);
                o32(SHF_INFO_LINK);
                o32(0);
                o32(roff[i]);
                o32(rsize[i]);
                o32(symidx);
                o32(s->idx);
                o32(4);
                o32(12);
            }
        }
        o32(strs[0].off), o32(SHT_SYMTAB), o32(0), o32(0), o32(symoff), o32(symsize), o32(stridx), o32(nlocal);
        o32(4), o32(16);
        o32(strs[1].off), o32(SHT_STRTAB), o32(0), o32(0), o32(stroff), o32(strsize), o32(0), o32(0), o32(1), o32(0);
        o32(strs[2].off), o32(SHT_STRTAB), o32(0), o32(0), o32(shstroff), o32(shstrsize), o32(0), o32(0), o32(1);
        o32(0);
        out[32] = shoff >> 24, out[33] = shoff >> 16, out[34] = shoff >> 8, out[35] = shoff;
        (void)ss;
        (void)nss;
    }
    (void)nameof;
    fd = open(outname, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        die("cannot create %s", outname);
    if (write(fd, out, nout) != (int)nout || close(fd) < 0) {
        unlink(outname);
        die("cannot write %s", outname);
    }
    if (verbose) {
        u32 code = 0, nr = 0;
        for (i = 0; i < ns; i++) {
            if (osec[i]->flags & SHF_ALLOC)
                code += osec[i]->loc;
            nr += osec[i]->nrel + osec[i]->ndrel;
        }
        os(1, outname);
        os(1, ": ");
        onum(1, code, 10);
        os(1, " bytes in ");
        onum(1, ns, 10);
        os(1, " sections, ");
        onum(1, nr, 10);
        os(1, " relocations, ");
        onum(1, nsym - 1, 10);
        os(1, " symbols, ");
        onum(1, pass, 10);
        os(1, " passes, the object ");
        onum(1, nout, 10);
        os(1, " bytes");
        oc(1, '\n');
        oflush(1);
    }
    return 0;
}

/* ---- one pass over the source ---- */
static struct src *mainsrc;
static void run(void)
{
    u32 i;
    char *l;
    for (i = 0; i < nsect; i++) {
        sects[i]->loc = 0;
        sects[i]->frag = 0;
        sects[i]->region = 0;
        sects[i]->fragloc = 0;
        sects[i]->fixseq = 0;
        if (final) {
            sects[i]->nrel = sects[i]->ndrel = 0;
        }
    }
    cur = sects[0];
    prevsec = 0;
    secsp = 0;
    nlcomm = 0;
    cpu = cpu0;         /* (.cpu and .arch hold from where they stand: each pass starts with the options' CPU) */
    rpos = 0;
    changed = 0;
    ncst = 0;
    nfr = 0;
    incomment = 0;
    macro_count = 0;
    while (macros) {
        struct macro *m = macros;
        int k;
        macros = m->next;
        for (k = 0; k < m->np; k++) {
            free(m->pn[k]);
            free(m->pdef[k]);
        }
        free(m->name);
        free(m->body);
        free(m);
    }
    push(mainsrc->buf, mainsrc->len, mainsrc->name, 0, 0);
    while ((l = nextline()) != 0)
        line(l);
    srcline = 0;
    {   /* the .lcomm objects, after .bss (gas: aligned to their size's power of 2, at most 8) */
        u32 k;
        cur = sects[2];
        for (k = 0; k < nlcomm; k++) {
            u32 v = lcomms[k].size;
            align(v >= 8 ? 8 : v >= 4 ? 4 : v >= 2 ? 2 : 1, 0, 1, 0);
            define(lcomms[k].y, 2, cur->loc, 0, 0);
            lcomms[k].y->type = 1;
            lcomms[k].y->size = v;
            putfill(v, 0);
        }
    }
    if (ncst)
        err(".if without .endif");
}

/* the one-system command line (docs/audit-2026-11-23-one-system.md section 3), written here: zas builds on the PC too.
 * (os() shows a control character as ?: the line's end goes by oc.) */
#include "../kernel/version.h"
static void usage_to(int fd)
{
    os(fd, "usage: zas [-v] [-m68000|-m68010|-m68020|-m68030|-m68040|-m68060|-mcpu=CPU] [-m68881|-mno-float] "
           "[--register-prefix-optional] [--pcrel] [-I DIR] [-o OBJECT] [FILE]");
    oc(fd, '\n');
    oflush(fd);
}
static void usage(void)
{
    usage_to(2);
    exit(2);
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--help")) { usage_to(1); return 0; }
    if (argc == 2 && !strcmp(argv[1], "--version")) { os(1, "zas " ZENIX_VSTR); oc(1, '\n'); oflush(1); return 0; }
    int i, fpu = -1;
    const char *in = 0;
    for (i = 1; i < argc; i++) {
        char *a = argv[i];
        if (!strcmp(a, "-o") && i + 1 < argc)
            outname = argv[++i];
        else if (!strcmp(a, "-I") && i + 1 < argc) {
            incdirs = xgrow(incdirs, &cincdir, nincdir + 1, sizeof *incdirs);
            incdirs[nincdir++] = argv[++i];
        } else if (!strncmp(a, "-I", 2) && a[2]) {
            incdirs = xgrow(incdirs, &cincdir, nincdir + 1, sizeof *incdirs);
            incdirs[nincdir++] = a + 2;
        } else if (!strcmp(a, "-mno-float"))
            fpu = 0;
        else if (!strcmp(a, "-m68881") || !strcmp(a, "-m68882"))
            fpu = 1;
        else if (!strcmp(a, "-m68851"))
            cpu |= CMMU;
        else if (!strncmp(a, "-mcpu=", 6) || (!strncmp(a, "-m680", 5) && strlen(a) == 7) ||
                 (!strncmp(a, "-m680", 5) && !strcmp(a + 7, "-60"))) {
            char *c = a + (a[2] == 'c' ? 6 : 2);
            if (!strcmp(c, "68020-60"))
                c = "68020";
            cpu_name(c);
        } else if (!strcmp(a, "-v"))
            verbose = 1;
        else if (!strcmp(a, "--register-prefix-optional"))
            regopt = 1;
        else if (!strcmp(a, "--pcrel"))
            pcrel_opt = 1;
        else if (!strncmp(a, "--gdwarf", 8) || !strcmp(a, "-g") || !strcmp(a, "--traditional-format") ||
                 !strcmp(a, "-W") || !strcmp(a, "--no-warn"))
            ;
        else if (!strcmp(a, "-") || a[0] != '-') {
            if (in)
                usage();
            in = a;
        } else {
            os(2, "zas: unknown option ");
            os(2, a);
            oc(2, '\n');
            usage();
        }
    }
    if (fpu == 0)
        cpu &= ~CFPU;
    else if (fpu == 1)
        cpu |= CFPU;
    if (!in)
        in = "-";
    cpu0 = cpu;
    mainsrc = loadfile(in);
    if (!mainsrc)
        die("cannot open %s", in);
    started = 1;
    newsect(".text", SHT_PROGBITS, SHF_ALLOC | SHF_EXEC, 0)->align = 4;
    newsect(".data", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, 0)->align = 4;
    newsect(".bss", SHT_NOBITS, SHF_ALLOC | SHF_WRITE, 0)->align = 4;
    for (pass = 1;; pass++) {
        run();
        if (final)
            break;
        if (!changed && pass > 2)
            final = 1;
        if (pass > 100)
            die("the sizes do not settle");
    }
    {   /* a numbered local label used but never defined */
        struct sym *y;
        for (y = symfirst; y; y = y->next)
            if (y->name[0] == 1 && strchr(y->name, 2) && (y->flags & F_USED) && y->sec == SEC_UNDEF) {
                char b[24];
                int k = strchr(y->name, 2) - y->name - 1;
                memcpy(b, y->name + 1, k < 20 ? k : 20);
                b[k < 20 ? k : 20] = 0;
                srcline = 0;
                err("local label %s is not defined", b);
            }
    }
    if (nerr) {
        unlink(outname);        /* (no stale object from an earlier run) */
        return 1;
    }
    return write_object();
}
