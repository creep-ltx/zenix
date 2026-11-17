/* SPDX-License-Identifier: GPL-2.0-only */
/* irc's core, its parts that need no server (DESIGN D58 section 8, tools/host/irc-test.sh "units"): the text made
 * clean (UTF-8 kept, ISO-8859-1 read, controls and mIRC codes dropped, never an escape), RFC 1459's case, base64,
 * the configuration's errors, the commands' answers without a connection, a typed line of 1,000 bytes cut into
 * pieces on whole characters, the scrollback's ring. */
#include "../../net/irc/irc.h"

static int bad;
static void ck(int ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    bad += !ok;
}

static char sent[8][IRC_LINEMAX + 4];
static int nsent;
static void out(void *u, const char *l, int n) { (void)u; if (nsent < 8) snprintf(sent[nsent++], sizeof sent[0], "%.*s", n, l); }
static char last[1100];
static void shown(void *u, int w, const struct irc_line *l) { (void)u; if (w >= 0) snprintf(last, sizeof last, "%s", l->text); }

int main(void)
{
    char o[256];
    irc_utf8("r\xe4ksm\xf6rg\xe5s", 10, o, sizeof o);
    ck(!strcmp(o, "räksmörgås"), "ISO-8859-1 read as such: r\\xe4ksm\\xf6rg\\xe5s is räksmörgås");
    irc_utf8("räksmörgås ✓", (int)strlen("räksmörgås ✓"), o, sizeof o);
    ck(!strcmp(o, "räksmörgås ✓"), "UTF-8 kept");
    irc_utf8("a\x1b[2Jb\x07\x7f c", 10, o, sizeof o);
    ck(!strcmp(o, "a[2Jb c"), "ESC, BEL and DEL dropped: no escape reaches the screen");
    irc_utf8("x\xc2\x9b" "2J\xc2\x85y", 8, o, sizeof o);
    ck(!strcmp(o, "x2Jy"), "C1 controls written as UTF-8 (CSI U+009B, NEL) dropped");
    irc_utf8("\x9b" "2J", 3, o, sizeof o);
    ck(!strcmp(o, "2J"), "a lone 0x9B (the Amiga's CSI) dropped");
    irc_utf8("\x03" "04,01red\x03 \x02" "b\x02\x0f\x16\x1d\x1f" "x\x03" "5y\x03" "12,5z", 28, o, sizeof o);
    ck(!strcmp(o, "red bxyz"), "mIRC's colours and format codes dropped");
    irc_utf8("\xed\xa0\x80\xc0\xaf", 5, o, sizeof o);
    ck(!strcmp(o, "\xc3\xad\xc2\xa0\xc3\x80\xc2\xaf"), "a surrogate and an overlong form are not UTF-8: read as ISO-8859-1");
    ck(irc_utf8("\t", 1, o, sizeof o) == 1 && !strcmp(o, " "), "a tab is a space");
    {
        char big[600];
        memset(big, 0xe4, sizeof big);
        int n = irc_utf8(big, sizeof big, o, sizeof o);
        ck(n < (int)sizeof o && n % 2 == 0 && n > 240, "a line longer than the buffer cut on a whole character");
    }
    ck(!irc_casecmp("[Zenix]~", "{zenix}^", -1) && irc_casecmp("a", "b", -1) < 0 && !irc_casecmp("ABc", "abD", 2),
       "RFC 1459's case: [ ] ~ are { } ^");
    irc_b64((const unsigned char *)"u\0u\0p", 5, o);
    ck(!strcmp(o, "dQB1AHA="), "base64 (SASL PLAIN's blob)");

    struct irc s;
    char err[160];
    memset(&s, 0, sizeof s);
    ck(irc_config(&s, strdup("server a\nbogus 1\n"), err, sizeof err) < 0 && strstr(err, "line 2: unknown setting 'bogus'"),
       "config: an unknown setting named with its line");
    ck(irc_config(&s, strdup("port 70000\n"), err, sizeof err) < 0 && strstr(err, "port out of range"), "config: a port out of range");
    ck(irc_config(&s, strdup("sasl onlyname\n"), err, sizeof err) < 0, "config: sasl without a password");
    ck(irc_config(&s, strdup("autojoin #a #b #c #d #e #f #g #h #i\n"), err, sizeof err) < 0 && strstr(err, "more than 8"),
       "config: more than 8 autojoin channels refused");
    ck(irc_config(&s, strdup("# c\n\nserver irc.test\r\nport 6697\ntls on\nnick me\nrealname A B C\nautojoin #a #b\n"
                             "highlight zen amiga\ncharset latin1\nsasl acc pass word\nscrollback 50\n"), err, sizeof err) == 0 &&
       !strcmp(s.c.server, "irc.test") && s.c.port == 6697 && s.c.tls && !strcmp(s.c.realname, "A B C") &&
       s.c.nautojoin == 2 && !strcmp(s.c.autojoin[1], "#b") && s.c.nhilite == 2 && s.c.latin1 &&
       !strcmp(s.c.sasl_pass, "pass word") && s.c.scrollback == 50, "config: every setting read");
    s.send = out;
    s.shown = shown;
    irc_init(&s);
    irc_command(&s, "hello");
    ck(strstr(last, "status window") != 0, "text in the status window: said where to go");
    irc_command(&s, "/join");
    ck(strstr(last, "usage: /join") != 0, "/join without a channel: its usage");
    irc_login(&s);
    ck(nsent == 3 && !strcmp(sent[0], "CAP REQ :sasl\r\n") && !strcmp(sent[1], "NICK me\r\n") &&
       !strcmp(sent[2], "USER me 0 * :A B C\r\n"), "the login: CAP REQ sasl, NICK, USER");
    nsent = 0;
    irc_input(&s, ":srv CAP * ACK :sasl\r\nAUTHENTICATE +\r\n:srv 903 me :ok\r\n", (int)strlen(":srv CAP * ACK :sasl\r\nAUTHENTICATE +\r\n:srv 903 me :ok\r\n"));
    ck(nsent == 3 && !strcmp(sent[0], "AUTHENTICATE PLAIN\r\n") && !strcmp(sent[1], "AUTHENTICATE YWNjAGFjYwBwYXNzIHdvcmQ=\r\n") &&
       !strcmp(sent[2], "CAP END\r\n"), "SASL PLAIN: the blob, then CAP END");
    nsent = 0;
    irc_input(&s, ":srv 001 me :hi\r\n", (int)strlen(":srv 001 me :hi\r\n"));
    ck(nsent == 2 && !strcmp(sent[0], "JOIN #a\r\n") && !strcmp(sent[1], "JOIN #b\r\n"), "001: the autojoin channels");
    nsent = 0;
    irc_input(&s, ":me!u@h JOIN #a\r\n", (int)strlen(":me!u@h JOIN #a\r\n"));
    {
        char t[1200];
        memset(t, 'x', sizeof t);
        memcpy(t + 395, "\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4", 8);
        t[1000] = 0;
        irc_command(&s, t);
        int ok = nsent == 3, total = 0;
        for (int i = 0; i < nsent; i++) {
            char *p = strstr(sent[i], " :");
            int n = p ? (int)strlen(p + 2) - 2 : 0;
            ok &= p && n <= IRC_TEXTMAX && ((unsigned char)p[2] & 0xC0) != 0x80;
            total += n;
        }
        /* charset latin1: each ä one byte on the wire */
        ck(ok && total == 1000 - 4, "a typed line of 1,000 bytes: three PRIVMSGs of whole characters, in ISO-8859-1");
    }
    nsent = 0;
    for (int i = 0; i < 120; i++)
        irc_input(&s, ":bob!b@h PRIVMSG #a :line\r\n", (int)strlen(":bob!b@h PRIVMSG #a :line\r\n"));
    ck(s.w[1]->n == 50 && s.w[1]->act == IRC_ACT_NONE, "the scrollback keeps its 50 newest lines");
    irc_input(&s, ":bob!b@h PRIVMSG #a :\x01" "ACTION waves\x01\r\n", (int)strlen(":bob!b@h PRIVMSG #a :\x01" "ACTION waves\x01\r\n"));
    ck(!strcmp(last, "* bob waves"), "a CTCP ACTION shown as * nick text");
    irc_select(&s, 0);
    irc_input(&s, ":bob!b@h PRIVMSG #a :hey ME there\r\n", (int)strlen(":bob!b@h PRIVMSG #a :hey ME there\r\n"));
    ck(s.w[1]->act == IRC_ACT_HI, "our nick in any case is a highlight: the window marked");
    irc_input(&s, ":bob!b@h PRIVMSG me :psst\r\n", (int)strlen(":bob!b@h PRIVMSG me :psst\r\n"));
    ck(s.nw == 3 && !strcmp(s.w[2]->name, "bob") && s.w[2]->kind == IRC_W_QUERY, "a private message opens a query");
    {                                   /* review 2026-11-17: limits that hold */
        char t[1100];
        memset(t, 'y', sizeof t);
        t[1050] = 0;
        nsent = 0;
        irc_select(&s, 1);
        irc_command(&s, t);
        ck(nsent == 0 && strstr(last, "over 1023 bytes: not sent"), "a typed line over 1,023 bytes: not sent, said");
        memset(t, 0x80, 450);           /* continuation bytes only (never from the program, which cleans) */
        t[450] = 0;
        irc_command(&s, t);
        ck(nsent == 2, "400 continuation bytes in a row: sent in pieces, no endless loop");
        char nk[8];
        for (int i = 0; i < 30; i++) {
            char l[64];
            snprintf(nk, sizeof nk, "n%d", i);
            snprintf(l, sizeof l, ":%s!u@h PRIVMSG me :hi\r\n", nk);
            irc_input(&s, l, (int)strlen(l));
        }
        ck(s.nw == IRC_QUERYMAX && strstr(last, "<n29:me> hi"), "strangers open at most 16 windows, the rest in the status window");
        irc_input(&s, ":srv 001 me2 :again\r\n", 20);
        ck(!strcmp(s.me, "me2"), "001: the nick the server gives is ours");
    }
    irc_free(&s);
    printf("%s\n", bad ? "irc-core-test: FAILED" : "irc-core-test: all passed");
    return bad != 0;
}
