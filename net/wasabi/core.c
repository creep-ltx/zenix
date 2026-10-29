/* SPDX-License-Identifier: GPL-2.0-only */
/* wasabid's core (DESIGN D24 3, 5): the start, the loop, the connections and their send queues, HELLO, discovery, the
 * address filter, the wrong-key pause and the dispatcher. */
#include "wasabi_int.h"

struct wsb g;

static const char *const screen_caps[] = { "grab", "screen", "windows", "key", "sum", "inputs", "text", 0 };

void say(const char *a, const char *b)
{
    char t[200];
    struct wsb_buf w = { t, 0, sizeof t };
    bs(&w, "wasabid: ");
    bs(&w, a);
    if (b) { bc(&w, ' '); bs(&w, b); }
    wsb_os_log(t);
}

int fd_take(int n)
{
    if (g.nfd + n > WSB_NFD)
        return -EMFILE;
    g.nfd += n;
    return 0;
}
void fd_give(int n) { g.nfd -= n; }

/* ---- the send queue ---- */
uint32_t qroom(struct conn *c)
{
    uint32_t used = c->outn - c->outoff;
    return WSB_OUTCAP - used >= 5 ? WSB_OUTCAP - used - 5 : 0;
}
uint8_t *qres(struct conn *c, uint32_t n)
{
    if (c->fd < 0 || qroom(c) < n)
        return 0;
    if (c->outoff) {
        memmove(c->out, c->out + c->outoff, c->outn - c->outoff);
        c->outn -= c->outoff;
        c->outoff = 0;
    }
    return c->out + c->outn + 5;
}
void qcommit(struct conn *c, uint8_t tag, uint32_t n)
{
    c->out[c->outn] = tag;
    put32(c->out + c->outn + 1, n);
    c->outn += 5 + n;
}
int q(struct conn *c, uint8_t tag, const void *p, uint32_t n)
{
    uint8_t *d = qres(c, n);
    if (!d)
        return -1;
    if (n)
        memcpy(d, p, n);
    qcommit(c, tag, n);
    return 0;
}
int qok(struct conn *c) { return q(c, WT_OK, 0, 0); }
int qend(struct conn *c) { return q(c, WT_END, 0, 0); }
int qerr(struct conn *c, int e, int write, const char *words)
{
    uint8_t m[6 + 200];
    struct wsb_buf b = { (char *)m + 6, 0, 200 };
    bs(&b, words);
    if (e) {
        const char *t = wsb_os_strerror(e < 0 ? e : -e);
        bs(&b, ": ");
        if (t) bs(&b, t); else { bs(&b, "error "); bi(&b, e < 0 ? -e : e); }
    }
    put32(m, wsb_errcode(e, write));
    put16(m + 4, b.n);
    return q(c, WT_ERR, m, 6 + b.n);
}
int qlines(struct conn *c, char *buf, uint32_t n, const char *errwords)
{
    c->job = J_OUT;
    c->buf = buf;
    c->blen = n;
    c->bpos = 0;
    c->bend = errwords;
    return 0;
}

void conn_jobdone(struct conn *c)
{
    if (c->buf) { wsb_os_free(c->buf); c->buf = 0; }
    c->blen = c->bpos = 0;
    c->bend = 0;
    c->xflags = 0;
    c->job = J_NONE;
    c->last = g.now;                    /* a job that ends is progress: a long TEXT wait or INPUTS is not a stall (3.2) */
}

static void conn_close(struct conn *c)
{
    wsb_os_close(c->fd);
    fd_give(1);
    c->fd = -1;
    wsb_os_free(c->in);
    wsb_os_free(c->out);
    c->in = c->out = 0;
}

void conn_drop(int i)
{
    struct conn *c = &g.c[i];
    if (c->fd < 0)
        return;
    xfer_abort(c);
    screen_free(c);
    if (c->job == J_RUN || c->job == J_FREE)
        run_conn_gone(i);
    if (g.logconn == i)
        g.logconn = -1;
    if (g.rebootconn == i)
        g.rebootconn = -1;
    conn_jobdone(c);
    conn_close(c);
}

