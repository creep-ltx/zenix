/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* The random seed file (DESIGN D19 9.3), shared by boot (at every start, and once more when the pool is first seeded)
 * and reboot. /var/lib/random-seed, root's, mode 600: "ZRS1", a flags byte (bit 0: the boot that wrote it had a seeded
 * pool, or its seed came from one that had), three zero bytes, 32 bytes of seed. No image ever ships one (mkroot.sh
 * and mkzup.py refuse it).
 * The rule: the old seed is mixed into the pool at every start (uncredited: a write to /dev/urandom); it is credited
 * (root's write to /dev/random) only when its flag says it came from a seeded pool AND the root is a writable,
 * persistent disk AND the file was rewritten with a new seed and synced first. Never on a RAM-disk root, a read-only
 * or rescue root: the same seed would come back at every boot. seed_credit and seed_flags are the rule; the host test
 * (tools/random-host-test.c) checks them. */
#define SEED_PATH  "/var/lib/random-seed"
#define SEED_BYTES 32
#define SEED_FILE  40                   /* magic 4, flags 1, 3 zero bytes, the seed */
#define SEED_F_SEEDED 1

static inline int seed_credit(int old_ok, int old_flags, int persistent, int rewrote)
{
    return old_ok && (old_flags & SEED_F_SEEDED) && persistent && rewrote;
}

/* the flags of the new file: from a seeded pool now, or from a creditable old seed mixed into it */
static inline int seed_flags(int seeded_now, int old_ok, int old_flags, int persistent)
{
    return seeded_now || (old_ok && (old_flags & SEED_F_SEEDED) && persistent) ? SEED_F_SEEDED : 0;
}

#ifndef RANDSEED_HOST
/* 1 and the seed and flags if the file is there and well formed */
static __attribute__((unused)) int seed_load(unsigned char *seed, int *flags)
{
    unsigned char b[SEED_FILE + 1];
    int fd = sys_open(SEED_PATH, O_RDONLY), n = fd < 0 ? 0 : sys_read(fd, b, sizeof b);
    if (fd >= 0)
        sys_close(fd);
    if (n != SEED_FILE || memcmp(b, "ZRS1", 4) || b[5] || b[6] || b[7])
        return 0;
    *flags = b[4];
    memcpy(seed, b + 8, SEED_BYTES);
    memset(b, 0, sizeof b);
    return 1;
}

/* a new seed from /dev/urandom into the file (sync: and synced, as boot needs before it credits): 0, or negative.
 * reboot passes 0: SYS_reboot syncs itself, each server at most 5 s, where a plain sync would wait for ever on a hung
 * one */
static int seed_store(int flags, int sync)
{
    unsigned char b[SEED_FILE];
    memcpy(b, "ZRS1", 4);
    b[4] = (unsigned char)flags;
    b[5] = b[6] = b[7] = 0;
    int u = sys_open("/dev/urandom", O_RDONLY);
    int r = u < 0 ? u : sys_read(u, b + 8, SEED_BYTES) == SEED_BYTES ? 0 : -EIO;
    if (u >= 0)
        sys_close(u);
    if (r)
        return r;
    sys_mkdir("/var");                  /* an image without /var/lib (a new root: nothing else made it) */
    sys_mkdir("/var/lib");
    int fd = sys_open(SEED_PATH, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return fd;
    sys_chmod(SEED_PATH, 0600);
    r = sys_write(fd, b, SEED_FILE) == SEED_FILE ? 0 : -EIO;
    sys_close(fd);
    memset(b, 0, sizeof b);
    return r ? r : sync ? sys_sync() : 0;
}

/* is the pool seeded (would /dev/random answer without waiting)? */
static int seed_pool_ready(void)
{
    struct pollfd p;
    p.fd = sys_open("/dev/random", O_RDONLY);
    p.events = POLLIN;
    p.revents = 0;
    int r = p.fd >= 0 && sys_evwait(&p, 1, 0) == 1 && (p.revents & POLLIN);
    if (p.fd >= 0)
        sys_close(p.fd);
    return r;
}
#endif
