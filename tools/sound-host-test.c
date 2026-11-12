/* SPDX-License-Identifier: GPL-2.0-only */
/* DESIGN D40's host test driver (tools/sound-host-test.py runs it): the sound file reader and the MOD replayer as
 * play and modplay build them (user/snddec.c, user/modrep.c), on the PC.
 *   sound-host-test dec FILE MAXRATE MONO OUT   decode FILE (read 7 bytes at a time, as a pipe would give it) into
 *                                               OUT; prints "fmt rate chans bits vol div outrate mono frames"
 *   sound-host-test mod FILE TICKS              play TICKS ticks; one line a tick: "pos row tick ended ticklen" and
 *                                               per channel "period vol flags start len loop looplen" */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../user/snddec.h"
#include "../user/modrep.h"

void *sdec_alloc(unsigned n) { return malloc(n); }
void sdec_free(void *p) { free(p); }

static int rd(void *ctx, void *buf, int n)
{
    return (int)fread(buf, 1, n > 7 ? 7 : (size_t)n, (FILE *)ctx);
}

static unsigned char *slurp(const char *path, unsigned *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    long z = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)z + 1);
    if (fread(b, 1, (size_t)z, f) != (size_t)z) { perror(path); exit(2); }
    fclose(f);
    *n = (unsigned)z;
    return b;
}

int main(int argc, char **argv)
{
    if (argc == 6 && !strcmp(argv[1], "dec")) {
        FILE *f = fopen(argv[2], "rb");
        if (!f) { perror(argv[2]); return 2; }
        unsigned char h[12];
        int n = (int)fread(h, 1, 12, f);
        fseek(f, 0, SEEK_SET);
        static struct sdec d;
        d.rd = rd;
        d.ctx = f;
        d.fmt = sdec_sniff(h, n);
        if (d.fmt == SDF_RAW) { d.rate = 8000; d.chans = 1; d.bits = 8; }
        if (sdec_open(&d) < 0) { printf("error %s\n", d.why); return 0; }
        sdec_out(&d, (unsigned)atoi(argv[3]), atoi(argv[4]));
        FILE *o = fopen(argv[5], "wb");
        signed char buf[2 * 333];
        unsigned frames = 0;
        for (;;) {
            int k = sdec_read(&d, buf, 333);
            if (k < 0) { printf("error %s\n", d.why); return 0; }
            if (!k) break;
            fwrite(buf, 1, (size_t)k * (d.mono ? 1 : 2), o);
            frames += (unsigned)k;
        }
        fclose(o);
        printf("%d %u %d %d %d %u %u %d %u\n", d.fmt, d.rate, d.chans, d.bits, d.vol, d.div, d.outrate, d.mono, frames);
        sdec_close(&d);
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "mod")) {
        unsigned n;
        unsigned char *b = slurp(argv[2], &n);
        static struct mod m;
        char why[64];
        if (mod_load(&m, b, n, why) < 0) { printf("error %s\n", why); return 0; }
        printf("load %d %d %d %u %u\n", m.nsmp, m.songlen, m.npat, mod_bankoff(&m), m.banksize);
        for (int t = atoi(argv[3]); t > 0; t--) {
            int pos = m.pos, row = m.row, tick = m.tick;
            unsigned len = mod_ticklen(&m, 709379);
            mod_tick(&m);
            printf("%d %d %d %d %u", pos, row, tick, m.ended, len);
            for (int c = 0; c < 4; c++) {
                const struct modout *o = &m.out[c];
                printf(" %d %d %d %u %u %u %u", o->period, o->vol, o->flags, o->start, o->len, o->loop, o->looplen);
            }
            printf("\n");
        }
        return 0;
    }
    fprintf(stderr, "usage: sound-host-test dec FILE MAXRATE MONO OUT | mod FILE TICKS\n");
    return 2;
}
