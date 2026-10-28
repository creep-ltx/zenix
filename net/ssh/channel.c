/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ssh's connection layer (RFC 4254; DESIGN D21 4.2): one "session" channel, pty-req (the terminal's name and size,
 * the modes VINTR 3, VERASE 127, IUTF8 on), shell or exec, a 64 KB window adjusted as the data is handed on, a
 * 16 KB maximum packet, EOF and close, exit-status (exit-signal gives 255, as OpenSSH's client), and every
 * request with want-reply that ssh does not handle answered with a failure (keepalive@openssh.com).
 */
#include <string.h>
#include "ssh_int.h"

#define C_OPEN  1                       /* CHANNEL_OPEN sent */
#define C_PTY   2                       /* pty-req sent */
#define C_SHELL 3                       /* shell or exec sent */
#define C_RUN   4

static void start_shell(struct ssh *s)
{
    struct wr *w = pkt_begin(s, M_CHANNEL_REQUEST);

    if (!w)
        return;
    wr_u32(w, s->ch_remote);
    if (s->command) {
        wr_cstr(w, "exec");
        wr_u8(w, 1);
        wr_cstr(w, s->command);
    } else {
        wr_cstr(w, "shell");
        wr_u8(w, 1);
    }
    pkt_send(s);
    s->ch_step = C_SHELL;
}

void chan_begin(struct ssh *s)
{
    struct wr *w = pkt_begin(s, M_CHANNEL_OPEN);

    if (!w)
        return;
    wr_cstr(w, "session");
    wr_u32(w, 0);                       /* our channel number */
    wr_u32(w, SSH_WINDOW);
    wr_u32(w, SSH_MAXPKT);
    pkt_send(s);
    s->ch_lwin = SSH_WINDOW;
    s->ch_step = C_OPEN;
}

static void adjust(struct ssh *s)
{
    struct wr *w;

    if (s->ch_consumed < SSH_WINDOW / 2 || s->ch_close_out || s->in_kex)
        return;
    w = pkt_begin(s, M_CHANNEL_WINDOW_ADJUST);
    if (!w)
        return;
    wr_u32(w, s->ch_remote);
    wr_u32(w, s->ch_consumed);
    pkt_send(s);
    s->ch_lwin += s->ch_consumed;
    s->ch_consumed = 0;
}

static void fail_reply(struct ssh *s)
{
    struct wr *w;

    if (s->in_kex) {
        s->pend_cfail++;
        return;
    }
    w = pkt_begin(s, M_CHANNEL_FAILURE);
    if (!w)
        return;
    wr_u32(w, s->ch_remote);
    pkt_send(s);
}

void chan_after_kex(struct ssh *s)
{
    struct wr *w;

    for (; s->pend_gfail > 0; s->pend_gfail--) {
        w = pkt_begin(s, M_REQUEST_FAILURE);
        if (!w)
            return;
        pkt_send(s);
    }
    for (; s->pend_cfail > 0; s->pend_cfail--) {
        w = pkt_begin(s, M_CHANNEL_FAILURE);
        if (!w)
            return;
        wr_u32(w, s->ch_remote);
        pkt_send(s);
    }
    if (s->state >= SSH_ST_OPEN)
        adjust(s);
    /* asked for while the exchange ran: dropped before, so a remote `cat > f` whose input ended then never ended
     * (audit 2026-10-28 S17) */
    if (s->pend_winch) { s->pend_winch = 0; ssh_winsize(s, s->cols, s->rows); }
    if (s->pend_eof) { s->pend_eof = 0; ssh_eof(s); }
    if (s->pend_close) { s->pend_close = 0; ssh_close(s); }
}

static void deliver(struct ssh *s, int ext, const unsigned char *d, long n)
{
    if ((unsigned long)n > s->ch_lwin) {
        ssh_fail(s, "the server sent more than the channel's window");
        return;
    }
    s->ch_lwin -= (unsigned long)n;
    if (n && s->data)
        s->data(s->u, ext, d, n);
    s->ch_consumed += (unsigned long)n;
    adjust(s);
}

int chan_packet(struct ssh *s, unsigned type, struct rd *r)
{
    unsigned long ch = rd_u32(r);

    if (r->bad || ch != 0) {
        ssh_fail(s, "a channel message for a channel we do not have");
        return 1;
    }
    switch (type) {
    case M_CHANNEL_OPEN_CONFIRMATION:
        if (s->ch_step != C_OPEN)
            break;
        s->ch_remote = rd_u32(r);
        s->ch_rwin = rd_u32(r);
        s->ch_rmax = rd_u32(r);
        if (r->bad) {
            ssh_fail(s, "a damaged channel confirmation");
            return 1;
        }
        if (s->pty) {
            struct wr *w = pkt_begin(s, M_CHANNEL_REQUEST);
            /* the modes (RFC 4254 8): VINTR 1 = 3, VERASE 3 = 127, IUTF8 42 = 1, TTY_OP_END */
            static const unsigned char modes[] = { 1, 0, 0, 0, 3, 3, 0, 0, 0, 127, 42, 0, 0, 0, 1, 0 };

            if (!w)
                return 1;
            wr_u32(w, s->ch_remote);
            wr_cstr(w, "pty-req");
            wr_u8(w, 1);
            wr_cstr(w, s->term ? s->term : "xterm-16color");
            wr_u32(w, (unsigned long)s->cols);
            wr_u32(w, (unsigned long)s->rows);
            wr_u32(w, 0);
            wr_u32(w, 0);
            wr_str(w, modes, sizeof modes);
            pkt_send(s);
            s->ch_step = C_PTY;
        } else {
            start_shell(s);
        }
        return 1;
    case M_CHANNEL_OPEN_FAILURE: {
        long n;
        const unsigned char *d;

        if (s->ch_step != C_OPEN)
            break;
        rd_u32(r);
        d = rd_str(r, &n);
        ssh_failf(s, "the server refused the session", d, d ? n : 0);
        return 1;
    }
    case M_CHANNEL_SUCCESS:
    case M_CHANNEL_FAILURE:
        if (s->ch_step == C_PTY) {
            if (type == M_CHANNEL_FAILURE)
                ssh_say(s, SSH_TEXT_PROGRESS, "the server gave no terminal (pty-req refused)");
            start_shell(s);
        } else if (s->ch_step == C_SHELL) {
            if (type == M_CHANNEL_FAILURE) {
                ssh_fail(s, s->command ? "the server refused to run the command" : "the server refused a shell");
                return 1;
            }
            s->ch_step = C_RUN;
            s->state = SSH_ST_SESSION;
            ssh_say(s, SSH_TEXT_PROGRESS, "session open");
        }
        return 1;                       /* (no other request of ours wants a reply) */
    case M_CHANNEL_WINDOW_ADJUST: {
        unsigned long add = rd_u32(r);

        if (r->bad)
            break;
        s->ch_rwin = add > 0xFFFFFFFFUL - s->ch_rwin ? 0xFFFFFFFFUL : s->ch_rwin + add;
        return 1;
    }
    case M_CHANNEL_DATA:
    case M_CHANNEL_EXTENDED_DATA: {
        unsigned long code = type == M_CHANNEL_EXTENDED_DATA ? rd_u32(r) : 0;
        long n;
        const unsigned char *d = rd_str(r, &n);

        if (r->bad || s->ch_step < C_PTY) {        /* (data may come before the shell's reply: RFC 4254) */
            ssh_fail(s, "damaged or early channel data");
            return 1;
        }
        deliver(s, code == 1 ? 1 : (int)code, d, n);
        return 1;
    }
    case M_CHANNEL_EOF:
        s->ch_eof_in = 1;
        return 1;
    case M_CHANNEL_CLOSE: {
        struct wr *w;

        s->ch_close_in = 1;
        if (!s->ch_close_out && (w = pkt_begin(s, M_CHANNEL_CLOSE)) != 0) {
            wr_u32(w, s->ch_remote);
            pkt_send(s);
            s->ch_close_out = 1;
        }
        if (s->state != SSH_ST_FAILED)
            s->state = SSH_ST_CLOSED;
        return 1;
    }
    case M_CHANNEL_REQUEST: {
        long n;
        const unsigned char *name = rd_str(r, &n);
        int want = rd_bool(r);

        if (r->bad) {
            ssh_fail(s, "a damaged channel request");
            return 1;
        }
        if (n == 11 && !memcmp(name, "exit-status", 11)) {
            unsigned long st = rd_u32(r);

            if (!r->bad)
                s->exit_status = (int)(st & 0xFF);
        } else if (n == 11 && !memcmp(name, "exit-signal", 11)) {
            s->exit_status = 255;
        } else if (want) {
            fail_reply(s);              /* keepalive@openssh.com and anything else we do not do */
        }
        return 1;
    }
    }
    return 0;
}

long ssh_write(struct ssh *s, const unsigned char *b, long n)
{
    struct wr *w;
    unsigned long k = (unsigned long)n;
    unsigned long room = (unsigned long)(s->scap - 5 - 255 - ZC_CHAPOL_TAG - 16);

    if (s->state != SSH_ST_SESSION || s->in_kex || s->ch_eof_out || s->ch_close_out || n <= 0)
        return 0;
    if (k > s->ch_rwin) k = s->ch_rwin;
    if (k > s->ch_rmax) k = s->ch_rmax;
    if (k > room) k = room;
    if (!k)
        return 0;
    w = pkt_begin(s, M_CHANNEL_DATA);
    if (!w)
        return 0;
    wr_u32(w, s->ch_remote);
    wr_str(w, b, (long)k);
    pkt_send(s);
    s->ch_rwin -= k;
    return (long)k;
}

void ssh_eof(struct ssh *s)
{
    struct wr *w;

    if (s->state == SSH_ST_SESSION && !s->ch_eof_out && s->in_kex)
        s->pend_eof = 1;
    if (s->state != SSH_ST_SESSION || s->ch_eof_out || s->in_kex)
        return;
    w = pkt_begin(s, M_CHANNEL_EOF);
    if (!w)
        return;
    wr_u32(w, s->ch_remote);
    pkt_send(s);
    s->ch_eof_out = 1;
}

void ssh_close(struct ssh *s)
{
    struct wr *w;

    if (s->state == SSH_ST_SESSION && !s->ch_close_out && s->in_kex)
        s->pend_close = 1;
    if (s->state != SSH_ST_SESSION || s->ch_close_out || s->in_kex)
        return;
    w = pkt_begin(s, M_CHANNEL_CLOSE);
    if (!w)
        return;
    wr_u32(w, s->ch_remote);
    pkt_send(s);
    s->ch_close_out = 1;
}

void ssh_winsize(struct ssh *s, int cols, int rows)
{
    struct wr *w;

    s->cols = cols;
    s->rows = rows;
    if (s->state == SSH_ST_SESSION && s->pty && s->in_kex)
        s->pend_winch = 1;
    if (s->state != SSH_ST_SESSION || !s->pty || s->in_kex)
        return;
    w = pkt_begin(s, M_CHANNEL_REQUEST);
    if (!w)
        return;
    wr_u32(w, s->ch_remote);
    wr_cstr(w, "window-change");
    wr_u8(w, 0);
    wr_u32(w, (unsigned long)cols);
    wr_u32(w, (unsigned long)rows);
    wr_u32(w, 0);
    wr_u32(w, 0);
    pkt_send(s);
}

void ssh_keepalive(struct ssh *s)
{
    struct wr *w;

    if (s->state < SSH_ST_AUTH || s->state > SSH_ST_SESSION || s->in_kex)
        return;
    w = pkt_begin(s, M_GLOBAL_REQUEST);
    if (!w)
        return;
    wr_cstr(w, "keepalive@openssh.com");
    wr_u8(w, 1);
    pkt_send(s);
}
