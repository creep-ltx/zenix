/* SPDX-License-Identifier: GPL-2.0-only */
/* DESIGN D56's host test driver (tools/trk-host-test.py runs it): the tracker player's replayers and mixer
 * (user/trk.c, modrep.c, medrep.c, s3mrep.c, xmrep.c, trkmix.c) as modplay builds them, on the PC.
 *   trk-host-test trk FILE TICKS        load FILE and play TICKS ticks: a head line "load KIND NCH SONGLEN NINST
 *                                       NATIVE BANKSIZE", then one line a tick: "pos row tick ended tnum tden" and per
 *                                       channel "flags per hz16 vol pan sp start len loop looplen" (sp: the sample's
 *                                       place in the file, -1 another block, -2 none)
 *   trk-host-test mix FILE RATE FRAMES OUT   the mixer's bytes (left, right) for FRAMES frames at RATE into OUT
 *   trk-host-test cells FILE PAT ROW    the pattern view's cells of one row */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../user/trk.h"
#include "../user/trkmix.h"

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
    if (argc < 3) goto usage;
    unsigned n;
    unsigned char *b = slurp(argv[2], &n);
    static struct trk t;
    char why[64];
    if (trk_load(&t, b, n, why) < 0) { printf("error %s\n", why); free(b); return 0; }
    if (!strcmp(argv[1], "trk") && argc == 4) {
        printf("load %s|%d %d %d %d %u\n", t.kind, t.nch, t.songlen, t.ninst, t.native, t.banksize);
        for (int k = atoi(argv[3]); k > 0; k--) {
            unsigned tn = t.tnum, td = t.tden;
            trk_tick(&t);
            printf("%d %d %d %d %u %u", t.pos, t.row, t.tick, t.ended, tn, td);
            for (int c = 0; c < t.nch; c++) {
                const struct tvoice *v = &t.v[c];
                long sp = !v->sp ? -2 : (v->sp >= (const signed char *)b && v->sp <= (const signed char *)b + n) ?
                          (long)(v->sp - (const signed char *)b) : -1;
                printf(" %d %d %u %d %d %ld %u %u %u %u", v->flags, v->per, v->hz16, v->vol, v->pan, sp, v->start, v->len,
                       v->loop, v->looplen);
                /* every place the mixer would read must be inside its block */
                if (v->flags & 1 && v->sp && sp >= 0 && ((unsigned long)sp + v->start + v->len > n ||
                    (v->looplen && (unsigned long)sp + v->loop + v->looplen > n))) { printf(" OUTSIDE\n"); return 3; }
                /* a native module's places must be inside its bank (what the sound server gets) */
                if (t.native && (v->flags & 1) && v->sp && (v->len || v->looplen) &&
                    (v->sp + v->start < t.bank || v->sp + v->start + v->len > t.bank + t.banksize ||
                     (v->looplen && (v->sp + v->loop < t.bank || v->sp + v->loop + v->looplen > t.bank + t.banksize)))) {
                    printf(" OUTSIDE-BANK\n");
                    return 3;
                }
            }
            printf("\n");
            if (t.stop) break;
        }
    } else if (!strcmp(argv[1], "mix") && argc == 6) {
        static struct mix m;
        unsigned rate = (unsigned)atoi(argv[3]);
        if (mix_init(&m, rate, 3546895, t.nch) < 0) return 2;
        FILE *o = fopen(argv[5], "wb");
        int frames = atoi(argv[4]);
        unsigned acc = 0, fnv = 2166136261u;
        static signed char buf[2 * 8192];
        while (frames > 0) {                                /* the tick's frames as modplay counts them */
            acc += rate * t.tnum;
            unsigned len = acc / (t.tden ? t.tden : 1);
            acc -= len * (t.tden ? t.tden : 1);
            if (len > 8192) len = 8192;
            if ((int)len > frames) len = (unsigned)frames;
            trk_tick(&t);
            mix_set(&m, t.v);
            mix_render(&m, buf, (int)len);
            fwrite(buf, 1, (size_t)len * 2, o);
            for (unsigned i = 0; i < 2 * len; i++) fnv = (fnv ^ (unsigned char)buf[i]) * 16777619u;
            frames -= (int)len;
            if (t.stop && !len) break;
        }
        fclose(o);
        mix_free(&m);
        printf("amp %d fnv %08x\n", m.amp, fnv);
    } else if (!strcmp(argv[1], "cells") && argc == 5) {
        char cell[16];
        for (int c = 0; c < t.nch; c++) {
            t.ops->cell(&t, atoi(argv[3]), atoi(argv[4]), c, cell);
            printf("%s|", cell);
        }
        printf("\n");
    } else
        goto usage;
    trk_free(&t);
    free(b);
    return 0;
usage:
    fprintf(stderr, "usage: trk-host-test trk FILE TICKS | mix FILE RATE FRAMES OUT | cells FILE PAT ROW\n");
    return 2;
}
