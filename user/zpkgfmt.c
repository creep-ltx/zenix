// SPDX-License-Identifier: GPL-2.0-only
/* The manifest and bundle parsers of DESIGN D15's frozen formats, one copy for zup, zpkg and the tests (user/zpkgfmt.h;
 * DESIGN D22 4.2). Everything here is untrusted input: every length, offset and count is checked before use. */
#ifdef ZHOST
#include "../tools/host/zup-host.h"
#else
#include "libz.h"
#include "sha256.h"
#endif
#include "zpkgfmt.h"

static unsigned be32(const unsigned char *p) { return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3]; }
static int has_nul(const void *p, unsigned n) { const unsigned char *q = p; while (n--) if (!*q++) return 1; return 0; }
static int hexval(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

int zf_parse_sha(const char *s, unsigned char out[32])
{
    for (int i = 0; i < 32; i++) {
        int a = hexval(s[2 * i]), b = a < 0 ? -1 : hexval(s[2 * i + 1]);
        if (b < 0) return -1;
        out[i] = (unsigned char)(a << 4 | b);
    }
    return s[64] ? -1 : 0;
}
void zf_put_sha(char *out, const unsigned char h[32])
{
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[2 * i] = hx[h[i] >> 4]; out[2 * i + 1] = hx[h[i] & 15]; }
    out[64] = 0;
}
int zf_path_ok(const char *p)
{
    unsigned n = strlen(p);
    if (p[0] != '/' || n < 2 || n > ZF_PMAX - 9 || p[n - 1] == '/' || strstr(p, "//") || strstr(p, "/./") || strstr(p, "/../"))
        return 0;
    if (!strcmp(p + n - 2, "/.") || (n >= 3 && !strcmp(p + n - 3, "/..")))    /* a final . or .. (audit3 B6) */
        return 0;
    for (const char *q = p; *q; q++)
        if (!isalnum(*q) && !strchr("._+/-", *q)) return 0;
    return 1;
}
/* a package name (used in paths: the database's NAME.manifest): [a-z0-9][a-z0-9+.-]*, at most 24 (audit3 B6) */
int zf_name_ok(const char *p)
{
    unsigned n = strlen(p);
    if (n < 1 || n > 24 || !((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9'))) return 0;
    for (; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '+' || *p == '.' || *p == '-')) return 0;
    return 1;
}
unsigned zf_num(const char *s, int base, int *bad)
{
    char *e;
    if (!*s) { *bad = 1; return 0; }
    unsigned v = (unsigned)strtoul(s, &e, base);
    if (*e) *bad = 1;
    return v;
}

int zf_manifest_parse(struct zf_pkg *p, char *text, unsigned len, const char *where)
{
    memset(p, 0, sizeof *p);
    p->text = text; p->textlen = len;
    if (strncmp(text, "zpkg 1\n", 7)) { zf_say("%s: not a zpkg 1 manifest", where); return -EINVAL; }
    int cap = 0;
    for (char *l = text, *e; *l; l = e) {
        for (e = l; *e && *e != '\n'; e++) ;
        if (*e) *e++ = 0;
        char *w[10]; int nw = 0;
        for (char *q = l; *q && nw < 10; ) {
            while (*q == ' ') *q++ = 0;
            if (!*q) break;
            w[nw++] = q;
            while (*q && *q != ' ') q++;
        }
        if (!nw) continue;
        int bad = 0;
        if (!strcmp(w[0], "name") && nw == 2) { p->name = w[1]; if (!zf_name_ok(w[1])) bad = 1; }
        else if (!strcmp(w[0], "version") && nw == 3) { p->vnum = zf_num(w[1], 10, &bad); p->vstr = w[2]; }
        else if (!strcmp(w[0], "group") && nw == 2) p->group = w[1];
        else if (!strcmp(w[0], "depends") && nw == 3) {
            if (p->ndep == ZF_MAXDEP) { zf_say("%s: more than %d dependencies", where, ZF_MAXDEP); return -EINVAL; }
            p->dep[p->ndep] = w[1]; p->depv[p->ndep++] = zf_num(w[2], 10, &bad);
            if (!zf_name_ok(w[1])) bad = 1;
        } else if ((!strcmp(w[0], "f") || !strcmp(w[0], "c") || !strcmp(w[0], "d") || !strcmp(w[0], "l")) && nw >= 3) {
            struct zf_ent x;
            memset(&x, 0, sizeof x);
            x.kind = w[0][0];
            if (x.kind == 'f' || x.kind == 'c') {
                if (nw != 7) bad = 1;
                else {
                    x.mode = (unsigned short)zf_num(w[1], 8, &bad); x.uid = (unsigned short)zf_num(w[2], 10, &bad);
                    x.gid = (unsigned short)zf_num(w[3], 10, &bad);
                    if (zf_parse_sha(w[4], x.sha)) bad = 1;
                    x.size = zf_num(w[5], 10, &bad); x.path = w[6];
                }
            } else if (x.kind == 'd') {
                if (nw != 5) bad = 1;
                else {
                    x.mode = (unsigned short)zf_num(w[1], 8, &bad); x.uid = (unsigned short)zf_num(w[2], 10, &bad);
                    x.gid = (unsigned short)zf_num(w[3], 10, &bad); x.path = w[4];
                }
            } else {
                if (nw != 3) bad = 1;
                else { x.target = w[1]; x.path = w[2]; if (strlen(x.target) >= ZF_PMAX - 9) bad = 1; }
            }
            if (bad || !zf_path_ok(x.path) || (x.mode & ~07777u)) { zf_say("%s: a bad entry (%s)", where, w[0]); return -EINVAL; }
            if (p->nent == cap) {
                cap = cap ? cap * 2 : 64;
                struct zf_ent *ne = realloc(p->ent, (unsigned)cap * sizeof *ne);
                if (!ne) { zf_say("out of memory"); return -ENOMEM; }
                p->ent = ne;
            }
            p->ent[p->nent++] = x;
        }
        /* any other line: something a later format added (append-only): ignored */
        if (bad) { zf_say("%s: a bad line (%s)", where, w[0]); return -EINVAL; }
    }
    if (!p->name || !p->vstr || !p->vnum) { zf_say("%s: no name or version", where); return -EINVAL; }
    return 0;
}
struct zf_ent *zf_find_ent(struct zf_pkg *p, const char *path)
{
    for (int i = 0; p && i < p->nent; i++)
        if (!strcmp(p->ent[i].path, path)) return &p->ent[i];
    return 0;
}

/* ---- the bundle ---- */
int zf_bundle_head(struct zf_bundle *b, const unsigned char *p, unsigned avail, unsigned len, unsigned *need)
{
    memset(b, 0, sizeof *b);
    b->p = p; b->len = len;
    if (need) *need = ZF_HDR;
    if (len < ZF_HDR || (avail >= 12 && (memcmp(p, "ZUP1", 4) || be32(p + 4) != 1 || be32(p + 8) != ZF_HDR))) {
        zf_say("the bundle is not a ZUP1 bundle"); return -EINVAL;
    }
    if (avail < ZF_HDR) return -EAGAIN;
    unsigned ntoc = be32(p + 52), total = be32(p + 56);
    if (total != len) { zf_say("the bundle is %u bytes, its header says %u", len, total); return -EINVAL; }
    if (be32(p + 48) != ZF_HDR || ntoc < 1 || ntoc > ZF_MAXTOC || ntoc * ZF_TOCE > len - ZF_HDR) {
        zf_say("the bundle's table of contents is bad"); return -EINVAL;
    }
    if (need) *need = ZF_HDR + ntoc * ZF_TOCE;
    if (avail < ZF_HDR + ntoc * ZF_TOCE) return -EAGAIN;
    struct sha256 c; unsigned char h[32];
    sha256_init(&c); sha256_update(&c, p, 64); sha256_update(&c, p + ZF_HDR, ntoc * ZF_TOCE); sha256_final(&c, h);
    if (memcmp(h, p + 64, 32)) { zf_say("the bundle's table of contents does not match its SHA-256"); return -EINVAL; }
    b->ntoc = ntoc;
    b->vnum = be32(p + 12); b->minv = be32(p + 36);
    memcpy(b->vstr, p + 16, 16); b->vstr[16] = 0;
    memcpy(b->channel, p + 40, 8); b->channel[8] = 0;
    return 0;
}
int zf_bundle_entry(const struct zf_bundle *b, unsigned i, char name[41], unsigned *off, unsigned *size,
                    const unsigned char **sha)
{
    const unsigned char *e = b->p + ZF_HDR + i * ZF_TOCE;
    *off = be32(e + 40); *size = be32(e + 44); *sha = e + 48;
    memcpy(name, e, 40); name[40] = 0;
    if (!has_nul(e, 40)) { zf_say("part %u: its name is not terminated", i); return -EINVAL; }
    if (*off < ZF_HDR + b->ntoc * ZF_TOCE || *off > b->len || *size > b->len - *off) { zf_say("%s: outside the bundle", name); return -EINVAL; }
    return 0;
}
int zf_part_kind(const char *name, char pname[25])
{
    if (strncmp(name, "pkg/", 4)) return 0;                 /* boot/: zenixboot's */
    const char *dot = strrchr(name, '.');
    if (!dot || dot - name - 4 < 1 || dot - name - 4 > 24) { zf_say("%s: not a package part", name); return -EINVAL; }
    unsigned n = (unsigned)(dot - name - 4);
    memcpy(pname, name + 4, n); pname[n] = 0;
    if (!zf_name_ok(pname)) { zf_say("%.*s: a bad package name (only a-z 0-9 + . -)", (int)(dot - name), name); return -EINVAL; }
    if (!strcmp(dot + 1, "manifest")) return 1;
    if (!strcmp(dot + 1, "tgz")) return 2;
    zf_say("%s: not a package part", name);
    return -EINVAL;
}
int zf_bundle_parse(struct zf_bundle *b, const unsigned char *p, unsigned len)
{
    int r = zf_bundle_head(b, p, len, len, 0);
    if (r) return r == -EAGAIN ? -EINVAL : r;
    for (unsigned i = 0; i < b->ntoc; i++) {
        char name[41], pname[25];
        unsigned off, size;
        const unsigned char *sha;
        if (zf_bundle_entry(b, i, name, &off, &size, &sha)) return -EINVAL;
        unsigned char h[32];
        sha256(p + off, size, h);
        if (memcmp(h, sha, 32)) { zf_say("%s: SHA-256 does not match (the bundle is damaged)", name); return -EINVAL; }
        int kind = zf_part_kind(name, pname);
        if (kind < 0) return -EINVAL;
        if (!kind) continue;
        struct zf_pkg *k = 0;
        for (int j = 0; j < b->npkg; j++) if (!strcmp(b->pk[j].name, pname)) k = &b->pk[j];
        if (!k) {
            if (b->npkg == ZF_MAXPKG) { zf_say("more than %d packages in the bundle", ZF_MAXPKG); return -EINVAL; }
            memcpy(b->pname[b->npkg], pname, sizeof pname);
            k = &b->pk[b->npkg];
            k->name = b->pname[b->npkg++];
        }
        if (kind == 1) { k->man = p + off; k->manlen = size; }
        else { k->tgz = p + off; k->tgzlen = size; }
    }
    for (int j = 0; j < b->npkg; j++) {
        struct zf_pkg *k = &b->pk[j];
        char *pname = k->name;
        const unsigned char *man = k->man, *tgz = k->tgz; unsigned ml = k->manlen, tl = k->tgzlen;
        if (!man || !tgz) { zf_say("package %s: its manifest or its files are missing", pname); return -EINVAL; }
        char *t = malloc(ml + 1);
        if (!t) return -ENOMEM;
        memcpy(t, man, ml); t[ml] = 0;
        if (has_nul(t, ml)) { free(t); zf_say("package %s: a NUL in its manifest", pname); return -EINVAL; }
        if (zf_manifest_parse(k, t, ml, pname)) return -EINVAL;
        if (strcmp(k->name, pname)) { zf_say("package %s: its manifest names %s", pname, k->name); return -EINVAL; }
        k->man = man; k->manlen = ml; k->tgz = tgz; k->tgzlen = tl;
    }
    return 0;
}
void zf_pkg_free(struct zf_pkg *p)
{
    free(p->ent);
    free(p->text);
    p->ent = 0; p->text = 0; p->nent = 0;
}
void zf_bundle_free(struct zf_bundle *b)
{
    for (int j = 0; j < b->npkg; j++) zf_pkg_free(&b->pk[j]);
    b->npkg = 0;
}
