/* SPDX-License-Identifier: GPL-2.0-only */
/* mail's core on the PC (DESIGN D59 section 8, tools/host/mail-test.sh "units"), with the sanitizers: the transfer
 * encodings, the charsets, RFC 2047 words, dates, MIME trees (alternative, mixed, nested, forwarded, broken ones),
 * HTML made text, attachments and their names (RFC 2231), escape sequences never through, the settings' errors,
 * addresses, the message written (headers, words, quoted-printable, Bcc kept off) and the drafts (reply, reply to
 * all, forward), modified UTF-7. Exit 1 on a failure. */
#include "../../net/mail/mail.h"

static int fails;
#define CHECK(c, what) do { if (c) printf("PASS %s\n", what); else { printf("FAIL %s (line %d)\n", what, __LINE__); fails++; } } while (0)

static int clean(const char *s)             /* no control but newline, no C1, valid UTF-8 */
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if (*p < 32 && *p != '\n') return 0;
        if (*p == 127) return 0;
        if (*p >= 0x80) {
            int n = (*p & 0xE0) == 0xC0 ? 1 : (*p & 0xF0) == 0xE0 ? 2 : (*p & 0xF8) == 0xF0 ? 3 : -1;
            if (n < 0) return 0;
            unsigned u = *p & (0x3F >> n);
            for (int i = 1; i <= n; i++) {
                if ((p[i] & 0xC0) != 0x80) return 0;
                u = u << 6 | (p[i] & 0x3F);
            }
            if (u >= 0x80 && u < 0xA0) return 0;
            p += n + 1;
        } else
            p++;
    }
    return 1;
}

static char *hdec(const char *v) { return mime_hdecode(v, (unsigned)strlen(v)); }

static void parse(struct mmsg *m, const char *raw)
{
    if (mime_parse(m, raw, (unsigned)strlen(raw)) < 0) {
        printf("FAIL mime_parse: out of memory\n");
        fails++;
    }
}

