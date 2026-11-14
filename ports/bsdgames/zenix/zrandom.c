/* SPDX-License-Identifier: GPL-2.0-only */
/* Zenix's C library's random() and srandom() (picolibc's newlib/libc/stdlib/random.c: Knuth's 64-bit LCG, the top
 * 31 bits of the high word), for the PC's build of adventure's mkdata: the game's text is "encrypted" with random()
 * seeded with SEED, and the game on Zenix decrypts it with its C library's, so the PC must make the same numbers
 * (the PC's own C library has another generator). The Makefile names them random and srandom for setup.c. */
#include <stdint.h>
static uint64_t next = 1;
long zx_random(void)
{
    next = next * 6364136223846793005ULL + 1;
    return (long)((next >> 32) & 0x7fffffffL);
}
void zx_srandom(unsigned int seed) { next = seed; }
