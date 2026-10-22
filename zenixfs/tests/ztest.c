/* SPDX-License-Identifier: GPL-2.0-only */
/* ZenixFS host tests: basics, random operations against a model, power-cut replay, bit flips.
 * Usage: ztest [seed] [only-test-name] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../zenixfs.h"
#include "../host/zdev.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; return 1; } } while (0)

static uint64_t rs = 88172645463325252ull;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 11); }
static uint32_t rn(uint32_t n) { return rnd() % n; }

static void logmsg(void *ctx, const char *m) { (void)ctx; printf("  fsck: %s\n", m); }
static void quiet(void *ctx, const char *m) { (void)ctx; (void)m; }

/* ---------- signature of the whole tree (path|type|size|nlink|content hash), sorted ---------- */
struct sigbuf { char *s; size_t n, cap; };
static void sig_add(struct sigbuf *b, const char *line)
{
    size_t l = strlen(line) + 1;
    if (b->n + l + 1 > b->cap) { b->cap = (b->cap + l + 1) * 2; b->s = realloc(b->s, b->cap); }
    memcpy(b->s + b->n, line, l - 1); b->s[b->n + l - 1] = '\n'; b->n += l; b->s[b->n] = 0;
}
static uint64_t fnv(uint64_t h, const uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; } return h; }
static int sig_walk(struct zenixfs *fs, uint32_t dir, const char *path, struct sigbuf *b)
{
    uint64_t cookie = 0; char name[300]; uint32_t ino; uint8_t type;
    for (;;) {
        int r = zenixfs_readdir(fs, dir, &cookie, name, &ino, &type);
        if (r < 0) return r;
        if (!r) break;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        char p[2000]; snprintf(p, sizeof p, "%s/%s", path, name);
        struct zenixfs_inode in;
        r = zenixfs_getattr(fs, ino, &in); if (r < 0) return r;
        uint64_t h = 1469598103934665603ull;
        if ((in.mode & ZENIXFS_S_IFMT) != ZENIXFS_S_IFDIR) {
            uint8_t buf[2048]; uint64_t off = 0;
            while (off < in.size) {
                int n = zenixfs_read(fs, ino, off, buf, sizeof buf);
                if (n < 0) return n;
                if (n == 0) return ZENIXFS_EIO;
                h = fnv(h, buf, n); off += n;
            }
        }
        char line[2200];
        snprintf(line, sizeof line, "%s|%o|%llu|%u|%016llx", p, in.mode, (unsigned long long)in.size,
                 (in.mode & ZENIXFS_S_IFMT) == ZENIXFS_S_IFDIR ? 0 : in.nlink, (unsigned long long)h);
        sig_add(b, line);
        if ((in.mode & ZENIXFS_S_IFMT) == ZENIXFS_S_IFDIR) { r = sig_walk(fs, ino, p, b); if (r < 0) return r; }
    }
    return 0;
}
static int linecmp(const void *a, const void *b) { return strcmp(*(char **)a, *(char **)b); }
static char *sig_sorted(struct sigbuf *b)
{
    size_t nl = 0; for (size_t i = 0; i < b->n; i++) if (b->s[i] == '\n') nl++;
    char **lines = malloc((nl + 1) * sizeof *lines); size_t k = 0;
    char *p = b->s;
    while (p && *p) { lines[k++] = p; char *e = strchr(p, '\n'); if (!e) break; *e = 0; p = e + 1; }
    qsort(lines, k, sizeof *lines, linecmp);
    size_t tot = 1; for (size_t i = 0; i < k; i++) tot += strlen(lines[i]) + 1;
    char *out = malloc(tot), *o = out;
    for (size_t i = 0; i < k; i++) { size_t l = strlen(lines[i]); memcpy(o, lines[i], l); o += l; *o++ = '\n'; }
    *o = 0; free(lines); free(b->s);
    return out;
}
static char *fs_sig(struct zenixfs *fs)
{
    struct sigbuf b = {0};
    if (sig_walk(fs, ZENIXFS_ROOT_INO, "", &b) < 0) { free(b.s); return NULL; }
    if (!b.s) { b.s = strdup(""); b.n = 0; }
    return sig_sorted(&b);
}

