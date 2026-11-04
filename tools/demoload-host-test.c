/* SPDX-License-Identifier: GPL-2.0-only */
/* The demo floppy's unpacking on the PC (DESIGN D25 revision 3, 10 step 2; tools/demoload-host-test.sh): the
 * loader's own source (boot/demounpack.c with miniz's tinfl) unpacks an ADF made by tools/mkdemo.py, piece by
 * piece as the stage 2 reads it (a track at a time, and in random pieces), and must give the kernel and the RAM
 * disk's image byte for byte. Then damaged disks: the stream cut short, bytes flipped, the table's sizes wrong, the
 * table itself damaged, and random damage (a fuzzer): every one is refused cleanly, never accepted with other data;
 * built with the address and undefined-behaviour sanitizers, which stop the test at any read or write outside.
 * usage: demoload-host-test ADF KERNEL IMAGE [FUZZ_ROUNDS] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "demounpack.h"

static unsigned char *adf, *want;
static long adf_len;
static unsigned want_len, table_off;
static struct dtab tab;
static int fails, accepted_same;      /* damage deflate ignores: the same data came out (counted, said) */

static unsigned char *slurp(const char *p, long *n)
{
    FILE *f = fopen(p, "rb");
    if (!f) { perror(p); exit(2); }
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(*n ? *n : 1);
    if (fread(b, 1, *n, f) != (size_t)*n) { perror(p); exit(2); }
    fclose(f);
    return b;
}

static unsigned rnd_state = 12345;
static unsigned rnd(void) { rnd_state = rnd_state * 1103515245u + 12345u; return rnd_state >> 8; }

/* unpacks stream (len bytes as the table says, in_len for du_start) into an exact-size buffer; piece 0 = by track
 * as the loader, else random pieces up to that size. Returns du_feed's result; *out gets the buffer (to free). */
static int unpack(const unsigned char *stream, unsigned in_len, unsigned avail, unsigned out_len, unsigned piece,
                  unsigned char **out, const char **why)
{
    struct dunpack *u = malloc(du_state_size());
    unsigned char *o = malloc(out_len);
    du_start(u, o, out_len, in_len);
    unsigned off = 0, disk = tab.stream_off;
    int r = 0;
    while (r == 0 && off < avail) {
        unsigned n = piece ? 1 + rnd() % piece : tab.track - (disk + off) % tab.track;
        if (n > avail - off) n = avail - off;
        r = du_feed(u, stream + off, n);
        off += n;
    }
    if (r == 0) r = -100;               /* all given, more wanted (the loader says "ends too early") */
    *why = r < 0 && r != -100 ? du_why(r) : r == -100 ? "ran out of input" : 0;
    if (r == 1 && du_out_done(u) != out_len) { printf("FAIL: done with %u of %u bytes\n", du_out_done(u), out_len); fails++; }
    free(u);
    *out = o;
    return r;
}

static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
}

