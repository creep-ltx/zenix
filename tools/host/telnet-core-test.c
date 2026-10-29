/* SPDX-License-Identifier: GPL-2.0-only */
/* telnet's core on the PC (DESIGN D20 8.1 "telnet's core on the host"): net/telnet/ against expected bytes.
 *   telnet-core-test [TABLES-FILE]     the checks; with a file: also writes the two CP437 tables there (256 bytes
 *                                      each) for tools/host/telnet-test.sh to compare with Python's cp437 codec */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../net/telnet/telnet.h"

static int fails, passes;
#define CHECK(c, what) do { if (c) { printf("PASS core: %s\n", what); passes++; } \
    else { printf("FAIL core: %s\n", what); fails++; } } while (0)

/* ---- the protocol ---- */
static unsigned char sent[8192];
static long nsent;
static void tsend(void *u, const unsigned char *b, long n)
{
    (void)u;
    if (nsent + n <= (long)sizeof sent) {
        memcpy(sent + nsent, b, n);
        nsent += n;
    }
}

static void fresh(struct tn *t, int mode, int port)
{
    memset(t, 0, sizeof *t);
    t->send = tsend;
    t->mode = mode;
    t->cols = 80;
    t->rows = 32;
    nsent = 0;
    tn_init(t, port);
}

static long dec(struct tn *t, const char *in, long n, unsigned char *out)
{
    return tn_decode(t, (const unsigned char *)in, n, out);
}

static int sent_is(const char *b, long n) { return nsent == n && !memcmp(sent, b, n); }