/* ---------- 1. basics ---------- */
static int test_basic(void)
{
    struct zmem *m = zmem_new(12, 2048, 0);
    struct zenixfs *fs;
    CHECK(zenixfs_mkfs(&m->dev, "test", 0, 1000) == 0);
    CHECK(zenixfs_mount(&fs, &m->dev, 64) == 0);
    CHECK(zenixfs_charset(fs) == ZENIXFS_CHARSET_LATIN1);
    CHECK(zenixfs_set_charset(fs, ZENIXFS_CHARSET_UTF8) == 0);
    zenixfs_set_time(fs, 2000);
    {   /* both encodings are just bytes: Latin-1 a-diaeresis and UTF-8 a-diaeresis are different names and both work */
        uint32_t u1, u2, l;
        CHECK(zenixfs_create(fs, ZENIXFS_ROOT_INO, "\xe4", 1, 0644, 0, 0, &u1) == 0);
        CHECK(zenixfs_create(fs, ZENIXFS_ROOT_INO, "\xc3\xa4", 2, 0644, 0, 0, &u2) == 0 && u1 != u2);
        CHECK(zenixfs_lookup(fs, ZENIXFS_ROOT_INO, "\xe4", 1, &l) == 0 && l == u1);
        CHECK(zenixfs_lookup(fs, ZENIXFS_ROOT_INO, "\xc3\xa4", 2, &l) == 0 && l == u2);
        CHECK(zenixfs_unlink(fs, ZENIXFS_ROOT_INO, "\xe4", 1) == 0 && zenixfs_unlink(fs, ZENIXFS_ROOT_INO, "\xc3\xa4", 2) == 0);
    }
    uint32_t d, f, f2, x;
    CHECK(zenixfs_mkdir(fs, ZENIXFS_ROOT_INO, "etc", 3, 0755, 0, 0, &d) == 0);
    CHECK(zenixfs_create(fs, d, "passwd", 6, 0644, 0, 0, &f) == 0);
    CHECK(zenixfs_create(fs, d, "passwd", 6, 0644, 0, 0, &x) == ZENIXFS_EEXIST);
    CHECK(zenixfs_write(fs, f, 0, "root:x:0:0\n", 11) == 11);
    char buf[64];
    CHECK(zenixfs_read(fs, f, 0, buf, 64) == 11 && !memcmp(buf, "root:x:0:0\n", 11));
    /* big file crosses inline -> extents */
    CHECK(zenixfs_create(fs, ZENIXFS_ROOT_INO, "big", 3, 0644, 0, 0, &f2) == 0);
    uint8_t *data = malloc(300000); for (int i = 0; i < 300000; i++) data[i] = (uint8_t)(i * 7 + i / 251);
    CHECK(zenixfs_write(fs, f2, 0, data, 300000) == 300000);
    uint8_t *back = malloc(300000);
    CHECK(zenixfs_read(fs, f2, 0, back, 300000) == 300000 && !memcmp(back, data, 300000));
    CHECK(zenixfs_sync(fs) == 0);
    CHECK(zenixfs_fsck(fs, logmsg, NULL) == 0);
    CHECK(zenixfs_unmount(fs) == 0);
    CHECK(zenixfs_mount(&fs, &m->dev, 64) == 0);
    CHECK(zenixfs_charset(fs) == ZENIXFS_CHARSET_UTF8);
    CHECK(zenixfs_lookup(fs, ZENIXFS_ROOT_INO, "big", 3, &f2) == 0);
    memset(back, 0, 300000);
    CHECK(zenixfs_read(fs, f2, 0, back, 300000) == 300000 && !memcmp(back, data, 300000));
    CHECK(zenixfs_lookup(fs, ZENIXFS_ROOT_INO, "etc", 3, &d) == 0 && zenixfs_lookup(fs, d, "passwd", 6, &f) == 0);
    CHECK(zenixfs_read(fs, f, 0, buf, 64) == 11);
    /* sparse + truncate */
    CHECK(zenixfs_write(fs, f2, 1000000, "Z", 1) == 1);
    CHECK(zenixfs_read(fs, f2, 500000, back, 10) == 10 && back[0] == 0);
    CHECK(zenixfs_truncate(fs, f2, 5000) == 0);
    CHECK(zenixfs_read(fs, f2, 0, back, 5000) == 5000 && !memcmp(back, data, 5000));
    CHECK(zenixfs_truncate(fs, f2, 9000) == 0);
    CHECK(zenixfs_read(fs, f2, 4990, back, 20) == 20 && !memcmp(back, data + 4990, 10) && back[10] == 0);
    /* links, symlink, rename, rmdir */
    CHECK(zenixfs_link(fs, ZENIXFS_ROOT_INO, "big2", 4, f2) == 0);
    struct zenixfs_inode in; CHECK(zenixfs_getattr(fs, f2, &in) == 0 && in.nlink == 2);
    CHECK(zenixfs_symlink(fs, ZENIXFS_ROOT_INO, "sl", 2, "etc/passwd", 0, 0, &x) == 0);
    CHECK(zenixfs_readlink(fs, x, buf, 64) == 10 && !memcmp(buf, "etc/passwd", 10));
    CHECK(zenixfs_rmdir(fs, ZENIXFS_ROOT_INO, "etc", 3) == ZENIXFS_ENOTEMPTY);
    CHECK(zenixfs_rename(fs, d, "passwd", 6, ZENIXFS_ROOT_INO, "pw", 2) == 0);
    CHECK(zenixfs_rmdir(fs, ZENIXFS_ROOT_INO, "etc", 3) == 0);
    CHECK(zenixfs_unlink(fs, ZENIXFS_ROOT_INO, "big", 3) == 0);
    CHECK(zenixfs_unlink(fs, ZENIXFS_ROOT_INO, "big2", 4) == 0);
    CHECK(zenixfs_lookup(fs, ZENIXFS_ROOT_INO, "big", 3, &x) == ZENIXFS_ENOENT);
    CHECK(zenixfs_fsck(fs, logmsg, NULL) == 0);
    struct zenixfs_stats st; zenixfs_get_stats(fs, &st);
    printf("  basic: %u commits, %u writes, %u flushes, %u cache hits, %u misses\n", st.commits, st.writes, st.flushes, st.cache_hits, st.cache_misses);
    CHECK(zenixfs_unmount(fs) == 0);
    free(data); free(back); zmem_free(m);
    return 0;
}