/* a damaged copy of the stream must be refused, or (only if the damage changed nothing that counts) give the same */
static int refused_or_same(const unsigned char *s, unsigned in_len, unsigned avail, unsigned out_len)
{
    unsigned char *o; const char *why;
    int r = unpack(s, in_len, avail, out_len, rnd() % 2 ? 0 : 1 + rnd() % 9000, &o, &why);
    int good = r != 1 || (out_len == want_len && !memcmp(o, want, want_len) && !du_check_parts(o, &tab));
    if (r == 1 && good) accepted_same++;
    free(o);
    return good;
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: %s ADF KERNEL IMAGE [FUZZ_ROUNDS]\n", argv[0]); return 2; }
    long kn, in;
    adf = slurp(argv[1], &adf_len);
    unsigned char *k = slurp(argv[2], &kn), *img = slurp(argv[3], &in);
    int rounds = argc > 4 ? atoi(argv[4]) : 300;
    unsigned kpad = (kn + 511) & ~511u;
    want_len = kpad + in;
    want = calloc(1, want_len);
    memcpy(want, k, kn);
    memcpy(want + kpad, img, in);
    table_off = 1024 + ((unsigned)adf[8] << 24 | adf[9] << 16 | adf[10] << 8 | adf[11]);
    const char *why = dt_check(adf + table_off, table_off, &tab);
    check(!why && tab.disk_len == (unsigned)adf_len, "the table is accepted and names this disk's size");
    if (why) { printf("  %s\n", why); return 1; }
    check(tab.kern_len == kpad && tab.image_len == (unsigned)in, "the table's sizes are the kernel's and the image's");
    const unsigned char *stream = adf + tab.stream_off;
    unsigned slen = tab.stream_len, savail = (slen + 511) & ~511u, total = tab.kern_len + tab.image_len;

    unsigned char *o;
    int r = unpack(stream, slen, savail, total, 0, &o, &why);
    check(r == 1 && !memcmp(o, want, want_len), "unpacked a track at a time: kernel and image byte for byte");
    check(!du_check_parts(o, &tab), "the parts look right (ZEXE kernel, its memory size, ext2 image)");
    free(o);
    int same = 1;
    for (int i = 0; i < 5; i++) {
        r = unpack(stream, slen, savail, total, i == 0 ? 1 : 7 + rnd() % 30000, &o, &why);
        same &= r == 1 && !memcmp(o, want, want_len);
        free(o);
    }
    check(same, "unpacked in random pieces (one byte at a time once): the same bytes");

    /* the stream cut short: the table's length is the real one, the disk ends early */
    int ok = 1;
    unsigned cuts[] = { 0, 1, 2, 5, 100, slen / 2, slen - 5, slen - 4, slen - 1 };
    for (unsigned i = 0; i < sizeof cuts / sizeof cuts[0]; i++) {
        r = unpack(stream, slen, cuts[i], total, 0, &o, &why);
        ok &= r < 0;
        free(o);
    }
    check(ok, "a stream cut short anywhere (0 bytes to the last byte) is refused");
    /* the table's stream length too short (the end not fed) or too long (zeros after the end) */
    r = unpack(stream, slen - 1, savail, total, 0, &o, &why); free(o);
    int r2 = -1;
    if (slen < savail) { r2 = unpack(stream, slen + 1, savail, total, 0, &o, &why); free(o); }
    check(r < 0 && r2 < 0, "a table whose stream length is one byte short or long is refused");
    r = unpack(stream, slen, savail, total - 512, 0, &o, &why); free(o);
    r2 = unpack(stream, slen, savail, total + 512, 0, &o, &why); free(o);
    check(r < 0 && r2 < 0, "unpacked sizes 512 bytes smaller or bigger than the stream's are refused");

    /* one flipped byte anywhere in the stream: refused (the deflate codes or the Adler-32 see it) */
    unsigned char *s = malloc(savail);
    ok = 1;
    for (int i = 0; i < rounds; i++) {
        memcpy(s, stream, savail);
        unsigned at = i < 8 ? (i < 4 ? (unsigned)i : slen - 8 + i) : rnd() % slen;
        s[at] ^= 1 << (rnd() % 8);
        /* (a bit deflate ignores, such as the padding after a stored block's header, may give the same data) */
        if (!refused_or_same(s, slen, savail, total)) { printf("  a flip at %u gave other data\n", at); ok = 0; }
    }
    check(ok, "one flipped bit anywhere in the stream (the first 4 and last 4 bytes too): refused, or the same data");

    /* the table damaged: every one of its 128 longs changed in turn; refused by its checksum */
    unsigned char t[512];
    ok = 1;
    for (int i = 0; i < 512; i += 4) {
        memcpy(t, adf + table_off, 512);
        t[i + 3] ^= 0x10;
        struct dtab x;
        if (!dt_check(t, table_off, &x)) ok = 0;
    }
    check(ok, "a table with any long changed is refused (its checksum)");
    /* the table's fields wrong but its checksum fixed up: each range check holds */
    const struct { int off; unsigned v; } bad[] = {
        { DT_O_MAGIC, 0 }, { DT_O_VERSION, 2 }, { DT_O_STREAM_OFF, 0 }, { DT_O_STREAM_OFF, 1536 + 1 },
        { DT_O_STREAM_OFF, 1802240 }, { DT_O_STREAM_LEN, 0 }, { DT_O_STREAM_LEN, 0xFFFFFFFF },
        { DT_O_STREAM_LEN, tab.disk_len - tab.stream_off + 1 }, { DT_O_KERN_LEN, 0 }, { DT_O_KERN_LEN, 513 }, { DT_O_KERN_LEN, 0x80000000 },
        { DT_O_KERN_MEM, 0 }, { DT_O_KERN_MEM, 0xFFFFFFF0 }, { DT_O_IMAGE_LEN, 1024 }, { DT_O_IMAGE_LEN, 0x7FFFFC00 },
        { DT_O_IMAGE_LEN, 3000 }, { DT_O_DISK_LEN, 0xFFFFFE00 }, { DT_O_DISK_LEN, 901121 }, { DT_O_TRACK, 512 },
        { DT_O_TRACK, 0 },
    };
    ok = 1;
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        memcpy(t, adf + table_off, 512);
        t[bad[i].off] = bad[i].v >> 24; t[bad[i].off + 1] = bad[i].v >> 16; t[bad[i].off + 2] = bad[i].v >> 8; t[bad[i].off + 3] = bad[i].v;
        unsigned sum = 0;
        for (int j = 0; j < 508; j += 4) sum += (unsigned)t[j] << 24 | t[j + 1] << 16 | t[j + 2] << 8 | t[j + 3];
        sum = -sum;
        t[508] = sum >> 24; t[509] = sum >> 16; t[510] = sum >> 8; t[511] = sum;
        struct dtab x;
        if (!dt_check(t, table_off, &x)) { printf("  field %d = 0x%x accepted\n", bad[i].off, bad[i].v); ok = 0; }
    }
    check(ok, "a table with a field out of range (checksum right) is refused: 20 cases");
    /* the parts: a good stream of a kernel that is not one, or an image that is not ext2 */
    {
        struct dtab x = tab;
        unsigned char *c = malloc(total);
        memcpy(c, want, total); c[5] = 'Q';
        int a = du_check_parts(c, &x) != 0;
        memcpy(c, want, total); c[kpad + 1080] = 0;
        int b = du_check_parts(c, &x) != 0;
        x.kern_mem++;
        memcpy(c, want, total);
        int d = du_check_parts(c, &x) != 0;
        free(c);
        check(a && b && d, "a kernel that is not ZEXE, an image that is not ext2, a wrong memory size: refused");
    }

    /* the fuzzer: random damage (1-8 bytes changed, a random cut, random pieces); never a crash or a read or write
     * outside (the sanitizers), never an accepted stream that differs */
    ok = 1;
    for (int i = 0; i < rounds; i++) {
        memcpy(s, stream, savail);
        int nb = 1 + rnd() % 8;
        for (int j = 0; j < nb; j++) s[rnd() % slen] = rnd();
        unsigned avail = rnd() % 4 ? savail : rnd() % savail;
        unsigned in_len = rnd() % 8 ? slen : rnd() % (slen + 600);
        unsigned out_len = rnd() % 8 ? total : rnd() % (total + 4096);
        if (!refused_or_same(s, in_len, avail, out_len)) { printf("  round %d accepted other data\n", i); ok = 0; }
    }
    check(ok, "random damage: refused or the same data, never anything else");
    /* the table fuzzed: random bytes in a valid table (with its checksum fixed): accepted tables are in range */
    ok = 1;
    for (int i = 0; i < rounds * 10; i++) {
        memcpy(t, adf + table_off, 512);
        int nb = 1 + rnd() % 4;
        for (int j = 0; j < nb; j++) t[rnd() % 40] = rnd();
        unsigned sum = 0;
        for (int j = 0; j < 508; j += 4) sum += (unsigned)t[j] << 24 | t[j + 1] << 16 | t[j + 2] << 8 | t[j + 3];
        sum = -sum;
        t[508] = sum >> 24; t[509] = sum >> 16; t[510] = sum >> 8; t[511] = sum;
        struct dtab x;
        if (dt_check(t, table_off, &x)) continue;
        if (x.stream_off + (unsigned long long)x.stream_len > x.disk_len || x.stream_off < table_off + 512 ||
            x.kern_len > DT_KERN_MAX || x.image_len > DT_IMAGE_MAX || x.disk_len > DT_DISK_MAX) ok = 0;
    }
    check(ok, "random tables: whatever is accepted lies inside the disk and the limits");
    free(s); free(k); free(img); free(adf); free(want);
    printf("%s: %d failed (%d damaged streams gave the same data: bits deflate ignores)\n", fails ? "FAIL" : "PASS", fails, accepted_same);
    return fails != 0;
}