/* the answer of one J_OUT step: whole lines (or bytes, xflags 1) as DATA, then END or the ERR */
static void produce_out(struct conn *c)
{
    while (c->bpos < c->blen) {
        uint32_t room = qroom(c), k = c->blen - c->bpos;
        if (room < 256 && room < k)
            return;
        if (k > room)
            k = room;
        if (!(c->xflags & 1) && c->bpos + k < c->blen) {
            uint32_t e = k;
            while (e && c->buf[c->bpos + e - 1] != '\n')
                e--;
            if (e)
                k = e;
        }
        q(c, WT_DATA, c->buf + c->bpos, k);
        c->bpos += k;
    }
    if (c->bend ? qerr(c, 0, 0, c->bend) : qend(c))
        return;
    conn_jobdone(c);
}

/* ---- paths ---- */
int path_ok(const char *p)
{
    if (p[0] != '/')
        return -EINVAL;
    if (strlen(p) > WSB_PATHMAX)
        return -ENAMETOOLONG;
    return 0;
}

/* the mount a path lies on (resolved: links and .., or its directory's for a name not there yet): its index, or -1;
 * *nfs: an nfs mount */
int mount_of(const char *path, int *nfs)
{
    char r[WSB_PATHMAX + 1], d[WSB_PATHMAX + 1];
    struct wsb_mount m;
    int best = -1;
    uint32_t bestl = 0;
    *nfs = 0;
    if (wsb_os_realpath(path, r, sizeof r) < 0) {
        uint32_t n = (uint32_t)strlen(path);
        memcpy(d, path, n + 1);
        while (n > 1 && d[n - 1] != '/')
            n--;
        d[n > 1 ? n - 1 : 1] = 0;
        if (wsb_os_realpath(d, r, sizeof r) < 0)
            return -1;
    }
    for (unsigned i = 0; i < 64 && !wsb_os_mount(i, &m); i++) {
        uint32_t l;
        m.path[sizeof m.path - 1] = 0;
        m.type[sizeof m.type - 1] = 0;
        l = (uint32_t)strlen(m.path);
        if (l == 1 && m.path[0] == '/')
            l = 0;
        if (strncmp(r, m.path, l) || (r[l] != '/' && r[l]))
            continue;
        if (best < 0 || l >= bestl) {
            best = (int)i;
            bestl = l;
            *nfs = !strcmp(m.type, "nfs");
        }
    }
    return best;
}
int on_nfs(const char *path)
{
    int nfs;
    mount_of(path, &nfs);
    return nfs;
}

/* ---- the refused count (/var/lib/wasabid.refused) ---- */
static void refused_load(void)
{
    char t[16];
    int fd = wsb_os_open("/var/lib/wasabid.refused", WSB_O_READ, 0);
    long n;
    if (fd < 0)
        return;
    n = wsb_os_read(fd, t, sizeof t - 1);
    wsb_os_close(fd);
    g.refused = 0;
    for (long i = 0; i < n && t[i] >= '0' && t[i] <= '9'; i++)
        g.refused = g.refused * 10 + (uint32_t)(t[i] - '0');
    g.refused_saved = g.refused;
}
static void refused_save(void)
{
    char t[16];
    struct wsb_buf b = { t, 0, sizeof t };
    int fd;
    if (g.trial || g.refused == g.refused_saved || fd_take(1))
        return;
    bu(&b, g.refused);
    bc(&b, '\n');
    wsb_os_unlink("/var/lib/.wsb-refused");
    fd = wsb_os_open("/var/lib/.wsb-refused", WSB_O_WRITE, 0644);
    if (fd >= 0) {
        int ok = wsb_os_write(fd, t, b.n) == (long)b.n;
        wsb_os_close(fd);
        if (ok && !wsb_os_rename("/var/lib/.wsb-refused", "/var/lib/wasabid.refused"))
            g.refused_saved = g.refused;
        else
            wsb_os_unlink("/var/lib/.wsb-refused");
    }
    fd_give(1);
    g.refused_at = g.now;
}
static void refuse(void)
{
    g.refused++;
}