/* ---------- model for the random test ---------- */
#define MAXN 4000
struct mnode { int used, type; uint8_t *data; size_t size; int nlink; int parent; };   /* type: 0 file 1 dir 2 symlink */
struct ment { int used, dir, node; char name[4]; };
static struct mnode mn[MAXN];
static struct ment me[MAXN * 2];
static void model_reset(void)
{
    for (int i = 0; i < MAXN; i++) free(mn[i].data);
    memset(mn, 0, sizeof mn); memset(me, 0, sizeof me);
    mn[0].used = 1; mn[0].type = 1; mn[0].nlink = 2; mn[0].parent = 0;
}
static int m_find(int dir, const char *name) { for (int i = 0; i < MAXN * 2; i++) if (me[i].used && me[i].dir == dir && !strcmp(me[i].name, name)) return i; return -1; }
static int m_newnode(int type) { for (int i = 1; i < MAXN; i++) if (!mn[i].used) { memset(&mn[i], 0, sizeof mn[i]); mn[i].used = 1; mn[i].type = type; mn[i].nlink = type == 1 ? 2 : 1; return i; } return -1; }
static int m_newent(int dir, const char *name, int node) { for (int i = 0; i < MAXN * 2; i++) if (!me[i].used) { me[i].used = 1; me[i].dir = dir; me[i].node = node; strcpy(me[i].name, name); return i; } return -1; }
static int m_dir_empty(int dir) { for (int i = 0; i < MAXN * 2; i++) if (me[i].used && me[i].dir == dir) return 0; return 1; }
static void m_unref(int node)
{
    if (--mn[node].nlink <= 0) { free(mn[node].data); mn[node].data = NULL; mn[node].used = 0; }
}
static void m_resize(int node, size_t sz)
{
    mn[node].data = realloc(mn[node].data, sz ? sz : 1);
    if (sz > mn[node].size) memset(mn[node].data + mn[node].size, 0, sz - mn[node].size);
    mn[node].size = sz;
}
static void model_sig_walk(int dir, const char *path, struct sigbuf *b)
{
    for (int i = 0; i < MAXN * 2; i++) {
        if (!me[i].used || me[i].dir != dir) continue;
        int n = me[i].node; char p[2000]; snprintf(p, sizeof p, "%s/%s", path, me[i].name);
        uint64_t h = 1469598103934665603ull;
        if (mn[n].type != 1) h = fnv(h, mn[n].data ? mn[n].data : (uint8_t *)"", mn[n].size);
        unsigned mode = mn[n].type == 1 ? 040755 : mn[n].type == 2 ? 0120777 : 0100644;
        char line[2200];
        snprintf(line, sizeof line, "%s|%o|%llu|%u|%016llx", p, mode, (unsigned long long)(mn[n].type == 1 ? 0 : mn[n].size),
                 mn[n].type == 1 ? 0 : mn[n].nlink, (unsigned long long)h);
        sig_add(b, line);
        if (mn[n].type == 1) model_sig_walk(n, p, b);
    }
}
static char *model_sig(void)
{
    struct sigbuf b = {0};
    model_sig_walk(0, "", &b);
    if (!b.s) b.s = strdup("");
    return sig_sorted(&b);
}
static int node_ino(struct zenixfs *fs, int node, uint32_t *ino)    /* resolve a model directory to an inode number */
{
    if (node == 0) { *ino = ZENIXFS_ROOT_INO; return 0; }
    uint32_t pi; int e = node_ino(fs, mn[node].parent, &pi);
    if (e) return e;
    for (int i = 0; i < MAXN * 2; i++) if (me[i].used && me[i].node == node && me[i].dir == mn[node].parent)
        return zenixfs_lookup(fs, pi, me[i].name, strlen(me[i].name), ino);
    return -1;
}

static const char *NAMES[] = { "a", "b", "c", "d", "e", "f", "g", "h", "i", "j" };
static int pick_dir(void) { int c[MAXN], n = 0; for (int i = 0; i < MAXN; i++) if (mn[i].used && mn[i].type == 1) c[n++] = i; return c[rn(n)]; }
static int pick_ent(int want_dir)   /* a random entry; want_dir: -1 any, 0 non-dir, 1 dir */
{
    int c[MAXN * 2], n = 0;
    for (int i = 0; i < MAXN * 2; i++) if (me[i].used && (want_dir < 0 || (mn[me[i].node].type == 1) == want_dir)) c[n++] = i;
    return n ? c[rn(n)] : -1;
}
static int is_under(int node, int anc) { while (1) { if (node == anc) return 1; if (node == 0) return 0; node = mn[node].parent; } }

