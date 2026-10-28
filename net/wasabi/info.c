/* SPDX-License-Identifier: GPL-2.0-only */
/* wasabid's machine views (DESIGN D24 4, 5.4, 5.6): INFO, HEALTH and DEBUG, the kernel log as a stream. */
#include "wasabi_int.h"

#define LOGCHUNK 1024
#define REPLAYMAX 2048

/* every line of the current boot's log, from its oldest character, to fn */
static uint32_t log_walk(void (*fn)(const char *line, uint32_t n))
{
    static char chunk[256];
    char line[160];
    uint32_t ln = 0;
    uint32_t off = (uint32_t)wsb_os_klog(WSB_KLOG_OLDEST, chunk, 1);
    if (off >= WSB_KLOG_OLDEST)
        off = 0;
    for (;;) {
        int n = wsb_os_klog(off, chunk, sizeof chunk);
        if (n <= 0)
            break;
        off += (uint32_t)n;
        for (int i = 0; fn && i < n; i++) {
            if (chunk[i] == '\n') {
                line[ln] = 0;
                fn(line, ln);
                ln = 0;
            } else if (ln + 1 < sizeof line)
                line[ln++] = chunk[i];
        }
    }
    return off;
}

static int replaying;
static void replay_add(const char *s, uint32_t n)
{
    if (!g.replay && !(g.replay = wsb_os_alloc(REPLAYMAX)))
        return;
    if (g.nreplay + n + 1 >= REPLAYMAX)
        return;
    memcpy(g.replay + g.nreplay, s, n);
    g.nreplay += n;
    g.replay[g.nreplay++] = '\n';
}
static void scan_line(const char *l, uint32_t n)
{
    if (!g.cpu[0] && !strncmp(l, "cpu: ", 5)) {
        uint32_t k = n - 5 < sizeof g.cpu - 1 ? n - 5 : sizeof g.cpu - 1;
        memcpy(g.cpu, l + 5, k);
        g.cpu[k] = 0;
    }
    if (g.emu68 < 0 && !strncmp(l, "pi: ", 4))
        g.emu68 = !strncmp(l + 4, "Emu68", 5);
    if (replaying) {                            /* after a panic: the previous boot's last lines (the kernel's "> ") */
        replay_add(l, n);
        if (!strncmp(l, "=== end", 7))
            replaying = 0;
        return;
    }
    if (!g.nreplay && (!strncmp(l, "=== previous boot (", 19) || !strncmp(l, "klog: previous boot (", 21))) {
        replay_add(l, n);
        replaying = l[0] == '=' && strstr(l, "kernel panic") != 0;
        if (l[0] == '=' && !replaying)
            g.nreplay--, replay_add(" (its last lines: dmesg -p)", 27);
    }
}
void log_scan(void)
{
    replaying = 0;
    log_walk(scan_line);
    replaying = 0;
}

void farewell(const char *why)
{
    char t[80];
    struct wsb_buf b = { t, 0, sizeof t };
    uint8_t *d;
    struct conn *c;
    if (g.logconn < 0)
        return;
    c = &g.c[g.logconn];
    bs(&b, "[wasabi: ");
    bs(&b, why);
    bs(&b, " - closing this stream]\n");
    if ((d = qres(c, 10 + b.n))) {
        put32(d, 0);
        put32(d + 4, ++c->lseq);
        put16(d + 8, b.n);
        memcpy(d + 10, t, b.n);
        qcommit(c, WT_LOG, 10 + b.n);
    }
}

static int log_frame(struct conn *c, const char *s, uint32_t n)
{
    uint8_t *d = qres(c, 10 + n);
    if (!d)
        return -1;
    put32(d, 0);
    put32(d + 4, ++c->lseq);
    put16(d + 8, n);
    memcpy(d + 10, s, n);
    qcommit(c, WT_LOG, 10 + n);
    c->lhb = g.now;
    return 0;
}