static void protocol(void)
{
    struct tn t;
    unsigned char out[512];
    long k;

    fresh(&t, TN_AMIGA, 23);
    CHECK(sent_is("\377\373\000\377\375\000\377\375\003\377\373\037", 12),
          "port 23: asks at once: WILL BINARY, DO BINARY, DO SGA, WILL NAWS");

    fresh(&t, TN_AMIGA, 2323);
    CHECK(nsent == 0, "another port: nothing sent before the other side's IAC");
    k = dec(&t, "hello\r\n", 7, out);
    CHECK(k == 7 && nsent == 0 && !memcmp(out, "hello\r\n", 7), "another port: text alone starts nothing");
    k = dec(&t, "\377\375\030", 3, out);
    CHECK(k == 0 && sent_is("\377\373\000\377\375\000\377\375\003\377\373\037\377\373\030", 15),
          "another port: at the first IAC our asks go first, then WILL TTYPE for its DO");

    /* answers to our asks: agreed, no reply (no loop) */
    nsent = 0;
    k = dec(&t, "\377\375\000\377\373\000\377\373\003\377\375\037", 12, out);
    CHECK(k == 0 && tn_on(&t, 0, TO_BINARY) && tn_on(&t, 1, TO_BINARY) && tn_on(&t, 1, TO_SGA) &&
          sent_is("\377\372\037\000\120\000\040\377\360", 9),
          "agreed: BINARY both ways, SGA; NAWS agreed sends 80 x 32 at once and nothing else");
    nsent = 0;
    dec(&t, "\377\375\000\377\373\003", 6, out);
    CHECK(nsent == 0, "an agreed option asked again: no answer (RFC 1143)");
    nsent = 0;
    dec(&t, "\377\373\001", 3, out);
    CHECK(sent_is("\377\375\001", 3) && tn_on(&t, 1, TO_ECHO), "WILL ECHO: DO ECHO (the board echoes)");
    nsent = 0;
    dec(&t, "\377\375\001", 3, out);
    CHECK(sent_is("\377\374\001", 3), "DO ECHO: WONT (we never echo for them)");
    nsent = 0;
    dec(&t, "\377\375\042\377\373\005\377\375\047\377\375\310\377\373\310", 15, out);
    CHECK(sent_is("\377\374\042\377\376\005\377\374\047\377\374\310\377\376\310", 15),
          "LINEMODE, STATUS, NEW-ENVIRON, option 200: refused");
    nsent = 0;
    dec(&t, "\377\374\001", 3, out);
    CHECK(sent_is("\377\376\001", 3) && !tn_on(&t, 1, TO_ECHO), "WONT ECHO after YES: DONT, off");
    nsent = 0;
    dec(&t, "\377\374\001\377\376\042", 6, out);
    CHECK(nsent == 0, "WONT/DONT for what is off: no answer");

    /* TTYPE */
    nsent = 0;
    dec(&t, "\377\372\030\001\377\360", 6, out);
    CHECK(sent_is("\377\372\030\000ANSI\377\360", 10), "TTYPE SEND: IS ANSI (a BBS mode)");
    nsent = 0;
    dec(&t, "\377\372\030\001\377\360", 6, out);
    CHECK(sent_is("\377\372\030\000ANSI\377\360", 10), "a second TTYPE SEND: the same name");
    t.mode = TN_UNIX;
    nsent = 0;
    dec(&t, "\377\372\030\001\377\360", 6, out);
    CHECK(sent_is("\377\372\030\000XTERM-16COLOR\377\360", 19), "TTYPE in the Unix mode: XTERM-16COLOR");

    /* NAWS with a 255 doubled */
    nsent = 0;
    tn_size(&t, 255, 0x1FF);
    CHECK(sent_is("\377\372\037\000\377\377\001\377\377\377\360", 11), "NAWS: big-endian, a 255 doubled");

    /* data */
    fresh(&t, TN_AMIGA, 23);
    k = dec(&t, "a\377\377b\r\000c\r\nd", 10, out);
    CHECK(k == 8 && !memcmp(out, "a\377b\rc\r\nd", 8), "IAC IAC is 0xFF; CR NUL is CR; CR LF stays");
    dec(&t, "\377\373\000", 3, out);
    k = dec(&t, "\r\000", 2, out);
    CHECK(k == 2 && !memcmp(out, "\r\000", 2), "CR NUL kept once their BINARY is on");
    k = dec(&t, "x\377\361y\377\366z\377\360w", 10, out);
    CHECK(k == 4 && !memcmp(out, "xyzw", 4), "NOP, AYT, a stray SE: ignored");
    {
        char big[100];
        memset(big, 'x', sizeof big);
        big[0] = (char)TN_IAC; big[1] = (char)TN_SB; big[2] = TO_TTYPE; big[3] = 1;
        big[96] = (char)TN_IAC; big[97] = (char)TN_SE; big[98] = 'O'; big[99] = 'K';
        nsent = 0;
        k = dec(&t, big, 100, out);
        CHECK(k == 2 && !memcmp(out, "OK", 2) && nsent == 0, "a subnegotiation over 64 bytes: dropped whole");
    }
    nsent = 0;
    k = dec(&t, "\377\372\030\001\377\375\000Z", 8, out);
    CHECK(k == 1 && out[0] == 'Z' && nsent == 0, "IAC DO inside SB: the SB dropped, the DO taken (BINARY: agreed already, no answer)");

    /* one byte at a time = all at once */
    {
        static const char stream[] = "ab\377\375\030c\r\000\377\372\030\001\377\360d\377\377e\377\373\001f";
        unsigned char o1[64], o2[64], s1[256];
        long k1, k2 = 0, n1, i;
        fresh(&t, TN_PC, 4000);
        k1 = dec(&t, stream, sizeof stream - 1, o1);
        memcpy(s1, sent, nsent);
        n1 = nsent;
        fresh(&t, TN_PC, 4000);
        for (i = 0; i < (long)sizeof stream - 1; i++)
            k2 += dec(&t, stream + i, 1, o2 + k2);
        CHECK(k1 == k2 && !memcmp(o1, o2, k1) && n1 == nsent && !memcmp(s1, sent, n1),
              "the decoder: a byte at a time gives the same data and answers");
    }

    /* encoder */
    fresh(&t, TN_AMIGA, 23);
    k = tn_encode(&t, (const unsigned char *)"a\377b\rc", 5, out);
    CHECK(k == 7 && !memcmp(out, "a\377\377b\r\000c", 7), "encode: 0xFF doubled, CR NUL while our BINARY is off");
    dec(&t, "\377\375\000", 3, out);
    k = tn_encode(&t, (const unsigned char *)"\r\377", 2, out);
    CHECK(k == 3 && !memcmp(out, "\r\377\377", 3), "encode: CR alone once our BINARY is on");

    /* raw */
    fresh(&t, TN_AMIGA, 23);
    t.raw = 1;
    nsent = 0;
    tn_init(&t, 23);
    k = dec(&t, "\377\375\030\r\000", 5, out);
    CHECK(k == 5 && nsent == 0 && !memcmp(out, "\377\375\030\r\000", 5), "raw (-r): no decoding, no answers");
    k = tn_encode(&t, (const unsigned char *)"\377\r", 2, out);
    CHECK(k == 2, "raw (-r): no encoding");

    /* local echo */
    fresh(&t, TN_UNIX, 23);
    CHECK(tn_local_echo(&t), "Unix mode, the host does not echo: local echo");
    dec(&t, "\377\373\001", 3, out);
    CHECK(!tn_local_echo(&t), "Unix mode, the host echoes: none");
    fresh(&t, TN_AMIGA, 23);
    CHECK(!tn_local_echo(&t), "a BBS mode: never local echo");
}

