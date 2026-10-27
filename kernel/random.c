/* SPDX-License-Identifier: GPL-2.0-only */
/* Random numbers (DESIGN D19, docs/d19-network.md section 9.3): the kernel's entropy pool, /dev/urandom and /dev/random.
 *
 * As Linux since 5.17: samples are accumulated with BLAKE2s (RFC 7693), output comes from ChaCha20 (RFC 8439) with
 * fast key erasure. /dev/urandom never waits; /dev/random waits until the pool was first seeded with an estimated 256
 * bits, then is the same, and polls readable from then on (Linux since 5.6).
 *
 * The sources: interrupt handlers only store a 16-bit sample (the beam position, VHPOSR: one chip-bus read) in a small
 * ring with the credit it earns (kbd.c, serial.c; floppy.c in its polled wait); process context (kwork, rnd_work) mixes
 * the ring into the pool. BLAKE2s keeps its own 64-byte block buffer, so mixing a sample is a copy and a compression
 * runs only once a block is full: samples are mixed in batches, never one hash per event.
 * Credited, in eighths of a bit: a key press or release 8 (the keyboard's own controller has its own clock), the end
 * of a SLIP frame received (0xC0, the far machine's clock) 4, the floppy's index pulse while the motor runs 8 (motor
 * speed jitter), the Pi's RNG200 (when stage0 found it) 32 a word. Mixed but credited 0: disk and SD completions,
 * mouse movement, the boot's clocks and a sparse hash of chip RAM, anything a program writes to /dev/urandom.
 * Root's write to /dev/random credits 8 bits (64 eighths) a byte: the seed file at boot (user/randseed.h decides
 * when; never on a RAM-disk, read-only or rescue root).
 *
 * Built for the host too (tools/random-host-test.c, RANDOM_HOST): the hashes against the RFCs' vectors and the
 * crediting rules. */
#ifdef RANDOM_HOST
#include <stdint.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#else
#include "kernel.h"
#include "hw.h"
#include "hwinfo.h"
#endif
#include "random.h"

/* ---- BLAKE2s (RFC 7693), little-endian words read byte by byte: the same code on the 68k and the host ---- */
static inline u32 ld32(const u8 *p) { return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static inline void st32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static inline u32 ror32(u32 v, int n) { return v >> n | v << (32 - n); }
static inline u32 rol32(u32 v, int n) { return v << n | v >> (32 - n); }

static const u32 b2s_iv[8] = {
    0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au, 0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u
};
static const u8 b2s_sigma[10][16] = {
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 }, { 14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3 },
    { 11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4 }, { 7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8 },
    { 9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13 }, { 2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9 },
    { 12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11 }, { 13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10 },
    { 6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5 }, { 10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0 },
};

/* the quarter rounds' words, for BLAKE2s's G and ChaCha20's QR alike: the four columns, then the four diagonals */
static const u8 cc_ix[8][4] = { { 0, 4, 8, 12 }, { 1, 5, 9, 13 }, { 2, 6, 10, 14 }, { 3, 7, 11, 15 },
                                 { 0, 5, 10, 15 }, { 1, 6, 11, 12 }, { 2, 7, 8, 13 }, { 3, 4, 9, 14 } };

static void b2s_compress(struct b2s *s, int last)
{
    u32 v[16], m[16];
    for (int i = 0; i < 16; i++)
        m[i] = ld32(s->buf + 4 * i);
    for (int i = 0; i < 8; i++) {
        v[i] = s->h[i];
        v[i + 8] = b2s_iv[i];
    }
    v[12] ^= s->t[0];
    v[13] ^= s->t[1];
    if (last)
        v[14] = ~v[14];
    for (int r = 0; r < 10; r++) {
        const u8 *sg = b2s_sigma[r];
        for (int i = 0; i < 8; i++) {           /* the four columns, then the four diagonals */
            u32 *a = &v[cc_ix[i][0]], *b = &v[cc_ix[i][1]], *c = &v[cc_ix[i][2]], *d = &v[cc_ix[i][3]];
            *a += *b + m[sg[2 * i]];
            *d = ror32(*d ^ *a, 16);
            *c += *d;
            *b = ror32(*b ^ *c, 12);
            *a += *b + m[sg[2 * i + 1]];
            *d = ror32(*d ^ *a, 8);
            *c += *d;
            *b = ror32(*b ^ *c, 7);
        }
    }
    for (int i = 0; i < 8; i++)
        s->h[i] ^= v[i] ^ v[i + 8];
}