/* one random operation on fs and model; returns 0, or 1 when fs and model disagree */
static int random_op(struct zenixfs *fs, int bounded_write)
{
    int op = rn(100); uint32_t di, ino;
    if (op < 22) {                                           /* create */
        int d = pick_dir(); const char *nm = NAMES[rn(10)];
        if (node_ino(fs, d, &di)) { printf("  resolve failed\n"); return 1; }
        int exists = m_find(d, nm) >= 0;
        int r = zenixfs_create(fs, di, nm, strlen(nm), 0644, 1, 2, &ino);
        if (exists) { if (r != ZENIXFS_EEXIST) { printf("  create existing gave %d\n", r); return 1; } return 0; }
        if (r) { printf("  create failed %d\n", r); return 1; }
        int n = m_newnode(0); if (n < 0) return 0; m_newent(d, nm, n);
    } else if (op < 50) {                                    /* write */
        int e = pick_ent(0); if (e < 0) return 0;
        int n = me[e].node; if (mn[n].type != 0) return 0;
        if (node_ino(fs, me[e].dir, &di) || zenixfs_lookup(fs, di, me[e].name, strlen(me[e].name), &ino)) { printf("  lookup failed\n"); return 1; }
        size_t off, len;
        if (bounded_write == 2) { off = rn(30000); len = 1 + rn(30000); }                    /* fat: fills a big volume across several map pages */
        else if (bounded_write) { size_t blk = 1024; size_t b0 = rn(6) * blk; off = b0 + rn(blk); len = 1 + rn(blk - (off - b0)); }
        else { off = rn(7000); len = 1 + rn(900); }
        static uint8_t buf[32768]; for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)rnd();
        int r = zenixfs_write(fs, ino, off, buf, len);
        if (r != (int)len) { printf("  write gave %d\n", r); return 1; }
        if (off + len > mn[n].size) m_resize(n, off + len);
        memcpy(mn[n].data + off, buf, len);
    } else if (op < 58) {                                    /* truncate */
        int e = pick_ent(0); if (e < 0) return 0;
        int n = me[e].node; if (mn[n].type != 0) return 0;
        if (node_ino(fs, me[e].dir, &di) || zenixfs_lookup(fs, di, me[e].name, strlen(me[e].name), &ino)) return 1;
        size_t sz = bounded_write == 2 ? rn(60000) : rn(7000);
        if (bounded_write == 1) sz = rn(2) ? 0 : mn[n].size;      /* a shrink to a block edge can be split by a commit; keep it atomic */
        int r = zenixfs_truncate(fs, ino, sz);
        if (r) { printf("  truncate gave %d\n", r); return 1; }
        m_resize(n, sz);
    } else if (op < 70) {                                    /* unlink */
        int e = pick_ent(0); if (e < 0) return 0;
        if (node_ino(fs, me[e].dir, &di)) return 1;
        int r = zenixfs_unlink(fs, di, me[e].name, strlen(me[e].name));
        if (r) { printf("  unlink gave %d\n", r); return 1; }
        int n = me[e].node; me[e].used = 0; m_unref(n);
    } else if (op < 78) {                                    /* mkdir */
        int d = pick_dir(); const char *nm = NAMES[rn(10)];
        if (node_ino(fs, d, &di)) return 1;
        int exists = m_find(d, nm) >= 0;
        int r = zenixfs_mkdir(fs, di, nm, strlen(nm), 0755, 0, 0, &ino);
        if (exists) return r == ZENIXFS_EEXIST ? 0 : 1;
        if (r) { printf("  mkdir gave %d\n", r); return 1; }
        int n = m_newnode(1); if (n < 0) return 0;
        mn[n].parent = d; m_newent(d, nm, n); mn[d].nlink++;
    } else if (op < 84) {                                    /* rmdir */
        int e = pick_ent(1); if (e < 0) return 0;
        int n = me[e].node;
        if (node_ino(fs, me[e].dir, &di)) return 1;
        int r = zenixfs_rmdir(fs, di, me[e].name, strlen(me[e].name));
        if (!m_dir_empty(n)) return r == ZENIXFS_ENOTEMPTY ? 0 : (printf("  rmdir nonempty gave %d\n", r), 1);
        if (r) { printf("  rmdir gave %d\n", r); return 1; }
        mn[me[e].dir].nlink--; me[e].used = 0; mn[n].used = 0;
    } else if (op < 92) {                                    /* rename */
        int e = pick_ent(-1); if (e < 0) return 0;
        int n = me[e].node, nd = pick_dir(); const char *nm = NAMES[rn(10)];
        if (mn[n].type == 1 && is_under(nd, n)) {
            uint32_t oi; if (node_ino(fs, me[e].dir, &oi) || node_ino(fs, nd, &di)) return 1;
            int r = zenixfs_rename(fs, oi, me[e].name, strlen(me[e].name), di, nm, strlen(nm));
            return r == ZENIXFS_EINVAL ? 0 : (printf("  rename into itself gave %d\n", r), 1);
        }
        int t = m_find(nd, nm);
        if (t >= 0 && (mn[me[t].node].type == 1 || mn[n].type == 1)) return 0;   /* skip the dir-replace cases */
        if (t >= 0 && me[t].node == n) return 0;
        uint32_t oi; if (node_ino(fs, me[e].dir, &oi) || node_ino(fs, nd, &di)) return 1;
        int r = zenixfs_rename(fs, oi, me[e].name, strlen(me[e].name), di, nm, strlen(nm));
        if (r) { printf("  rename gave %d\n", r); return 1; }
        if (t >= 0) { int tn = me[t].node; me[t].used = 0; m_unref(tn); }
        if (mn[n].type == 1) { mn[me[e].dir].nlink--; mn[nd].nlink++; mn[n].parent = nd; }
        me[e].dir = nd; strcpy(me[e].name, nm);
    } else if (op < 96) {                                    /* hard link */
        int e = pick_ent(0); if (e < 0) return 0;
        int nd = pick_dir(); const char *nm = NAMES[rn(10)];
        if (m_find(nd, nm) >= 0) return 0;
        uint32_t oi; if (node_ino(fs, me[e].dir, &oi) || node_ino(fs, nd, &di) || zenixfs_lookup(fs, oi, me[e].name, strlen(me[e].name), &ino)) return 1;
        int r = zenixfs_link(fs, di, nm, strlen(nm), ino);
        if (r) { printf("  link gave %d\n", r); return 1; }
        m_newent(nd, nm, me[e].node); mn[me[e].node].nlink++;
    } else {                                                 /* symlink */
        int d = pick_dir(); const char *nm = NAMES[rn(10)];
        if (m_find(d, nm) >= 0) return 0;
        if (node_ino(fs, d, &di)) return 1;
        char tgt[300]; int tl = 1 + rn(250); for (int i = 0; i < tl; i++) tgt[i] = 'a' + rn(26); tgt[tl] = 0;
        int r = zenixfs_symlink(fs, di, nm, strlen(nm), tgt, 0, 0, &ino);
        if (r) { printf("  symlink gave %d\n", r); return 1; }
        int n = m_newnode(2); if (n < 0) return 0;
        m_resize(n, tl); memcpy(mn[n].data, tgt, tl); m_newent(d, nm, n);
    }
    return 0;
}