/* ---- the start ---- */
static int conf_load(void)
{
    struct wsb_stat st;
    char *t;
    long n;
    int fd = wsb_os_open("/etc/wasabid.conf", WSB_O_READ, 0), line;
    const char *why;
    if (fd < 0) {
        say("/etc/wasabid.conf:", fd == -ENOENT ? "missing (wasabid's page says how to write one): stopped" :
                                                  "cannot be read: stopped");
        return -1;
    }
    if (wsb_os_fstat(fd, &st) || (st.mode & 0170000) != 0100000 || st.uid != 0 || (st.mode & 077)) {
        wsb_os_close(fd);
        say("/etc/wasabid.conf:", "must be root's regular file with mode 600: stopped");
        return -1;
    }
    t = wsb_os_alloc(2049);
    if (!t) {
        wsb_os_close(fd);
        say("out of memory", 0);
        return -1;
    }
    n = wsb_os_read(fd, t, 2049);
    wsb_os_close(fd);
    if (n < 0 || n > 2048) {
        wsb_os_free(t);
        say("/etc/wasabid.conf:", n < 0 ? "cannot be read: stopped" : "longer than 2 KB: stopped");
        return -1;
    }
    int r = wsb_conf_parse(&g.conf, t, (uint32_t)n, &line, &why);
    wsb_os_free(t);
    if (r) {
        char m[120];
        struct wsb_buf b = { m, 0, sizeof m };
        if (line) { bs(&b, "line "); bu(&b, (uint32_t)line); bs(&b, ": "); }
        bs(&b, why);
        bs(&b, ": stopped");
        say("/etc/wasabid.conf:", m);
        return -1;
    }
    return 0;
}

static void make_caps(void)
{
    const char *s = WSB_CAPS;
    uint32_t n = 0;
    while (*s) {
        const char *e = s;
        int skip = 0;
        while (*e && *e != ',')
            e++;
        if (!g.conops)
            for (int i = 0; screen_caps[i]; i++)
                if ((uint32_t)(e - s) == strlen(screen_caps[i]) && !strncmp(s, screen_caps[i], (uint32_t)(e - s)))
                    skip = 1;
        if (!skip) {
            if (n)
                g.caps[n++] = ',';
            memcpy(g.caps + n, s, (uint32_t)(e - s));
            n += (uint32_t)(e - s);
        }
        s = *e ? e + 1 : e;
    }
    g.caps[n] = 0;
}

static int open_listener(void)
{
    int fd;
    if (fd_take(1))
        return -EMFILE;
    fd = wsb_os_listen(g.port);
    if (fd < 0) {
        fd_give(1);
        return fd;
    }
    g.lfd = fd;
    g.netretry = 0;
    return 0;
}

/* the discovery sockets (3.1): with the listener, never for a trial instance; a port that is taken is said once */
static void open_disc(void)
{
    unsigned ports[2] = { WSB_DISC_A, g.port };
    if (g.trial)
        return;
    for (int i = 0; i < 2; i++) {
        if ((i && ports[1] == ports[0]) || g.ufd[i] >= 0)
            continue;
        if (fd_take(1))
            break;
        g.ufd[i] = wsb_os_udp(ports[i]);
        if (g.ufd[i] < 0) {
            fd_give(1);
            g.ufd[i] = -1;
            say("no discovery on a UDP port (in use?)", 0);
        }
    }
}

/* the network server went away (netd killed or crashed: every socket of its epoch is dead, D19 rule 9): the listener
 * and the discovery sockets are closed and opened again at once, which starts netd again (D11), then every 10 s
 * (5.7). Without this a dead listener would wake every poll. The connections find out by themselves (their recv). */
static void net_lost(void)
{
    if (g.lfd >= 0) { wsb_os_close(g.lfd); fd_give(1); g.lfd = -1; }
    for (int i = 0; i < 2; i++)
        if (g.ufd[i] >= 0) { wsb_os_close(g.ufd[i]); fd_give(1); g.ufd[i] = -1; }
    g.netretry = g.now | 1;
    say("the network server went away: listening again when it is back", 0);
}