/* two clients back to back, and a peer that answers everything with the opposite: the talk always ends */
static unsigned char qa[65536], qb[65536];
static long na, nb;
static void to_b(void *u, const unsigned char *b, long n) { (void)u; if (nb + n <= (long)sizeof qb) { memcpy(qb + nb, b, n); nb += n; } }
static void to_a(void *u, const unsigned char *b, long n) { (void)u; if (na + n <= (long)sizeof qa) { memcpy(qa + na, b, n); na += n; } }

static void no_loops(void)
{
    struct tn a, b;
    unsigned char out[65536], tmp[65536];
    long rounds = 0, n;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    a.send = to_b; b.send = to_a;
    a.cols = b.cols = 80; a.rows = b.rows = 25;
    na = nb = 0;
    tn_init(&a, 23);
    tn_init(&b, 23);
    while ((na || nb) && rounds < 100) {
        memcpy(tmp, qa, na); n = na; na = 0;
        tn_decode(&a, tmp, n, out);
        memcpy(tmp, qb, nb); n = nb; nb = 0;
        tn_decode(&b, tmp, n, out);
        rounds++;
    }
    CHECK(rounds < 10 && tn_on(&a, 0, TO_BINARY) && tn_on(&b, 1, TO_BINARY) && tn_on(&a, 1, TO_SGA) == tn_on(&b, 0, TO_SGA),
          "two clients back to back: the talk ends in a few rounds, both agree");

    /* a contrary peer: every WILL/DO we send is answered WONT/DONT, every WONT/DONT with WILL/DO */
    {
        struct tn t;
        long i, k;
        unsigned char peer[64];
        memset(&t, 0, sizeof t);
        t.send = to_a;
        t.cols = 80; t.rows = 32;
        na = 0;
        tn_init(&t, 23);
        rounds = 0;
        while (na && rounds < 1000) {
            memcpy(tmp, qa, na); n = na; na = 0;
            k = 0;
            for (i = 0; i + 2 < n; i++)
                if (tmp[i] == TN_IAC && tmp[i + 1] >= TN_WILL && tmp[i + 1] <= TN_DONT) {
                    int v = tmp[i + 1];
                    peer[k++] = TN_IAC;
                    peer[k++] = (unsigned char)(v == TN_WILL ? TN_DONT : v == TN_DO ? TN_WONT : v == TN_WONT ? TN_DO : TN_WILL);
                    peer[k++] = tmp[i + 2];
                    i += 2;
                    if (k > 60) break;
                }
            tn_decode(&t, peer, k, out);
            rounds++;
        }
        CHECK(rounds < 1000, "a contrary peer (refuses what we want, asks what we refuse): the talk ends");
    }
}

