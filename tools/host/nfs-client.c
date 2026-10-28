/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * nfscd's core (net/nfs/, DESIGN D23 11.1) on the PC: a Linux OS layer (a TCP socket, poll, a counted heap) and a
 * fake of the kernel's side that sends Zenix requests from a script, one command per line on stdin:
 *   mount                       FS_ROOT
 *   open PATH FLAGS             FS_OPEN (flags: r w rw c t x e, combined: "wct"; x O_EXCL, e O_EXEC) -> inode
 *   close INO | fstat INO | stat PATH | lstat PATH | chdir PATH | readlink PATH
 *   read INO OFF N              FS_READ: the count and the bytes' FNV-1a hash (and the text if short)
 *   write INO OFF TEXT          FS_WRITE ("@N:C": N bytes of the character C)
 *   get PATH FILE [CHUNK]       open, read it whole into the PC's FILE, close
 *   put FILE PATH [CHUNK]       open "wct", write the PC's FILE, close
 *   ls PATH                     every entry (FS_READDIR), "name ino" per line, sorted, then the count
 *   mkdir P | unlink P | rmdir P | rename A B | link A B | symlink TARGET P | chmod P MODE | chown P UID GID
 *   utimes P T|now | statfs | sync [1] | quit
 *   as UID GID [G...]          later requests come from this user (supplementary groups G)
 *   raw on|off                 later requests come as a user's own SYS_call (no UMSG_KERNEL)
 *   links on|off               later requests carry UMSG_LINKS (DESIGN D26): an FS_LINKED answer prints
 *                              "CMD -> -102N a0|a1 TEXT"; any path buffer changed otherwise is a FAIL line
 *   bg CMD...                  start a request without waiting; "id N" (its id) is printed
 *   wait ID                    wait for that request's answer
 *   cancel ID                  the message gets UMSG_CANCEL: nfs_cancel
 *   cancelled CMD...           send a request whose message is already cancelled (queued, then killed)
 *   sleep MS                   run the loop for MS milliseconds
 *   kill                       nfscd killed (nfs_kill)
 *   stats                      the core's counters
 * Every command prints one line "CMD -> RESULT". Lines from the core's log are printed "log: TEXT".
 * Options: -h HOST (127.0.0.1) -e EXPORT -c CODES -L (large profile) -P PMAPPORT -N NFSPORT (the PC test ports for
 * 111 and 2049) -s SRCPORT (bind this source port, SO_REUSEADDR) -S FILE (the saved table: read at the start, written
 * by nfs_save) -O INO,INO (SVC_OPENS after a restart) -m BYTES (the heap's cap) -t SECONDS (a command's longest wait)
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "../../net/nfs/nfs.h"
#include "../../kernel/fsproto.h"

struct ustat_ { uint32_t ino, mode, size, mtime, uid, gid, links; };
struct udirent_ { uint32_t ino, type; char name[64]; };

static const char *host = "127.0.0.1";
static int pmap_port = 111, nfs_port = 2049, src_port, conn_pending, sock = -1, cmd_timeout = 120;
static long memcap = 4 << 20, memused, mempeak;
static const char *savefile;

/* ---- the OS layer ---- */
uint32_t nfs_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}
void nfs_log(const char *text) { printf("log: %s\n", text); fflush(stdout); }
void *nfs_alloc(uint32_t n)
{
    if (memused + (long)n > memcap) return 0;
    long *p = malloc(n + 16);
    if (!p) return 0;
    p[0] = n; memused += n;
    if (memused > mempeak) mempeak = memused;
    return p + 2;
}
void nfs_free(void *q)
{
    if (!q) return;
    long *p = (long *)q - 2;
    memused -= p[0];
    free(p);
}
void nfs_save(const void *buf, uint32_t n)
{
    if (!savefile) return;
    FILE *f = fopen(savefile, "wb");
    if (f) { fwrite(buf, 1, n, f); fclose(f); }
}

