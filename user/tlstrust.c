/* SPDX-License-Identifier: GPL-2.0-only */
/* The clock's rules, the random seed and the trust anchors for Zenix's TLS clients (user/tlstrust.h; moved out of
 * wget.c unchanged, 2026-11-17, for irc's TLS helper, DESIGN D58). */
#include "tlstrust.h"

#define LOCALMAX   16                   /* the owner's roots taken from /etc/ssl/local/ */
#define PEMMAX     65536                /* a PEM file read there */
#define DERMAX     8192                 /* one certificate in it */

void (*tt_say)(const char *fmt, ...);
#define say(...) do { if (tt_say) tt_say(__VA_ARGS__); } while (0)

/* ---- the clock (D21 8.3) ---- */
/* Is the clock trusted this boot (the battery clock read at boot, or sntp's guarded answer: SYS_ticks 2)? If not, the
 * certificates' "not after" is judged against the floor and "not before" skipped (zc_clock.h). */
int tt_clock_trusted(void)
{
    int b = sys_clockbits();
    return b > 0 && (b & (CLOCK_RTC | CLOCK_NET));
}

/* the floor: the newer of the build date (the commit's, builddate.h) and /var/lib/clock */
u32 tt_clock_floor(u32 build_date)
{
    char b[ZC_CLOCK_LINE];
    uint32_t t = 0;
    int fd = sys_open(ZC_CLOCK_PATH, O_RDONLY), n = 0, ok = 0;
    if (fd >= 0) {
        n = sys_read(fd, b, sizeof b);
        sys_close(fd);
        ok = n > 0 && zc_clock_parse(b, (size_t)n, &t) == 0;
    }
    return zc_clock_floor(build_date, ok, t);
}

void tt_civil(u32 t, int *y, int *m, int *d)
{
    static const unsigned char ml[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    u32 days = t / 86400;
    *y = 1970;
    for (;;) {
        u32 yl = (*y % 4 == 0 && (*y % 100 != 0 || *y % 400 == 0)) ? 366 : 365;
        if (days < yl)
            break;
        days -= yl;
        ++*y;
    }
    for (*m = 0; *m < 11; ++*m) {
        u32 l = ml[*m] + (*m == 1 && *y % 4 == 0 && (*y % 100 != 0 || *y % 400 == 0));
        if (days < l)
            break;
        days -= l;
    }
    ++*m;
    *d = (int)days + 1;
}

/* ---- the random seed: 32 bytes from /dev/random for each handshake (D21 5); said when the pool is not ready ---- */
int tt_seed32(unsigned char *b)
{
    int fd = sys_open("/dev/random", O_RDONLY | O_NONBLOCK), r;
    if (fd < 0)
        return fd;
    r = sys_read(fd, b, 32);
    if (r == -EAGAIN) {
        say("waiting for the random pool (type on the keyboard)\n");
        sys_close(fd);
        if ((fd = sys_open("/dev/random", O_RDONLY)) < 0)
            return fd;
        r = sys_read(fd, b, 32);
    }
    sys_close(fd);
    return r == 32 ? 0 : r < 0 ? r : -EIO;
}

/* ---- the trust anchors: /etc/ssl/anchors (D21 8.1) and the owner's PEM roots in /etc/ssl/local/ ---- */
static int anchors_fd = -2;             /* -2: not opened yet */
static struct { unsigned char *der; size_t n; unsigned char hash[8]; } local[LOCALMAX];
static int nlocal;

static int anchors_rd(void *u, unsigned long off, void *buf, unsigned long n)
{
    unsigned char *p = buf;
    (void)u;
    if (anchors_fd < 0 || sys_lseek(anchors_fd, (int)off, 0) != (int)off)
        return -1;
    while (n) {
        int r = sys_read(anchors_fd, p, n);
        if (r <= 0)
            return -1;
        p += r;
        n -= (unsigned long)r;
    }
    return 0;
}

static void dn_hash(void *ctx, const void *buf, size_t len)
{
    br_sha256_update(ctx, buf, len);
}

/* a local root's subject name hash (as the anchors file's index: the first 8 bytes of SHA-256 of the DER name) */
static int subject_hash(const unsigned char *der, size_t n, unsigned char out[8])
{
    static br_x509_decoder_context d;
    br_sha256_context c;
    unsigned char h[32];
    br_sha256_init(&c);
    br_x509_decoder_init(&d, dn_hash, &c);
    br_x509_decoder_push(&d, der, n);
    if (br_x509_decoder_last_error(&d) || !br_x509_decoder_get_pkey(&d))
        return -1;
    br_sha256_out(&c, h);
    memcpy(out, h, 8);
    return 0;
}

static void load_local(void)
{
    char **names, *pem = 0;
    int n = dir_list("/etc/ssl/local", &names), i;
    if (n < 0 && zerrno != ENOENT)
        say("/etc/ssl/local: %s: its roots are not used\n", zwhy("cannot read"));
    for (i = 0; i < n; i++) {
        char *p = path_join("/etc/ssl/local", names[i]);
        int fd = p ? sys_open(p, O_RDONLY) : -1, len = 0, r;
        struct ustat st;
        size_t off = 0;
        free(p);
        if (fd < 0)
            continue;
        if (sys_fstat(fd, &st) == 0 && (st.mode & S_IFMT) == S_IFREG) {
            if (st.size > PEMMAX)
                say("/etc/ssl/local/%s: over %d bytes, not read\n", names[i], PEMMAX);
            else if (pem || (pem = malloc(PEMMAX + 1)))
                while (len < (int)st.size && (r = sys_read(fd, pem + len, st.size - (u32)len)) > 0)
                    len += r;
        }
        sys_close(fd);
        while (len > 0) {
            unsigned char *der;
            long k;
            if (nlocal == LOCALMAX) {
                if (strstr(pem + off, "-----BEGIN CERTIFICATE"))
                    say("/etc/ssl/local: more than %d roots, the rest not used\n", LOCALMAX);
                break;
            }
            pem[len] = 0;
            if (!(der = malloc(DERMAX)))
                break;
            k = zc_pem_cert(pem, (size_t)len, &off, der, DERMAX);
            if (k > 0 && subject_hash(der, (size_t)k, local[nlocal].hash) == 0) {
                local[nlocal].der = der;
                local[nlocal++].n = (size_t)k;
                continue;
            }
            free(der);
            if (k == 0)
                break;
            say("/etc/ssl/local/%s: a damaged certificate, not used\n", names[i]);
            if (k < 0)
                break;
        }
    }
    free(pem);
    if (n >= 0)
        dir_free(names, n);
}

int tt_find(void *u, struct zc_x509 *x, const unsigned char hash[8], const unsigned char *dn, size_t dnlen)
{
    int i, r;
    (void)u;
    (void)dn;
    (void)dnlen;
    if (anchors_fd >= 0 && (r = zc_anchors_find(x, anchors_rd, 0, hash)))
        return r;
    for (i = 0; i < nlocal; i++)
        if (!memcmp(local[i].hash, hash, 8) && (r = zc_x509_add_cert(x, local[i].der, local[i].n)))
            return r;
    return 0;
}

void tt_anchors_open(void)
{
    if (anchors_fd != -2)
        return;
    anchors_fd = sys_open("/etc/ssl/anchors", O_RDONLY);
    load_local();
    if (anchors_fd < 0 && !nlocal)
        say("no /etc/ssl/anchors and no roots in /etc/ssl/local: no certificate can be checked\n");
}