static int compare_to_model(struct zenixfs *fs, const char *when)
{
    char *a = fs_sig(fs), *b = model_sig();
    int bad = !a || strcmp(a, b);
    if (bad) { printf("  MISMATCH %s\n--- fs ---\n%s\n--- model ---\n%s\n", when, a ? a : "(walk error)", b); }
    free(a); free(b);
    return bad;
}

/* ---------- 2. random operations against the model ---------- */
static int test_random(void)
{
    for (int bs = 10; bs <= 12; bs += 2) {
        struct zmem *m = zmem_new(bs, bs == 10 ? 8192 : 2048, 0);
        struct zenixfs *fs;
        CHECK(zenixfs_mkfs(&m->dev, NULL, 0, 5) == 0);
        CHECK(zenixfs_mount(&fs, &m->dev, 40) == 0);
        model_reset();
        int ops = 3000;
        for (int i = 0; i < ops; i++) {
            zenixfs_set_time(fs, 100 + i);
            if (random_op(fs, 0)) { printf("  op %d (bshift %d)\n", i, bs); failures++; return 1; }
            if (i % 250 == 249) {
                CHECK(!compare_to_model(fs, "periodic"));
                CHECK(zenixfs_fsck(fs, logmsg, NULL) == 0);
            }
            if (i % 700 == 699) { CHECK(zenixfs_unmount(fs) == 0); CHECK(zenixfs_mount(&fs, &m->dev, 40) == 0); CHECK(!compare_to_model(fs, "after remount")); }
        }
        struct zenixfs_stats st; zenixfs_get_stats(fs, &st);
        printf("  random bshift %d: %d ops, %u commits, %u writes, %u flushes\n", bs, ops, st.commits, st.writes, st.flushes);
        CHECK(zenixfs_unmount(fs) == 0);
        zmem_free(m);
    }
    return 0;
}

/* ---------- 3. power cut: replay every prefix of the recorded writes ---------- */
static int powercut_run(int bshift, uint32_t nblocks, int mode, int ops, int stride, int bmpages, int cacheblk, int prefill)
{
    struct zmem *rec = zmem_new(bshift, nblocks, 1);
    struct zenixfs *fs;
    CHECK(zenixfs_mkfs(&rec->dev, NULL, 0, 5) == 0);
    if (prefill) {                                       /* fill the first map page so that the recorded workload runs on later pages */
        CHECK(zenixfs_mount_ex(&fs, &rec->dev, cacheblk, bmpages) == 0);
        uint32_t f; static uint8_t chunk[65536]; memset(chunk, 0x5A, sizeof chunk);
        CHECK(zenixfs_create(fs, ZENIXFS_ROOT_INO, "fill", 4, 0644, 0, 0, &f) == 0);
        for (int k = 0; k < prefill; k++) CHECK(zenixfs_write(fs, f, (uint64_t)k * sizeof chunk, chunk, sizeof chunk) == (int)sizeof chunk);
        CHECK(zenixfs_unmount(fs) == 0);
    }
    struct zmem *base = zmem_clone(rec);                 /* the image as mkfs (and the prefill) left it; the log starts after it */
    for (size_t i = 0; i < rec->nlog; i++) free(rec->log[i].data);
    rec->nlog = 0;
    CHECK(zenixfs_mount_ex(&fs, &rec->dev, cacheblk, bmpages) == 0);
    model_reset();
    enum { MAXSIG = 400 };
    char *sigs[MAXSIG]; int nsig = 0; 
    int synced_sig[MAXSIG]; size_t synced_pos[MAXSIG]; int nsynced = 0;
    sigs[nsig] = fs_sig(fs); nsig++;
    for (int i = 0; i < ops && nsig < MAXSIG - 1; i++) {
        zenixfs_set_time(fs, 100 + i);
        CHECK(!random_op(fs, mode));
        sigs[nsig] = fs_sig(fs); nsig++;
        if (rn(12) == 0) {
            CHECK(zenixfs_sync(fs) == 0);
            synced_sig[nsynced] = nsig - 1; synced_pos[nsynced] = rec->nlog; nsynced++;
        }
    }
    CHECK(zenixfs_sync(fs) == 0);
    synced_sig[nsynced] = nsig - 1; synced_pos[nsynced] = rec->nlog; nsynced++;
    struct zenixfs_stats st; zenixfs_get_stats(fs, &st);
    int high = 0;
    for (uint32_t b = 8000; b < rec->dev.nblocks && (size_t)b << bshift < ((size_t)rec->dev.nblocks << bshift); b++) { const uint8_t *q = rec->img + ((size_t)b << bshift); for (int i = 0; i < (1 << bshift); i++) if (q[i]) { high++; break; } }
    printf("  power cut: %d states, %zu log entries, %u commits, %d blocks used beyond block 8000\n", nsig, rec->nlog, st.commits, high);
    zenixfs_unmount(fs);
    size_t nlog = rec->nlog - 1;     /* the unmount's own (empty) activity is not part of the workload */
    (void)nlog;
    /* replay */
    struct zmem *work = zmem_clone(base);
    int tested = 0, reorder_tests = 0;
    size_t total = rec->nlog;
    for (size_t k = 0; k <= total; k++) {
        /* prefix of k log entries is applied in `work` */
        int sample = stride <= 1 || k % stride == 0 || k == total || (k > 0 && rec->log[k - 1].is_flush);
        if (!sample) { if (k < total) zmem_apply(work, &rec->log[k]); continue; }
        struct zmem *img = zmem_clone(work);
        struct zenixfs *f2; int e = zenixfs_mount_ex(&f2, &img->dev, cacheblk, bmpages);
        if (e) { printf("  prefix %zu: mount failed %d\n", k, e); failures++; return 1; }
        char *s = fs_sig(f2);
        int bad = zenixfs_fsck(f2, quiet, NULL) != 0;
        int match = -1;
        for (int j = 0; j < nsig && s; j++) if (!strcmp(s, sigs[j])) match = j;
        int need = 0;                                      /* newest state that was already durable */
        for (int j = 0; j < nsynced; j++) if (synced_pos[j] <= k) need = synced_sig[j];
        if (!s || bad || match < 0 || match < need) {
            printf("  prefix %zu/%zu: %s (match=%d need>=%d fsck_bad=%d)\n", k, total, !s ? "walk error" : match < 0 ? "state is not any committed state" : "older than a durable state", match, need, bad);
            failures++; zenixfs_unmount(f2); zmem_free(img); free(s); return 1;
        }
        free(s); zenixfs_unmount(f2); zmem_free(img); tested++;
        if (k < total) {
            /* reordering: before applying a whole epoch, try random subsets of it */
            if (k == 0 || rec->log[k - 1].is_flush) {
                size_t end = k; while (end < total && !rec->log[end].is_flush) end++;
                if (end > k + 1) for (int t = 0; t < 4; t++) {
                    struct zmem *im2 = zmem_clone(work);
                    for (size_t q = k; q < end; q++) if (rn(2)) zmem_apply(im2, &rec->log[q]);
                    struct zenixfs *f3; int e3 = zenixfs_mount_ex(&f3, &im2->dev, cacheblk, bmpages);
                    if (e3) { printf("  reorder at %zu: mount failed %d\n", k, e3); failures++; return 1; }
                    char *s3 = fs_sig(f3); int ok = s3 != NULL, bad3 = zenixfs_fsck(f3, quiet, NULL) != 0;
                    int m3 = -1; for (int j = 0; j < nsig && s3; j++) if (!strcmp(s3, sigs[j])) m3 = j;
                    if (!ok || bad3 || m3 < 0) { printf("  reorder at %zu..%zu: bad state (match=%d fsck_bad=%d)\n", k, end, m3, bad3); failures++; return 1; }
                    free(s3); zenixfs_unmount(f3); zmem_free(im2); reorder_tests++;
                }
            }
            zmem_apply(work, &rec->log[k]);
        }
    }
    printf("  power cut: %d prefixes and %d reordered epochs all mount, pass fsck and equal a committed state\n", tested, reorder_tests);
    for (int j = 0; j < nsig; j++) free(sigs[j]);
    zmem_free(work); zmem_free(base); zmem_free(rec);
    return 0;
}