#define NANS 256
/* D26: a request's path (and FS_SYMLINK's target) buffers as sent: an answer may change only the one its FS_LINKED says */
static struct { uint32_t id; long res; int done; uint32_t type; char *p[2]; char was[2][FS_PATHMAX + 1]; } ans[NANS];
static int path_args(uint32_t type)              /* bit 0: a0 is a path (or target text), bit 1: a1 */
{
    switch (type) {
    case FS_OPEN: case FS_STAT: case FS_LSTAT: case FS_CHDIR: case FS_MKDIR: case FS_UNLINK: case FS_RMDIR:
    case FS_CHMOD: case FS_CHOWN: case FS_UTIMES: case FS_READLINK: return 1;
    case FS_RENAME: case FS_LINK: case FS_SYMLINK: return 3;
    }
    return 0;
}
static uint32_t next_id = 1;
void nfs_answer(uint32_t id, long result)
{
    for (int i = 0; i < NANS; i++) if (ans[i].id == id && !ans[i].done) {
        ans[i].res = result; ans[i].done = 1;
        int may = fsl_is(result) ? ((-result - FS_LINKED) & FSL_A1 ? 1 : 0) : -1;
        if (fsl_is(result) && !fsl_ok(ans[i].type, (unsigned)(-result - FS_LINKED)))
            printf("FAIL request %u (type %u): FS_LINKED bits %ld outside the table\n", id, ans[i].type, -result - FS_LINKED);
        for (int k = 0; k < 2; k++)
            if (ans[i].p[k] && k != may && strcmp(ans[i].p[k], ans[i].was[k]))
                printf("FAIL request %u (type %u, answer %ld): its a%d changed to \"%s\"\n", id, ans[i].type, result, k, ans[i].p[k]);
        return;
    }
    printf("FAIL an answer for an unknown request %u\n", id);
}

long nfs_send(const void *buf, long n)
{
    if (sock < 0 || conn_pending) return 0;
    long k = send(sock, buf, (size_t)n, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (k < 0) return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -ECONNRESET;
    return k;
}

static int connect_err;
int nfs_connect(int port)
{
    if (sock >= 0) { close(sock); sock = -1; }
    int p = port == 111 ? pmap_port : port == 2049 ? nfs_port : port;
    sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (sock < 0) return -ENOBUFS;
    int one = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (src_port) {
        struct sockaddr_in b = { .sin_family = AF_INET, .sin_port = htons((uint16_t)src_port) };
        if (bind(sock, (struct sockaddr *)&b, sizeof b) < 0) printf("log(os): bind to source port %d: %s\n", src_port, strerror(errno));
    }
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)p) };
    inet_pton(AF_INET, host, &a.sin_addr);
    int r = connect(sock, (struct sockaddr *)&a, sizeof a);
    conn_pending = 1;
    connect_err = 0;
    if (r < 0 && errno != EINPROGRESS) connect_err = -errno;
    return 0;                                    /* the answer comes through the loop, never from in here */
}
void nfs_disconnect(void)
{
    if (sock < 0) return;
    struct linger l = { 1, 0 };                  /* close by abort: a reset, no TIME_WAIT */
    setsockopt(sock, SOL_SOCKET, SO_LINGER, &l, sizeof l);
    close(sock);
    sock = -1;
    conn_pending = 0;
}

/* one turn of the loop: wait at most ms */
static void turn(int ms)
{
    uint32_t t;
    int b = nfs_poll(&t);
    if (t < (uint32_t)ms) ms = (int)t;
    struct pollfd p = { sock, 0, 0 };
    if (sock >= 0 && conn_pending && connect_err) {
        int e = connect_err; connect_err = 0; conn_pending = 0;
        close(sock); sock = -1;
        nfs_connected(e);
        nfs_tick();
        return;
    }
    if (sock >= 0) p.events = conn_pending ? POLLOUT : (POLLIN | ((b & NFS_P_OUT) ? POLLOUT : 0));
    int n = poll(&p, sock >= 0 ? 1 : 0, ms);
    if (n > 0 && sock >= 0) {
        if (conn_pending) {
            int err = 0; socklen_t l = sizeof err;
            getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &l);
            conn_pending = 0;
            if (err) { close(sock); sock = -1; nfs_connected(-err); }
            else nfs_connected(0);
        } else {
            if (p.revents & (POLLIN | POLLHUP | POLLERR)) {
                static unsigned char buf[65536];
                long k = recv(sock, buf, sizeof buf, MSG_DONTWAIT);
                if (k > 0) nfs_input(buf, k);
                else if (k == 0) { close(sock); sock = -1; nfs_input(0, 0); }
                else if (errno != EAGAIN) { close(sock); sock = -1; nfs_input(0, -ECONNRESET); }
            }
        }
    }
    nfs_tick();
}