int wsb_init(int argc, char **argv)
{
    int r;
    memset(&g, 0, sizeof g);
    g.lfd = g.ufd[0] = g.ufd[1] = g.nullfd = -1;
    g.logconn = g.rebootconn = -1;
    g.rebootout = -1;
    g.quit = -1;
    g.emu68 = -1;
    g.nfd = 1;                                  /* the log (standard error) */
    for (int i = 0; i < WSB_CONNS; i++)
        g.c[i].fd = -1;
    g.now = wsb_os_now();
    unsigned argport = 0;
    for (int i = 1; i < argc; i++) {
        uint32_t v = 0;
        int ok = 1;
        if (!strcmp(argv[i], "--init"))
            g.init = 1;
        else if (!strcmp(argv[i], "--trial"))
            g.trial = 1;
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) {
            const char *s = argv[++i];
            if (!*s) ok = 0;
            for (; *s && ok; s++)
                if (*s < '0' || *s > '9' || (v = v * 10 + (uint32_t)(*s - '0')) > 65535)
                    ok = 0;
            if (!v) ok = 0;
            argport = v;
        } else
            ok = 0;
        if (!ok) {
            say("usage: wasabid [--init] [--trial] [--port N]; not understood:", argv[i]);
            return WSB_ST_STOP;
        }
    }
    if (g.init && g.trial) {
        say("--init and --trial do not go together", 0);
        return WSB_ST_STOP;
    }
    if (conf_load())
        return WSB_ST_STOP;
    g.port = argport ? argport : g.conf.port;
    g.self = wsb_os_self();
    g.conops = wsb_os_con_ops();
    make_caps();
    if (wsb_os_uname(g.uname, sizeof g.uname) < 0)
        memcpy(g.uname, "Zenix", 6);
    {
        struct wsb_buf b = { g.banner, 0, sizeof g.banner };
        const char *v = g.uname;
        if (!strncmp(v, "Zenix ", 6)) {
            v += 6;
            bs(&b, "Zenix ");
            while (*v && *v != ' ')
                bc(&b, *v++);
        } else
            bs(&b, v);
    }
    refused_load();
    log_scan();
    if (fd_take(1))
        return WSB_ST_FAIL;
    g.nullfd = wsb_os_open("/dev/null", WSB_O_READ, 0);
    if (g.nullfd < 0) {
        say("cannot open /dev/null: stopped", 0);
        return WSB_ST_FAIL;
    }
    r = open_listener();
    if (r == -ENETDOWN) {
        g.netretry = g.now + WSB_NETWAIT_MS;
        say("the network is down: trying again every 10 s", 0);
    } else if (r < 0) {
        char m[24];
        struct wsb_buf b = { m, 0, sizeof m };
        bu(&b, g.port);
        say("cannot listen on TCP port", m);
        return WSB_ST_FAIL;
    }
    if (!r)
        open_disc();
    {
        char m[120];
        struct wsb_buf b = { m, 0, sizeof m };
        bs(&b, "port ");
        bu(&b, g.port);
        bs(&b, ", name ");
        bs(&b, g.conf.name);
        bs(&b, g.init ? ", under init" : g.trial ? ", a trial instance" : ", started by hand");
        say("started:", m);
        if (g.conf.allow_any)
            say("WARNING: allow any - every address may connect, and whoever has the key runs commands as root", 0);
    }
    return WSB_GOON;
}

/* ---- the end ---- */
static int finish(void)
{
    for (int i = 0; i < WSB_CONNS; i++)
        conn_drop(i);
    run_stopall();
    if (g.lfd >= 0) { wsb_os_close(g.lfd); fd_give(1); g.lfd = -1; }
    for (int i = 0; i < 2; i++)
        if (g.ufd[i] >= 0) { wsb_os_close(g.ufd[i]); fd_give(1); g.ufd[i] = -1; }
    if (g.nullfd >= 0) { wsb_os_close(g.nullfd); fd_give(1); g.nullfd = -1; }
    refused_save();
    say(g.quit == WSB_ST_RESTART ? "restarting" : "stopped", 0);
    return g.quit;
}
void wsb_end(void)
{
    if (g.nullfd >= 0 || g.lfd >= 0) {
        if (g.quit < 0)
            g.quit = WSB_ST_STOP;
        finish();
    }
    if (g.replay) { wsb_os_free(g.replay); g.replay = 0; }
}

