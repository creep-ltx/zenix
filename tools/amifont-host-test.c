/* SPDX-License-Identifier: GPL-2.0-only */
/* Host test of the AmigaOS bitmap font reader (display/server/amifont.c, the file displayd builds): the fonts and
 * damaged files tools/mkamifont.py cases writes, each read and compared with what the generator says it holds (the
 * metrics, the glyph count, a checksum of the converted pictures, widths), every damaged one refused with a reason;
 * then thousands of mutations of a good font (bytes changed, cut at every length) read under AddressSanitizer: each
 * read or refused, never a read outside. Run by tools/amifont-host-test.sh.
 *
 * usage: amifont-host-test DIR              (DIR/cases.txt)
 *        amifont-host-test -r FONTSDIR      read every NAME.font there and each of its sizes (a local check of real
 *                                          fonts: prints, compares nothing) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include "../display/server/amifont.h"

static int fails;
static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        fails++;
}
static unsigned char *slurp(const char *path, unsigned *n)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    unsigned char *b = 0;
    unsigned sz = 0, cap = 0;
    for (;;) {
        if (sz == cap) {
            cap = cap ? cap * 2 : 4096;
            b = realloc(b, cap);
        }
        size_t r = fread(b + sz, 1, cap - sz, f);
        if (!r)
            break;
        sz += (unsigned)r;
    }
    fclose(f);
    /* an exact-size copy: AddressSanitizer catches a read one byte past the file */
    unsigned char *e = malloc(sz ? sz : 1);
    memcpy(e, b, sz);
    free(b);
    *n = sz;
    return e;
}
static void *al(unsigned n) { return malloc(n); }
static unsigned crc(const unsigned char *b, unsigned n)
{
    unsigned v = 0x811C9DC5u;
    for (unsigned i = 0; i < n; i++)
        v = (v ^ b[i]) * 0x01000193u;
    return v;
}
static void describe(const struct af_font *f, char *o, size_t sz)
{
    int aA = f->g['A'].have ? f->g['A'].adv : -1, ai = f->g['i'].have ? f->g['i'].adv : -1;
    snprintf(o, sz, "ok %u %u %u %u %u %u %u %08x %d %d %d", f->h, f->base, f->xsize, f->prop, f->colour, f->nglyphs,
             f->nbytes, f->nbytes ? crc(f->bits, f->nbytes) : crc((const unsigned char *)"", 0), aA, ai,
             f->g['A'].have ? f->g['A'].kern : 0);
}

static void cases(const char *dir)
{
    char path[512], line[512];
    snprintf(path, sizeof path, "%s/cases.txt", dir);
    FILE *c = fopen(path, "r");
    if (!c) {
        check(0, "cases.txt is there");
        return;
    }
    int n = 0;
    while (fgets(line, sizeof line, c)) {
        line[strcspn(line, "\n")] = 0;
        char kind[16], name[128], rest[400];
        if (sscanf(line, "%15s %127s %399[^\n]", kind, name, rest) != 3)
            continue;
        snprintf(path, sizeof path, "%s/%s", dir, name);
        unsigned len;
        unsigned char *d = slurp(path, &len);
        if (!d) {
            check(0, name);
            continue;
        }
        char got[400], what[900];
        if (!strcmp(kind, "font")) {
            struct af_font f;
            const char *why = af_load(d, len, &f, al);
            if (why)
                snprintf(got, sizeof got, "bad");
            else
                describe(&f, got, sizeof got);
            snprintf(what, sizeof what, "font %s: %s%s%s", name, got, why ? ": " : "", why ? why : "");
            check(!strcmp(got, rest), what);
            if (strcmp(got, rest))
                printf("     wanted %s\n", rest);
            if (!why)
                free(f.bits);
        } else {
            int size = atoi(rest);
            const char *want = strchr(rest, ' ') + 1;
            char file[256], sizes[64];
            const char *why = af_contents(d, len, size, file, sizeof file, sizes, sizeof sizes);
            snprintf(got, sizeof got, why ? "bad" : "ok %s", file);
            snprintf(what, sizeof what, "contents %s size %d: %s%s%s (lists %s)", name, size, got, why ? ": " : "",
                     why ? why : "", sizes);
            check(!strcmp(got, want), what);
        }
        free(d);
        n++;
    }
    fclose(c);
    check(n >= 40, "the generator's cases were all read");
}