/* ---- the kernel's side ---- */
static uint16_t cuid, cgid, cumask = 022, raw, links, ngroups;
static uint32_t groups[NFS_NGROUPS];

static uint32_t send_req(uint32_t type, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, int cancelled)
{
    struct nfs_req r;
    memset(&r, 0, sizeof r);
    r.id = next_id++;
    r.type = type; r.a[0] = a0; r.a[1] = a1; r.a[2] = a2; r.a[3] = a3;
    r.uid = cuid; r.gid = cgid; r.umask = cumask;
    r.kflags = (raw ? 0 : NFS_REQ_KERNEL) | (cancelled ? NFS_REQ_CANCEL : 0) | (links ? NFS_REQ_LINKS : 0);
    r.ngroups = ngroups; memcpy(r.groups, groups, sizeof groups);
    int i;
    for (i = 0; i < NANS; i++) if (!ans[i].id) break;
    if (i == NANS) { printf("FAIL too many requests in the test\n"); exit(1); }
    ans[i].id = r.id; ans[i].done = 0; ans[i].type = type;
    for (int k = 0; k < 2; k++) {
        ans[i].p[k] = (path_args(type) >> k & 1) && r.a[k] ? (char *)r.a[k] : 0;
        if (ans[i].p[k]) snprintf(ans[i].was[k], sizeof ans[i].was[k], "%s", ans[i].p[k]);
    }
    /* as nfscd: a message stays queued on the port while the core has no free slot (NFS_P_TAKE); FS_CLOSE and a
     * cancelled message are taken at once (they need none) */
    uint32_t t, start = nfs_now();
    while (type != FS_CLOSE && !cancelled && !(nfs_poll(&t) & (NFS_P_TAKE | NFS_P_DONE)) && nfs_now() - start < 60000) turn(100);
    nfs_request(&r);
    return r.id;
}
static int slot_of(uint32_t id) { for (int i = 0; i < NANS; i++) if (ans[i].id == id) return i; return -1; }
static long wait_for(uint32_t id)
{
    int i = slot_of(id);
    uint32_t start = nfs_now();
    while (!ans[i].done) {
        if (nfs_now() - start > (uint32_t)cmd_timeout * 1000) return -99999;
        turn(100);
    }
    long r = ans[i].res;
    ans[i].id = 0;
    return r;
}
static long call(uint32_t type, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3)
{
    return wait_for(send_req(type, a0, a1, a2, a3, 0));
}