/* ---- HELLO ---- */
static int badkey_at(uint32_t a)
{
    for (int i = 0; i < g.nbad; i++)
        if (g.badaddr[i] == a)
            return i;
    return -1;
}
static void hello(int i, const uint8_t *p, uint32_t len)
{
    struct conn *c = &g.c[i];
    char key[WSB_KEYMAX + 2];
    uint32_t off = 0, v;
    if (wsb_get16(p, len, &off, &v) || v != WSB_PROTO) {
        qerr(c, 0, 0, "protocol version mismatch");
        c->closing = 1;
        return;
    }
    if (wsb_getstr(p, len, &off, key, sizeof key)) {
        qerr(c, 0, 0, "malformed HELLO");
        c->closing = 1;
        return;
    }
    uint32_t kl = (uint32_t)strlen(key), cl = (uint32_t)strlen(g.conf.key), diff = kl ^ cl;
    for (uint32_t j = 0; j <= WSB_KEYMAX; j++)  /* the whole length every time: no early end to time */
        diff |= (uint32_t)((j < kl ? (uint8_t)key[j] : 0) ^ (j < cl ? (uint8_t)g.conf.key[j] : 0));
    int b = badkey_at(c->peer);
    if (diff) {
        qerr(c, 0, 0, "bad key");
        c->closing = 1;
        refuse();
        if (b < 0) {
            if (g.nbad < WSB_BADKEY)
                g.badaddr[g.nbad++] = c->peer;
            else {
                memmove(g.badaddr, g.badaddr + 1, (WSB_BADKEY - 1) * sizeof g.badaddr[0]);
                g.badaddr[WSB_BADKEY - 1] = c->peer;
            }
        }
        return;
    }
    if (b >= 0) {
        g.badaddr[b] = g.badaddr[--g.nbad];
    }
    uint8_t w[2 + 2 + sizeof g.banner + 2 + sizeof g.caps + 4];
    uint32_t n = 0, l = (uint32_t)strlen(g.banner);
    put16(w, WSB_PROTO); n = 2;
    put16(w + n, l); memcpy(w + n + 2, g.banner, l); n += 2 + l;
    l = (uint32_t)strlen(g.caps);
    put16(w + n, l); memcpy(w + n + 2, g.caps, l); n += 2 + l;
    put32(w + n, g.refused); n += 4;
    q(c, WT_WELCOME, w, n);
    c->hello = 1;
}

/* ---- the dispatcher ---- */
static int known(uint8_t t)
{
    switch (t) {
    case WT_PING: case WT_INFO: case WT_LS: case WT_PUT: case WT_GET: case WT_DEL: case WT_MKDIR: case WT_RUN:
    case WT_DEBUG: case WT_REBOOT: case WT_RESTART: case WT_PS: case WT_KILL: case WT_SPEED: case WT_QUIT:
    case WT_GRAB: case WT_SCREEN: case WT_KEY: case WT_WINDOWS: case WT_HEALTH: case WT_SLOTS: case WT_FREE:
    case WT_SUM: case WT_INPUTS: case WT_TEXT:
        return 1;
    }
    return 0;
}
static int screen_tag(uint8_t t)
{
    return t == WT_GRAB || t == WT_SCREEN || t == WT_KEY || t == WT_WINDOWS || t == WT_SUM || t == WT_INPUTS ||
           t == WT_TEXT;
}

static void dispatch(int i, uint8_t tag, const uint8_t *p, uint32_t len)
{
    struct conn *c = &g.c[i];
    if (!c->hello) {
        if (tag != WT_HELLO) {
            qerr(c, 0, 0, "expected HELLO");
            c->closing = 1;
            return;
        }
        if (badkey_at(c->peer) >= 0) {          /* the wrong-key pause: served in a second */
            c->held = 1;
            c->holdto = g.now + WSB_PAUSE_MS;
            c->blen = len;
            return;
        }
        hello(i, p, len);
        return;
    }
    if (c->job != J_NONE) {
        /* one command at a time; beside a log stream only what answers at once (a SNOOP: not on Zenix) */
        if (c->job == J_LOG && tag != WT_DEBUG && (tag == WT_PING || !known(tag)))
            ;
        else {
            conn_drop(i);
            return;
        }
    }
    if (!known(tag)) {
        if (qerr(c, 0, 0, "not on Zenix"))
            conn_drop(i);
        return;
    }
    if (screen_tag(tag) && !g.conops) {
        qerr(c, 0, 0, "not in this Zenix yet: its kernel has no console operations for wasabid (D24 step 3)");
        return;
    }
    switch (tag) {
    case WT_PING: if (q(c, WT_PONG, 0, 0)) conn_drop(i); return;
    case WT_INFO: cmd_info(c); return;
    case WT_HEALTH: cmd_health(c); return;
    case WT_LS: cmd_ls(c, p, len); return;
    case WT_GET: cmd_get(c, p, len); return;
    case WT_PUT: cmd_put(c, p, len); return;
    case WT_DEL: cmd_del(c, p, len, 0); return;
    case WT_MKDIR: cmd_del(c, p, len, 1); return;
    case WT_SPEED: cmd_speed(c, p, len); return;
    case WT_RUN: cmd_run(c, p, len); return;
    case WT_SLOTS: cmd_slots(c); return;
    case WT_FREE: cmd_free(c, p, len); return;
    case WT_PS: cmd_ps(c); return;
    case WT_KILL: cmd_kill(c, p, len); return;
    case WT_DEBUG: cmd_debug(c); return;
    case WT_REBOOT: cmd_reboot(c, p, len); return;
    case WT_RESTART: cmd_exit(c, p, len, 1); return;
    case WT_QUIT: cmd_exit(c, p, len, 0); return;
    case WT_TEXT: cmd_text(c, p, len); return;
    case WT_GRAB: cmd_grab(c, p, len); return;
    case WT_SUM: cmd_sum(c, p, len); return;
    case WT_SCREEN: cmd_screen(c, p, len); return;
    case WT_WINDOWS: cmd_windows(c); return;
    case WT_KEY: cmd_key(c, p, len); return;
    case WT_INPUTS: cmd_inputs(c, p, len); return;
    }
}