/* ---- the filter ---- */
static unsigned char screen[16384], ans[1024];
static long nscreen, nans;
static int evs[64][2], nev;
static void sput(void *u, const unsigned char *b, long n)
{
    (void)u;
    if (n > 256) { printf("FAIL core: a put of %ld bytes (at most 256)\n", n); fails++; }
    if (nscreen + n <= (long)sizeof screen) { memcpy(screen + nscreen, b, n); nscreen += n; }
}
static void sans(void *u, const unsigned char *b, long n) { (void)u; if (nans + n <= (long)sizeof ans) { memcpy(ans + nans, b, n); nans += n; } }
static void sev(void *u, int ev, int arg) { (void)u; if (nev < 64) { evs[nev][0] = ev; evs[nev][1] = arg; nev++; } }

static void ffresh(struct tf *f, int mode)
{
    memset(f, 0, sizeof *f);
    f->put = sput; f->answer = sans; f->event = sev;
    f->cols = 80; f->rows = 32;
    tf_init(f, mode);
    nscreen = nans = nev = 0;
}

/* in -> the screen must be want (and, byte by byte, the same) */
static void fcase(int mode, int fallback, const char *in, long n, const char *want, long wn, const char *what)
{
    struct tf f;
    unsigned char s1[16384];
    long n1, i;
    int ok;
    ffresh(&f, mode);
    f.fallback = fallback;
    tf_write(&f, (const unsigned char *)in, n);
    ok = nscreen == wn && !memcmp(screen, want, wn);
    memcpy(s1, screen, nscreen);
    n1 = nscreen;
    ffresh(&f, mode);
    f.fallback = fallback;
    for (i = 0; i < n; i++)
        tf_write(&f, (const unsigned char *)in + i, 1);
    ok = ok && n1 == nscreen && !memcmp(s1, screen, n1);
    if (!ok) {
        printf("     got (%ld):", n1);
        for (i = 0; i < n1 && i < 80; i++) printf(" %02x", s1[i]);
        printf("\n");
    }
    CHECK(ok, what);
}
#define FC(m, in, want, what) fcase(m, 0, in, sizeof in - 1, want, sizeof want - 1, what)