void cmd_debug(struct conn *c)
{
    if (g.logconn >= 0 && &g.c[g.logconn] != c) {
        qerr(c, 0, 0, "the debug stream is already in use");
        return;
    }
    g.logconn = (int)(c - g.c);
    c->job = J_LOG;
    c->lseq = 0;
    c->lpos = log_walk(0);                      /* the stream starts at the log's end: what is written from now on */
    c->lreplay = g.nreplay ? 0 : -1;
    c->lhb = g.now;
    c->until = g.now;
}

void produce_log(struct conn *c)
{
    static char t[LOGCHUNK];
    while (c->lreplay >= 0) {                   /* the previous boot's ending first, a line a frame */
        uint32_t s = (uint32_t)c->lreplay, e = s;
        while (e < g.nreplay && g.replay[e] != '\n')
            e++;
        if (e < g.nreplay)
            e++;
        if (e == s) { c->lreplay = -1; break; }
        if (log_frame(c, g.replay + s, e - s))
            return;
        c->lreplay = e < g.nreplay ? (int)e : -1;
    }
    while (DUE(c->until)) {
        uint32_t room = qroom(c);
        if (room < 10 + 128)
            return;
        uint32_t oldest = (uint32_t)wsb_os_klog(WSB_KLOG_OLDEST, t, 1);
        if (oldest < WSB_KLOG_OLDEST && c->lpos < oldest) {     /* the ring wrapped past us: say how much */
            char m[80];
            struct wsb_buf b = { m, 0, sizeof m };
            bs(&b, "[wasabi: ");
            bu(&b, oldest - c->lpos);
            bs(&b, " bytes of the kernel log were lost]\n");
            if (log_frame(c, m, b.n))
                return;
            c->lpos = oldest;
            continue;
        }
        uint32_t want = room - 10 < sizeof t ? room - 10 : sizeof t, sent = 0;
        int n = wsb_os_klog(c->lpos, t, want);
        if (n <= 0) {
            c->until = g.now + WSB_LOG_MS;
            return;
        }
        for (uint32_t i = 0; i < (uint32_t)n; i++)
            if (t[i] == '\n') {
                if (log_frame(c, t + sent, i + 1 - sent)) {
                    c->lpos += sent;
                    return;
                }
                sent = i + 1;
            }
        if (!sent && (uint32_t)n == want) {     /* a line longer than the piece: as it is */
            if (log_frame(c, t, (uint32_t)n))
                return;
            sent = (uint32_t)n;
        }
        c->lpos += sent;
        if (sent < (uint32_t)n || (uint32_t)n < want)
            c->until = g.now + WSB_LOG_MS;      /* caught up, or a part line whose end comes later */
    }
}

void log_tick(uint32_t *tmo)
{
    struct conn *c;
    if (g.logconn < 0)
        return;
    c = &g.c[g.logconn];
    if (c->fd < 0 || c->job != J_LOG) {
        g.logconn = -1;
        return;
    }
    if ((uint32_t)(g.now - c->lhb) >= WSB_HB_MS && c->outn == c->outoff)
        log_frame(c, "", 0);                    /* a heartbeat, only into an empty queue (a slow peer: skipped) */
    TMO(tmo, LEFT(c->until));
    TMO(tmo, WSB_HB_MS - (g.now - c->lhb < WSB_HB_MS ? g.now - c->lhb : WSB_HB_MS));
}

static void kb(struct wsb_buf *b, uint32_t bytes) { bu(b, bytes >> 10); bs(b, " KB"); }

static int count_procs(int *ready)
{
    struct wsb_proc p;
    int n = 0;
    *ready = 0;
    for (unsigned i = 0;; i++) {
        int r = wsb_os_proc(i, &p);
        if (r < 0)
            break;
        if (!r) {
            n++;
            if (p.state == 1)
                (*ready)++;
        }
    }
    return n;
}