void b2s_init(struct b2s *s, u32 outlen, const void *key, u32 keylen)
{
    for (int i = 0; i < 8; i++)
        s->h[i] = b2s_iv[i];
    s->h[0] ^= 0x01010000u ^ keylen << 8 ^ outlen;
    s->t[0] = s->t[1] = 0;
    s->n = 0;
    s->outlen = (u8)outlen;
    memset(s->buf, 0, sizeof s->buf);
    if (keylen) {
        b2s_update(s, key, keylen);
        s->n = 64;                              /* the key is a whole block of its own */
    }
}

void b2s_update(struct b2s *s, const void *in, u32 len)
{
    const u8 *p = in;
    while (len--) {
        if (s->n == 64) {                       /* a full block waits: compress it only when more input follows */
            s->t[0] += 64;
            if (s->t[0] < 64)
                s->t[1]++;
            b2s_compress(s, 0);
            s->n = 0;
        }
        s->buf[s->n++] = *p++;
    }
}

void b2s_final(struct b2s *s, void *out)
{
    s->t[0] += s->n;
    if (s->t[0] < s->n)
        s->t[1]++;
    while (s->n < 64)
        s->buf[s->n++] = 0;
    b2s_compress(s, 1);
    u8 h[32];
    for (int i = 0; i < 8; i++)
        st32(h + 4 * i, s->h[i]);
    memcpy(out, h, s->outlen);
    memset(h, 0, sizeof h);
}

void b2s(void *out, u32 outlen, const void *key, u32 keylen, const void *in, u32 len)
{
    struct b2s s;
    b2s_init(&s, outlen, key, keylen);
    b2s_update(&s, in, len);
    b2s_final(&s, out);
    memset(&s, 0, sizeof s);
}

/* ---- ChaCha20's block function (RFC 8439 2.3): key 32 bytes, a 32-bit block counter, a 96-bit nonce ---- */
void chacha20_block(const u8 *key, u32 counter, const u8 *nonce, u8 *out)
{
    u32 x[16], s[16];
    s[0] = 0x61707865u; s[1] = 0x3320646Eu; s[2] = 0x79622D32u; s[3] = 0x6B206574u;   /* "expand 32-byte k" */
    for (int i = 0; i < 8; i++)
        s[4 + i] = ld32(key + 4 * i);
    s[12] = counter;
    for (int i = 0; i < 3; i++)
        s[13 + i] = ld32(nonce + 4 * i);
    for (int i = 0; i < 16; i++)
        x[i] = s[i];
    for (int r = 0; r < 80; r++) {              /* 10 double rounds: the four columns, then the four diagonals */
        const u8 *q = cc_ix[r & 7];
        u32 *a = &x[q[0]], *b = &x[q[1]], *c = &x[q[2]], *d = &x[q[3]];
        *a += *b; *d = rol32(*d ^ *a, 16);
        *c += *d; *b = rol32(*b ^ *c, 12);
        *a += *b; *d = rol32(*d ^ *a, 8);
        *c += *d; *b = rol32(*b ^ *c, 7);
    }
    for (int i = 0; i < 16; i++)
        st32(out + 4 * i, x[i] + s[i]);
    memset(x, 0, sizeof x);
    memset(s, 0, sizeof s);
}

/* ---- the pool ---- */
static struct b2s pool;                 /* every sample, in order */
static u8 crng_key[32];                 /* the output generator's key: replaced at every read (fast key erasure) */
static u32 pool_new;                    /* bytes mixed since the last reseed */
u32 rnd_credit8;                        /* credited eighths of a bit since the start (until seeded; it then stays) */
int rnd_seeded;                         /* the pool had 256 bits: /dev/random no longer waits */
static u32 reseed_at;                   /* the tick of the last reseed */

/* the generator's key from the pool: a hash of the pool's state so far becomes part of the key, and the pool is keyed
 * with another hash of it (what is read can never be traced back to the samples, and the pool goes on) */
static void reseed(void)
{
    struct b2s c = pool;
    u8 s[32], k[32];
    b2s_final(&c, s);
    struct b2s h;
    b2s_init(&h, 32, s, 32);
    b2s_update(&h, "\x01", 1);
    b2s_update(&h, crng_key, 32);
    b2s_final(&h, crng_key);
    b2s(k, 32, s, 32, "\x02", 1);
    b2s_init(&pool, 32, k, 32);
    memset(s, 0, sizeof s);
    memset(k, 0, sizeof k);
    memset(&c, 0, sizeof c);
    pool_new = 0;
}