static void filter(void)
{
    struct tf f;
    char big[3000];
    FC(TN_PC, "a\fb", "a\x1b[2J\x1b[Hb", "FF: clear and home (PC)");
    FC(TN_AMIGA, "a\fb", "a\x1b[2J\x1b[Hb", "FF: clear and home (Amiga)");
    FC(TN_UNIX, "a\fb", "a\fb", "FF: as it is in the Unix mode");
    FC(TN_PC, "a\x1a" "b", "ab", "SUB dropped");
    FC(TN_PC, "x\x1b[NT120cde\x0ey", "xy", "music: BANSI CSI N ... SO swallowed");
    FC(TN_PC, "\x1b[|abc!", "!", "music: CSI | ends at the first byte outside the set");
    FC(TN_PC, "\x1b[Mabc", "\x1b[Mabc", "BANSI: CSI M is delete line, passed on");
    FC(TN_PC, "\x1b[=2M\x1b[MCDE\x0eZ", "Z", "CSI = 2 M: CSI M is music too");
    FC(TN_PC, "\x1b[=0M\x1b[NCDE", "\x1b[NCDE", "CSI = 0 M: CSI N is not music");
    FC(TN_PC, "\x1b[=0M\x1b[|CDE\x0e", "", "CSI = 0 M: CSI | still is");
    FC(TN_PC, "\x1b[NCD\rEF", "\rEF", "music broken by a text byte: the rest is output");
    FC(TN_PC, "\x1b[2;5MZ", "\x1b[2;5MZ", "CSI M with parameters: delete line");
    memcpy(big, "\x1b[|", 3);
    memset(big + 3, 'C', 2000);
    {
        char want[976];
        memset(want, 'C', sizeof want);
        fcase(TN_PC, 0, big, 2003, want, sizeof want, "music: at most 1,024 bytes, the rest is output");
    }
    FC(TN_AMIGA, "\x9b" "1;31mX", "\x1b[1;31mX", "Amiga: 0x9B is CSI");
    FC(TN_PC, "\x9b", "\x9b", "PC: 0x9B is a CP437 character");
    FC(TN_AMIGA, "\x9b" "80uX\x1b[u", "X\x1b[u", "Amiga: CSI n u (line length) dropped, CSI u kept");
    FC(TN_PC, "\x1b[80u", "\x1b[80u", "PC: CSI n u passed on");
    FC(TN_AMIGA, "\x9b" "0 p\x9b p\x9b" "1 p", "\x1b[?25l\x1b[?25h\x1b[?25h", "Amiga: cursor off/on -> CSI ? 25 l/h");
    FC(TN_PC, "\x1b[0 q", "\x1b[0 q", "PC: CSI 0 SP q passed on (cursor shape)");
    FC(TN_PC, "\x1b[3\x18" "1m", "1m", "CAN cancels a sequence: neither drawn");
    FC(TN_PC, "\x1b[1\r2m", "\r\x1b[12m", "a control inside a sequence runs first");
    FC(TN_PC, "\x1b[1\f2m", "\x1b[2J\x1b[H\x1b[12m", "FF inside a sequence: the clear first, the sequence whole");
    FC(TN_PC, "\x1b" "c\x1b(B", "\x1b" "c\x1b(B", "other escapes passed on");
    FC(TN_PC, "\x1b[1\x1b[2m", "\x1b[1\x1b[2m", "a sequence cut by ESC: passed on as it was");
    memset(big, '1', sizeof big);
    memcpy(big, "\x1b[", 2);
    big[100] = 'm';
    fcase(TN_PC, 0, big, 101, big, 101, "a sequence over 64 bytes: passed on whole");
    fcase(TN_PC, 1, "\xc4\xb3\xdb\x84\x01\x86\x9b\x07\x0d", 9,
          "\xe2\x94\x80\xe2\x94\x82\xe2\x96\x88\xc3\xa4\xe2\x98\xba\xc3\xa5\xc2\xa2\x07\x0d", 20,
          "PC without the font (D28): CP437 as UTF-8 (box lines, a block, a-umlaut, the smiley), BEL and CR kept");
    FC(TN_UNIX, "\xc3\xa5\xe2\x94\x80\xc3" "A\x9b\x1b[31m", "\xc3\xa5\xe2\x94\x80\xc3" "A\x9b\x1b[31m",
       "Unix (D28): the host's bytes as they are (the console decodes UTF-8)");

    ffresh(&f, TN_PC);
    tf_write(&f, (const unsigned char *)"\x1b[?33hA\x1b[?33;25lB", 18);
    CHECK(nev == 2 && evs[0][0] == TF_EV_ICE && evs[0][1] == 1 && evs[1][1] == 0 &&
          nscreen == 12 && !memcmp(screen, "A\x1b[?33;25lB", 12), "iCE: CSI ? 33 h/l events; a mixed one passed on");
    ffresh(&f, TN_AMIGA);
    tf_write(&f, (const unsigned char *)"a\x1b[ Db\x1b[0;42 Dc\x1b[1;0 D\x1b[0;36 Dd", 31);
    CHECK(nev == 2 && evs[0][0] == TF_EV_MODE && evs[0][1] == TN_PC && evs[1][1] == TN_AMIGA &&
          nscreen == 4 && !memcmp(screen, "abcd", 4) && f.mode == TN_AMIGA,
          "font switch: CSI SP D -> PC, 0;42 -> Amiga, slots 1-3 and 36 swallowed");
    ffresh(&f, TN_AMIGA);
    tf_write(&f, (const unsigned char *)"x\x9b" "0 qy\x1b[ qz", 11);
    CHECK(nans == 24 && !memcmp(ans, "\x9b" "1;1;32;80 r\x9b" "1;1;32;80 r", 24) && nscreen == 3 &&
          !memcmp(screen, "xyz", 3) && nev == 0, "Amiga: CSI 0 SP q and CSI SP q answered (0x9B 1;1;32;80 SP r), not drawn");
    ffresh(&f, TN_PC);
    tf_write(&f, (const unsigned char *)"\x1b[ D", 4);
    CHECK(nev == 0, "font switch to the mode in force: no event");
}