static uint32_t flags_of(const char *s)
{
    uint32_t f = 0;
    int r = strchr(s, 'r') != 0, w = strchr(s, 'w') != 0;
    if (r && w) f |= 2; else if (w) f |= 1;
    if (strchr(s, 'c')) f |= 0x40;
    if (strchr(s, 't')) f |= 0x200;
    if (strchr(s, 'x')) f |= O_EXCL;
    if (strchr(s, 'e')) f |= O_EXEC;
    return f;
}
static uint32_t fnv(const unsigned char *b, long n)
{
    uint32_t h = 2166136261u;
    for (long i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}
static char *lastp[2];                           /* the last request's path buffers (FS_LINKED's TEXT) */
static int plinked(const char *what, long r)
{
    if (!fsl_is(r)) return 0;
    int k = (-r - FS_LINKED) & FSL_A1;
    printf("%s -> %ld a%d %s\n", what, r, k, lastp[k] ? lastp[k] : "?");
    return 1;
}
static void pstat(const char *what, long r, struct ustat_ *st)
{
    if (plinked(what, r)) return;
    if (r < 0) printf("%s -> %ld\n", what, r);
    else printf("%s -> %ld ino=%u mode=%o size=%u uid=%u gid=%u links=%u\n", what, r, st->ino, st->mode, st->size,
                st->uid, st->gid, st->links);
}
static int cmpstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static char *bgline;
static void do_cmd(char *line, int bg, int cancelled);

int main(int argc, char **argv)
{
    struct nfs_conf c;
    memset(&c, 0, sizeof c);
    c.dev = "nfs0"; c.export_path = "/export"; c.seed = (uint32_t)time(0) ^ (uint32_t)getpid();
    static unsigned char saved[600];
    static uint32_t opens[64];
    int opt;
    while ((opt = getopt(argc, argv, "h:e:c:LP:N:s:S:O:m:t:")) != -1) {
        switch (opt) {
        case 'h': host = optarg; break;
        case 'e': c.export_path = optarg; break;
        case 'c': c.codes = optarg; break;
        case 'L': c.large = 1; break;
        case 'P': pmap_port = atoi(optarg); break;
        case 'N': nfs_port = atoi(optarg); break;
        case 's': src_port = atoi(optarg); break;
        case 'S': {
            savefile = optarg;
            FILE *f = fopen(optarg, "rb");
            if (f) { c.nsaved = (uint32_t)fread(saved, 1, sizeof saved, f); c.saved = saved; fclose(f); }
            break;
        }
        case 'O': {
            char *s = optarg;
            while (*s && c.nopens < 64) { opens[c.nopens++] = (uint32_t)strtoul(s, &s, 10); if (*s == ',') s++; }
            c.opens = opens;
            break;
        }
        case 'm': memcap = atol(optarg); break;
        case 't': cmd_timeout = atoi(optarg); break;
        default: fprintf(stderr, "usage: see the comment at the top\n"); return 2;
        }
    }
    int r = nfs_init(&c);
    if (r) { printf("init -> %d\n", r); return 1; }
    char line[4096];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\n")] = 0;
        if (!line[0] || line[0] == '#') continue;
        do_cmd(line, 0, 0);
        fflush(stdout);
    }
    /* drain: nothing may still be held (each request was answered) */
    struct nfs_stats s;
    nfs_get_stats(&s);
    nfs_end();
    if (memused) printf("FAIL the core left %ld bytes allocated\n", memused);
    printf("end -> peak heap %ld bytes\n", mempeak);
    (void)bgline;
    return 0;
}

