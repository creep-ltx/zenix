/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * libFuzzer harness for nfscd's core (DESIGN D23 10, 11.1): everything that comes from the NAS is untrusted. The
 * harness mounts with valid answers of its own (GETPORT, MNT, FSINFO, GETATTR), then plays the input as a list of
 * steps against a core with requests of every kind held:
 *   0 LEN BYTES         raw bytes from the socket (record marking, fragments, lengths)
 *   1 K LEN BYTES       a record: the xid of the K-th call out, then BYTES (the rest of the RPC reply)
 *   2 K LEN BYTES       a record: the xid of the K-th call out, an accepted SUCCESS header, then BYTES (the result)
 *   3 N                 N x 300 ms pass (timeouts, retries, commits)
 *   4                   the connection breaks (it is made again at once)
 *   5 T A               a new request of type T (FS_*), argument choice A
 *   6 K                 the K-th held request is cancelled
 *   7 P LEN BYTES       BYTES decoded straight as the result of procedure P (every decoder, READDIR entries too)
 * Checks (abort = a finding): every request answered exactly once, nothing written outside the buffers (ASan), no
 * leak after nfs_kill and nfs_end, the heap counter back at 0.
 *   nfs-fuzz [libFuzzer options]          (built with -fsanitize=fuzzer)
 *   nfs-fuzz-seeds DIR                    (built with -DSEEDS: writes seed inputs)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../net/nfs/nfs_int.h"

static uint32_t now_ms;
static long heap;
static int want_connect, connected;
static unsigned char out[1 << 17];
static long outn;

uint32_t nfs_now(void) { return now_ms; }
void nfs_log(const char *t) { (void)t; }
void *nfs_alloc(uint32_t n)
{
    if (heap + n > (6 << 20)) return 0;
    long *p = malloc(n + 16);
    if (!p) return 0;
    p[0] = n; heap += n;
    return p + 2;
}
void nfs_free(void *q) { if (q) { long *p = (long *)q - 2; heap -= p[0]; free(p); } }
void nfs_save(const void *b, uint32_t n) { (void)b; (void)n; }
int nfs_connect(int port) { (void)port; want_connect = 1; connected = 0; outn = 0; return 0; }
void nfs_disconnect(void) { connected = 0; want_connect = 0; }
long nfs_send(const void *b, long n)
{
    if (!connected) return 0;
    if (outn + n > (long)sizeof out) outn = 0;    /* (only the newest records matter) */
    memcpy(out + outn, b, (size_t)n);
    outn += n;
    return n;
}

#define NREQ 64
/* the paths are FS_PATHMAX buffers, as the kernel's: with UMSG_LINKS (DESIGN D26) an answer may write TEXT there */
static struct { uint32_t id, type; int done, used; unsigned char buf[9000]; char path[2][FS_PATHMAX]; char was[2][FS_PATHMAX]; uint32_t ck; struct fsrdx q; } rq[NREQ];
static uint32_t next_id = 1;
void nfs_answer(uint32_t id, long result)
{
    (void)result;
    for (int i = 0; i < NREQ; i++)
        if (rq[i].used && rq[i].id == id) {
            if (rq[i].done) { fprintf(stderr, "request %u answered twice\n", id); abort(); }
            rq[i].done = 1;
            /* D26: only an FS_LINKED answer may change a path, only the argument its bits name, as the table says, and
             * TEXT ends within FS_PATHMAX; a link answer only to a request that asked (the kflags in the type's top) */
            int k = fsl_is(result) ? ((-result - FS_LINKED) & FSL_A1) : -1;
            if (fsl_is(result) && (!(rq[i].type & 0x80000000u) || !fsl_ok(rq[i].type & 0xFFFF, (unsigned)(-result - FS_LINKED)) ||
                                   !memchr(rq[i].path[k], 0, FS_PATHMAX))) {
                fprintf(stderr, "request %u: an FS_LINKED answer %ld it may not have\n", id, result); abort();
            }
            for (int j = 0; j < 2; j++)
                if (j != k && memcmp(rq[i].path[j], rq[i].was[j], FS_PATHMAX)) {
                    fprintf(stderr, "request %u: its a%d changed (answer %ld)\n", id, j, result); abort();
                }
            return;
        }
    fprintf(stderr, "an answer for a request never sent (%u)\n", id);
    abort();
}

