/* SPDX-License-Identifier: GPL-2.0-only */
/* zld: Zenix's linker (DESIGN D52 section 5, docs/d52-self-hosting.md; docs/man/zld.txt). It reads ELF32 m68k
 * relocatable objects and ar archives (libc.a, libgcc.a) and writes a Zenix program directly: ZEXE (a plain program,
 * the layout of link/user.ld) or ZEX2 (shared code, link/pure.ld), chosen by the magic word crt0 leaves at offset 4.
 * It does what `gcc -nostartfiles -T link/user.ld|pure.ld -Wl,-q -Wl,--gc-sections` followed by tools/mkzexe.py does,
 * and gives the same bytes: the order GNU ld 2.45 places sections in, pulls archive members in, merges strings in
 * (SHF_MERGE, bfd/merge.c's rules) and numbers the shared programs' address table in (bfd/elf32-m68k.c's GOT, whose
 * order follows libiberty's hash tables and ld's numbering of its input files) are followed rule for rule; each rule
 * is named where it is coded. tools/zld-host-test.sh links every program of the build both ways and compares.
 *
 * Standard C and POSIX only (open, read, lseek, write, malloc, qsort): the same file builds for the PC (the host
 * test) and for Zenix (a C library program). Every input is untrusted: every offset, size, index and count read from
 * a file is checked before use, and an error names the file and stops the link; nothing is written until the whole
 * program is ready. */
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

#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_RELA 4
#define SHT_NOTE 7
#define SHT_NOBITS 8
#define SHT_REL 9
#define SHT_GROUP 17
#define SHF_WRITE 1
#define SHF_ALLOC 2
#define SHF_MERGE 0x10
#define SHF_STRINGS 0x20
#define SHF_GROUP 0x200
#define SHF_GNU_RETAIN 0x200000
#define SHN_UNDEF 0
#define SHN_LORESERVE 0xff00
#define SHN_ABS 0xfff1
#define SHN_COMMON 0xfff2
#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STB_WEAK 2
#define STT_SECTION 3