/* mix bytes into the pool, credit eighths of a bit; crossing 256 bits seeds the generator. 1 when this call seeded it */
int rnd_mix(const void *p, u32 n, u32 credit8)
{
    b2s_update(&pool, p, n);
    pool_new += n;
    if (rnd_seeded)
        return 0;
    rnd_credit8 += credit8;
    if (rnd_credit8 < RND_SEED_BITS * 8)
        return 0;
    reseed();
    rnd_seeded = 1;
    return 1;
}

/* n bytes of output. Before the first seeding every read reseeds from what the pool has (the best there is); after it,
 * at most once a minute, and only when something new was mixed. Fast key erasure: the first block's first half is the
 * next key, its second half and the following blocks (from the old key, kept only here) the output */
void rnd_bytes(void *out, u32 n, u32 now)
{
    if (pool_new && (!rnd_seeded || now - reseed_at >= RND_RESEED_TICKS)) {
        reseed();
        reseed_at = now;
    }
    static const u8 nonce[12];
    u8 blk[64], key[32];
    u8 *o = out;
    memcpy(key, crng_key, 32);
    chacha20_block(key, 0, nonce, blk);
    memcpy(crng_key, blk, 32);
    u32 k = n < 32 ? n : 32;
    memcpy(o, blk + 32, k);
    o += k;
    n -= k;
    for (u32 ctr = 1; n; ctr++) {
        chacha20_block(key, ctr, nonce, blk);
        k = n < 64 ? n : 64;
        memcpy(o, blk, k);
        o += k;
        n -= k;
    }
    memset(blk, 0, sizeof blk);
    memset(key, 0, sizeof key);
}

#ifndef RANDOM_HOST
/* ---- the sample ring: filled by interrupt handlers (one 16-bit sample and its credit), emptied by rnd_work ---- */
#define RQ 32
static volatile u16 rq[RQ];
static volatile u8 rq_cr[RQ];
static volatile u8 rq_head, rq_tail;
u32 rnd_wait;                           /* the slots waiting for /dev/random (event.c) */
static u32 rng200;                      /* the Pi's RNG200 registers (hwinfo), 0 if none */
static u32 rng200_words;                /* words taken from it; it stops counting when they repeat */
static u32 rng200_last;

#define VHPOSR REG16(0xDFF006)

/* several producers (levels 2 and 5, and process context): the update is masked; the one consumer is rnd_work */
void rnd_isr(u16 v, u32 credit8)
{
    u16 s = irq_save();
    u8 h = rq_head, nh = (u8)((h + 1) % RQ);
    if (nh != rq_tail) {                /* full: the sample is dropped and earns nothing */
        rq[h] = v;
        rq_cr[h] = (u8)credit8;
        rq_head = nh;
        if (credit8 || (u8)((nh - rq_tail) % RQ) >= RQ / 2)
            kwork_pending = 1;          /* credit is wanted soon; plain samples wait for a half-full ring */
    }
    irq_restore(s);
}

void rnd_event(u32 credit8)
{
    rnd_isr(VHPOSR, credit8);
}

/* little-endian registers through Emu68's window, as emmc.c */
static inline u32 rng_rd(u32 off)
{
    u32 v = __builtin_bswap32(*(volatile u32 *)(rng200 + off));
    __asm__ volatile("nop" ::: "memory");
    return v;
}

static void rng200_take(void)
{
    u32 n = rng_rd(0x24) & 0xFF, w[8], got = 0;        /* RNG_FIFO_COUNT: words waiting */
    while (n-- && got < 8) {
        u32 v = rng_rd(0x20);                           /* RNG_FIFO_DATA */
        if (v == rng200_last && rng200_words) {         /* a stuck generator earns nothing from now on */
            kprintf("random: the RNG200 repeats a word: no longer credited\n");
            rng200 = 0;
            break;
        }
        rng200_last = v;
        w[got++] = v;
        rng200_words++;
    }
    if (got && rnd_mix(w, got * 4, rng200 ? got * 32 * 8 : 0))
        rnd_seeded_now("the RNG200");
    memset(w, 0, sizeof w);
}