/* mutations of a good font: a byte (or a few) changed at every place, and the file cut at every length */
static void fuzz(const char *dir)
{
    char path[512];
    snprintf(path, sizeof path, "%s/ztest-11", dir);
    unsigned len;
    unsigned char *good = slurp(path, &len);
    if (!good) {
        check(0, "ztest-11 for the mutations");
        return;
    }
    unsigned ok = 0, bad = 0;
    srand(1);
    for (unsigned cut = 0; cut <= len; cut++) {
        unsigned char *d = malloc(cut ? cut : 1);
        memcpy(d, good, cut);
        struct af_font f;
        if (!af_load(d, cut, &f, al)) { ok++; free(f.bits); } else bad++;
        free(d);
    }
    for (int round = 0; round < 20000; round++) {
        unsigned char *d = malloc(len);
        memcpy(d, good, len);
        int k = 1 + rand() % 3;
        for (int j = 0; j < k; j++) {
            unsigned at = (unsigned)rand() % len;
            unsigned v = (unsigned)rand();
            d[at] = (unsigned char)(v & 0x100 ? v >> 9 : v & 0x80 ? 0xFF : v & 0x40 ? 0 : d[at] ^ (1u << (v & 7)));
        }
        struct af_font f;
        if (!af_load(d, len, &f, al)) {
            ok++;
            if (f.h < 1 || f.h > AF_MAXH || f.nbytes > AF_MAXBYTES)
                check(0, "a mutated font read with metrics in range");
            for (int c = 0; c < 256; c++)
                if (f.g[c].have && (unsigned)f.g[c].off + (unsigned)f.g[c].bpr * f.h > f.nbytes)
                    check(0, "a mutated font's glyph inside its pictures");
            free(f.bits);
        } else
            bad++;
        free(d);
    }
    printf("     %u read, %u refused\n", ok, bad);
    check(bad > 0 && ok > 0, "every mutation of ztest 11 read or refused, nothing read outside (AddressSanitizer)");
    free(good);
    snprintf(path, sizeof path, "%s/c-sizes", dir);
    good = slurp(path, &len);
    if (good) {
        for (int round = 0; round < 5000; round++) {
            unsigned char *d = malloc(len);
            memcpy(d, good, len);
            d[(unsigned)rand() % len] = (unsigned char)rand();
            unsigned cut = (unsigned)rand() % (len + 1);
            char file[256];
            af_contents(d, round & 1 ? cut : len, 11, file, sizeof file, 0, 0);
            free(d);
        }
        check(1, "mutated contents files read or refused, nothing read outside");
        free(good);
    }
}

static void real(const char *dir)
{
    DIR *dp = opendir(dir);
    struct dirent *e;
    if (!dp) {
        perror(dir);
        return;
    }
    while ((e = readdir(dp))) {
        size_t l = strlen(e->d_name);
        if (l < 6 || strcmp(e->d_name + l - 5, ".font"))
            continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        unsigned n;
        unsigned char *d = slurp(path, &n);
        if (!d)
            continue;
        char sizes[128], file[256];
        af_contents(d, n, -1, file, sizeof file, sizes, sizeof sizes);
        printf("%s: sizes %s\n", e->d_name, sizes);
        for (char *s = sizes; *s;) {
            int sz = atoi(s);
            const char *why = af_contents(d, n, sz, file, sizeof file, 0, 0);
            if (why) {
                printf("  %d: %s\n", sz, why);
            } else {
                snprintf(path, sizeof path, "%s/%s", dir, file);
                unsigned fn;
                unsigned char *fd = slurp(path, &fn);
                struct af_font f;
                const char *w2 = fd ? af_load(fd, fn, &f, al) : "not there";
                if (w2)
                    printf("  %d %s: %s\n", sz, file, w2);
                else {
                    printf("  %d %s: h %u base %u xsize %u prop %u colour %u glyphs %u bytes %u lo %u hi %u\n", sz,
                           file, f.h, f.base, f.xsize, f.prop, f.colour, f.nglyphs, f.nbytes, f.lo, f.hi);
                    free(f.bits);
                }
                free(fd);
            }
            while (*s && *s != ' ') s++;
            while (*s == ' ') s++;
        }
        free(d);
    }
    closedir(dp);
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "-r")) {
        real(argv[2]);
        return 0;
    }
    if (argc != 2) {
        fprintf(stderr, "usage: amifont-host-test DIR | -r FONTSDIR\n");
        return 2;
    }
    cases(argv[1]);
    fuzz(argv[1]);
    printf("%s\n", fails ? "amifont-host-test: FAILED" : "amifont-host-test: all passed");
    return fails != 0;
}
