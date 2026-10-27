/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* The entropy pool and the random devices (random.c, DESIGN D19 section 9.3). Internal to the kernel (and the host
 * test, tools/random-host-test.c): nothing here is a program's interface; /dev/random and /dev/urandom are. */
struct b2s {                            /* a BLAKE2s state (RFC 7693) */
    u32 h[8], t[2];
    u8 buf[64];
    u8 n, outlen;
};
void b2s_init(struct b2s *s, u32 outlen, const void *key, u32 keylen);
void b2s_update(struct b2s *s, const void *in, u32 len);
void b2s_final(struct b2s *s, void *out);
void b2s(void *out, u32 outlen, const void *key, u32 keylen, const void *in, u32 len);
void chacha20_block(const u8 *key, u32 counter, const u8 *nonce, u8 *out);   /* RFC 8439 2.3 */

#define RND_SEED_BITS    256            /* /dev/random waits until this much was credited */
#define RND_RESEED_TICKS 6000           /* once seeded: a new key from the pool at most once a minute */
#define RND_READ_MAX     4096           /* the most one read or write of the devices moves (a short count, as Linux) */
/* credit in eighths of a bit (section 9.3) */
#define RND_CR_KEY       8              /* a key press or release */
#define RND_CR_SLIP      4              /* the end of a received SLIP frame (0xC0) */
#define RND_CR_FLOPPY    8              /* the floppy's index pulse, seen while the motor runs */

extern u32 rnd_credit8;
extern int rnd_seeded;
int rnd_mix(const void *p, u32 n, u32 credit8);      /* 1 when this call seeded the pool */
void rnd_bytes(void *out, u32 n, u32 now);
#ifndef RANDOM_HOST
struct bootinfo;
void rnd_init(const struct bootinfo *bi);            /* main.c, after mem_init */
void rnd_isr(u16 v, u32 credit8);                    /* an interrupt handler's sample */
void rnd_event(u32 credit8);                         /* a sample of the beam position now (interrupts or process) */
void rnd_work(void);                                 /* kwork: the ring into the pool */
void rnd_tick(void);                                 /* the 100 Hz tick */
void rnd_seeded_now(const char *by);
long rnd_read(int blocking, int nonblock, void *buf, u32 n);
long rnd_write(int credit, const void *buf, u32 n);
u16 rnd_poll(int blocking);
extern u32 rnd_wait;
#endif