/* the pool was seeded just now: wake /dev/random's readers, say so once */
void rnd_seeded_now(const char *by)
{
    kprintf("random: pool seeded (%u bits) after %u.%u s, last by %s\n", rnd_credit8 / 8, ticks / 100, ticks / 10 % 10, by);
    ev_wake(&rnd_wait);
}

/* process context (kwork): the ring into the pool */
void rnd_work(void)
{
    u16 buf[RQ];
    u32 n = 0, cr = 0;
    u16 s = irq_save();
    while (rq_tail != rq_head) {
        u8 t = rq_tail;
        buf[n++] = rq[t];
        cr += rq_cr[t];
        rq_tail = (u8)((t + 1) % RQ);
    }
    irq_restore(s);
    if (n) {
        u32 tk = ticks;
        b2s_update(&pool, &tk, 4);      /* when the batch was taken, uncredited */
        if (rnd_mix(buf, n * 2, cr))
            rnd_seeded_now(cr ? "keys, the serial line or the floppy" : "a sample");
    }
    if (rng200 && !rnd_seeded)
        rng200_take();
}

/* from the tick: while the pool waits for its first seed and the RNG200 is there, ask kwork for a look once a second */
void rnd_tick(void)
{
    if (rng200 && !rnd_seeded && !(ticks % 50))
        kwork_pending = 1;
}

/* at boot, after mem_init: the clocks, the boot information, a sparse hash of chip RAM (a cold start leaves it random;
 * credited 0) and the RNG200 if stage0 found one */
void rnd_init(const struct bootinfo *bi)
{
    b2s_init(&pool, 32, 0, 0);
    u32 t[4] = { hr_time(), ticks, wclock.base + wclock.secs, VPOSL };
    b2s_update(&pool, t, sizeof t);
    b2s_update(&pool, bi, sizeof *bi);
    u32 top = 0;
    for (int i = 0; i < bi->nregions; i++)
        if ((bi->r[i].attrs & MEMF_CHIP) && bi->r[i].upper > top)
            top = bi->r[i].upper;
    if (top > 0x200000)
        top = 0x200000;
    for (u32 a = 0x4000; a + 4 <= top; a += 4096)      /* one word per 4 KB: 2 KB hashed on a 2 MB machine */
        b2s_update(&pool, (const void *)a, 4);
    const struct hwinfo *hw = (const struct hwinfo *)HWINFO_ADDR;
    if (hw->magic == HWINFO_MAGIC && (hw->flags & HW_DT) && hw->rng200) {
        rng200 = hw->rng200;
        u32 c = rng_rd(0x00);                           /* RNG_CTRL: the generator on (RBGEN 1 in its 13-bit field) */
        *(volatile u32 *)(rng200 + 0x00) = __builtin_bswap32((c & ~0x1FFFu) | 1u);
        __asm__ volatile("nop" ::: "memory");
        kprintf("random: RNG200 at %p\n", rng200);
    }
    pool_new = 1;
    reseed();
    reseed_at = ticks;
}

/* /dev/urandom (blocking 0) and /dev/random (1): read at most RND_READ_MAX bytes a call */
long rnd_read(int blocking, int nonblock, void *buf, u32 n)
{
    while (blocking && !rnd_seeded) {
        if (current->killed)
            return -EINTR;
        if (nonblock)
            return -EAGAIN;
        ev_arm(&rnd_wait);
        if (rnd_seeded)
            break;
        long e = ev_block(EV_IO, 0, 0, 1);
        if (e)
            return e;
    }
    if (n > RND_READ_MAX)
        n = RND_READ_MAX;
    rnd_bytes(buf, n, ticks);
    return (long)n;
}

/* anyone's write mixes (no credit); root's write to /dev/random credits 8 bits a byte. At most RND_READ_MAX a call */
long rnd_write(int credit, const void *buf, u32 n)
{
    if (n > RND_READ_MAX)
        n = RND_READ_MAX;
    if (rnd_mix(buf, n, credit && current->uid == 0 ? n * 64 : 0))
        rnd_seeded_now("a seed written by root");
    return (long)n;
}

u16 rnd_poll(int blocking)
{
    if (!blocking)
        return POLLIN | POLLOUT;
    ev_arm(&rnd_wait);
    return rnd_seeded ? POLLIN | POLLOUT : POLLOUT;
}
#endif