/* bytes from a connection: frames, or a body's pieces */
static void input(int i, const uint8_t *b, uint32_t n)
{
    struct conn *c = &g.c[i];
    while (n && c->fd >= 0 && !c->closing && !c->held) {
        uint32_t used, len = 0;
        uint8_t tag = 0;
        const uint8_t *piece = 0;
        int last = 0, r = wsb_rx_feed(&c->rx, b, n, &used, &tag, &piece, &len, &last);
        b += used;
        n -= used;
        if (r == WSB_RX_TLS) {                  /* a TLS ClientHello: the client's sign to go plain */
            conn_drop(i);
            return;
        }
        if (r == WSB_RX_BIG) {
            qerr(c, 0, 0, "a frame longer than this daemon takes");
            c->closing = 1;
            return;
        }
        if (r == WSB_RX_PIECE) {
            body_piece(c, piece, len, last);
            continue;
        }
        if (r != WSB_RX_FRAME)
            continue;
        if (c->job == J_PUT || c->job == J_SPEEDIN) {
            if (tag == WT_END)
                body_end(c);
            else if (tag == WT_DATA)
                body_piece(c, piece, len, 1);   /* an empty DATA */
            else
                conn_drop(i);                   /* a command inside a body (Wasabi's rule) */
            continue;
        }
        dispatch(i, tag, piece, len);
        if (c->fd >= 0 && c->held && n) {       /* a client must wait for WELCOME */
            conn_drop(i);
            return;
        }
    }
}

static void accept_one(void)
{
    uint32_t peer = 0;
    int fd, i;
    if (fd_take(1))
        return;
    fd = wsb_os_accept(g.lfd, &peer);
    if (fd < 0) {
        fd_give(1);
        if (fd == -ENETDOWN)
            net_lost();
        return;
    }
    if (!wsb_allowed(&g.conf, peer)) {          /* before HELLO, and no word to it */
        refuse();
        wsb_os_close(fd);
        fd_give(1);
        return;
    }
    for (i = 0; i < WSB_CONNS && g.c[i].fd >= 0; i++)
        ;
    if (i == WSB_CONNS || g.quit >= 0 || g.rebootpend || g.rebootpid) {      /* (a reboot under way: nobody new, step 1) */
        static const uint8_t busy[] = { WT_ERR, 0, 0, 0, 10, 0, 0, 0, 0, 0, 4, 'b', 'u', 's', 'y' };
        wsb_os_send(fd, busy, sizeof busy);
        wsb_os_close(fd);
        fd_give(1);
        return;
    }
    struct conn *c = &g.c[i];
    memset(c, 0, sizeof *c);
    c->in = wsb_os_alloc(WSB_SMALL);
    c->out = wsb_os_alloc(WSB_OUTCAP);
    if (!c->in || !c->out) {
        wsb_os_free(c->in);
        wsb_os_free(c->out);
        c->in = c->out = 0;
        c->fd = -1;
        wsb_os_close(fd);
        fd_give(1);
        say("out of memory for a connection", 0);
        return;
    }
    c->fd = fd;
    c->peer = peer;
    c->xfd = -1;
    c->slot = -1;
    c->last = c->born = g.now;
    wsb_rx_init(&c->rx, c->in);
}