static int test_powercut(void) { return powercut_run(10, 700, 1, 300, 1, 0, 32, 0); }
/* a volume of several map pages with only one page in RAM, large writes, and a sampled replay (slow: not in the default run) */
static int test_bigpower(void) { return powercut_run(10, 30000, 2, 380, 11, 1, 1000, 120); }     /* 120 x 64 KB = 7.5 MB of fill */

/* ---------- 4. bit flips in metadata must never read back as silently different content ---------- */
static int test_bitflip(void)
{
    struct zmem *m = zmem_new(10, 4096, 0);
    struct zenixfs *fs;
    CHECK(zenixfs_mkfs(&m->dev, NULL, 0, 5) == 0);
    CHECK(zenixfs_mount(&fs, &m->dev, 40) == 0);
    model_reset();
    for (int i = 0; i < 400; i++) { zenixfs_set_time(fs, i); CHECK(!random_op(fs, 0)); }
    CHECK(zenixfs_unmount(fs) == 0);
    CHECK(zenixfs_mount(&fs, &m->dev, 40) == 0);
    char *want = fs_sig(fs);
    zenixfs_unmount(fs);
    int detected = 0, harmless = 0, trials = 0;
    for (int t = 0; t < 600; t++) {
        uint32_t blk; int tries = 0;
        do { blk = rn(m->dev.nblocks); tries++; } while (tries < 5000 && !(m->img[(size_t)blk * 1024 + 4] == 0x5A && m->img[(size_t)blk * 1024 + 5] == 0x42));
        if (tries >= 5000) break;
        struct zmem *c = zmem_clone(m);
        size_t byte = (size_t)blk * 1024 + rn(1024);
        c->img[byte] ^= 1 << rn(8);
        trials++;
        struct zenixfs *f2; int e = zenixfs_mount(&f2, &c->dev, 40);
        if (e) { detected++; zmem_free(c); continue; }
        char *s = fs_sig(f2);
        int bad = zenixfs_fsck(f2, quiet, NULL) != 0;
        struct zenixfs_stats mst; zenixfs_get_stats(f2, &mst);
        if (!s || bad || mst.mount_damage) detected++;
        else if (!strcmp(s, want)) harmless++;
        else { printf("  SILENT CORRUPTION: flip in block %u byte %zu (type %d level %d) changed content with no error\n", blk, byte - (size_t)blk * 1024, m->img[(size_t)blk*1024+6], m->img[(size_t)blk*1024+7]); failures++; return 1; }
        free(s); zenixfs_unmount(f2); zmem_free(c);
    }
    printf("  bit flips: %d trials: %d detected, %d harmless (unused or old copy), 0 silent\n", trials, detected, harmless);
    free(want); zmem_free(m);
    return 0;
}