static void do_cmd(char *line, int bg, int cancelled)
{
    char *v[8] = { 0 };
    char copy[4096];
    snprintf(copy, sizeof copy, "%s", line);
    int n = 0;
    for (char *t = strtok(copy, " "); t && n < 8; t = strtok(0, " ")) v[n++] = t;
    if (!n) return;
    const char *c = v[0];
    static struct ustat_ st;
    static char pathbuf[3][4096];
    if (!strcmp(c, "bg") || !strcmp(c, "cancelled")) {
        char *rest = line + strlen(c);
        while (*rest == ' ') rest++;
        do_cmd(rest, !strcmp(c, "bg"), !strcmp(c, "cancelled"));
        return;
    }
    /* requests whose arguments must outlive the command (bg): kept in static buffers per id */
    static struct { char p[2][256]; struct ustat_ st; unsigned char buf[65536]; struct udirent_ de; uint32_t ck; struct fsinfo fi; struct ustatfs sf; } bgs[16];
    static int bgi;
    void *keep = 0;
    if (bg) { keep = &bgs[bgi]; bgi = (bgi + 1) % 16; }
#define P(k) (bg ? ((typeof(bgs[0]) *)keep)->p[k] : pathbuf[k])
#define ST (bg ? &((typeof(bgs[0]) *)keep)->st : &st)
#define BUF (bg ? ((typeof(bgs[0]) *)keep)->buf : bigbuf)
    static unsigned char bigbuf[1 << 20];
    uint32_t id = 0;
    long r = 0;
    int handled = 1;
    if (v[1]) snprintf(P(0), 256, "%s", v[1]);
    if (v[2]) snprintf(P(1), 256, "%s", v[2]);
    lastp[0] = P(0); lastp[1] = P(1);
    if (!strcmp(c, "mount")) {
        static struct fsinfo fi;
        id = send_req(FS_ROOT, (uintptr_t)(bg ? &((typeof(bgs[0]) *)keep)->fi : &fi), 0, 0, 0, cancelled);
        if (!bg) { r = wait_for(id); printf("mount -> %ld dev=%.16s type=%.8s\n", r, fi.dev, fi.type); return; }
    } else if (!strcmp(c, "open") && n >= 3) {
        id = send_req(FS_OPEN, (uintptr_t)P(0), 2, flags_of(v[2]), (uintptr_t)ST, cancelled);
        if (!bg) { r = wait_for(id); pstat(line, r, &st); return; }
    } else if (!strcmp(c, "close") && n >= 2) {
        id = send_req(FS_CLOSE, strtoul(v[1], 0, 10), 0, 0, 0, cancelled);
    } else if ((!strcmp(c, "stat") || !strcmp(c, "lstat")) && n >= 2) {
        id = send_req(c[0] == 's' ? FS_STAT : FS_LSTAT, (uintptr_t)P(0), 2, (uintptr_t)ST, 0, cancelled);
        if (!bg) { r = wait_for(id); pstat(line, r, &st); return; }
    } else if (!strcmp(c, "fstat") && n >= 2) {
        id = send_req(FS_FSTAT, strtoul(v[1], 0, 10), (uintptr_t)ST, 0, 0, cancelled);
        if (!bg) { r = wait_for(id); pstat(line, r, &st); return; }
    } else if (!strcmp(c, "chdir") && n >= 2) {
        id = send_req(FS_CHDIR, (uintptr_t)P(0), 2, 0, 0, cancelled);
    } else if (!strcmp(c, "readlink") && n >= 2) {
        static char lb[512];
        memset(lb, 0, sizeof lb);
        id = send_req(FS_READLINK, (uintptr_t)P(0), 2, (uintptr_t)lb, 255, cancelled);
        if (!bg) { r = wait_for(id); if (plinked(line, r)) return; if (r >= 0) printf("%s -> %ld %.*s\n", line, r, (int)r, lb); else printf("%s -> %ld\n", line, r); return; }
    } else if (!strcmp(c, "read") && n >= 4) {
        long len = atol(v[3]);
        if (len > (long)sizeof bigbuf) len = sizeof bigbuf;
        id = send_req(FS_READ, strtoul(v[1], 0, 10), strtoul(v[2], 0, 10), (uintptr_t)BUF, (uintptr_t)len, cancelled);
        if (!bg) {
            r = wait_for(id);
            if (r >= 0 && r <= 64) printf("%s -> %ld hash=%08x text=%.*s\n", line, r, fnv(bigbuf, r), (int)r, bigbuf);
            else if (r >= 0) printf("%s -> %ld hash=%08x\n", line, r, fnv(bigbuf, r));
            else printf("%s -> %ld\n", line, r);
            return;
        }
    } else if (!strcmp(c, "write") && n >= 4) {
        const char *text = line;
        for (int k = 0; k < 3; k++) { text = strchr(text, ' '); text++; }
        unsigned char *b = BUF;
        long len;
        if (text[0] == '@') { len = atol(text + 1); const char *ch = strchr(text, ':'); if (len > 65536) len = 65536; memset(b, ch ? ch[1] : 'x', (size_t)len); }
        else { len = (long)strlen(text); memcpy(b, text, (size_t)len); }
        id = send_req(FS_WRITE, strtoul(v[1], 0, 10), strtoul(v[2], 0, 10), (uintptr_t)b, (uintptr_t)len, cancelled);
    } else if (!strcmp(c, "get") && n >= 3) {
        long chunk = n >= 4 ? atol(v[3]) : 4096;
        r = call(FS_OPEN, (uintptr_t)P(0), 2, 0, (uintptr_t)&st);
        if (r < 0) { printf("%s -> %ld\n", line, r); return; }
        uint32_t ino = (uint32_t)r, off = 0;
        FILE *f = fopen(v[2], "wb");
        for (;;) {
            r = call(FS_READ, ino, off, (uintptr_t)bigbuf, (uintptr_t)chunk);
            if (r <= 0) break;
            fwrite(bigbuf, 1, (size_t)r, f);
            off += (uint32_t)r;
        }
        if (f) fclose(f);
        call(FS_CLOSE, ino, 0, 0, 0);
        printf("%s -> %ld bytes=%u\n", line, r, off);
        return;
    } else if (!strcmp(c, "put") && n >= 3) {
        long chunk = n >= 4 ? atol(v[3]) : 4096;
        FILE *f = fopen(v[1], "rb");
        if (!f) { printf("%s -> no such PC file\n", line); return; }
        r = call(FS_OPEN, (uintptr_t)P(1), 2, 1 | 0x40 | 0x200, (uintptr_t)&st);
        if (r < 0) { printf("%s -> %ld\n", line, r); fclose(f); return; }
        uint32_t ino = (uint32_t)r, off = 0;
        long k;
        r = 0;
        while ((k = (long)fread(bigbuf, 1, (size_t)chunk, f)) > 0) {
            r = call(FS_WRITE, ino, off, (uintptr_t)bigbuf, (uintptr_t)k);
            if (r != k) break;
            off += (uint32_t)k;
        }
        fclose(f);
        call(FS_CLOSE, ino, 0, 0, 0);
        printf("%s -> %ld bytes=%u\n", line, r < 0 ? r : 0, off);
        return;
    } else if (!strcmp(c, "ls") && n >= 2) {
        r = call(FS_OPEN, (uintptr_t)P(0), 2, 0, (uintptr_t)&st);
        if (r < 0) { printf("%s -> %ld\n", line, r); return; }
        uint32_t ino = (uint32_t)r, ck = 0, cnt = 0;
        static char *names[4096];
        struct udirent_ de;
        for (;;) {
            uint32_t nc = 0;
            r = call(FS_READDIR, ino, ck, (uintptr_t)&de, (uintptr_t)&nc);
            if (r <= 0) break;
            if (cnt < 4096) { char b[100]; snprintf(b, sizeof b, "%s %u", de.name, de.ino); names[cnt++] = strdup(b); }
            ck = nc;
        }
        call(FS_CLOSE, ino, 0, 0, 0);
        qsort(names, cnt, sizeof names[0], cmpstr);
        for (uint32_t k = 0; k < cnt; k++) { printf("  %s\n", names[k]); free(names[k]); }
        printf("%s -> %ld entries=%u\n", line, r, cnt);
        return;
    } else if (!strcmp(c, "readdir") && n >= 3) {   /* one FS_READDIR at a cookie: "name ino next" */
        static struct udirent_ de; static uint32_t nc;
        r = call(FS_READDIR, strtoul(v[1], 0, 10), strtoul(v[2], 0, 10), (uintptr_t)&de, (uintptr_t)&nc);
        if (r > 0) printf("%s -> %ld %s %u next=%u\n", line, r, de.name, de.ino, nc); else printf("%s -> %ld next=%u\n", line, r, nc);
        return;
    } else if (!strcmp(c, "mkdir") && n >= 2) id = send_req(FS_MKDIR, (uintptr_t)P(0), 2, 0, 0, cancelled);
    else if (!strcmp(c, "unlink") && n >= 2) id = send_req(FS_UNLINK, (uintptr_t)P(0), 2, 0, 0, cancelled);
    else if (!strcmp(c, "rmdir") && n >= 2) id = send_req(FS_RMDIR, (uintptr_t)P(0), 2, 0, 0, cancelled);
    else if (!strcmp(c, "rename") && n >= 3) id = send_req(FS_RENAME, (uintptr_t)P(0), (uintptr_t)P(1), 2, 0, cancelled);
    else if (!strcmp(c, "link") && n >= 3) id = send_req(FS_LINK, (uintptr_t)P(0), (uintptr_t)P(1), 2, 0, cancelled);
    else if (!strcmp(c, "symlink") && n >= 3) id = send_req(FS_SYMLINK, (uintptr_t)P(0), (uintptr_t)P(1), 2, 0, cancelled);
    else if (!strcmp(c, "chmod") && n >= 3) id = send_req(FS_CHMOD, (uintptr_t)P(0), 2, strtoul(v[2], 0, 8), 0, cancelled);
    else if (!strcmp(c, "chown") && n >= 4) id = send_req(FS_CHOWN, (uintptr_t)P(0), 2, strtoul(v[2], 0, 10), strtoul(v[3], 0, 10), cancelled);
    else if (!strcmp(c, "utimes") && n >= 3) id = send_req(FS_UTIMES, (uintptr_t)P(0), 2, !strcmp(v[2], "now") ? FS_UTIME_NOW : strtoul(v[2], 0, 10), 0, cancelled);
    else if (!strcmp(c, "statfs")) {
        static struct ustatfs sf;
        r = call(FS_STATFS, (uintptr_t)&sf, 0, 0, 0);
        printf("statfs -> %ld bsize=%u blocks=%u bfree=%u files=%u ffree=%u\n", r, sf.bsize, sf.blocks, sf.bfree, sf.files, sf.ffree);
        return;
    } else if (!strcmp(c, "sync")) id = send_req(FS_SYNC, n >= 2 ? strtoul(v[1], 0, 10) : 0, 0, 0, 0, cancelled);
    else if (!strcmp(c, "quit")) id = send_req(FS_QUIT, 0, 0, 0, 0, cancelled);
    else if (!strcmp(c, "mark")) id = send_req(FS_MARK, 1, 0, 0, 0, cancelled);
    else if (!strcmp(c, "type") && n >= 2) id = send_req(strtoul(v[1], 0, 10), 0, 0, 0, 0, cancelled);
    else handled = 0;
    if (handled) {
        if (bg) { printf("%s -> id %u\n", line, id); return; }
        r = wait_for(id);
        if (!plinked(line, r)) printf("%s -> %ld\n", line, r);
        return;
    }
    if (!strcmp(c, "wait") && n >= 2) {
        uint32_t w = (uint32_t)atoi(v[1]);
        if (slot_of(w) < 0) { printf("%s -> no such request\n", line); return; }
        r = wait_for(w);
        printf("%s -> %ld\n", line, r);
    } else if (!strcmp(c, "cancel") && n >= 2) {
        nfs_cancel((uint32_t)atoi(v[1]));
        int i = slot_of((uint32_t)atoi(v[1]));
        printf("%s -> %s\n", line, i >= 0 && ans[i].done ? "answered" : "held");
    } else if (!strcmp(c, "sleep") && n >= 2) {
        uint32_t start = nfs_now(), ms = (uint32_t)atoi(v[1]);
        while (nfs_now() - start < ms) turn((int)(ms - (nfs_now() - start)));
        printf("%s -> ok\n", line);
    } else if (!strcmp(c, "as") && n >= 3) {
        cuid = (uint16_t)atoi(v[1]); cgid = (uint16_t)atoi(v[2]); ngroups = 0;
        for (int k = 3; k < n && ngroups < NFS_NGROUPS; k++) groups[ngroups++] = (uint32_t)atoi(v[k]);
        printf("%s -> ok\n", line);
    } else if (!strcmp(c, "umask") && n >= 2) { cumask = (uint16_t)strtoul(v[1], 0, 8); printf("%s -> ok\n", line); }
    else if (!strcmp(c, "raw") && n >= 2) { raw = !strcmp(v[1], "on"); printf("%s -> ok\n", line); }
    else if (!strcmp(c, "links") && n >= 2) { links = !strcmp(v[1], "on"); printf("%s -> ok\n", line); }
    else if (!strcmp(c, "kill")) { nfs_kill(); printf("kill -> ok\n"); }
    else if (!strcmp(c, "poll")) { uint32_t t; int b = nfs_poll(&t); printf("poll -> %d\n", b); }
    else if (!strcmp(c, "stats")) {
        struct nfs_stats s;
        nfs_get_stats(&s);
        printf("stats -> calls=%u replies=%u resends=%u reconnects=%u timeouts=%u unsaved=%u late=%u ops=%u nodes=%u mem=%u heap=%ld\n",
               s.calls, s.replies, s.resends, s.reconnects, s.timeouts, s.unsaved, s.dropped_late, s.ops, s.nodes, s.mem, memused);
    } else printf("%s -> unknown command\n", line);
}
