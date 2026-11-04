// SPDX-License-Identifier: GPL-2.0-only
#include "pw.h"

/* The password hash, built on SHA-256 (user/sha256.c). The stored form is "ROUNDS$salt$hex": hex of SHA-256
 * applied ROUNDS times, starting from salt + password. New hashes use PW_ROUNDS; the older form "salt$hex" (no round
 * count, one '$') means 100 rounds and stays valid. No MMU means no real security boundary on this system, and
 * /etc/passwd is world-readable (no shadow file): the hash only makes guessing a password from it slow.
 * PW_ROUNDS 1000 is a choice, not a measurement: one SHA-256 block per round, estimated at about a second on a
 * 14 MHz 68020 and well under that on the PiStorm (not measured on hardware: docs/limits.md). */
#define OLD_ROUNDS 100
#define PW_ROUNDS 1000
#define MAX_ROUNDS 100000

static void hash_rounds(const char *password, const char *salt, u32 rounds, unsigned char x[32])
{
    unsigned char d[32 + 40 + 1];
    char in[128];
    u32 sl = strlen(salt); if (sl > 16) sl = 16;
    memcpy(in, salt, sl); strncpy(in + sl, password, 100); in[sl + 100] = 0;
    sha256(in, sl + strlen(in + sl), x);
    for (u32 i = 1; i < rounds; i++) { memcpy(d, x, 32); memcpy(d + 32, salt, sl); sha256(d, 32 + sl, x); }
}

static void put_hex(char *out, const unsigned char x[32])
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[2*i] = hex[x[i] >> 4]; out[2*i + 1] = hex[x[i] & 15]; }
    out[64] = 0;
}

/* out: "ROUNDS$salt$hex" (at most 6 + 16 + 1 + 64 + 1 bytes) */
void pw_hash(const char *password, const char *salt, char *out)
{
    unsigned char x[32];
    u32 sl = strlen(salt); if (sl > 16) sl = 16;
    hash_rounds(password, salt, PW_ROUNDS, x);
    int n = snprintf(out, 8, "%u$", (unsigned)PW_ROUNDS);
    memcpy(out + n, salt, sl); out[n + sl] = '$';
    put_hex(out + n + sl + 1, x);
}

int pw_check(const char *password, const char *stored)
{
    if (!stored[0]) return 1;                           /* no password set */
    if (!strcmp(stored, "*")) return 0;                 /* locked */
    const char *d1 = strchr(stored, '$');
    if (!d1) return 0;
    const char *d2 = strchr(d1 + 1, '$');
    u32 rounds = OLD_ROUNDS;
    const char *salt = stored;
    if (d2) {                                           /* "ROUNDS$salt$hex": digits only, in range */
        if (d1 == stored || d1 - stored > 6) return 0;
        rounds = 0;
        for (const char *p = stored; p < d1; p++) { if (*p < '0' || *p > '9') return 0; rounds = rounds * 10 + (u32)(*p - '0'); }
        if (rounds < 1 || rounds > MAX_ROUNDS) return 0;
        salt = d1 + 1;
        d1 = d2;
    }
    char sbuf[20], h[70];
    u32 sl = (u32)(d1 - salt); if (sl > 16) return 0;
    memcpy(sbuf, salt, sl); sbuf[sl] = 0;
    unsigned char x[32];
    hash_rounds(password, sbuf, rounds, x);
    put_hex(h, x);
    return !strcmp(h, d1 + 1);
}

int pw_auth(const char *name, const char *password, struct pw *p)
{
    if (pw_byname(name, p) == 0)
        return pw_check(password, p->hash);
    unsigned char x[32];                /* an unknown name costs the same rounds: its answer comes no sooner */
    hash_rounds(password, "unknown", PW_ROUNDS, x);
    return 0;
}