/* ---------- a volume of several free-space map pages (6) with ONE page in RAM, big random writes against the model ---------- */
static int test_bigvol(void)
{
    struct zmem *m = zmem_new(10, 40000, 0);
    struct zenixfs *fs;
    CHECK(zenixfs_mkfs(&m->dev, NULL, 0, 5) == 0);
    CHECK(zenixfs_mount_ex(&fs, &m->dev, 48, 1) == 0);
    model_reset();
    for (int i = 0; i < 20000; i++) {
        zenixfs_set_time(fs, 100 + i);
        if (random_op(fs, 2)) { printf("  op %d\n", i); failures++; return 1; }
        if (i % 2000 == 1999) { CHECK(!compare_to_model(fs, "bigvol periodic")); CHECK(zenixfs_fsck(fs, logmsg, NULL) == 0); }
        if (i % 5000 == 4999) { CHECK(zenixfs_unmount(fs) == 0); CHECK(zenixfs_mount_ex(&fs, &m->dev, 48, 1) == 0); CHECK(!compare_to_model(fs, "bigvol remount")); }
    }
    struct zenixfs_stats st; zenixfs_get_stats(fs, &st);
    struct zenixfs_statfs sf; zenixfs_statfs(fs, &sf);
    printf("  bigvol: 20000 ops, %u commits, %u writes, %u reads; %u of %u blocks free at the end\n", st.commits, st.writes, st.reads, sf.bfree, sf.blocks);
    CHECK(zenixfs_unmount(fs) == 0);
    int high = 0;                                   /* blocks used beyond the first map page (7936 blocks): proves the other pages were in play */
    for (uint32_t b = 8000; b < m->dev.nblocks; b++) { const uint8_t *q = m->img + ((size_t)b << 10); for (int i = 0; i < 1024; i++) if (q[i]) { high++; break; } }
    printf("  bigvol: %d blocks written beyond the first map page\n", high);
    CHECK(high > 100);
    zmem_free(m);
    return 0;
}

/* ---------- 5. full volume: unlink must still work, and space comes back ---------- */
static int test_full(void)
{
    struct zmem *m = zmem_new(10, 1024, 0);
    struct zenixfs *fs; uint32_t f;
    CHECK(zenixfs_mkfs(&m->dev, NULL, 0, 5) == 0);
    CHECK(zenixfs_mount(&fs, &m->dev, 32) == 0);
    struct zenixfs_statfs a, b;
    CHECK(zenixfs_statfs(fs, &a) == 0);
    uint8_t buf[1024]; memset(buf, 7, sizeof buf);
    int n = 0, r = 0;
    for (; n < 100; n++) {
        char nm[16]; snprintf(nm, sizeof nm, "f%d", n);
        r = zenixfs_create(fs, ZENIXFS_ROOT_INO, nm, strlen(nm), 0644, 0, 0, &f);
        if (r) break;
        for (int k = 0; k < 40 && r >= 0; k++) r = zenixfs_write(fs, f, (uint64_t)k * 1024, buf, 1024);
        if (r < 0) break;
    }
    CHECK(r == ZENIXFS_ENOSPC);
    printf("  full: ENOSPC after %d files\n", n);
    for (int i = 0; i <= n; i++) { char nm[16]; snprintf(nm, sizeof nm, "f%d", i); int u = zenixfs_unlink(fs, ZENIXFS_ROOT_INO, nm, strlen(nm)); CHECK(u == 0 || u == ZENIXFS_ENOENT); }
    CHECK(zenixfs_sync(fs) == 0);
    CHECK(zenixfs_statfs(fs, &b) == 0);
    CHECK(b.bfree + 8 >= a.bfree);
    CHECK(zenixfs_fsck(fs, logmsg, NULL) == 0);
    CHECK(zenixfs_create(fs, ZENIXFS_ROOT_INO, "again", 5, 0644, 0, 0, &f) == 0);
    zenixfs_unmount(fs); zmem_free(m);
    return 0;
}

/* ---------- 6. orphans: unlinked while open, power cut, space is returned at mount ---------- */
static int test_orphan(void)
{
    struct zmem *m = zmem_new(10, 2048, 0);
    struct zenixfs *fs; uint32_t f;
    CHECK(zenixfs_mkfs(&m->dev, NULL, 0, 5) == 0);
    CHECK(zenixfs_mount(&fs, &m->dev, 32) == 0);
    struct zenixfs_statfs a, b; zenixfs_statfs(fs, &a);
    CHECK(zenixfs_create(fs, ZENIXFS_ROOT_INO, "tmp", 3, 0644, 0, 0, &f) == 0);
    uint8_t buf[4096]; memset(buf, 9, sizeof buf);
    CHECK(zenixfs_write(fs, f, 0, buf, 4096) == 4096);
    CHECK(zenixfs_orphan_add(fs, f) == 0);
    CHECK(zenixfs_unlink(fs, ZENIXFS_ROOT_INO, "tmp", 3) == 0);
    uint8_t r[16]; CHECK(zenixfs_read(fs, f, 0, r, 16) == 16 && r[0] == 9);   /* still readable */
    CHECK(zenixfs_sync(fs) == 0);                                              /* crash here: do not unmount */
    struct zmem *c = zmem_clone(m);
    struct zenixfs *f2; CHECK(zenixfs_mount(&f2, &c->dev, 32) == 0);               /* mount frees the orphan */
    CHECK(zenixfs_sync(f2) == 0);
    CHECK(zenixfs_fsck(f2, logmsg, NULL) == 0);
    zenixfs_statfs(f2, &b);
    CHECK(b.bfree + 4 >= a.bfree);
    zenixfs_unmount(f2); zmem_free(c);
    CHECK(zenixfs_orphan_done(fs, f) == 0);                                    /* the normal close path */
    CHECK(zenixfs_fsck(fs, logmsg, NULL) == 0);
    zenixfs_unmount(fs); zmem_free(m);
    return 0;
}