int main(void)
{
    char b[4096];
    char *s;
    /* transfer encodings */
    const char *q = "caf=C3=A9 =\r\nsoft=3D x=ZZ =";
    unsigned n = qp_decode(q, (unsigned)strlen(q), b, 0);
    b[n] = 0;
    CHECK(!strcmp(b, "café soft= x=ZZ "), "quoted-printable: hex, a soft break, a bad = kept");
    q = "csOk\r\na3Ntw7ZyZ8Olcw==\n";
    n = b64_decode(q, (unsigned)strlen(q), b);
    b[n] = 0;
    CHECK(!strcmp(b, "räksmörgås"), "base64 across lines");
    b64_encode((const unsigned char *)"\0u\0p", 4, b);
    CHECK(!strcmp(b, "AHUAcA=="), "base64 written");
    /* charsets */
    struct sb t;
    sb_init(&t, 1000);
    mime_text(&t, "\xe5\xe4\xf6 \x80 \x93q\x94", 9, "windows-1252");
    CHECK(t.p && !strcmp(t.p, "åäö € “q”"), "Windows-1252");
    sb_free(&t);
    sb_init(&t, 1000);
    mime_text(&t, "\xa4 \xbd", 3, "ISO-8859-15");
    CHECK(t.p && !strcmp(t.p, "€ œ"), "ISO-8859-15");
    sb_free(&t);
    sb_init(&t, 1000);
    mime_text(&t, "ok \xc3\xa5 \xe5 \x1b[2J \xc2\x9b" "31m \x07 end\r\n", 25, "x-unknown");
    CHECK(t.p && !strcmp(t.p, "ok å å [2J 31m  end\n") && clean(t.p), "an unknown charset: UTF-8 else Latin-1; ESC, CSI (U+009B) and BEL dropped");
    sb_free(&t);
    /* RFC 2047 */
    s = hdec("=?UTF-8?B?UsOka3Ntw7ZyZ8Olcw==?= =?ISO-8859-1?Q?_och_=F6l?= today");
    CHECK(s && !strcmp(s, "Räksmörgås och öl today"), "encoded words: B and Q, joined, the plain word after");
    free(s);
    s = hdec("=?utf-8?q?a=1B=5B2Jb?= \x1b]0;title\x07");
    CHECK(s && clean(s) && !strchr(s, 27), "escape sequences in encoded and raw header text dropped");
    free(s);
    s = hdec("=?broken? plain =?utf-8?b?w6U=?=");
    CHECK(s && !strcmp(s, "=?broken? plain å"), "a broken word left as text");
    free(s);
    /* dates */
    CHECK(mime_date("Tue, 17 Nov 2026 21:06:14 +0100") == 1794945974u, "a date with a zone");
    CHECK(mime_date("17 Nov 26 20:06 GMT") == 1794945960u, "a two-digit year, no seconds");
    CHECK(mime_date("garbage") == 0 && mime_date("") == 0 && mime_date("99 Foo 99999 99:99") == 0, "bad dates: 0");
    /* a multipart message */
    const char *m1 =
        "From: =?UTF-8?Q?Bj=C3=B6rn?= <bjorn@example.org>\r\nTo: a@example.org,\r\n b@example.org\r\n"
        "Subject: =?iso-8859-1?q?r=E4k?=\r\nDate: Tue, 17 Nov 2026 20:06:14 +0000\r\nMessage-ID: <x1@example.org>\r\n"
        "MIME-Version: 1.0\r\nContent-Type: multipart/mixed; boundary=\"OUT\"\r\n\r\npreamble\r\n"
        "--OUT\r\nContent-Type: multipart/alternative; boundary=IN\r\n\r\n"
        "--IN\r\nContent-Type: text/plain; charset=iso-8859-1\r\nContent-Transfer-Encoding: quoted-printable\r\n\r\n"
        "Hej =E5 dig=\r\n, line two\r\n--IN\r\nContent-Type: text/html\r\n\r\n<p>html version</p>\r\n--IN--\r\n"
        "--OUT\r\nContent-Type: application/pdf; name=\"x.pdf\"\r\nContent-Disposition: attachment;\r\n"
        " filename*=UTF-8''r%C3%A4kna%2F..%2Fx.pdf\r\nContent-Transfer-Encoding: base64\r\n\r\nSGVsbG8gUERG\r\n"
        "--OUT\r\nContent-Type: image/png\r\nContent-Disposition: attachment; filename*0=\"part\"; filename*1=\"s.png\"\r\n"
        "Content-Transfer-Encoding: base64\r\n\r\niVBORw==\r\n--OUT--\r\nepilogue\r\n";
    struct mmsg m;
    parse(&m, m1);
    CHECK(!strcmp(m.from, "Björn <bjorn@example.org>") && !strcmp(m.subject, "räk") && !strcmp(m.to, "a@example.org, b@example.org"),
          "headers: unfolded, words decoded");
    CHECK(m.text && !strcmp(m.text, "Hej å dig, line two") && !m.html, "alternative: the plain part, quoted-printable, ISO-8859-1");
    CHECK(m.natt == 2 && !strcmp(m.att[0].name, "räkna_.._x.pdf") && !strcmp(m.att[0].type, "application/pdf") &&
          !strcmp(m.att[1].name, "parts.png"), "attachments: RFC 2231 names (with charset; in pieces), no '/'");
    unsigned dn;
    char *d = mime_decode_part(&m.att[0], &dn);
    CHECK(d && dn == 9 && !memcmp(d, "Hello PDF", 9), "an attachment decoded");
    free(d);
    mime_free(&m);
    /* HTML only, with a script, entities and a link */
    parse(&m, "Content-Type: text/html; charset=utf-8\r\nContent-Transfer-Encoding: base64\r\n\r\n"
              "PGh0bWw+PGhlYWQ+PHRpdGxlPlQ8L3RpdGxlPjxzdHlsZT5we308L3N0eWxlPjwvaGVhZD48Ym9k\r\n"
              "eT48cD5IZWxsbyAmYW1wOyB3ZWxjb21lPC9wPjxzY3JpcHQ+YWxlcnQoMSk8L3NjcmlwdD48dWw+\r\n"
              "PGxpPm9uZTwvbGk+PGxpPnR3bzwvbGk+PC91bD48YSBocmVmPSJodHRwOi8vZXhhbXBsZS5vcmcv\r\n"
              "Ij5zaXRlPC9hPiYjMjc7WzJKJiN4OUI7MzFtIGRvbmU8YnI+bmV4dDwvYm9keT48L2h0bWw+\r\n");
    if (m.text && strcmp(m.text, "Hello & welcome\n\n* one\n* two\n\nsite <http://example.org/>[2J\u203a31m done\nnext"))
        printf("got [%s]\n", m.text);
    CHECK(m.html && m.text && !strcmp(m.text, "Hello & welcome\n\n* one\n* two\n\nsite <http://example.org/>[2J\u203a31m done\nnext"),
          "HTML: head, style and script dropped, paragraphs, a list, a link, an ESC entity dropped, U+009B read as Windows-1252");
    CHECK(clean(m.text), "HTML: the text is clean");
    mime_free(&m);
    /* hostile: nesting past the limit, no closing delimiter, NULs, a forwarded message */
    char deep[4000];
    int o = 0;
    for (int i = 0; i < 20; i++) o += sprintf(deep + o, "Content-Type: multipart/mixed; boundary=b%d\r\n\r\n--b%d\r\n", i, i);
    o += sprintf(deep + o, "Content-Type: text/plain\r\n\r\ndeep text\r\n");
    parse(&m, deep);
    CHECK(m.text && clean(m.text), "20 levels of multipart: bounded, no crash");
    mime_free(&m);
    parse(&m, "Content-Type: multipart/mixed; boundary=z\r\n\r\n--z\r\nContent-Type: message/rfc822\r\n\r\n"
              "From: inner@example.org\r\nSubject: inside\r\n\r\ninner body\r\n--z\r\n\r\nno headers\0 here");
    CHECK(m.text && strstr(m.text, "Subject: inside") && strstr(m.text, "inner body") && strstr(m.text, "no headers"),
          "a forwarded message shown inline; a part with no headers; no closing delimiter");
    mime_free(&m);
    parse(&m, "");
    CHECK(m.text && !m.text[0] && m.from && !m.from[0], "an empty message");
    mime_free(&m);
    parse(&m, "Content-Type: multipart/mixed; boundary=q\r\n\r\n--q\r\nContent-Type: \x1b[2J\x9b" "31m/evil; charset=\"\x1b[0m\"\r\n"
              "Content-Disposition: attachment; filename=\"\x1b]0;x\x07.txt\"\r\n\r\nx\r\n--q--\r\n");
    CHECK(m.natt == 1 && clean(m.att[0].type) && !strchr(m.att[0].type, 27) && !strchr(m.att[0].charset, 27) &&
          clean(m.att[0].name) && !strchr(m.att[0].name, 27), "an attachment's type, charset and name: clean");
    mime_free(&m);
    {
        struct sb c;
        sb_init(&c, 5);
        sb_puts(&c, "abcdä");               /* 6 bytes: the 'ä' would be cut in two */
        CHECK(c.full && c.n == 4 && clean(c.p), "a full string is cut between characters");
        sb_free(&c);
    }
    /* settings */
    char err[160];
    struct mail_cfg c;
    char *ct = strdup("# x\nimap imap.example.org\n  user  me\npassword  p w  \nsmtp smtp.example.org\nsmtp_tls on\nlimit 50\n");
    CHECK(mail_config(&c, ct, err, sizeof err) == 0 && !strcmp(c.imap, "imap.example.org") && c.port == 993 &&
          !strcmp(c.pass, "p w") && c.smtp_port == 465 && c.limit == 50 && c.has_pass && !strcmp(c.folder, "INBOX"),
          "settings read, ports by TLS");
    mail_cfg_free(&c);
    ct = strdup("pop p.example.org\ntls starttls\nfrobnicate 1\n");
    CHECK(mail_config(&c, ct, err, sizeof err) < 0 && strstr(err, "line 3") && strstr(err, "frobnicate"), "an unknown setting: its line");
    mail_cfg_free(&c);
    ct = strdup("port 70000\n");
    CHECK(mail_config(&c, ct, err, sizeof err) < 0 && strstr(err, "line 1"), "a bad port");
    mail_cfg_free(&c);
    ct = strdup("imap a\npop b\n");
    CHECK(mail_config(&c, ct, err, sizeof err) < 0, "imap and pop both refused");
    mail_cfg_free(&c);
    /* addresses */
    struct maddr a[8];
    int k = addr_parse("\"Doe, John\" <j@example.org>, bare@example.org (comment), , Åsa <asa@example.org>", a, 8, b, sizeof b,
                       err, sizeof err);
    CHECK(k == 3 && !strcmp(a[0].name, "Doe, John") && !strcmp(a[0].addr, "j@example.org") && !strcmp(a[1].addr, "bare@example.org")
          && !strcmp(a[2].name, "Åsa"), "addresses: quoted names, comments, empty items");
    CHECK(addr_parse("bad address", a, 8, b, sizeof b, err, sizeof err) < 0 && strstr(err, "not an address"), "a bad address refused");
    CHECK(addr_parse("a@b\r\nRCPT", a, 8, b, sizeof b, err, sizeof err) < 0, "an address with a line break refused");
    k = addr_parse("a@b\r\nRCPT TO:<x@example.org>", a, 8, b, sizeof b, err, sizeof err);
    CHECK(k == 1 && !strcmp(a[0].addr, "x@example.org") && !strchr(a[0].name, '\r') && !strchr(a[0].name, '\n'),
          "a line break in a name: made a space");
    /* the message written */
    char *rc[60], rb[2000], from[300];
    int nr;
    unsigned mn;
    char *msg = mail_build("From: Me Myself <me@example.org>\nTo: Åsa <asa@example.org>, \"Doe, John\" <j@example.org>\n"
                           "Cc: \nBcc: hidden@example.org\nSubject: Räksmörgås för alla, a long subject to be folded in words\n"
                           "X-Extra: yes\n\n.leading dot\ncafé\ttab\n" "\x1b[2J" "end\n\n\n",
                           1794945974u, 0x1234u, &mn, rc, &nr, rb, sizeof rb, from, sizeof from, err, sizeof err);
    CHECK(msg != 0, "a message built");
    if (msg) {
        CHECK(nr == 3 && !strcmp(rc[0], "asa@example.org") && !strcmp(rc[2], "hidden@example.org") && !strcmp(from, "me@example.org"),
              "the envelope: To, Cc and Bcc, the sender");
        CHECK(!strstr(msg, "hidden@") && !strstr(msg, "Bcc"), "Bcc kept off the message");
        CHECK(strstr(msg, "Date: Tue, 17 Nov 2026 20:06:14 +0000\r\n") && strstr(msg, "Message-ID: <6afcb3b6.00001234@example.org>\r\n"),
              "Date and Message-ID");
        CHECK(strstr(msg, "To: =?UTF-8?B?w4VzYQ==?= <asa@example.org>, \"Doe, John\" <j@example.org>\r\n") != 0, "names: words and quotes");
        CHECK(strstr(msg, "Subject: =?UTF-8?B?") && strstr(msg, "?=\r\n =?UTF-8?B?"), "a subject outside ASCII: words, folded");
        CHECK(strstr(msg, "X-Extra: yes\r\n") && strstr(msg, "charset=utf-8\r\nContent-Transfer-Encoding: quoted-printable\r\n\r\n"
              ".leading dot\r\ncaf=C3=A9\ttab\r\n[2Jend\r\n") && msg[mn - 1] == '\n' && mn == strlen(msg),
              "the body: quoted-printable, CR LF, the escape dropped, the trailing blank lines gone");
        free(msg);
    }
    msg = mail_build("From: me@example.org\nSubject: x\n\nbody\n", 0, 0, &mn, rc, &nr, rb, sizeof rb, from, sizeof from, err, sizeof err);
    CHECK(!msg && strstr(err, "no recipient"), "no recipient refused");
    msg = mail_build("From: me@example.org\nTo x@example.org\n\nbody\n", 0, 0, &mn, rc, &nr, rb, sizeof rb, from, sizeof from, err, sizeof err);
    CHECK(!msg && strstr(err, "line 2"), "a line that is not a header: said with its number");
    msg = mail_build("From: me@example.org\nTo: x@example.org\nDate: forged\nX-\xc3\xa5: y\nSubject: plain\n\nascii\n", 0, 0, &mn, rc, &nr, rb, sizeof rb, from,
                     sizeof from, err, sizeof err);
    CHECK(msg && strstr(msg, "charset=us-ascii\r\nContent-Transfer-Encoding: 7bit\r\n\r\nascii\r\n") && !strstr(msg, "forged")
          && !strstr(msg, "X-"), "ASCII: 7bit; a Date of the user's and a header name outside ASCII dropped");
    free(msg);
    /* drafts */
    parse(&m, "From: Bob <bob@example.org>\r\nTo: me@example.org, carol@example.org\r\nCc: Bob <bob@example.org>, dave@example.org\r\n"
              "Subject: Re: plans\r\nDate: Tue, 17 Nov 2026 20:06:14 +0000\r\nMessage-ID: <m2@example.org>\r\n"
              "References: <m1@example.org>\r\n\r\nline one\r\n> old\r\n");
    s = mail_draft(DRAFT_ALL, &m, "Me <me@example.org>", 0);
    CHECK(s && strstr(s, "To: Bob <bob@example.org>\nCc: carol@example.org, dave@example.org\n") && strstr(s, "Subject: Re: plans\n")
          && strstr(s, "In-Reply-To: <m2@example.org>\nReferences: <m1@example.org> <m2@example.org>\n") &&
          strstr(s, "wrote:\n> line one\n>> old\n"), "reply to all: without me and the sender twice; Re: once; quoted");
    free(s);
    mime_free(&m);
    parse(&m, "From: Bob <bob@example.org>\r\nTo: team: a@example.org;, broken address, carol@example.org\r\n"
              "Subject: x\r\n\r\ntext\r\n");
    s = mail_draft(DRAFT_ALL, &m, "Me <me@example.org>", 0);
    CHECK(s && strstr(s, "Cc: carol@example.org\n"), "reply to all: a bad address skipped, the good ones kept");
    free(s);
    s = mail_draft(DRAFT_FWD, &m, "Me <me@example.org>", 0);
    CHECK(s && strstr(s, "Subject: Fwd: x\n") && strstr(s, "-------- Forwarded message --------\nFrom: Bob <bob@example.org>")
          && strstr(s, "\ntext\n") && !strstr(s, "In-Reply-To"), "forward");
    free(s);
    mime_free(&m);
    /* folder names */
    CHECK(mutf7_decode("Entw&APw-rfe &-", b, sizeof b) > 0 && !strcmp(b, "Entwürfe &"), "modified UTF-7");
    CHECK(mutf7_decode("bad&ZZZ", b, sizeof b) < 0, "modified UTF-7: a broken one refused");
    printf("%s: %d failed\n", fails ? "FAIL mail-core-test" : "mail-core-test: all passed", fails);
    return fails != 0;
}