void cmd_info(struct conn *c)
{
    char *t = wsb_os_alloc(1024);
    struct wsb_buf b = { t, 0, 1024 };
    struct wsb_iface ifs[4];
    uint32_t chip = 0, fast = 0;
    if (!t) { qerr(c, -ENOMEM, 0, "out of memory"); return; }
    bs(&b, g.uname);
    bs(&b, "\nwasabid, protocol v1: port ");
    bu(&b, g.port);
    bs(&b, ", name ");
    bs(&b, g.conf.name);
    bs(&b, g.init ? ", under init" : g.trial ? ", a trial instance" : ", started by hand");
    bs(&b, ", run slots ");
    bu(&b, WSB_SLOTS);
    bs(&b, ", unencrypted\nallow: the home network's ranges");
    for (int i = 0; i < g.conf.nallow; i++) {
        uint32_t m = g.conf.allow_mask[i], bits = 0;
        while (m) { bits++; m <<= 1; }
        bs(&b, ", ");
        bip(&b, g.conf.allow_net[i]);
        bc(&b, '/');
        bu(&b, bits);
    }
    if (g.conf.allow_any)
        bs(&b, ", any (every address)");
    if (g.cpu[0]) { bs(&b, "\ncpu: "); bs(&b, g.cpu); }
    if (!wsb_os_meminfo(&chip, &fast)) {
        bs(&b, "\nchip free ");
        kb(&b, chip);
        bs(&b, ", fast free ");
        kb(&b, fast);
    }
    if (g.emu68 >= 0)
        bs(&b, g.emu68 ? "\nEmu68: yes" : "\nEmu68: no");
    bs(&b, "\nuptime: ");
    bu(&b, wsb_os_uptime());
    bs(&b, " s");
    int n = wsb_os_ifaces(ifs, 4);
    for (int i = 0; i < n; i++) {
        ifs[i].name[7] = 0;
        bs(&b, i ? "\n  " : "\ninterfaces:\n  ");
        bs(&b, ifs[i].name);
        bc(&b, ' ');
        bip(&b, ifs[i].addr);
        bs(&b, " rx ");
        bu(&b, ifs[i].rx);
        bs(&b, " tx ");
        bu(&b, ifs[i].tx);
        bs(&b, " bad ");
        bu(&b, ifs[i].bad);
    }
    bc(&b, '\n');
    qlines(c, t, b.n, 0);
}

void cmd_health(struct conn *c)
{
    char *t = wsb_os_alloc(1024);
    struct wsb_buf b = { t, 0, 1024 };
    struct wsb_iface ifs[4];
    struct wsb_statfs sf;
    uint32_t chip, fast;
    int ready, nfs, mi;
    if (!t) { qerr(c, -ENOMEM, 0, "out of memory"); return; }
    if (!wsb_os_meminfo(&chip, &fast)) {
        bs(&b, "mem.chip.free ");
        bu(&b, chip);
        bs(&b, "\nmem.fast.free ");
        bu(&b, fast);
        bc(&b, '\n');
    }
    if (g.emu68 >= 0)
        bs(&b, g.emu68 ? "emu68 yes\n" : "emu68 no\n");
    bs(&b, "zenix.version ");
    bs(&b, g.banner + (strncmp(g.banner, "Zenix ", 6) ? 0 : 6));
    bs(&b, "\nuptime ");
    bu(&b, wsb_os_uptime());
    bs(&b, "\nprocs ");
    bu(&b, (uint32_t)count_procs(&ready));
    bs(&b, "\nload.ready ");
    bu(&b, (uint32_t)ready);
    bc(&b, '\n');
    int n = wsb_os_ifaces(ifs, 4);
    for (int i = 0; i < n; i++) {
        static const char *const what[3] = { ".rx ", ".tx ", ".bad " };
        uint32_t v[3] = { ifs[i].rx, ifs[i].tx, ifs[i].bad };
        ifs[i].name[7] = 0;
        for (int k = 0; k < 3; k++) {
            bs(&b, "net.");
            bs(&b, ifs[i].name);
            bs(&b, what[k]);
            bu(&b, v[k]);
            bc(&b, '\n');
        }
    }
    if ((mi = mount_of("/", &nfs)) >= 0 && !wsb_os_statfs((unsigned)mi, &sf)) {
        bs(&b, "disk.root.free.kb ");
        bu(&b, sf.bsize >= 1024 ? sf.bfree * (sf.bsize >> 10) : sf.bfree / (1024 / (sf.bsize ? sf.bsize : 1024)));
        bc(&b, '\n');
    }
    qlines(c, t, b.n, 0);
}