/* ---------- 7. names with the same directory hash form a chain: find, remove from the middle, list, check ---------- */
static uint32_t h24(const char *n)
{
    uint32_t h = 2166136261u;
    for (; *n; n++) { h ^= (uint8_t)*n; h *= 16777619u; }
    return ((h >> 8) ^ h) & 0xFFFFFF;
}
struct hp { uint32_t h, i; };
static int hpcmp(const void *a, const void *b)
{
    const struct hp *x = a, *y = b;
    return x->h != y->h ? (x->h < y->h ? -1 : 1) : (x->i < y->i ? -1 : x->i > y->i);
}
static int test_collide(void)
{
    enum { N = 9000000 };
    struct hp *v = malloc((size_t)N * sizeof *v);
    char nm[32];
    for (uint32_t i = 0; i < N; i++) { snprintf(nm, sizeof nm, "c%07u", i); v[i].h = h24(nm); v[i].i = i; }
    qsort(v, N, sizeof *v, hpcmp);
    uint32_t best = 0, bl = 0;
    for (uint32_t i = 0; i < N; ) { uint32_t j = i; while (j < N && v[j].h == v[i].h) j++; if (j - i > bl) { bl = j - i; best = i; } i = j; }
    printf("  longest chain found among %d names: %u names share one hash\n", (int)N, bl);
    CHECK(bl >= 5);
    uint32_t group[16]; uint32_t g = bl > 16 ? 16 : bl;
    for (uint32_t k = 0; k < g; k++) group[k] = v[best + k].i;
    free(v);
    struct zmem *m = zmem_new(11, 4096, 0);
    struct zenixfs *fs; uint32_t f, x;
    CHECK(zenixfs_mkfs(&m->dev, NULL, 0, 5) == 0);
    CHECK(zenixfs_mount(&fs, &m->dev, 64) == 0);
    for (uint32_t k = 0; k < g; k++) { snprintf(nm, sizeof nm, "c%07u", group[k]); CHECK(zenixfs_create(fs, ZENIXFS_ROOT_INO, nm, strlen(nm), 0644, 0, 0, &f) == 0); CHECK(zenixfs_write(fs, f, 0, nm, strlen(nm)) > 0); }
    for (int round = 0; round < 3; round++) {
        for (uint32_t k = 0; k < g; k++) {
            snprintf(nm, sizeof nm, "c%07u", group[k]);
            if (zenixfs_lookup(fs, ZENIXFS_ROOT_INO, nm, strlen(nm), &x) != 0) continue;     /* removed earlier in this round */
            char back[32]; int n = zenixfs_read(fs, x, 0, back, sizeof back);
            CHECK(n == (int)strlen(nm) && !memcmp(back, nm, n));
        }
        /* remove the middle, then the first, then the last of what is left; each time all the rest must be found and listed */
        uint32_t order[3] = { g / 2, 0, g - 1 };
        snprintf(nm, sizeof nm, "c%07u", group[order[round]]);
        CHECK(zenixfs_unlink(fs, ZENIXFS_ROOT_INO, nm, strlen(nm)) == 0);
        CHECK(zenixfs_lookup(fs, ZENIXFS_ROOT_INO, nm, strlen(nm), &x) == ZENIXFS_ENOENT);
        uint64_t cookie = 0; char dn[300]; uint32_t di; uint8_t dt; int seen = 0;
        while (zenixfs_readdir(fs, ZENIXFS_ROOT_INO, &cookie, dn, &di, &dt) > 0) if (dn[0] == 'c') seen++;
        CHECK(seen == (int)g - 1 - round);
        CHECK(zenixfs_fsck(fs, logmsg, NULL) == 0);
    }
    /* put one back: it joins the end of the chain, everything still found */
    snprintf(nm, sizeof nm, "c%07u", group[0]);
    CHECK(zenixfs_create(fs, ZENIXFS_ROOT_INO, nm, strlen(nm), 0644, 0, 0, &f) == 0);
    CHECK(zenixfs_lookup(fs, ZENIXFS_ROOT_INO, nm, strlen(nm), &x) == 0 && x == f);
    CHECK(zenixfs_unmount(fs) == 0);
    CHECK(zenixfs_mount(&fs, &m->dev, 64) == 0);
    CHECK(zenixfs_fsck(fs, logmsg, NULL) == 0);
    zenixfs_unmount(fs); zmem_free(m);
    return 0;
}

int main(int argc, char **argv)
{
    uint64_t seed = argc > 1 ? strtoull(argv[1], NULL, 10) : 1;
    const char *only = argc > 2 ? argv[2] : NULL;
    rs ^= seed * 0x9E3779B97F4A7C15ull; for (int i = 0; i < 10; i++) rnd();
    struct { const char *name; int (*fn)(void); } tests[] = {
        { "basic", test_basic }, { "random", test_random }, { "powercut", test_powercut },
        { "bitflip", test_bitflip }, { "full", test_full }, { "orphan", test_orphan }, { "collide", test_collide }, { "bigvol", test_bigvol }, { "bigpower", test_bigpower },
    };
    int ran = 0;
    for (unsigned i = 0; i < sizeof tests / sizeof *tests; i++) {
        if (only && strcmp(only, tests[i].name)) continue;
        if (!only && !strcmp(tests[i].name, "bigpower")) continue;     /* slow: run it by name */
        printf("%s (seed %llu)\n", tests[i].name, (unsigned long long)seed);
        int before = failures;
        tests[i].fn();
        printf("%s: %s\n", tests[i].name, failures == before ? "ok" : "FAILED");
        ran++;
    }
    printf("%d tests, %d failures\n", ran, failures);
    return failures != 0;
}