static void probe(int s)
{
    char b[64], r[200];
    uint32_t a;
    unsigned port;
    long n = wsb_os_recvfrom(s, b, sizeof b, &a, &port);
    if (n < 8 || memcmp(b, "WASABI?1", 8))
        return;
    if (!wsb_allowed(&g.conf, a)) {
        refuse();
        return;
    }
    struct wsb_buf w = { r, 0, sizeof r };
    bs(&w, "WASABI!1 ");
    bs(&w, g.conf.name);
    bc(&w, ' ');
    bu(&w, g.port);
    bc(&w, ' ');
    bs(&w, g.banner);
    bc(&w, '\n');
    wsb_os_sendto(s, r, w.n, a, port);
}

/* is the connection waiting on its peer (the 10 s progress rule, 3.2)? Not while a command runs or a stream flows. */
static int waits_on_peer(struct conn *c)
{
    return !c->hello || c->outn > c->outoff || c->rx.st == RX_SMALL || c->rx.st == RX_BODY ||
           (c->rx.st == RX_HDR && c->rx.got) || c->job == J_NONE || c->job == J_PUT || c->job == J_SPEEDIN ||
           c->closing;
}

static void produce(struct conn *c)
{
    switch (c->job) {
    case J_OUT: produce_out(c); break;
    case J_GET: case J_SPEEDOUT: case J_LS: produce_files(c); break;
    case J_LOG: produce_log(c); break;
    case J_GRAB: produce_screen(c); break;
    }
}