/* ---- the calls the core sent: their xids, in order ---- */
static uint32_t xids[256], procs[256], progs[256];
static int nx;
static uint32_t rd32(const unsigned char *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void scan_out(void)
{
    long p = 0;
    nx = 0;
    while (p + 4 <= outn) {
        uint32_t len = rd32(out + p) & 0x7FFFFFFF;
        if (p + 4 + (long)len > outn || len < 24) break;
        if (nx < 256) { xids[nx] = rd32(out + p + 4); progs[nx] = rd32(out + p + 16); procs[nx] = rd32(out + p + 24); nx++; }
        p += 4 + len;
    }
}

static void feed(const unsigned char *b, long n) { nfs_input(b, n); }
static void put32(unsigned char *p, uint32_t v) { p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16); p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v; }

static void record(uint32_t xid, int success, const unsigned char *b, long n)
{
    static unsigned char r[70000];
    long k = 4;
    if (n > 60000) n = 60000;
    put32(r + k, xid); k += 4;
    if (success) {
        put32(r + k, 1); put32(r + k + 4, 0); put32(r + k + 8, 0); put32(r + k + 12, 0); put32(r + k + 16, 0);
        k += 20;
    }
    memcpy(r + k, b, (size_t)n); k += n;
    put32(r, 0x80000000u | (uint32_t)(k - 4));
    feed(r, k);
}

/* valid answers: a fattr3, and the mount's four */
static long fattr(unsigned char *p, uint32_t type, uint64_t fileid, uint64_t size)
{
    uint32_t w[21] = { type, 0755, 1, 1000, 100, (uint32_t)(size >> 32), (uint32_t)size, 0, 0, 0, 0, 0, 1,
                       (uint32_t)(fileid >> 32), (uint32_t)fileid, 1, 0, 2, 0, 3, 0 };
    for (int i = 0; i < 21; i++) put32(p + 4 * i, w[i]);
    return 84;
}
static void answer_mount(void)
{
    for (int round = 0; round < 8; round++) {
        if (want_connect) { want_connect = 0; connected = 1; outn = 0; nfs_connected(0); }
        scan_out();
        if (!nx) return;
        unsigned char b[400];
        long k = 0;
        uint32_t x = xids[nx - 1], pg = progs[nx - 1], pc = procs[nx - 1];
        outn = 0;
        if (pg == PMAP_PROG) { put32(b, 20048); k = 4; }
        else if (pg == MOUNT_PROG) { put32(b, 0); put32(b + 4, 28); memset(b + 8, 0x11, 28); put32(b + 36, 1); put32(b + 40, 1); k = 44; }
        else if (pc == P3_FSINFO) {
            put32(b, 0); put32(b + 4, 0); k = 8;
            uint32_t v[] = { 8192, 8192, 4096, 8192, 8192, 4096, 4096, 0, 0xFFFFFFFF, 0, 1, 0x1B };
            for (unsigned i = 0; i < sizeof v / 4; i++) { put32(b + k, v[i]); k += 4; }
        } else if (pc == P3_GETATTR) { put32(b, 0); k = 4 + fattr(b + 4, NF3DIR, 77, 4096); }
        else return;
        record(x, 1, b, k);
    }
}

static int new_req(uint32_t type, unsigned a)
{
    int i;
    for (i = 0; i < NREQ; i++) if (!rq[i].used || rq[i].done) break;
    if (i == NREQ) return -1;
    static const char *paths[] = { "a", "d/b", "/x/y/z", "ln", "..", "a/../b", "", "/ln/q", "/d/ln" };
    memset(&rq[i], 0, sizeof rq[i]);
    rq[i].used = 1; rq[i].id = next_id++;
    snprintf(rq[i].path[0], FS_PATHMAX, "%s", paths[a % 9]);
    snprintf(rq[i].path[1], FS_PATHMAX, "%s", paths[(a / 9) % 9]);
    memcpy(rq[i].was, rq[i].path, sizeof rq[i].was);
    struct nfs_req r;
    memset(&r, 0, sizeof r);
    int links = (a >> 6 & 1) != 0;                    /* D26: the kernel's UMSG_LINKS on some requests */
    rq[i].type = type | (links ? 0x80000000u : 0);
    r.id = rq[i].id; r.type = type; r.kflags = NFS_REQ_KERNEL | (links ? NFS_REQ_LINKS : 0); r.uid = (uint16_t)(a & 1 ? 1000 : 0); r.gid = 100;
    uintptr_t buf = (uintptr_t)rq[i].buf, p0 = (uintptr_t)rq[i].path[0], p1 = (uintptr_t)rq[i].path[1];
    static const uint32_t inos[5] = { 2, 1234, 500, 501, 0x80000001u };   /* the root, the seeds' files, a probe */
    uint32_t ino = inos[a % 5];
    switch (type) {
    case FS_OPEN: r.a[0] = p0; r.a[1] = 2; r.a[2] = a >> 2 & 0x2C3; r.a[3] = buf; break;
    case FS_READ: case FS_WRITE: r.a[0] = ino; r.a[1] = a * 1000u; r.a[2] = buf; r.a[3] = 1 + a * 37u % 8999; break;
    case FS_STAT: case FS_LSTAT: case FS_CHDIR: case FS_MKDIR: case FS_UNLINK: case FS_RMDIR: case FS_CHMOD:
    case FS_UTIMES: case FS_CHOWN:
        r.a[0] = p0; r.a[1] = 2; r.a[2] = type == FS_CHMOD ? 0644 : buf; r.a[3] = 5; break;
    case FS_FSTAT: r.a[0] = ino; r.a[1] = buf; break;
    case FS_READDIR: r.a[0] = ino; r.a[1] = a % 50; r.a[2] = buf; r.a[3] = (uintptr_t)&rq[i].ck; break;
    case FS_READDIRS: rq[i].q.size = 104 + a * 61u % 8800; r.a[0] = ino; r.a[1] = a % 50; r.a[2] = buf; r.a[3] = (uintptr_t)&rq[i].q; break;   /* D49 */
    case FS_RENAME: case FS_LINK: case FS_SYMLINK: r.a[0] = p0; r.a[1] = p1; r.a[2] = 2; break;
    case FS_READLINK: r.a[0] = p0; r.a[1] = 2; r.a[2] = buf; r.a[3] = 100; break;
    case FS_STATFS: r.a[0] = buf; break;
    case FS_SYNC: r.a[0] = a & 1; break;
    case FS_ROOT: r.a[0] = buf; break;
    case FS_CLOSE: r.a[0] = ino; break;
    default: break;
    }
    nfs_request(&r);
    return i;
}