/* ---- output: errors and the -v summary, without printf (its size) ---- */
static char obuf[256];
static int olen;
static int nerr;
static const char *outname;

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
/* %s, %u, %x (0x...), %d; the line goes to fd at the end */
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
            if (!s)
                s = "?";
            for (; *s; s++)    /* (a damaged file's names: no control characters to the terminal) */
                oc(fd, (u8)*s < 32 || *s == 127 ? '?' : *s);
        } else {
            u32 v = va_arg(ap, u32), base = *f == 'x' ? 16 : 10;
            char t[12];
            int n = 0;
            if (*f == 'd' && (i32)v < 0) {
                oc(fd, '-');
                v = -v;
            }
            if (base == 16) {
                oc(fd, '0');
                oc(fd, 'x');
            }
            do
                t[n++] = "0123456789abcdef"[v % base];
            while ((v /= base) != 0);
            while (n)
                oc(fd, t[--n]);
        }
    }
    oflush(fd);
}
static void say(const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    vsay(1, f, ap);
    va_end(ap);
}
/* an error: counted, the link goes on to find more where it can; die() stops at once */
static void err(const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    vsay(2, "zld: ", ap);
    vsay(2, f, ap);
    va_end(ap);
    oc(2, '\n');
    oflush(2);
    nerr++;
}
static void die(const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    vsay(2, "zld: ", ap);
    vsay(2, f, ap);
    va_end(ap);
    oc(2, '\n');
    oflush(2);
    exit(1);
}
static void *xalloc(u32 n)
{
    void *p = calloc(1, n ? n : 1);
    if (!p)
        die("out of memory (%u bytes more)", n);
    return p;
}
static void *xgrow(void *p, u32 n)
{
    p = realloc(p, n ? n : 1);
    if (!p)
        die("out of memory (%u bytes more)", n);
    return p;
}
static char *xstrdup(const char *s)
{
    u32 n = strlen(s) + 1;
    return memcpy(xalloc(n), s, n);
}
static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static u16 be16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }
static void put32(u8 *p, u32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

/* ---- input files: opened on demand, one at a time (Zenix gives a process 16 descriptors) ---- */
struct file {
    const char *path;
    u32 size;
    int argi;                 /* its place among the inputs (the order sections are placed in) */
};
static struct file *files;
static int nfiles, curf = -1, curfd = -1;

static void rd(int f, u32 off, void *buf, u32 len, const char *what)
{
    u8 *b = buf;
    if (f != curf) {
        if (curfd >= 0)
            close(curfd);
        curf = -1;
        curfd = open(files[f].path, O_RDONLY);
        if (curfd < 0)
            die("%s: cannot open", files[f].path);
        curf = f;
    }
    if (off > files[f].size || len > files[f].size - off)
        die("%s: %s lies past the end of the file", what, "data");
    if (lseek(curfd, off, SEEK_SET) != (off_t)off)
        die("%s: cannot seek", files[f].path);
    while (len) {
        long n = read(curfd, b, len);
        if (n <= 0)
            die("%s: read error", files[f].path);
        b += n;
        len -= n;
    }
}

/* ---- objects ---- */
struct rela {
    u32 off, info;
    i32 add;
};
struct msec;
struct sec {
    const char *name;
    u32 type, flags, size, off, entsize;
    u32 addr;                 /* the address in the program, once laid out */
    u32 nrel;
    struct rela *rel;         /* the relocations of an allocated section */
    struct msec *mg;          /* a merged section's map (SHF_MERGE) */
    u8 align;                 /* log2 of sh_addralign */
    u8 keep;                  /* reached from the start (--gc-sections) */
    u8 stmt;                  /* the place the layout gives it (S_*) */
    u8 gone;                  /* merged into another section: not placed */
};
struct esym {
    u32 name, value, size;
    u8 info, other;
    u16 shndx;
};
struct gsym;
struct obj {
    char *name;               /* "file.o" or "lib.a(member.o)" */
    int file;
    u32 base, size;           /* where its bytes are in the file */
    u32 moff;                 /* its place in the archive (0 for a plain object) */
    int id;                   /* ld's number for it (the address table's order, gotorder()) */
    int nsec, nsym, nlocal;
    struct sec *sec;
    struct esym *sym;
    char *str;
    u32 strsz;
    struct gsym **g;          /* the global symbol behind each symbol from nlocal on */
    struct obj *next;         /* in load order (ld's input_bfds) */
};
static struct obj *objs, **objtail = &objs;
static int nobjs;

/* ---- global symbols ---- */
enum { G_NEW, G_UNDEF, G_UNDEFW, G_DEF, G_DEFW };
enum { SC_NONE, SC_DEFSYM, SC_SCRIPT };
struct gsym {
    struct gsym *next;
    char *name;
    u8 st, script, outsec;    /* outsec: for a linker script symbol */
    struct obj *o;            /* definition: object and section index (script: none) */
    u16 shndx;
    u32 value;
    struct gsym *alias;       /* --defsym NAME=OTHER */
    struct obj *ref;          /* the first object that names it undefined */
    u32 gotkey;               /* ld's number for its address table entry (0: none) */
};
#define HSIZE 1024
static struct gsym *htab[HSIZE];
static u32 newundefs;         /* symbols that became undefined (the archive loop goes round again) */

static u32 strhash(const char *s)
{
    u32 h = 5381;
    while (*s)
        h = h * 33 + (u8)*s++;
    return h;
}
static struct gsym *lookup(const char *name, int create)
{
    u32 h = strhash(name) % HSIZE;
    struct gsym *g;
    for (g = htab[h]; g; g = g->next)
        if (!strcmp(g->name, name))
            return g;
    if (!create)
        return 0;
    g = xalloc(sizeof *g);
    g->name = xstrdup(name);
    g->next = htab[h];
    htab[h] = g;
    return g;
}

/* --wrap=SYM: an undefined SYM means __wrap_SYM, an undefined __real_SYM means SYM (ld's bfd_wrapped_link_hash_lookup) */
static char **wraps;
static int nwraps;
static char wbuf[512];
static const char *wrapped(const char *n)
{
    int i;
    for (i = 0; i < nwraps; i++) {
        u32 l = strlen(wraps[i]);
        if (!strcmp(n, wraps[i]) && l + 8 < sizeof wbuf) {
            memcpy(wbuf, "__wrap_", 7);
            memcpy(wbuf + 7, n, l + 1);
            return wbuf;
        }
        if (!strncmp(n, "__real_", 7) && !strcmp(n + 7, wraps[i]))
            return n + 7;
    }
    return n;
}

static void undefine(struct gsym *g, int weak, struct obj *o)
{
    if (g->st == G_NEW) {
        g->st = weak ? G_UNDEFW : G_UNDEF;
        g->ref = o;
        newundefs++;
    } else if (g->st == G_UNDEFW && !weak) {
        g->st = G_UNDEF;      /* a strong reference makes a weak one strong (not a new entry for ld's archive loop) */
        g->ref = o;
    }
}

/* the object's global symbols into the table (elf_link_add_object_symbols, for what static links meet) */
static void add_globals(struct obj *o)
{
    int i;
    for (i = o->nlocal; i < o->nsym; i++) {
        struct esym *s = &o->sym[i];
        const char *n = o->str + s->name;
        int bind = s->info >> 4;
        struct gsym *g;
        if (bind != STB_GLOBAL && bind != STB_WEAK) {
            err("%s: symbol %s has binding %u, which is not supported", o->name, n, bind);
            continue;
        }
        if (s->shndx == SHN_UNDEF) {
            g = lookup(wrapped(n), 1);
            undefine(g, bind == STB_WEAK, o);
        } else if (s->shndx == SHN_COMMON) {
            err("%s: %s is a common symbol (compile with -fno-common)", o->name, n);
            g = lookup(n, 1);
        } else {
            g = lookup(n, 1);
            if (g->script) {
                /* a --defsym or a linker script symbol: the script's value wins, as with ld */
            } else if (g->st == G_DEF) {
                if (bind == STB_GLOBAL)
                    err("multiple definition of `%s': %s and %s", n, g->o ? g->o->name : "?", o->name);
            } else if (g->st != G_DEFW || bind == STB_GLOBAL) {
                g->st = bind == STB_WEAK ? G_DEFW : G_DEF;
                g->o = o;
                g->shndx = s->shndx;
                g->value = s->value;
            }
        }
        o->g[i - o->nlocal] = g;
    }
}

static int lg2(u32 a)
{
    int n = 0;
    if (a & (a - 1))
        return -1;
    while (a > 1) {
        a >>= 1;
        n++;
    }
    return n;
}

static void set_stmt(struct sec *s);

/* read one ELF relocatable object (a file, or a member of an archive at base) */
static struct obj *load_obj(int f, u32 base, u32 size, char *name, u32 moff)
{
    struct obj *o = xalloc(sizeof *o);
    u8 eh[52], *sh;
    u32 shoff, shnum, shstrndx, i, symi = 0;
    struct sec *ss;
    char *shstr;
    u32 shstrsz;

    o->name = name;
    o->file = f;
    o->base = base;
    o->size = size;
    o->moff = moff;
    if (size < 52)
        die("%s: not an ELF object (too short)", name);
    rd(f, base, eh, 52, name);
    if (memcmp(eh, "\177ELF", 4))
        die("%s: not an ELF object", name);
    if (eh[4] != 1 || eh[5] != 2 || eh[6] != 1)
        die("%s: not a 32-bit big-endian ELF file", name);
    if (be16(eh + 16) != 1)
        die("%s: not a relocatable object (ELF type %u)", name, be16(eh + 16));
    if (be16(eh + 18) != 4)
        die("%s: not a 68k object (machine %u)", name, be16(eh + 18));
    shoff = be32(eh + 32);
    shnum = be16(eh + 48);
    shstrndx = be16(eh + 50);
    if (be16(eh + 46) != 40 || shnum == 0 || shnum >= SHN_LORESERVE)
        die("%s: bad section header table (%u entries of %u bytes)", name, shnum, be16(eh + 46));
    if (shoff > size || shnum * 40 > size - shoff)
        die("%s: the section header table lies past the end", name);
    if (shstrndx >= shnum)
        die("%s: bad section name table index %u", name, shstrndx);
    sh = xalloc(shnum * 40);
    rd(f, base + shoff, sh, shnum * 40, name);
    o->nsec = shnum;
    o->sec = ss = xalloc(shnum * sizeof *ss);
    for (i = 0; i < shnum; i++) {
        u8 *p = sh + i * 40;
        struct sec *s = &ss[i];
        int a;
        s->type = be32(p + 4);
        s->flags = be32(p + 8);
        s->off = be32(p + 16);
        s->size = be32(p + 20);
        s->entsize = be32(p + 36);
        a = lg2(be32(p + 32) ? be32(p + 32) : 1);
        if (a < 0 || a > 15)
            die("%s: section %u has alignment %u", name, i, be32(p + 32));
        s->align = a;
        if (s->type != SHT_NOBITS && i && (s->off > size || s->size > size - s->off))
            die("%s: section %u lies past the end of the file", name, i);
        s->stmt = 0;
    }
    /* section names */
    if (ss[shstrndx].type != SHT_STRTAB)
        die("%s: the section name table is not a string table", name);
    shstrsz = ss[shstrndx].size;
    shstr = xalloc(shstrsz + 1);
    rd(f, base + ss[shstrndx].off, shstr, shstrsz, name);
    for (i = 0; i < shnum; i++) {
        u32 no = be32(sh + i * 40);
        ss[i].name = no < shstrsz ? shstr + no : "";
    }
    /* the symbol table */
    for (i = 1; i < shnum; i++)
        if (ss[i].type == SHT_SYMTAB) {
            if (symi)
                die("%s: two symbol tables", name);
            symi = i;
        }
    if (symi) {
        struct sec *st = &ss[symi], *sstr;
        u32 link = be32(sh + symi * 40 + 24), info = be32(sh + symi * 40 + 28), n;
        u8 *raw;
        if (st->entsize != 16 || st->size % 16)
            die("%s: bad symbol table entry size", name);
        if (link >= shnum || ss[link].type != SHT_STRTAB)
            die("%s: the symbol table has no string table", name);
        n = st->size / 16;
        if (info > n || n > 0x1000000)
            die("%s: bad symbol table (%u symbols, %u local)", name, n, info);
        sstr = &ss[link];
        o->strsz = sstr->size;
        o->str = xalloc(sstr->size + 1);
        rd(f, base + sstr->off, o->str, sstr->size, name);
        raw = xalloc(st->size);
        rd(f, base + st->off, raw, st->size, name);
        o->nsym = n;
        o->nlocal = info ? info : (n ? 1 : 0);
        o->sym = xalloc(n * sizeof *o->sym);
        for (i = 0; i < n; i++) {
            u8 *p = raw + i * 16;
            struct esym *s = &o->sym[i];
            s->name = be32(p);
            s->value = be32(p + 4);
            s->size = be32(p + 8);
            s->info = p[12];
            s->other = p[13];
            s->shndx = be16(p + 14);
            if (s->name >= o->strsz)
                die("%s: symbol %u has a bad name", name, i);
            if (s->shndx >= shnum && s->shndx != SHN_ABS && s->shndx != SHN_COMMON)
                die("%s: symbol %s has a bad section index %u", name, o->str + s->name, s->shndx);
            if (i && ((s->info >> 4) == STB_LOCAL) != (i < (u32)o->nlocal))
                die("%s: symbol %s: a local symbol among the globals or a global among the locals", name,
                    o->str + s->name);
        }
        free(raw);
        o->g = xalloc((n - o->nlocal + 1) * sizeof *o->g);
    }
    /* relocations of the loaded sections (and of notes, which ld keeps for their references) */
    for (i = 1; i < shnum; i++) {
        struct sec *rs = &ss[i];
        u32 tgt = be32(sh + i * 40 + 28), link = be32(sh + i * 40 + 24), n, k;
        u8 *raw;
        if (rs->type == SHT_GROUP)
            die("%s: section groups (COMDAT) are not supported", name);
        if (rs->type != SHT_RELA && rs->type != SHT_REL)
            continue;
        if (tgt >= shnum || tgt == 0)
            die("%s: relocation section %s names section %u", name, rs->name, tgt);
        if (!(ss[tgt].flags & SHF_ALLOC) && ss[tgt].type != SHT_NOTE)
            continue;
        if (rs->type == SHT_REL)
            die("%s: REL relocations (%s) are not supported, only RELA", name, rs->name);
        if (rs->entsize != 12 || rs->size % 12 || link != symi || !symi)
            die("%s: bad relocation section %s", name, rs->name);
        if (ss[tgt].rel)
            die("%s: two relocation sections for %s", name, ss[tgt].name);
        n = rs->size / 12;
        raw = xalloc(rs->size);
        rd(f, base + rs->off, raw, rs->size, name);
        ss[tgt].rel = xalloc(n * sizeof(struct rela));
        ss[tgt].nrel = n;
        for (k = 0; k < n; k++) {
            struct rela *r = &ss[tgt].rel[k];
            r->off = be32(raw + k * 12);
            r->info = be32(raw + k * 12 + 4);
            r->add = (i32)be32(raw + k * 12 + 8);
            if ((r->info >> 8) >= (u32)o->nsym)
                die("%s: relocation %u in %s names symbol %u of %u", name, k, rs->name, r->info >> 8, o->nsym);
        }
        free(raw);
    }
    free(sh);
    for (i = 1; i < shnum; i++)
        set_stmt(&ss[i]);
    o->id = -1;
    *objtail = o;
    objtail = &o->next;
    nobjs++;
    add_globals(o);
    return o;
}

/* ---- ld's numbering of its input files (bfd ids). It only decides the order of a shared program's address table
 * (gotorder()). Found by fitting every ZEX2 program of the build: the output file is 0; each object takes two (its
 * own, then the LTO plugin's empty stand-in, since gcc always passes the plugin); each archive takes two when opened
 * (itself and its first member, opened to check the format), then two for each member it gives, in the order given. */
static int nextid = 1;

/* ---- archives ---- */
static char *member_name(const char *ar, const u8 *hdr, const char *longn, u32 longsz)
{
    char nm[260];
    u32 n = 0, i;
    if (hdr[0] == '/' && hdr[1] >= '0' && hdr[1] <= '9') {
        u32 o = 0;
        for (i = 1; i < 16 && hdr[i] >= '0' && hdr[i] <= '9'; i++)
            o = o * 10 + hdr[i] - '0';
        while (o < longsz && longn[o] != '/' && longn[o] != '\n' && n < 255)
            nm[n++] = longn[o++];
    } else
        for (i = 0; i < 16 && hdr[i] != '/' && hdr[i] != ' '; i++)
            nm[n++] = hdr[i];
    nm[n] = 0;
    {
        u32 la = strlen(ar);
        char *r = xalloc(la + n + 3);
        memcpy(r, ar, la);
        r[la] = '(';
        memcpy(r + la + 1, nm, n);
        r[la + 1 + n] = ')';
        return r;
    }
}
static u32 ar_size(const u8 *h, const char *path)
{
    u32 v = 0, i;
    for (i = 48; i < 58 && h[i] != ' '; i++) {
        if (h[i] < '0' || h[i] > '9' || v > 0x19999999)
            die("%s: bad archive member header", path);
        v = v * 10 + h[i] - '0';
    }
    if (h[58] != '`' || h[59] != '\n')
        die("%s: bad archive member header", path);
    return v;
}
static void load_archive(int f)
{
    const char *path = files[f].path;
    u32 fsz = files[f].size, p = 8, nsym = 0, i, longsz = 0, *offs = 0, mapsz = 0;
    char *names = 0, *longn = 0, **namep;
    u8 h[60], *included;
    int loop;
    struct { u32 off; struct obj *o; } *mem = 0;
    int nmem = 0;

    nextid += 2;              /* the archive and the first member ld opens to check it */
    /* the symbol index ("/") and the long name table ("//") */
    while (p + 60 <= fsz) {
        u32 sz;
        rd(f, p, h, 60, path);
        sz = ar_size(h, path);
        if (sz > fsz - p - 60)
            die("%s: archive member at %u runs past the end", path, p);
        if (h[0] == '/' && h[1] == ' ') {
            u8 *m;
            if (sz < 4)
                die("%s: bad archive symbol index", path);
            m = xalloc(sz + 1);
            rd(f, p + 60, m, sz, path);
            nsym = be32(m);
            if (nsym > (sz - 4) / 5)
                die("%s: bad archive symbol index (%u symbols)", path, nsym);
            offs = xalloc((nsym + 1) * 4);
            for (i = 0; i < nsym; i++)
                offs[i] = be32(m + 4 + i * 4);
            mapsz = sz - 4 - nsym * 4;
            names = xalloc(mapsz + 1);
            memcpy(names, m + 4 + nsym * 4, mapsz);
            free(m);
        } else if (h[0] == '/' && h[1] == '/' && h[2] == ' ') {
            longn = xalloc(sz + 1);
            rd(f, p + 60, longn, sz, path);
            longsz = sz;
        } else if (h[0] == '/' && !memcmp(h, "/SYM64/", 7))
            die("%s: 64-bit archive symbol index not supported", path);
        else
            break;
        p += 60 + sz + (sz & 1);
    }
    if (!offs) {
        if (p + 60 > fsz)
            return;           /* an empty archive */
        die("%s: archive has no symbol index; run ranlib", path);
    }
    namep = xalloc((nsym + 1) * sizeof *namep);
    {
        u32 q = 0;
        for (i = 0; i < nsym; i++) {
            if (q >= mapsz)
                die("%s: bad archive symbol index (names)", path);
            namep[i] = names + q;
            while (q < mapsz && names[q])
                q++;
            q++;
        }
    }
    included = xalloc(nsym + 1);
    mem = xalloc((nsym + 1) * sizeof *mem);
    /* elf_link_add_archive_symbols: go through the index in order, pulling the member of each symbol that is
     * undefined (not weakly), again while a member added new undefined symbols */
    do {
        loop = 0;
        for (i = 0; i < nsym; i++) {
            struct gsym *g;
            u32 off = offs[i], sz;
            int k;
            if (included[i])
                continue;
            for (k = 0; k < nmem && mem[k].off != off; k++)
                ;
            if (k < nmem) {
                included[i] = 1;
                continue;
            }
            g = lookup(namep[i], 0);
            if (!g || g->st == G_NEW)
                continue;
            if (g->st != G_UNDEF) {
                if (g->st != G_UNDEFW)
                    included[i] = 1;
                continue;
            }
            if (off < 8 || off > fsz || fsz - off < 60)
                die("%s: archive symbol %s points outside the file", path, namep[i]);
            rd(f, off, h, 60, path);
            sz = ar_size(h, path);
            if (sz > fsz - off - 60)
                die("%s: archive member at %u runs past the end", path, off);
            {
                u32 before = newundefs;
                struct obj *o;
                mem[nmem].off = off;
                o = load_obj(f, off + 60, sz, member_name(path, h, longn, longsz), off);
                o->id = nextid;
                nextid += 2;
                mem[nmem++].o = o;
                if (newundefs != before)
                    loop = 1;
                included[i] = 1;
            }
        }
    } while (loop);
    free(included);
    free(mem);
    free(namep);
    free(names);
    free(offs);
    free(longn);
}

/* ---- the layout (link/user.ld, link/pure.ld): where each section goes, by the first rule its name matches ---- */
enum {
    S_NONE, S_START, S_TEXT, S_RODATA, S_INITS, S_INIT, S_FINIS, S_FINI, S_FINIR, S_DATA, S_BSS, S_DISCARD,
    S_ORPHAN
};
enum { O_TEXT, O_RODATA, O_GOT, O_GOTPLT, O_DATA, O_BSS, O_NONE };
static const u8 stmt_out[] = { O_NONE, O_TEXT, O_TEXT, O_RODATA, O_DATA, O_DATA, O_DATA, O_DATA, O_DATA, O_DATA,
                               O_BSS, O_NONE, O_NONE };
static int pure;              /* the ZEX2 layout */

static int pfx(const char *s, const char *p) { return !strncmp(s, p, strlen(p)); }
static void set_stmt(struct sec *s)
{
    const char *n = s->name;
    if (!(s->flags & SHF_ALLOC))
        s->stmt = S_NONE;
    else if (!strcmp(n, ".text.start"))
        s->stmt = S_START;
    else if (pfx(n, ".text"))
        s->stmt = S_TEXT;
    else if (pfx(n, ".rodata"))
        s->stmt = S_RODATA;
    else if (pfx(n, ".init_array."))
        s->stmt = S_INITS;
    else if (!strcmp(n, ".init_array"))
        s->stmt = S_INIT;
    else if (pfx(n, ".fini_array."))
        s->stmt = S_FINIS;
    else if (!strcmp(n, ".fini_array"))
        s->stmt = S_FINI;
    else if (pfx(n, ".fini_array"))
        s->stmt = S_FINIR;
    else if (pfx(n, ".data"))
        s->stmt = S_DATA;
    else if (pfx(n, ".bss"))
        s->stmt = S_BSS;
    else if (pfx(n, ".comment") || pfx(n, ".note") || pfx(n, ".eh_frame"))
        s->stmt = S_DISCARD;  /* .eh_frame: pure.ld discards it; user.ld leaves it to --gc-sections (checked) */
    else
        s->stmt = S_ORPHAN;
}

/* ---- symbols as relocations see them ---- */
struct target {
    u32 s;                    /* S, or S+A when absorbed */
    u8 kind;                  /* T_SEC, T_ABS, T_WEAK (undefined weak: 0), T_UNDEF */
    u8 out;                   /* the output section (O_*) */
    u8 absorbed;              /* the addend is in s already (a section symbol of a merged section) */
    const char *name;
};
enum { T_SEC, T_ABS, T_WEAK, T_UNDEF };
static u32 merged_off(struct sec *s, u32 off);
static u32 got_addr, gotplt_addr, ngot;

static u32 sec_value(struct obj *o, u32 shndx, u32 v)
{
    struct sec *s = &o->sec[shndx];
    return s->mg ? merged_off(s, v) : s->addr + v;
}
static void gtarget(struct gsym *g, struct target *t, int depth)
{
    t->name = g->name;
    if (g->script == SC_DEFSYM) {
        if (!g->alias) {
            t->kind = T_ABS;
            t->s = g->value;
            t->out = O_NONE;
            return;
        }
        if (depth > 8)
            die("--defsym %s: a loop", g->name);
        gtarget(g->alias, t, depth + 1);
        t->name = g->name;
        /* the target in a dropped section: ld makes it absolute (its value in the section) */
        return;
    }
    if (g->script == SC_SCRIPT) {
        t->kind = T_SEC;
        t->s = g->value;
        t->out = g->outsec;
        return;
    }
    if (g->st == G_UNDEF || g->st == G_NEW) {
        t->kind = T_UNDEF;
        t->s = 0;
        t->out = O_NONE;
        return;
    }
    if (g->st == G_UNDEFW) {
        t->kind = T_WEAK;
        t->s = 0;
        t->out = O_NONE;
        return;
    }
    if (g->shndx == SHN_ABS) {
        t->kind = T_ABS;
        t->s = g->value;
        t->out = O_NONE;
        return;
    }
    {
        struct sec *s = &g->o->sec[g->shndx];
        if (!s->keep || stmt_out[s->stmt] == O_NONE) {
            t->kind = T_ABS;
            t->s = g->value;
            t->out = O_NONE;
            return;
        }
        t->kind = T_SEC;
        t->s = sec_value(g->o, g->shndx, g->value);
        t->out = stmt_out[s->stmt];
    }
}
/* the target of relocation r of object o */
static void rtarget(struct obj *o, struct rela *r, struct target *t)
{
    u32 si = r->info >> 8;
    t->absorbed = 0;
    if (si >= (u32)o->nlocal) {
        gtarget(o->g[si - o->nlocal], t, 0);
        return;
    }
    {
        struct esym *e = &o->sym[si];
        t->name = e->name ? o->str + e->name : 0;
        if (si == 0 || e->shndx == SHN_UNDEF) {
            t->kind = T_WEAK;
            t->s = 0;
            t->out = O_NONE;
            return;
        }
        if (e->shndx == SHN_ABS || e->shndx == SHN_COMMON) {
            t->kind = T_ABS;
            t->s = e->value;
            t->out = O_NONE;
            return;
        }
        {
            struct sec *s = &o->sec[e->shndx];
            if (!t->name || !*t->name)
                t->name = s->name;
            if (!s->keep || stmt_out[s->stmt] == O_NONE) {
                t->kind = T_ABS;
                t->s = e->value;
                t->out = O_NONE;
                return;
            }
            t->kind = T_SEC;
            t->out = stmt_out[s->stmt];
            if (s->mg && (e->info & 15) == STT_SECTION) {
                t->s = merged_off(s, e->value + r->add);   /* _bfd_elf_rela_local_sym */
                t->absorbed = 1;
            } else
                t->s = sec_value(o, e->shndx, e->value);
        }
    }
}
/* the section a relocation's symbol lies in, for --gc-sections */
static struct sec *rsec(struct obj *o, struct rela *r, struct obj **ro)
{
    u32 si = r->info >> 8;
    struct gsym *g;
    int depth = 0;
    if (si == 0)
        return 0;
    if (si < (u32)o->nlocal) {
        struct esym *e = &o->sym[si];
        if (e->shndx == SHN_UNDEF || e->shndx >= SHN_LORESERVE)
            return 0;
        *ro = o;
        return &o->sec[e->shndx];
    }
    g = o->g[si - o->nlocal];
    while (g->script == SC_DEFSYM && g->alias && depth++ < 8)
        g = g->alias;
    if (g->script || (g->st != G_DEF && g->st != G_DEFW) || g->shndx >= SHN_LORESERVE)
        return 0;
    *ro = g->o;
    return &g->o->sec[g->shndx];
}

/* ---- --gc-sections (bfd_elf_gc_sections): keep what the start, KEEP() and notes reach ---- */
struct mark {
    struct obj *o;
    struct sec *s;
};
static struct mark *mstack;
static u32 msp, mcap;
static void markpush(struct obj *o, struct sec *s)
{
    if (s->keep)
        return;
    s->keep = 1;
    if (msp == mcap) {
        mcap = mcap ? mcap * 2 : 256;
        mstack = xgrow(mstack, mcap * sizeof *mstack);
    }
    mstack[msp].o = o;
    mstack[msp++].s = s;
}
static void gc(void)
{
    struct obj *o;
    struct gsym *g;
    static const char *const roots[] = { "_start", "_init", "_fini" };
    unsigned i;
    for (i = 0; i < 3; i++) {
        g = lookup(roots[i], 0);
        if (g && !g->script && (g->st == G_DEF || g->st == G_DEFW) && g->shndx < SHN_LORESERVE)
            markpush(g->o, &g->o->sec[g->shndx]);
        else if (!i)
            die("no _start: link crt0.o (or crt0-pure.o) first");
    }
    for (o = objs; o; o = o->next)
        for (i = 1; i < (unsigned)o->nsec; i++) {
            struct sec *s = &o->sec[i];
            if ((s->stmt >= S_START && s->stmt <= S_FINIR && s->stmt != S_TEXT && s->stmt != S_RODATA) ||
                s->type == SHT_NOTE || (s->flags & SHF_GNU_RETAIN))
                markpush(o, s);
        }
    while (msp) {
        struct mark m = mstack[--msp];
        u32 k;
        for (k = 0; k < m.s->nrel; k++) {
            struct obj *ro;
            struct sec *t = rsec(m.o, &m.s->rel[k], &ro);
            if (t)
                markpush(ro, t);
        }
    }
    free(mstack);
    mstack = 0;
    mcap = 0;
}

/* ---- the shared programs' address table (.got): ld's order, bfd/elf32-m68k.c with libiberty's hash tables ---- */
static const u32 primes[] = { 7, 13, 31, 61, 127, 251, 509, 1021, 2039, 4093, 8191, 16381, 32749, 65521, 131071 };
struct gent {
    u32 hash;
    int bid;                  /* the object's number, -1 for a global symbol */
    u32 symndx;               /* the local symbol's index, or the global's gotkey */
    u8 cls;                   /* 0: an 8-bit offset reaches it, 1: 16-bit, 2: 32-bit */
    u32 off;                  /* its offset in .got */
    struct gsym *g;           /* the global (to find its value) */
    struct obj *o;            /* the object of a local */
};
struct ht {
    u32 size, pi, n;
    void **e;
    int bfd;                  /* entries are per-object tables (hash = their id) rather than gents */
};
static u32 ehash(void *x, int bfd) { return bfd ? (u32)((struct ht *)x)->bfd : ((struct gent *)x)->hash; }
static int heq(void *a, void *b, int bfd)
{
    if (bfd)
        return ((struct ht *)a)->bfd == ((struct ht *)b)->bfd;
    return ((struct gent *)a)->bid == ((struct gent *)b)->bid && ((struct gent *)a)->symndx == ((struct gent *)b)->symndx;
}
static u32 hpi(u32 n)
{
    u32 i;
    for (i = 0; i < sizeof primes / sizeof primes[0]; i++)
        if (primes[i] >= n)
            return i;
    die("the address table is too large");
    return 0;
}
static void htinit(struct ht *t, u32 n, int bfd)
{
    t->pi = hpi(n);
    t->size = primes[t->pi];
    t->n = 0;
    t->e = xalloc(t->size * sizeof *t->e);
    t->bfd = bfd ? -2 : -3;   /* (marks the kind; a per-object table's own id is set by its creator) */
}
static void htexpand(struct ht *t, int kind)
{
    void **old = t->e;
    u32 osize = t->size, i;
    if (t->n * 2 > t->size || (t->n * 8 < t->size && t->size > 32)) {
        t->pi = hpi(t->n * 2);
        t->size = primes[t->pi];
    }
    t->e = xalloc(t->size * sizeof *t->e);
    for (i = 0; i < osize; i++)
        if (old[i]) {
            u32 h = ehash(old[i], kind), j = h % t->size;
            if (t->e[j]) {
                u32 h2 = 1 + h % (t->size - 2);
                do {
                    j += h2;
                    if (j >= t->size)
                        j -= t->size;
                } while (t->e[j]);
            }
            t->e[j] = old[i];
        }
    free(old);
}
/* htab_find_slot_with_hash: the entry equal to x, or x itself inserted (insert) */
static void *htfind(struct ht *t, void *x, int insert, int kind)
{
    u32 h, j;
    if (insert && t->size * 3 <= t->n * 4)
        htexpand(t, kind);
    h = ehash(x, kind);
    j = h % t->size;
    if (t->e[j]) {
        u32 h2;
        if (heq(t->e[j], x, kind))
            return t->e[j];
        h2 = 1 + h % (t->size - 2);
        for (;;) {
            j += h2;
            if (j >= t->size)
                j -= t->size;
            if (!t->e[j])
                break;
            if (heq(t->e[j], x, kind))
                return t->e[j];
        }
    }
    if (!insert)
        return 0;
    t->e[j] = x;
    t->n++;
    return x;
}
static struct ht bfd2got;     /* entries: struct objgot */
struct objgot {
    struct ht h;              /* .bfd = its id (the hash) */
    struct ht ents;
};
static struct ht big;         /* the merged table */
static u32 gkeys = 1;

static int got_type(u32 t) { return t >= 7 && t <= 12; }
static u8 got_cls(u32 t) { return t == 9 || t == 12 ? 0 : t == 8 || t == 11 ? 1 : 2; }

static void got_scan(void)
{
    struct obj *o;
    for (o = objs; o; o = o->next) {
        int i;
        struct objgot *og;
        for (i = 1; i < o->nsec; i++) {
            struct sec *s = &o->sec[i];
            u32 k;
            if (!s->keep || !(s->flags & SHF_ALLOC))
                continue;
            for (k = 0; k < s->nrel; k++) {
                u32 t = s->rel[k].info & 255, si = s->rel[k].info >> 8;
                struct gent *e, key;
                if (!got_type(t))
                    continue;
                /* ld looks both tables up with INSERT for every relocation, and libiberty grows a table on such a
                 * lookup even when the entry is there: the same here, or the order differs */
                {
                    struct objgot *q = xalloc(sizeof *q);
                    q->h.bfd = o->id;
                    og = htfind(&bfd2got, q, 1, 1);
                    if (og != q)
                        free(q);
                    else
                        htinit(&og->ents, 32, 0);
                }
                memset(&key, 0, sizeof key);
                if (si >= (u32)o->nlocal) {
                    struct gsym *g = o->g[si - o->nlocal];
                    if (!g->gotkey)
                        g->gotkey = gkeys++;
                    key.bid = -1;
                    key.symndx = g->gotkey;
                    key.g = g;
                } else {
                    key.bid = o->id;
                    key.symndx = si;
                    key.o = o;
                }
                /* elf_m68k_got_entry_hash: symndx + (bfd ? bfd->id : -1) + the GOT reloc type (R_68K_GOT32O) */
                key.hash = key.symndx + (u32)key.bid + 10;
                key.cls = got_cls(t);
                {
                    struct gent *ne = xalloc(sizeof *ne);
                    *ne = key;
                    e = htfind(&og->ents, ne, 1, 0);
                    if (e != ne) {
                        free(ne);
                        if (key.cls < e->cls)
                            e->cls = key.cls;
                    }
                }
            }
        }
    }
}
/* htab_traverse (which first shrinks a table that is too empty) */
static void **travlist(struct ht *t, int resize, int kind, u32 *n)
{
    void **l;
    u32 i, k = 0;
    if (resize && t->n * 8 < t->size && t->size > 32)
        htexpand(t, kind);
    l = xalloc((t->n + 1) * sizeof *l);
    for (i = 0; i < t->size; i++)
        if (t->e[i])
            l[k++] = t->e[i];
    *n = k;
    return l;
}
/* elf_m68k_partition_multi_got and elf_m68k_finalize_got_offsets for one table: the offsets of every entry */
static void got_assign(void)
{
    u32 nb, i, n8 = 0, n16 = 0, nall = 0, off[3];
    void **bl;
    htinit(&big, 32, 0);
    if (!bfd2got.n)
        return;
    bl = travlist(&bfd2got, 1, 1, &nb);
    for (i = 0; i < nb; i++) {
        struct objgot *og = bl[i];
        u32 ne, k;
        void **el = travlist(&og->ents, 0, 0, &ne);
        if (i == 0) {
            for (k = 0; k < ne; k++)
                htfind(&big, el[k], 1, 0);
        } else {
            struct ht diff;
            void **dl;
            u32 nd;
            htinit(&diff, 32, 0);
            for (k = 0; k < ne; k++) {
                struct gent *e = el[k], *b = htfind(&big, e, 0, 0);
                if (!b)
                    htfind(&diff, e, 1, 0);
                else if (e->cls < b->cls)
                    b->cls = e->cls;
            }
            dl = travlist(&diff, 0, 0, &nd);
            for (k = 0; k < nd; k++)
                htfind(&big, dl[k], 1, 0);
            free(dl);
            free(diff.e);
        }
        free(el);
    }
    free(bl);
    {
        u32 ne;
        void **el = travlist(&big, 0, 0, &ne);
        for (i = 0; i < ne; i++) {
            struct gent *e = el[i];
            if (e->cls == 0)
                n8++;
            if (e->cls <= 1)
                n16++;
            nall++;
        }
        free(el);
    }
    if (n8 > 0x20 || n16 > 0x2000)
        die("the address table is too large: %u entries reached with 16-bit offsets (at most 8192)", n16);
    off[0] = 0;
    off[1] = 4 * n8;
    off[2] = 4 * n16;
    {
        u32 ne;
        void **el = travlist(&big, 1, 0, &ne);
        for (i = 0; i < ne; i++) {
            struct gent *e = el[i];
            e->off = off[e->cls];
            off[e->cls] += 4;
        }
        free(el);
    }
    ngot = nall;
}
static struct gent *got_find(struct obj *o, u32 si)
{
    struct gent key;
    memset(&key, 0, sizeof key);
    if (si >= (u32)o->nlocal) {
        key.bid = -1;
        key.symndx = o->g[si - o->nlocal]->gotkey;
    } else {
        key.bid = o->id;
        key.symndx = si;
    }
    key.hash = key.symndx + (u32)key.bid + 10;
    return htfind(&big, &key, 0, 0);
}

/* ---- SHF_MERGE: identical constants and strings once, a string that ends another inside it (bfd/merge.c) ---- */
struct ment {
    u32 str, len, align, idx, next;
    u32 suffix;               /* the entry this one is the tail of (when align is 0) */
    u32 hnext;
};
struct mgrp {
    struct mgrp *next;
    u32 flags, entsize;
    u8 align, out;
    struct sec *repr;
    u8 *pool;
    u32 poolsz, poolcap;
    struct ment *ent;
    u32 nent, entcap;
    u32 *hash, hsize;
    u32 first, last;          /* the chain in the order first seen (1-based entry numbers) */
    u32 size;
    int padok;
};
struct msec {
    struct mgrp *g;
    u32 n, *ofs, *ent;        /* input offset -> entry (sorted), n entries and a sentinel */
};
static struct mgrp *mgrps;
static struct mgrp *sortg;

static u32 blobhash(const u8 *p, u32 n)
{
    u32 h = 2166136261u;
    while (n--)
        h = (h ^ *p++) * 16777619u;
    return h;
}
static u32 ment_lookup(struct mgrp *g, const u8 *p, u32 len, u32 align)
{
    u32 h = blobhash(p, len), b, e;
    if (!g->hsize || g->nent * 2 >= g->hsize) {
        u32 i, ns = g->hsize ? g->hsize * 2 : 256;
        u32 *nh = xalloc(ns * sizeof *nh);
        for (i = 0; i < g->nent; i++) {
            struct ment *m = &g->ent[i];
            u32 bb = blobhash(g->pool + m->str, m->len) & (ns - 1);
            while (nh[bb])
                bb = (bb + 1) & (ns - 1);
            nh[bb] = i + 1;
        }
        free(g->hash);
        g->hash = nh;
        g->hsize = ns;
    }
    for (b = h & (g->hsize - 1); (e = g->hash[b]) != 0; b = (b + 1) & (g->hsize - 1)) {
        struct ment *m = &g->ent[e - 1];
        if (m->len == len && !memcmp(g->pool + m->str, p, len)) {
            if (m->align < align)
                m->align = align;
            return e;
        }
    }
    if (g->nent == g->entcap) {
        g->entcap = g->entcap ? g->entcap * 2 : 64;
        g->ent = xgrow(g->ent, g->entcap * sizeof *g->ent);
    }
    while (g->poolsz + len > g->poolcap) {
        g->poolcap = g->poolcap ? g->poolcap * 2 : 1024;
        g->pool = xgrow(g->pool, g->poolcap);
    }
    memcpy(g->pool + g->poolsz, p, len);
    e = ++g->nent;
    g->ent[e - 1].str = g->poolsz;
    g->ent[e - 1].len = len;
    g->ent[e - 1].align = align;
    g->ent[e - 1].next = 0;
    g->ent[e - 1].idx = 0;
    g->ent[e - 1].suffix = 0;
    g->poolsz += len;
    g->hash[b] = e;
    if (g->last)
        g->ent[g->last - 1].next = e;
    else
        g->first = e;
    g->last = e;
    return e;
}
static int revcmp(const void *a, const void *b)
{
    const struct ment *A = &sortg->ent[*(const u32 *)a - 1], *B = &sortg->ent[*(const u32 *)b - 1];
    const u8 *s = sortg->pool + A->str + A->len - 1, *t = sortg->pool + B->str + B->len - 1;
    u32 l = A->len < B->len ? A->len : B->len;
    while (l--) {
        if (*s != *t)
            return (int)*s - (int)*t;
        s--;
        t--;
    }
    return (int)A->len - (int)B->len;
}
static int revcmp_align(const void *a, const void *b)
{
    const struct ment *A = &sortg->ent[*(const u32 *)a - 1], *B = &sortg->ent[*(const u32 *)b - 1];
    int ta = (int)(A->len & (A->align - 1)) - (int)(B->len & (A->align - 1));
    return ta ? ta : revcmp(a, b);
}
/* _bfd_elf_merge_sections: sections alike (flags, entry size, alignment, output) form a group; the first one (in load
 * order) holds the group's merged contents, the rest take no room */
static void merge(void)
{
    struct obj *o;
    struct mgrp *g;
    for (o = objs; o; o = o->next) {
        int i;
        for (i = 1; i < o->nsec; i++) {
            struct sec *s = &o->sec[i];
            u32 al, pad;
            u8 *buf, *p, *end;
            struct msec *ms;
            if (!(s->flags & SHF_MERGE) || !s->keep || stmt_out[s->stmt] == O_NONE)
                continue;
            if (!s->size || s->type == SHT_NOBITS || !s->entsize || s->size % s->entsize || s->nrel)
                continue;
            al = 1u << s->align;
            if ((s->entsize < al && ((s->entsize & (s->entsize - 1)) || !(s->flags & SHF_STRINGS))) ||
                (s->entsize > al && (s->entsize & (al - 1))))
                continue;
            for (g = mgrps; g; g = g->next)
                if (g->flags == (s->flags & (SHF_MERGE | SHF_STRINGS)) && g->entsize == s->entsize &&
                    g->align == s->align && g->out == stmt_out[s->stmt])
                    break;
            if (!g) {
                g = xalloc(sizeof *g);
                g->flags = s->flags & (SHF_MERGE | SHF_STRINGS);
                g->entsize = s->entsize;
                g->align = s->align;
                g->out = stmt_out[s->stmt];
                g->repr = s;
                g->padok = 1;
                g->next = mgrps;
                mgrps = g;
            } else
                s->gone = 1;
            /* record_section */
            pad = (s->flags & SHF_STRINGS) ? s->entsize : 0;
            buf = xalloc(s->size + pad);
            rd(o->file, o->base + s->off, buf, s->size, o->name);
            ms = s->mg = xalloc(sizeof *ms);
            ms->g = g;
            ms->ofs = xalloc((s->size / s->entsize + 1) * 4);
            ms->ent = xalloc((s->size / s->entsize + 1) * 4);
            end = buf + s->size;
            for (p = buf; p < end;) {
                u32 len, ofs = p - buf, ea, mask = al - 1;
                if (s->flags & SHF_STRINGS) {
                    if (s->entsize == 1)
                        len = strlen((char *)p) + 1;
                    else {
                        u32 k, n = 0;
                        const u8 *q = p;
                        for (;;) {
                            for (k = 0; k < s->entsize && !q[k]; k++)
                                ;
                            if (k == s->entsize)
                                break;
                            q += s->entsize;
                            n++;
                        }
                        len = (n + 1) * s->entsize;
                    }
                } else
                    len = s->entsize;
                ea = ofs ? (ofs & -ofs) : 0;
                if (!ea || ea > mask)
                    ea = mask + 1;
                ms->ofs[ms->n] = ofs;
                ms->ent[ms->n++] = ment_lookup(g, p, len, ea);
                p += len;
            }
            ms->ofs[ms->n] = s->size;
            ms->ent[ms->n] = 0;
            free(buf);
            if (g->padok && (s->size & (al - 1)))
                g->padok = 0;
            else if (g->padok)
                g->padok = al;
        }
    }
    for (g = mgrps; g; g = g->next) {
        u32 size = 0, e, es = g->entsize;
        if (g->flags & SHF_STRINGS) {
            u32 *arr = xalloc(g->nent * 4 + 4), n = 0, alignment = 0, a;
            for (e = g->first; e; e = g->ent[e - 1].next) {
                struct ment *m = &g->ent[e - 1];
                arr[n++] = e;
                m->len -= es;
                if (alignment != m->align)
                    alignment = alignment ? (u32)-1 : m->align;
            }
            sortg = g;
            qsort(arr, n, 4, alignment != (u32)-1 && alignment > es ? revcmp_align : revcmp);
            if (n) {
                struct ment *last = &g->ent[arr[n - 1] - 1];
                last->len += es;
                for (a = n - 1; a-- > 0;) {
                    struct ment *c = &g->ent[arr[a] - 1];
                    c->len += es;
                    if (last->align >= c->align && !((last->len - c->len) & (c->align - 1)) &&
                        last->len > c->len &&
                        !memcmp(g->pool + last->str + last->len - c->len, g->pool + c->str, c->len)) {
                        c->suffix = last - g->ent + 1;
                        c->align = 0;
                    } else
                        last = c;
                }
            }
            free(arr);
        }
        for (e = g->first; e; e = g->ent[e - 1].next) {
            struct ment *m = &g->ent[e - 1];
            if (m->align) {
                size = (size + m->align - 1) & ~(m->align - 1);
                m->idx = size;
                size += m->len;
            }
        }
        for (e = g->first; e; e = g->ent[e - 1].next) {
            struct ment *m = &g->ent[e - 1];
            if (!m->align && m->suffix) {
                struct ment *t = &g->ent[m->suffix - 1];
                m->idx = t->idx + (t->len - m->len);
            }
        }
        if (g->padok > 1)
            size = (size + g->padok - 1) & ~(g->padok - 1);
        g->size = size;
        g->repr->size = size; /* the representative takes the merged size; its old contents are not read again */
    }
}
/* _bfd_merged_section_offset: an offset in an input section to the address in the merged contents */
static u32 merged_off(struct sec *s, u32 off)
{
    struct msec *ms = s->mg;
    u32 lo = 0, hi = ms->n;
    if (off >= ms->ofs[ms->n])
        return ms->g->repr->addr + ms->g->size;
    while (hi - lo > 1) {
        u32 mid = (lo + hi) / 2;
        if (ms->ofs[mid] <= off)
            lo = mid;
        else
            hi = mid;
    }
    return ms->g->repr->addr + ms->g->ent[ms->ent[lo] - 1].idx + off - ms->ofs[lo];
}
static void merge_emit(struct mgrp *g, u8 *img)
{
    u32 e, off = 0;
    u8 *p = img + g->repr->addr;
    for (e = g->first; e; e = g->ent[e - 1].next) {
        struct ment *m = &g->ent[e - 1];
        if (!m->align)
            continue;
        off = (off + m->align - 1) & ~(m->align - 1);
        memcpy(p + off, g->pool + m->str, m->len);
        off += m->len;
    }
}

/* ---- laying the program out ---- */
struct place {
    struct obj *o;
    struct sec *s;
};
static struct place *plist[S_ORPHAN];
static u32 pn[S_ORPHAN];
static u32 oend[O_NONE], ostart[O_NONE];
static u8 oload[O_NONE], ohas[O_NONE];
static u32 text_end, bss_start, bss_end;

static void sort_stmt(int st)
{
    /* SORT(): by name, those named alike in input order (ld's wild_sort); a stable insertion sort */
    u32 i, j;
    for (i = 1; i < pn[st]; i++) {
        struct place t = plist[st][i];
        for (j = i; j > 0 && strcmp(plist[st][j - 1].s->name, t.s->name) > 0; j--)
            plist[st][j] = plist[st][j - 1];
        plist[st][j] = t;
    }
}
static struct gsym *provide(const char *n)
{
    struct gsym *g = lookup(n, 0);
    if (g && !g->script && (g->st == G_UNDEF || g->st == G_UNDEFW)) {
        g->script = SC_SCRIPT;
        g->st = G_DEF;
        g->outsec = O_DATA;
        return g;
    }
    return 0;
}
static u32 place_stmt(int st, u32 dot)
{
    u32 i;
    for (i = 0; i < pn[st]; i++) {
        struct sec *s = plist[st][i].s;
        if (s->gone)
            continue;
        dot = (dot + (1u << s->align) - 1) & ~((1u << s->align) - 1);
        s->addr = dot;
        if (dot + s->size < dot)
            die("the program is larger than 4 GB");
        dot += s->size;
        if (s->type != SHT_NOBITS)
            oload[stmt_out[st]] = 1;
    }
    return dot;
}
static u32 out_align(int lo, int hi)
{
    u32 a = 0, i;
    int st;
    for (st = lo; st <= hi; st++)
        for (i = 0; i < pn[st]; i++)
            if (plist[st][i].s->align > a)
                a = plist[st][i].s->align;
    return a;
}
static u32 out_begin(int o, int lo, int hi, u32 dot)
{
    int st, any = 0;
    for (st = lo; st <= hi; st++)
        if (pn[st])
            any = 1;
    if (any) {
        u32 a = 1u << out_align(lo, hi);
        dot = (dot + a - 1) & ~(a - 1);
        ohas[o] = 1;
    }
    ostart[o] = dot;
    return dot;
}
static void layout(void)
{
    struct obj **chain = xalloc(nobjs * sizeof *chain), *o;
    int n = 0, i, st;
    u32 dot = 0;
    struct gsym *g, *p1, *p2, *p3, *p4, *p5, *p6;
    for (o = objs; o; o = o->next)
        chain[n++] = o;
    for (i = 0; i < n; i++) {
        int k;
        o = chain[i];
        for (k = 1; k < o->nsec; k++) {
            struct sec *s = &o->sec[k];
            if (!s->keep || s->stmt == S_NONE || s->stmt == S_DISCARD)
                continue;
            if (s->stmt == S_ORPHAN) {
                err("%s: section %s has no place in the program's layout", o->name, s->name);
                continue;
            }
            if ((pn[s->stmt] & (pn[s->stmt] - 1)) == 0 || !plist[s->stmt]) {
                u32 cap = pn[s->stmt] ? pn[s->stmt] * 2 : 16;
                plist[s->stmt] = xgrow(plist[s->stmt], cap * sizeof(struct place));
            }
            plist[s->stmt][pn[s->stmt]].o = o;
            plist[s->stmt][pn[s->stmt]++].s = s;
        }
    }
    free(chain);
    sort_stmt(S_INITS);
    sort_stmt(S_FINIS);
    /* .text */
    dot = out_begin(O_TEXT, S_START, S_TEXT, dot);
    dot = place_stmt(S_START, dot);
    dot = place_stmt(S_TEXT, dot);
    oend[O_TEXT] = dot;
    /* .rodata */
    dot = out_begin(O_RODATA, S_RODATA, S_RODATA, dot);
    dot = place_stmt(S_RODATA, dot);
    oend[O_RODATA] = dot;
    if (pure) {
        dot = (dot + 15) & ~15u;
        text_end = dot;
        if (bfd2got.n) {
            got_addr = dot;
            ohas[O_GOT] = oload[O_GOT] = 1;
            ostart[O_GOT] = dot;
            dot += ngot * 4;
            oend[O_GOT] = dot;
            gotplt_addr = dot;
            ohas[O_GOTPLT] = oload[O_GOTPLT] = 1;
            ostart[O_GOTPLT] = dot;
            dot += 12;
            oend[O_GOTPLT] = dot;
        }
    }
    /* .data with the constructor tables and their PROVIDEd bounds */
    p1 = provide("__bothinit_array_start");
    p2 = provide("__init_array_start");
    p3 = provide("__init_array_end");
    p4 = provide("__bothinit_array_end");
    p5 = provide("__fini_array_start");
    p6 = provide("__fini_array_end");
    dot = out_begin(O_DATA, S_INITS, S_DATA, dot);
    if (p1)
        p1->value = dot;
    if (p2)
        p2->value = dot;
    dot = place_stmt(S_INITS, dot);
    dot = place_stmt(S_INIT, dot);
    if (p3)
        p3->value = dot;
    if (p4)
        p4->value = dot;
    if (p5)
        p5->value = dot;
    for (st = S_FINIS; st <= S_FINIR; st++)
        dot = place_stmt(st, dot);
    if (p6)
        p6->value = dot;
    dot = place_stmt(S_DATA, dot);
    oend[O_DATA] = dot;
    dot = (dot + 3) & ~3u;
    bss_start = dot;
    dot = out_begin(O_BSS, S_BSS, S_BSS, dot);
    dot = place_stmt(S_BSS, dot);
    oend[O_BSS] = dot;
    dot = (dot + 3) & ~3u;
    bss_end = dot;
    if ((g = lookup("__bss_start", 0)) != 0 && g->script == SC_SCRIPT)
        g->value = bss_start;
    if ((g = lookup("__bss_end", 0)) != 0 && g->script == SC_SCRIPT)
        g->value = bss_end;
    if ((g = lookup("__text_end", 0)) != 0 && g->script == SC_SCRIPT)
        g->value = text_end;
    if ((g = lookup("_GLOBAL_OFFSET_TABLE_", 0)) != 0 && g->script == SC_SCRIPT)
        g->value = gotplt_addr;
}

/* ---- the image, the relocations, the program file ---- */
static u8 *img;
static u32 imgsz;
static u32 *rl[2];            /* ZEXE: rl[0]; ZEX2: places reaching the code part, the data part */
static u32 rn[2], rcap[2];
static int perr;              /* ZEX2 problems (counted as tools/mkzexe.py does) */

static void addrel(int part, u32 a)
{
    if (rn[part] == rcap[part]) {
        rcap[part] = rcap[part] ? rcap[part] * 2 : 256;
        rl[part] = xgrow(rl[part], rcap[part] * 4);
    }
    rl[part][rn[part]++] = a;
}
static const char *rname(u32 t)
{
    static const char *const n[] = { "NONE", "32", "16", "8", "PC32", "PC16", "PC8", "GOT32", "GOT16", "GOT8",
                                     "GOT32O", "GOT16O", "GOT8O", "PLT32", "PLT16", "PLT8" };
    static char b[24];
    if (t < 16) {
        memcpy(b, "R_68K_", 6);
        strcpy(b + 6, n[t]);
    } else
        strcpy(b, "R_68K_?");
    return b;
}
static int part_of(int out) { return out <= O_RODATA ? 0 : 1; }
static void perror2(struct obj *o, struct sec *s, struct rela *r, const char *what, const char *name)
{
    err("%s: %s at %s+%x: %s %s", o->name, rname(r->info & 255), s->name, r->off, what, name ? name : "");
    perr++;
}
static void classify(int part, u32 val, const char *name, struct obj *o, struct sec *s, struct rela *r, u32 place)
{
    if (val == text_end) {
        perror2(o, s, r, "the value equals the end of the shared part (ambiguous):", name);
        return;
    }
    if (part == 0 && !(val < text_end)) {
        perror2(o, s, r, "in the shared part but its value lies outside it:", name);
        return;
    }
    if (part == 1 && !(text_end < val && val <= bss_end)) {
        perror2(o, s, r, "in the data part but its value lies outside it:", name);
        return;
    }
    addrel(part, place);
}
static void relocate(struct obj *o, struct sec *s)
{
    u32 k;
    for (k = 0; k < s->nrel; k++) {
        struct rela *r = &s->rel[k];
        u32 t = r->info & 255, p = s->addr + r->off, v, n;
        struct target tg;
        u8 *q;
        n = t == 1 || t == 4 || t == 7 || t == 10 || t == 13 ? 4 : t == 2 || t == 5 || t == 8 || t == 11 || t == 14 ? 2 :
            t == 3 || t == 6 || t == 9 || t == 12 || t == 15 ? 1 : 0;
        if (t == 0) {
            if (!pure)
                err("%s: unsupported relocation R_68K_NONE at %s+%x", o->name, s->name, r->off);
            continue;
        }
        if (!n || t == 2 || t == 3 || (t >= 7 && t <= 9)) {
            err("%s: unsupported relocation %s (type %u) at %s+%x", o->name, rname(t), t, s->name, r->off);
            continue;
        }
        if (r->off > s->size || n > s->size - r->off) {
            err("%s: relocation at %s+%x lies outside the section", o->name, s->name, r->off);
            continue;
        }
        rtarget(o, r, &tg);
        if (tg.kind == T_UNDEF)
            continue;         /* reported already */
        q = img + p;
        if (t >= 10 && t <= 12) {
            struct gent *e = got_find(o, r->info >> 8);
            if (!e) {
                err("%s: %s at %s+%x: no address table entry", o->name, rname(t), s->name, r->off);
                continue;
            }
            v = e->off;
            if (pure) {
                if (tg.kind == T_WEAK)
                    perror2(o, s, r, "undefined (weak) symbol, its address would be 0:", tg.name);
                else if (tg.kind == T_ABS)
                    perror2(o, s, r, "absolute symbol", tg.name);
            }
        } else {
            v = tg.absorbed ? tg.s : tg.s + (u32)r->add;
            if (t >= 4)
                v -= p;
        }
        if (n == 2 && ((i32)v > 32767 || (i32)v < -32768)) {
            err("%s: %s at %s+%x: %s is out of reach (%d)", o->name, rname(t), s->name, r->off, tg.name, v);
            continue;
        }
        if (n == 1 && ((i32)v > 127 || (i32)v < -128)) {
            err("%s: %s at %s+%x: %s is out of reach (%d)", o->name, rname(t), s->name, r->off, tg.name, v);
            continue;
        }
        if (n == 4)
            put32(q, v);
        else if (n == 2) {
            q[0] = v >> 8;
            q[1] = v;
        } else
            q[0] = v;
        /* the program's relocation table (tools/mkzexe.py's rules) */
        if (!pure) {
            if (t == 1) {
                if (tg.kind == T_WEAK)
                    err("%s: undefined (weak) symbol %s at %x: its address would be 0", o->name, tg.name, p);
                else
                    addrel(0, p);
            } else if (t != 4 && t != 5 && t != 6)
                err("%s: unsupported relocation %s at %s+%x", o->name, rname(t), s->name, r->off);
            continue;
        }
        if (t == 1 || (t >= 4 && t <= 6) || (t >= 13 && t <= 15)) {
            int pp = p < text_end ? 0 : 1, tp;
            if (tg.kind == T_WEAK) {
                perror2(o, s, r, "undefined (weak) symbol, its address would be 0:", tg.name);
                continue;
            }
            if (tg.kind == T_ABS) {
                perror2(o, s, r, "absolute symbol", tg.name);
                continue;
            }
            tp = part_of(tg.out);
            if (t == 1) {
                if (pp == 0) {
                    perror2(o, s, r, tp ? "a relocation in the shared part (shared-to-data):" :
                            "a relocation in the shared part:", tg.name);
                    continue;
                }
                if (p & 1) {
                    perror2(o, s, r, "at an odd address", "");
                    continue;
                }
                classify(tp, v, tg.name, o, s, r, p);
            } else if (t <= 6) {
                if (tp != pp)
                    perror2(o, s, r, tp ? "a PC-relative reference from the shared part to the data part:" :
                            "a PC-relative reference from the data part to the shared part:", tg.name);
            } else if (tp != 0)
                perror2(o, s, r, "a call into the data part:", tg.name);
        }
    }
}
/* the address table's contents, and its entries in the relocation lists (each classified by its symbol's part) */
static void got_fill(void)
{
    u32 ne, i;
    void **el;
    if (!ngot)
        return;
    el = travlist(&big, 0, 0, &ne);
    for (i = 0; i < ne; i++) {
        struct gent *e = el[i];
        struct target tg;
        if (e->g)
            gtarget(e->g, &tg, 0);
        else {
            struct rela r;
            r.info = e->symndx << 8;
            r.add = 0;
            r.off = 0;
            rtarget(e->o, &r, &tg);
        }
        put32(img + got_addr + e->off, tg.s);
        if (tg.kind == T_SEC) {
            if (tg.s == text_end)
                err("address table slot %x: value %x equals the end of the shared part (ambiguous) (%s)",
                    got_addr + e->off, tg.s, tg.name), perr++;
            else if (part_of(tg.out) == 0 && !(tg.s < text_end))
                err("address table slot %x: %s is in the shared part but its value lies outside it", got_addr + e->off,
                    tg.name), perr++;
            else if (part_of(tg.out) == 1 && !(text_end < tg.s && tg.s <= bss_end))
                err("address table slot %x: %s is in the data part but its value lies outside it", got_addr + e->off,
                    tg.name), perr++;
            else
                addrel(part_of(tg.out), got_addr + e->off);
        }
    }
    free(el);
}
static int u32cmp(const void *a, const void *b)
{
    u32 x = *(const u32 *)a, y = *(const u32 *)b;
    return x < y ? -1 : x > y;
}

static void usage(void)
{
    die("usage: zld [-v] [-s STACK] [--defsym=SYM=VALUE] [--wrap=SYM] [-L DIR] -o OUT FILE.o|LIB.a|-lNAME ...");
}
static u32 num(const char *s, const char *what)
{
    char *e;
    unsigned long v = strtoul(s, &e, 0);
    if (!*s || *e)
        die("%s: not a number: %s", what, s);
    return v;
}

struct defsym {
    char *name, *val;
};

int main(int argc, char **argv)
{
    int i, verbose = 0, nin = 0, ndirs = 0;
    u32 stack = 0;
    struct defsym *ds;
    int nds = 0;
    const char **ins, **dirs;
    struct obj *o;
    struct gsym *g;
    u8 *out;
    u32 outsz;
    int fd;

    ds = xalloc(argc * sizeof *ds);
    ins = xalloc(argc * sizeof *ins);
    dirs = xalloc((argc + 1) * sizeof *dirs);
    wraps = xalloc(argc * sizeof *wraps);
    for (i = 1; i < argc; i++) {
        char *a = argv[i];
        if (!strcmp(a, "-o") && i + 1 < argc)
            outname = argv[++i];
        else if (!strcmp(a, "-s") && i + 1 < argc)
            stack = num(argv[++i], "-s");
        else if (!strncmp(a, "--stack=", 8))
            stack = num(a + 8, "--stack");
        else if (!strcmp(a, "-v"))
            verbose = 1;
        else if (!strncmp(a, "--defsym=", 9)) {
            char *eq = strchr(a + 9, '=');
            if (!eq || eq == a + 9 || !eq[1])
                die("--defsym needs SYM=VALUE: %s", a);
            *eq = 0;
            ds[nds].name = a + 9;
            ds[nds++].val = eq + 1;
            ins[nin++] = a;   /* in its place among the inputs (ld processes it there) */
        } else if (!strncmp(a, "--wrap=", 7) && a[7])
            wraps[nwraps++] = a + 7;
        else if (!strcmp(a, "-L") && i + 1 < argc)
            dirs[ndirs++] = argv[++i];
        else if (!strncmp(a, "-L", 2) && a[2])
            dirs[ndirs++] = a + 2;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help"))
            usage();
        else if (a[0] == '-' && a[1] != 'l')
            die("unknown option %s", a);
        else
            ins[nin++] = a;
    }
    if (!outname || !nin)
        usage();
    dirs[ndirs++] = "/usr/lib";
    files = xalloc(nin * sizeof *files);
    nds = 0;
    for (i = 0; i < nin; i++) {
        const char *a = ins[i];
        u8 m[8];
        int f, k;
        off_t sz;
        if (!strncmp(a, "--defsym=", 9)) {
            /* ld's first pass over an assignment: the value's symbol is now referenced (undefined), and the name
             * defined (as 0 until the end), so references to it pull nothing from archives */
            const char *v = ds[nds].val;
            g = lookup(ds[nds].name, 1);
            g->script = SC_DEFSYM;
            g->st = G_DEF;
            if ((v[0] >= '0' && v[0] <= '9')) {
                g->value = num(v, "--defsym");
                g->alias = 0;
            } else {
                struct gsym *t = lookup(wrapped(v), 1);
                undefine(t, 0, 0);
                g->alias = t;
            }
            nds++;
            continue;
        }
        if (a[0] == '-' && a[1] == 'l') {
            char *p = 0;
            for (k = 0; k < ndirs; k++) {
                u32 ld = strlen(dirs[k]), ln = strlen(a + 2);
                p = xalloc(ld + ln + 8);
                memcpy(p, dirs[k], ld);
                memcpy(p + ld, "/lib", 4);
                memcpy(p + ld + 4, a + 2, ln);
                memcpy(p + ld + 4 + ln, ".a", 3);
                if ((fd = open(p, O_RDONLY)) >= 0) {
                    close(fd);
                    break;
                }
                free(p);
                p = 0;
            }
            if (!p)
                die("cannot find lib%s.a (-l%s)", a + 2, a + 2);
            a = p;
        }
        f = nfiles++;
        files[f].path = a;
        files[f].argi = i;
        fd = open(a, O_RDONLY);
        if (fd < 0)
            die("%s: cannot open", a);
        sz = lseek(fd, 0, SEEK_END);
        close(fd);
        if (sz < 0 || (unsigned long)sz > 0x7fffffffUL)
            die("%s: cannot read its size", a);
        files[f].size = sz;
        if (sz < 8)
            die("%s: not an object or archive (too short)", a);
        rd(f, 0, m, 8, a);
        if (!memcmp(m, "!<arch>\n", 8))
            load_archive(f);
        else if (!memcmp(m, "\177ELF", 4)) {
            o = load_obj(f, 0, sz, xstrdup(a), 0);
            o->id = nextid;
            nextid += 2;
        } else
            die("%s: not an ELF object or an archive", a);
    }
    if (curfd >= 0)
        close(curfd);
    curfd = -1;
    curf = -1;
    if (nerr)
        exit(1);

    /* the link script's symbols (after the inputs: gcc puts -T last) */
    for (i = 0; i < 3; i++) {
        static const char *const sn[] = { "__bss_start", "__bss_end", "__text_end" };
        static const u8 so[] = { O_BSS, O_BSS, O_GOT };
        g = lookup(sn[i], 1);
        g->script = SC_SCRIPT;
        g->st = G_DEF;
        g->outsec = so[i];
    }
    gc();

    /* which layout: the magic word crt0 left at offset 4 */
    {
        struct sec *start = 0;
        struct obj *so = 0, **chain = xalloc(nobjs * sizeof *chain);
        int n = 0, k;
        u8 m[8];
        for (o = objs; o; o = o->next)
            chain[n++] = o;
        for (k = 0; k < n && !start; k++) {
            int s;
            for (s = 1; s < chain[k]->nsec; s++)
                if (chain[k]->sec[s].stmt == S_START && chain[k]->sec[s].keep) {
                    start = &chain[k]->sec[s];
                    so = chain[k];
                    break;
                }
        }
        free(chain);
        if (!start || start->size < 8 || start->type == SHT_NOBITS)
            die("no ZEXE header at offset 4 (crt0's .text.start first)");
        rd(so->file, so->base + start->off, m, 8, so->name);
        if (!memcmp(m + 4, "ZEX2", 4))
            pure = 1;
        else if (memcmp(m + 4, "ZEXE", 4))
            die("no ZEXE header at offset 4 (crt0's .text.start first)");
        if (!pure) {
            g = lookup("__text_end", 0);
            g->script = SC_NONE;
            g->st = G_NEW;
        }
    }
    /* undefined symbols that kept code refers to */
    for (o = objs; o; o = o->next)
        for (i = 1; i < o->nsec; i++) {
            struct sec *s = &o->sec[i];
            u32 k;
            if (!s->keep || !(s->flags & SHF_ALLOC))
                continue;
            for (k = 0; k < s->nrel; k++) {
                u32 si = s->rel[k].info >> 8;
                if (si >= (u32)o->nlocal) {
                    struct gsym *gg = o->g[si - o->nlocal];
                    struct target tg;
                    gtarget(gg, &tg, 0);
                    if (tg.kind == T_UNDEF && gg->ref != (struct obj *)1) {
                        err("%s: undefined reference to `%s'", o->name, tg.name);
                        gg->ref = (struct obj *)1;   /* said once */
                    }
                }
            }
        }
    for (i = 0; i < HSIZE; i++)
        for (g = htab[i]; g; g = g->next)
            if (g->script == SC_DEFSYM && g->alias && (g->alias->st == G_UNDEF || g->alias->st == G_NEW))
                err("--defsym %s: undefined symbol `%s'", g->name, g->alias->name);
    if (nerr)
        exit(1);

    /* the address table (check_relocs after --gc-sections, then its order) */
    htinit(&bfd2got, 1, 1);
    got_scan();
    if (bfd2got.n) {
        if (!pure)
            die("address table (GOT) relocations need the shared-code layout (crt0-pure.o)");
        got_assign();
        g = lookup("_GLOBAL_OFFSET_TABLE_", 0);
        if (g && (g->st == G_UNDEF || g->st == G_UNDEFW)) {
            g->script = SC_SCRIPT;
            g->st = G_DEF;
            g->outsec = O_GOTPLT;
        }
    }
    merge();
    layout();
    if (nerr)
        exit(1);

    /* the image: every loaded output section from address 0 (objcopy -O binary) */
    imgsz = 0;
    for (i = 0; i < O_NONE; i++)
        if (oload[i] && oend[i] > ostart[i] && oend[i] > imgsz)
            imgsz = oend[i];
    if (pure && imgsz < text_end)
        imgsz = text_end;
    if (imgsz > 0x7fff0000)
        die("the program is too large");
    img = xalloc(imgsz + 8);
    for (o = objs; o; o = o->next)
        for (i = 1; i < o->nsec; i++) {
            struct sec *s = &o->sec[i];
            if (!s->keep || stmt_out[s->stmt] == O_NONE || s->gone || s->mg || s->type == SHT_NOBITS || !s->size)
                continue;
            if (s->addr + s->size > imgsz)
                continue;     /* in .bss */
            rd(o->file, o->base + s->off, img + s->addr, s->size, o->name);
        }
    {
        struct mgrp *mg;
        for (mg = mgrps; mg; mg = mg->next)
            merge_emit(mg, img);
    }
    if (curfd >= 0)
        close(curfd);
    curfd = -1;
    curf = -1;
    for (o = objs; o; o = o->next)
        for (i = 1; i < o->nsec; i++) {
            struct sec *s = &o->sec[i];
            if (!s->keep || stmt_out[s->stmt] == O_NONE || s->gone || !s->nrel)
                continue;
            if (s->type == SHT_NOBITS || s->addr + s->size > imgsz) {
                err("%s: relocations in %s, which has no contents", o->name, s->name);
                continue;
            }
            relocate(o, s);
        }
    if (pure)
        got_fill();
    if (nerr) {
        if (perr)
            err("%s: cannot be a ZEX2 program (%u problems)", outname, perr);
        exit(1);
    }

    /* the program file (tools/mkzexe.py) */
    for (i = 0; i < 2; i++)
        if (rn[i])
            qsort(rl[i], rn[i], 4, u32cmp);
    if (!pure) {
        u32 isz = (imgsz + 3) & ~3u, mem;
        if (memcmp(img + 4, "ZEXE", 4))
            die("no ZEXE header at offset 4");
        outsz = isz + rn[0] * 4;
        mem = bss_end > outsz ? bss_end : outsz;
        mem = (mem + 3) & ~3u;
        out = img;            /* (allocated 8 bytes longer, zeroed: the padding to isz) */
        outsz = isz;
        put32(out + 8, isz);
        put32(out + 12, mem);
        put32(out + 16, rn[0]);
        put32(out + 20, isz);
        put32(out + 24, stack);
        g = lookup("stage0_blob", 0);
        if (g && !g->script && (g->st == G_DEF || g->st == G_DEFW)) {
            struct target tg;
            gtarget(g, &tg, 0);
            put32(out + 28, tg.s);
        }
        if (verbose)
            say("%s: image %uB, %u relocs, mem %uB\n", outname, isz, rn[0], mem);
    } else {
        u32 dlen = imgsz - text_end, mem;
        dlen = (dlen + 3) & ~3u;
        mem = (bss_end - text_end + 3) & ~3u;
        if (dlen > mem)
            mem = dlen;
        out = img;
        outsz = text_end + dlen;
        put32(out + 8, text_end);
        put32(out + 12, dlen);
        put32(out + 16, mem);
        put32(out + 20, rn[0]);
        put32(out + 24, rn[1]);
        put32(out + 28, text_end + dlen);
        put32(out + 32, stack);
        put32(out + 36, 0);
        if (verbose)
            say("%s: ZEX2 shared %uB, data %uB, mem %uB, %u relocs to code, %u to data, %u table slots\n", outname,
                text_end, dlen, mem, rn[0], rn[1], ngot);
    }
    fd = open(outname, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0)
        die("%s: cannot create", outname);
    /* the image, then the relocation lists (each turned into big-endian bytes where it lies) */
    for (i = 0; i < 3; i++) {
        u8 *p = i == 0 ? out : (u8 *)rl[i - 1];
        u32 left = i == 0 ? outsz : rn[i - 1] * 4, k;
        for (k = 0; i && k < rn[i - 1]; k++)
            put32(p + k * 4, rl[i - 1][k]);
        while (left) {
            long n = write(fd, p, left);
            if (n <= 0) {
                close(fd);
                unlink(outname);
                die("%s: write error", outname);
            }
            p += n;
            left -= n;
        }
    }
    if (close(fd) < 0) {
        unlink(outname);
        die("%s: write error", outname);
    }
    return 0;
}