/* ---- keys ---- */
static void keys(void)
{
    unsigned char out[64];
    int bell = 0;
    long k;
    k = tk_keys(TN_PC, 0, (const unsigned char *)"\x7f\x1b[3~\x1b[A\x03\xe5\xe4\xf6\xc5\xc4\xd6\xa4", 16, out, &bell);
    CHECK(k == 12 && !memcmp(out, "\x08\x7f\x1b[A\x03\x86\x84\x94\x8f\x8e\x99", 12) && bell == 1,
          "PC keys: Backspace BS, Del DEL, arrows and ^C as given, Swedish letters CP437, a currency sign: the bell");
    bell = 0;
    k = tk_keys(TN_AMIGA, 0, (const unsigned char *)"\x7f\x1b[3~\xe5\x1b[2~", 10, out, &bell);
    CHECK(k == 7 && !memcmp(out, "\x08\x7f\xe5\x1b[2~", 7) && bell == 0, "Amiga keys: BS, DEL, Latin-1 as given, Help as given");
    k = tk_keys(TN_UNIX, 1, (const unsigned char *)"\x7f\x1b[3~\xc3\xa5\xe5", 9, out, &bell);
    CHECK(k == 9 && !memcmp(out, "\x7f\x1b[3~\xc3\xa5\xe5", 9), "Unix keys (D28): as given, UTF-8 not encoded twice");
    k = tk_keys(TN_PC, 0, (const unsigned char *)"\x1b[3", 3, out, &bell);
    CHECK(k == 3 && !memcmp(out, "\x1b[3", 3), "a cut ESC[3 stays as it is (no waiting for more)");
    bell = 0;
    k = tk_keys(TN_PC, 1, (const unsigned char *)"a\xc3\xa5\xc3\xa4\xe2\x94\x80\xe2\x82\xac", 11, out, &bell);
    CHECK(k == 4 && !memcmp(out, "a\x86\x84\xc4", 4) && bell == 1,
          "PC keys from a UTF-8 console (no font): a-ring, a-umlaut CP437; a pasted box line its CP437 byte; the euro the bell");
    k = tk_keys(TN_PC, 1, (const unsigned char *)"\xc3", 1, out, &bell);
    long k2 = tk_keys(TN_PC, 1, (const unsigned char *)"\xa5x", 2, out + k, &bell);
    CHECK(k == 0 && k2 == 2 && !memcmp(out, "\x86x", 2), "... a letter split between two reads is finished by the next");
    k = tk_keys(TN_PC, 1, (const unsigned char *)"\xe2", 1, out, &bell);
    k2 = tk_keys(TN_PC, 1, (const unsigned char *)"y", 1, out + k, &bell);
    CHECK(k == 0 && k2 == 2 && out[0] == 0x83 && out[1] == 'y', "... a lead followed by ASCII: the byte as Latin-1, then the letter");
    bell = 0;
    k = tk_keys(TN_AMIGA, 1, (const unsigned char *)"\xc3\xa5\xe5\xe2\x94\x80", 6, out, &bell);
    CHECK(k == 2 && out[0] == 0xE5 && out[1] == 0xE5 && bell == 1,
          "Amiga keys from a UTF-8 console: Latin-1 (a stray byte as itself); a character Latin-1 lacks: the bell");
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    protocol();
    no_loops();
    filter();
    keys();
    if (argc > 1) {
        FILE *f = fopen(argv[1], "wb");
        unsigned char ub[512];
        for (int i = 0; i < 256; i++) { ub[2 * i] = (unsigned char)(tn_cp437_ucs[i] >> 8); ub[2 * i + 1] = (unsigned char)tn_cp437_ucs[i]; }
        if (!f || fwrite(ub, 1, 512, f) != 512 || fwrite(tn_latin1_cp437, 1, 256, f) != 256) {
            printf("FAIL core: cannot write %s\n", argv[1]);
            fails++;
        }
        if (f)
            fclose(f);
    }
    printf("core: %d passed, %d failed\n", passes, fails);
    return fails != 0;
}