static void run(const unsigned char *d, size_t n)
{
    struct nfs_conf c;
    memset(&c, 0, sizeof c);
    c.dev = "nfs0"; c.export_path = "/export";
    static const char *codes[] = { "t1", "st1r1", "t1y", "t1a0", "t1o", "lt1" };
    c.codes = codes[n ? d[0] % 6 : 0];
    c.large = n > 1 && (d[1] & 1);
    c.seed = 1;
    uint32_t opens[2] = { 0x80000005u, 9 };
    if (n > 1 && (d[1] & 2)) { c.opens = opens; c.nopens = 2; }
    now_ms = 1000; outn = 0; want_connect = connected = 0;
    memset(rq, 0, sizeof rq);
    if (nfs_init(&c)) abort();
    new_req(FS_ROOT, 0);
    new_req(FS_STAT, 0);
    answer_mount();
    /* a few requests held from the start */
    new_req(FS_OPEN, 0); new_req(FS_READDIR, 2); new_req(FS_READDIRS, 2); new_req(FS_STATFS, 0); new_req(FS_READ, 2);
    size_t p = 2;
    int steps = 0;
    while (p < n && steps++ < 200) {
        unsigned k = d[p++];
        if (want_connect) { want_connect = 0; connected = 1; nfs_connected(0); }
        scan_out();
        switch (k % 8) {
        case 0: case 1: case 2: case 7: {
            if (p + 3 > n) { p = n; break; }
            unsigned which = d[p];
            long len = d[p + 1] | d[p + 2] << 8;
            p += 3;
            if (len > (long)(n - p)) len = (long)(n - p);
            if (k % 8 == 0) feed(d + p, len);
            else if (k % 8 == 7) {
                struct call cl; struct res r; struct xdr x = { (u8 *)d + p, (u8 *)d + p + len, 0 };
                memset(&cl, 0, sizeof cl);
                cl.prog = PG_NFS; cl.proc = (u8)(which % 22); cl.count = 8192; cl.node = N_NONE;
                memset(&r, 0, sizeof r);
                if (dec_res(&cl, &x, &r) == 0 && (cl.proc == P3_READDIR || cl.proc == P3_READDIRPLUS) && !r.err) {
                    u64 fid, ck; const u8 *nm; u32 nl; struct res er;
                    memset(&er, 0, sizeof er);
                    for (int e = 0; e < 1000 && dec_dirent(&r.dir, cl.proc == P3_READDIRPLUS, &fid, &nm, &nl, &ck, &er) > 0; e++)
                        if (nl) (void)nm[nl - 1];
                }
                if (r.data && r.count) (void)r.data[r.count - 1];
            } else if (nx) record(xids[which % nx], k % 8 == 2, d + p, len);
            p += (size_t)len;
            break;
        }
        case 3: now_ms += 300u * (p < n ? d[p++] : 1); nfs_tick(); break;
        case 4: if (connected) { connected = 0; nfs_input(0, -104); } break;
        case 5: if (p + 2 <= n) { new_req(d[p] % 28, d[p + 1]); p += 2; } break;
        case 6:
            if (p < n) {
                int i = d[p++] % NREQ;
                if (rq[i].used && !rq[i].done) nfs_cancel(rq[i].id);
            }
            break;
        }
        nfs_tick();
    }
    nfs_kill();
    for (int i = 0; i < NREQ; i++)
        if (rq[i].used && !rq[i].done) {
            /* FS_CLOSE is answered at once; anything else held must have been answered by nfs_kill */
            fprintf(stderr, "request %u never answered\n", rq[i].id);
            abort();
        }
    nfs_end();
    if (heap) { fprintf(stderr, "the core left %ld bytes\n", heap); abort(); }
}