int wsb_step(void)
{
    static struct wsb_pollfd f[WSB_NFD + 4];
    static int who[WSB_NFD + 4];                /* >= 0 a connection, -1 the listener, -2/-3 discovery, else run's */
    static uint8_t rb[1024];
    uint32_t tmo = WSB_NETWAIT_MS;
    int n = 0, r;

    g.now = wsb_os_now();
    if (g.quit >= 0) {
        int open = 0;
        for (int i = 0; i < WSB_CONNS; i++)
            if (g.c[i].fd >= 0)
                open++;
        if (!open || DUE(g.quitby))
            return finish();
    }
    if (g.lfd < 0 && g.netretry && DUE(g.netretry)) {
        int e = open_listener();
        if (e == -ENETDOWN || e == -EMFILE)
            g.netretry = g.now + WSB_NETWAIT_MS;
        else if (e < 0) {
            say("cannot listen any more: stopped", 0);
            g.quit = WSB_ST_FAIL;
            return finish();
        } else {
            open_disc();
            say("the network is up: listening", 0);
        }
    }
    if (g.refused != g.refused_saved && (uint32_t)(g.now - g.refused_at) >= WSB_REFUSED_MS)
        refused_save();

    /* the jobs fill their queues; then the timers */
    for (int i = 0; i < WSB_CONNS; i++) {
        struct conn *c = &g.c[i];
        if (c->fd < 0)
            continue;
        if (c->held && DUE(c->holdto)) {
            c->held = 0;
            hello(i, c->in, c->blen);
            c->blen = 0;
        }
        if (!c->closing)
            produce(c);
        if (c->fd < 0)
            continue;
        if (c->closing && c->outn == c->outoff) {
            if (c->closing == 1) {
                conn_drop(i);
                continue;
            }
            if (c->closing == 2 && !c->closeby) {
                wsb_os_shutdown(c->fd);
                c->closeby = g.now + WSB_DRAIN_MS;
            }
        }
        if (c->closing == 2 && c->closeby && DUE(c->closeby)) {
            conn_drop(i);
            continue;
        }
        if (!c->hello && !c->held && !c->closing) {
            /* a HELLO trickled in a byte at a time would count as progress for ever: four such connections kept every
             * slot, the owner's too, for hours without the key (audit 2026-10-29 B2-1). So a cap from the accept; but
             * not 10 s: a loaded client on a busy line (a lost segment sent again) is honest and still waits on its
             * HELLO then (B2-15). Silence is the 10 s progress rule below, counted from the last byte received */
            if ((uint32_t)(g.now - c->born) >= WSB_HELLO_MS) {
                conn_drop(i);
                continue;
            }
            TMO(&tmo, WSB_HELLO_MS - (g.now - c->born));
        }
        if (waits_on_peer(c)) {
            if ((uint32_t)(g.now - c->last) >= WSB_STALL_MS) {
                conn_drop(i);
                continue;
            }
            TMO(&tmo, WSB_STALL_MS - (g.now - c->last));
        }
        if (c->held)
            TMO(&tmo, LEFT(c->holdto));
        if (c->closing == 2 && c->closeby)
            TMO(&tmo, LEFT(c->closeby));
        screen_tick(c, &tmo);
        if (c->fd >= 0 && c->job == J_OUT && c->outn == c->outoff)
            tmo = 0;
    }
    run_tick(&tmo);
    log_tick(&tmo);
    if (g.quit >= 0)
        TMO(&tmo, LEFT(g.quitby));
    if (g.netretry)
        TMO(&tmo, LEFT(g.netretry));

    /* the poll set */
    if (g.lfd >= 0 && g.nfd < WSB_NFD) { f[n].fd = g.lfd; f[n].events = WSB_POLLIN; who[n++] = -1; }
    for (int k = 0; k < 2; k++)
        if (g.ufd[k] >= 0) { f[n].fd = g.ufd[k]; f[n].events = WSB_POLLIN; who[n++] = -2 - k; }
    for (int i = 0; i < WSB_CONNS; i++) {
        struct conn *c = &g.c[i];
        short ev = 0;
        if (c->fd < 0)
            continue;
        if (!c->held && (!c->closing || c->closing == 2))
            ev |= WSB_POLLIN;
        if (c->outn > c->outoff)
            ev |= WSB_POLLOUT;
        if (!ev)
            continue;
        f[n].fd = c->fd;
        f[n].events = ev;
        who[n++] = i;
    }
    n = run_poll(f, who, n, &tmo);
    for (int k = 0; k < n; k++)
        f[k].revents = 0;
    r = wsb_os_poll(f, n, (int)tmo);
    g.now = wsb_os_now();
    if (r <= 0)
        return WSB_GOON;
    for (int k = 0; k < n; k++) {
        short rev = f[k].revents;
        int w = who[k];
        if (!rev)
            continue;
        if (w == -1) {
            if (rev & (WSB_POLLERR | WSB_POLLHUP | WSB_POLLNVAL)) {
                net_lost();
                continue;
            }
            for (int j = 0; j < 4 && g.lfd >= 0; j++)
                accept_one();
        } else if (w == -2 || w == -3) {
            if (g.ufd[-2 - w] != f[k].fd)
                continue;
            if (rev & (WSB_POLLERR | WSB_POLLHUP | WSB_POLLNVAL)) {
                net_lost();
                continue;
            }
            probe(f[k].fd);
        } else if (w >= 0 && w < WSB_CONNS) {
            struct conn *c = &g.c[w];
            if (c->fd != f[k].fd)
                continue;
            if (rev & WSB_POLLOUT) {
                long s = wsb_os_send(c->fd, c->out + c->outoff, (long)(c->outn - c->outoff));
                if (s > 0) {
                    c->outoff += (uint32_t)s;
                    c->last = g.now;
                    if (c->outoff == c->outn)
                        c->outoff = c->outn = 0;
                } else if (s != -EAGAIN) {
                    conn_drop(w);
                    continue;
                }
            }
            /* up to 8 KB a turn: a body moves at the network's pace, and the others still get their turns */
            for (int rd = 0; rd < 8 && c->fd >= 0 && !c->held && (rev & (WSB_POLLIN | WSB_POLLHUP | WSB_POLLERR)); rd++) {
                long got = wsb_os_recv(c->fd, rb, sizeof rb);
                if (got > 0) {
                    c->last = g.now;
                    if (c->closing != 2)
                        input(w, rb, (uint32_t)got);
                    if (got < (long)sizeof rb || (c->job != J_PUT && c->job != J_SPEEDIN))
                        break;
                } else {
                    if (got != -EAGAIN)
                        conn_drop(w);           /* the other side closed (or broke) */
                    break;
                }
            }
        } else
            run_ready(k, w, rev);
    }
    return WSB_GOON;
}