#ifdef SEEDS
static void seed(const char *dir, const char *name, const unsigned char *b, size_t n)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "wb");
    if (f) { fwrite(b, 1, n, f); fclose(f); }
}
int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    unsigned char b[2000];
    size_t k;
    /* a LOOKUP answer for the first call out: status, fh, attrs, dir attrs */
    k = 0; b[k++] = 0; b[k++] = 0;
    b[k++] = 2; b[k++] = 0; b[k++] = 0; { unsigned char r[300]; long n = 0;
        put32(r, 0); put32(r + 4, 28); memset(r + 8, 0x22, 28); n = 36; put32(r + n, 1); n += 4; n += fattr(r + n, NF3REG, 1234, 5000);
        put32(r + n, 1); n += 4; n += fattr(r + n, NF3DIR, 77, 4096);
        b[k - 2] = (unsigned char)n; b[k - 1] = (unsigned char)(n >> 8); memcpy(b + k, r, (size_t)n); k += (size_t)n; }
    b[k++] = 3; b[k++] = 2;
    seed(argv[1], "lookup", b, k);
    /* a READDIRPLUS answer with two entries */
    k = 0; b[k++] = 1; b[k++] = 1;
    b[k++] = 2; b[k++] = 1; b[k++] = 0; b[k++] = 0; { unsigned char r[600]; long n = 0;
        put32(r, 0); put32(r + 4, 1); n = 8; n += fattr(r + n, NF3DIR, 77, 4096); memset(r + n, 0, 8); n += 8;
        for (int e = 0; e < 2; e++) {
            put32(r + n, 1); put32(r + n + 4, 0); put32(r + n + 8, 500 + e); put32(r + n + 12, 1);
            r[n + 16] = (unsigned char)('a' + e); r[n + 17] = r[n + 18] = r[n + 19] = 0;
            put32(r + n + 20, 0); put32(r + n + 24, 1 + e); n += 28;
            put32(r + n, 1); n += 4; n += fattr(r + n, NF3REG, 500 + e, 10);
            put32(r + n, 1); put32(r + n + 4, 28); memset(r + n + 8, 0x30 + e, 28); n += 36;
        }
        put32(r + n, 0); put32(r + n + 4, 1); n += 8;
        b[k - 2] = (unsigned char)n; b[k - 1] = (unsigned char)(n >> 8); memcpy(b + k, r, (size_t)n); k += (size_t)n; }
    seed(argv[1], "readdirplus", b, k);
    /* raw record marking: two fragments */
    k = 0; b[k++] = 0; b[k++] = 0;
    b[k++] = 0; b[k++] = 0; b[k++] = 16; b[k++] = 0;
    { unsigned char r[16] = { 0, 0, 0, 4, 0, 0, 0, 1, 0x80, 0, 0, 4, 0, 0, 0, 0 }; memcpy(b + k, r, 16); k += 16; }
    seed(argv[1], "fragments", b, k);
    /* requests, time, a break, a cancel */
    unsigned char t[] = { 2, 3, 5, 1, 9, 5, 3, 4, 3, 20, 4, 6, 3, 5, 6, 1, 5, 13, 0, 3, 100 };
    seed(argv[1], "requests", t, sizeof t);
    /* every decoder straight */
    for (int pc = 0; pc < 22; pc++) {
        unsigned char r[200]; size_t n = 0;
        r[n++] = 0; r[n++] = 0; r[n++] = 7; r[n++] = (unsigned char)pc; r[n++] = 120; r[n++] = 0;
        put32(r + n, 0); n += 4; put32(r + n, 1); n += 4; n += (size_t)fattr(r + n, NF3REG, 5, 100);
        memset(r + n, 0, 200 - n); n = 126;
        char nm[16]; snprintf(nm, sizeof nm, "dec%02d", pc);
        seed(argv[1], nm, r, n);
    }
    return 0;
}
#else
int LLVMFuzzerTestOneInput(const unsigned char *d, size_t n)
{
    run(d, n);
    return 0;
}
#endif
