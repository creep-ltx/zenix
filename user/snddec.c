/* SPDX-License-Identifier: GPL-2.0-only */
/* play's sound file reader (DESIGN D40 section 9). Written from the formats' public descriptions: Microsoft's RIFF
 * WAVE (fmt and data chunks, WAVE_FORMAT_PCM 1 and WAVE_FORMAT_EXTENSIBLE 0xFFFE with the PCM subformat), EA's IFF
 * 8SVX (VHDR, CHAN, BODY; sCompression 1 is the Fibonacci delta of its appendix: a pad byte, the first value, then
 * two 4-bit codes a byte, high first), Apple's AIFF and AIFF-C (COMM with the rate as an 80-bit extended number, SSND
 * with its offset; NONE and sowt). Chunks are read in order and unknown ones skipped, so a pipe works.
 * The output for Paula: 8-bit signed. A file sample is first made 16-bit (8-bit << 8); div of them (and both channels
 * when mixing to mono) are added up and the sum s of k values gives floor((s + 128k) / 256k), at most 127. Uses no
 * library: the PC's test builds this file as it is. */
#include "snddec.h"

static void bcopy8(void *d, const void *s, unsigned n)
{
    unsigned char *a = d;
    const unsigned char *b = s;
    while (n--) *a++ = *b++;
}

static int same4(const unsigned char *p, const char *s)
{
    return p[0] == (unsigned char)s[0] && p[1] == (unsigned char)s[1] && p[2] == (unsigned char)s[2] &&
           p[3] == (unsigned char)s[3];
}

static unsigned be32(const unsigned char *p) { return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3]; }
static unsigned be16(const unsigned char *p) { return (unsigned)p[0] << 8 | p[1]; }
static unsigned le32(const unsigned char *p) { return (unsigned)p[3] << 24 | (unsigned)p[2] << 16 | (unsigned)p[1] << 8 | p[0]; }
static unsigned le16(const unsigned char *p) { return (unsigned)p[1] << 8 | p[0]; }

static int fail(struct sdec *d, const char *why)
{
    int i = 0;
    for (; why[i] && i < (int)sizeof d->why - 1; i++)
        d->why[i] = why[i];
    d->why[i] = 0;
    return -1;
}

/* exactly n bytes into buf (buf 0: skipped) -> n, or less at the end */
static int get(struct sdec *d, void *buf, unsigned n)
{
    unsigned char *o = buf;
    unsigned got = 0;
    while (got < n) {
        if (d->rawp == d->rawn) {
            int r = d->rd(d->ctx, d->raw, sizeof d->raw);
            if (r <= 0)
                return r < 0 ? r : (int)got;
            d->rawn = r;
            d->rawp = 0;
        }
        unsigned k = (unsigned)(d->rawn - d->rawp);
        if (k > n - got)
            k = n - got;
        if (o)
            bcopy8(o + got, d->raw + d->rawp, k);
        d->rawp += (int)k;
        got += k;
    }
    return (int)got;
}

int sdec_sniff(const unsigned char *h, int n)
{
    if (n < 12)
        return SDF_RAW;
    if (same4(h, "RIFF") && same4(h + 8, "WAVE"))
        return SDF_WAV;
    if (same4(h, "FORM") && same4(h + 8, "8SVX"))
        return SDF_8SVX;
    if (same4(h, "FORM") && (same4(h + 8, "AIFF") || same4(h + 8, "AIFC")))
        return SDF_AIFF;
    return SDF_RAW;
}

unsigned sdec_ext80(const unsigned char *p)
{
    int e = (int)(be16(p) & 0x7FFF) - 16383;
    unsigned m = be32(p + 2);                   /* the top 32 bits of the 64-bit mantissa (1.xxx) */
    if (p[0] & 0x80 || e < 0 || e > 31)
        return 0;
    return m >> (31 - e);
}

static int open_wav(struct sdec *d)
{
    unsigned char h[40];
    int fmt = 0;
    for (;;) {
        if (get(d, h, 8) != 8)
            return fail(d, fmt ? "no data chunk" : "no fmt chunk");
        unsigned n = le32(h + 4);
        if (same4(h, "fmt ")) {
            if (n < 16 || n > 4096)
                return fail(d, "bad fmt chunk");
            unsigned k = n < 40 ? n : 40;
            if (get(d, h, k) != (int)k || get(d, 0, n - k + (n & 1)) != (int)(n - k + (n & 1)))
                return fail(d, "short fmt chunk");
            unsigned tag = le16(h);
            if (tag == 0xFFFE && k >= 26)
                tag = le16(h + 24);             /* WAVE_FORMAT_EXTENSIBLE: the subformat GUID's first two bytes */
            if (tag != 1)
                return fail(d, "not PCM (compressed WAV)");
            d->chans = (int)le16(h + 2);
            d->rate = le32(h + 4);
            d->bits = (int)le16(h + 14);
            d->usign = d->bits == 8;
            fmt = 1;
        } else if (same4(h, "data")) {
            if (!fmt)
                return fail(d, "data before fmt");
            d->left = n;
            return 0;
        } else if (get(d, 0, n + (n & 1)) != (int)(n + (n & 1)))
            return fail(d, "short chunk");
    }
}

static int open_iff(struct sdec *d, int aiff)
{
    unsigned char h[32];
    int head = 0, aifc = 0;
    if (get(d, h, 12) != 12)
        return fail(d, "short header");
    aifc = same4(h + 8, "AIFC");
    for (;;) {
        if (get(d, h, 8) != 8)
            return fail(d, head ? "no sample data" : "no header chunk");
        unsigned n = be32(h + 4), pad = n & 1;
        if (!aiff && same4(h, "VHDR")) {
            if (n < 20 || get(d, h, 20) != 20 || get(d, 0, n - 20 + pad) != (int)(n - 20 + pad))
                return fail(d, "bad VHDR");
            d->rate = be16(h + 12);
            d->fib = h[15];
            if (h[14] > 1)                      /* several octaves: the first (highest) one only */
                d->first = be32(h) + be32(h + 4);
            if (d->fib > 1)
                return fail(d, "unknown 8SVX packing");
            unsigned v = be32(h + 16);          /* Fixed 16.16, 1.0 = full */
            d->vol = v >= 0x10000 ? 64 : (int)(v >> 10);
            head = 1;
        } else if (!aiff && same4(h, "CHAN")) {
            if (n < 4 || get(d, h, 4) != 4 || get(d, 0, n - 4 + pad) != (int)(n - 4 + pad))
                return fail(d, "bad CHAN");
            if (be32(h) == 6)
                d->chans = 2;                   /* 2 left, 4 right, 6 both: BODY holds left then right */
        } else if (!aiff && same4(h, "BODY")) {
            if (!head)
                return fail(d, "BODY before VHDR");
            d->left = d->first && d->first < n ? d->first : n;
            if (d->chans == 2) {                /* both halves are needed at once: the body in memory */
                d->body = sdec_alloc(n ? n : 1);
                if (!d->body)
                    return fail(d, "no memory for the stereo body");
                if (get(d, d->body, n) != (int)n)
                    return fail(d, "short BODY");
                d->bodylen = n;
                d->bodypos[0] = 0;
                d->bodypos[1] = n / 2;
            }
            return 0;
        } else if (aiff && same4(h, "COMM")) {
            if (n < 18 || get(d, h, 18) != 18)
                return fail(d, "bad COMM");
            d->chans = (int)be16(h);
            d->bits = (int)be16(h + 6);
            d->rate = sdec_ext80(h + 8);
            d->big = 1;
            unsigned rest = n - 18;
            if (aifc) {
                if (rest < 4 || get(d, h + 18, 4) != 4)
                    return fail(d, "bad AIFC COMM");
                rest -= 4;
                if (same4(h + 18, "sowt"))
                    d->big = 0;
                else if (!same4(h + 18, "NONE"))
                    return fail(d, "compressed AIFF-C");
            }
            if (get(d, 0, rest + pad) != (int)(rest + pad))
                return fail(d, "short COMM");
            head = 1;
        } else if (aiff && same4(h, "SSND")) {
            if (!head)
                return fail(d, "SSND before COMM (not playable from a pipe)");
            if (n < 8 || get(d, h, 8) != 8)
                return fail(d, "bad SSND");
            unsigned off = be32(h);
            if (off > n - 8 || get(d, 0, off) != (int)off)
                return fail(d, "bad SSND offset");
            d->left = n - 8 - off;
            return 0;
        } else if (get(d, 0, n + pad) != (int)(n + pad))
            return fail(d, "short chunk");
    }
}

int sdec_open(struct sdec *d)
{
    d->vol = 64;
    d->fibhalf[0] = d->fibhalf[1] = 0;
    if (d->fmt == SDF_RAW) {
        d->left = 0xFFFFFFFFu;
        if (!d->bits) d->bits = 8;
    } else {
        d->chans = 1;
        d->bits = 8;
        int r = d->fmt == SDF_WAV ? (get(d, 0, 12) == 12 ? open_wav(d) : fail(d, "short header"))
                                  : open_iff(d, d->fmt == SDF_AIFF);
        if (r)
            return r;
    }
    if (d->chans < 1 || d->chans > 2)
        return fail(d, "more than two channels");
    if (d->bits != 8 && d->bits != 16)
        return fail(d, "not 8 or 16 bits a sample");
    if (d->rate < 1000 || d->rate > 200000)
        return fail(d, "rate out of range");
    if (d->fmt == SDF_8SVX && d->fib) {
        /* each channel's data starts with a pad byte and its first value (a stereo body: both halves) */
        if (d->body) {
            unsigned h = d->bodylen / 2;
            if (h < 2) return fail(d, "short packed BODY");
            d->fibx[0] = (signed char)d->body[1];
            d->fibx[1] = (signed char)d->body[h + 1];
            d->bodypos[0] = 2;
            d->bodypos[1] = h + 2;
        } else {
            unsigned char p[2];
            if (d->left < 2 || get(d, p, 2) != 2) return fail(d, "short packed BODY");
            d->fibx[0] = (signed char)p[1];
            d->left -= 2;
        }
    }
    sdec_out(d, d->rate, 0);
    return 0;
}

void sdec_out(struct sdec *d, unsigned maxrate, int mono)
{
    d->div = 1;
    while (maxrate && d->rate / d->div > maxrate)
        d->div++;
    d->outrate = d->rate / d->div;
    d->mono = mono || d->chans == 1;
}

static const signed char fibtab[16] = { -34, -21, -13, -8, -5, -3, -2, -1, 0, 1, 2, 3, 5, 8, 13, 21 };

/* the next 8SVX sample of channel c from the stereo body in memory -> 1, or 0 at its end */
static int body_next(struct sdec *d, int c, int *v)
{
    unsigned end = c ? d->bodylen : d->bodylen / 2;
    if (d->bodypos[c] >= end)
        return 0;
    unsigned b = d->body[d->bodypos[c]];
    if (!d->fib) {
        d->bodypos[c]++;
        *v = (signed char)b * 256;
        return 1;
    }
    unsigned code = d->fibhalf[c] ? b & 15 : b >> 4;
    if (d->fibhalf[c]) d->bodypos[c]++;
    d->fibhalf[c] ^= 1;
    d->fibx[c] = (signed char)(d->fibx[c] + fibtab[code]);
    *v = d->fibx[c] * 256;
    return 1;
}

/* one frame of the file, each channel as a 16-bit value -> 1, 0 at the end, < 0 an error */
static int frame(struct sdec *d, int v[2])
{
    if (d->body) {
        if (!body_next(d, 0, &v[0]) || !body_next(d, 1, &v[1]))
            return 0;
        return 1;
    }
    for (int c = 0; c < d->chans; c++) {
        if (d->fmt == SDF_8SVX && d->fib) {
            if (!d->fibhalf[0]) {
                unsigned char b;
                if (!d->left || get(d, &b, 1) != 1)
                    return 0;
                d->left--;
                d->fibhalf[1] = b;              /* the byte's low code waits */
                d->fibx[0] = (signed char)(d->fibx[0] + fibtab[b >> 4]);
            } else
                d->fibx[0] = (signed char)(d->fibx[0] + fibtab[d->fibhalf[1] & 15]);
            d->fibhalf[0] ^= 1;
            v[c] = d->fibx[0] * 256;
            continue;
        }
        unsigned char b[2];
        unsigned k = (unsigned)d->bits / 8;
        if (d->left < k)
            return 0;
        int r = get(d, b, k);
        if (r < 0)
            return fail(d, "read error");
        if (r != (int)k)
            return 0;
        if (d->left != 0xFFFFFFFFu)
            d->left -= k;
        if (k == 1)
            v[c] = d->usign ? ((int)b[0] - 128) * 256 : (signed char)b[0] * 256;
        else
            v[c] = d->big ? (signed char)b[0] * 256 + b[1] : (signed char)b[1] * 256 + b[0];
    }
    if (d->chans == 1)
        v[1] = v[0];
    return 1;
}

static signed char to8(int s, unsigned k)
{
    /* floor((s + 128k) / 256k) with s >= -32768k: a shift when k is a power of two (the usual 1, 2, 4), else shifted up
     * to stay positive for the division */
    int q;
    if (!(k & (k - 1))) {
        int sh = 8;
        for (unsigned x = k; x > 1; x >>= 1) sh++;
        q = (s + 128 * (int)k) >> sh;   /* an arithmetic shift: the floor */
    } else
        q = (int)(((unsigned)(s + 32768 * (int)k + 128 * (int)k)) / (256u * k)) - 128;
    return (signed char)(q > 127 ? 127 : q);
}

/* at least need bytes in the buffer (fewer only at the end) -> the bytes there, < 0 an error */
static int refill(struct sdec *d, unsigned need)
{
    unsigned have = (unsigned)(d->rawn - d->rawp);
    if (have >= need)
        return (int)have;
    for (unsigned i = 0; i < have; i++)
        d->raw[i] = d->raw[d->rawp + i];
    d->rawn = (int)have;
    d->rawp = 0;
    while ((unsigned)d->rawn < need) {
        int r = d->rd(d->ctx, d->raw + d->rawn, (int)sizeof d->raw - d->rawn);
        if (r < 0)
            return r;
        if (!r)
            break;
        d->rawn += r;
    }
    return d->rawn;
}

static int shift_of(unsigned k)       /* 8 + log2(k) when k is a power of two, else 0 */
{
    if (k & (k - 1)) return 0;
    int sh = 8;
    for (; k > 1; k >>= 1) sh++;
    return sh;
}

static signed char clip(int q) { return (signed char)(q > 127 ? 127 : q); }

/* one group of div frames: the sum of each channel's 16-bit values. The sample's kind is a constant in each use
 * (GROUP is expanded once per kind), so the inner loop has no tests but the count */
#define GROUP(GET)                                                                      \
    for (unsigned g = 0; g < groups; g++) {                                             \
        int sl = 0, sr = 0;                                                             \
        for (unsigned k = 0; k < div; k++, p += fb) {                                   \
            sl += GET(p);                                                               \
            if (two) sr += GET(p + bps);                                                \
        }                                                                               \
        if (!two)                                                                       \
            *out++ = sh1 ? clip((sl + 128 * (int)div) >> sh1) : to8(sl, div);           \
        else if (mono)                                                                  \
            *out++ = sh2 ? clip((sl + sr + 256 * (int)div) >> sh2) : to8(sl + sr, 2 * div); \
        else {                                                                          \
            *out++ = sh1 ? clip((sl + 128 * (int)div) >> sh1) : to8(sl, div);           \
            *out++ = sh1 ? clip((sr + 128 * (int)div) >> sh1) : to8(sr, div);           \
        }                                                                               \
    }
#define U8(q) (((int)(q)[0] - 128) * 256)
#define S8(q) ((signed char)(q)[0] * 256)
#define LE16(q) ((signed char)(q)[1] * 256 + (q)[0])
#define BE16(q) ((signed char)(q)[0] * 256 + (q)[1])

/* the common case, read straight from the buffer: no packing, no stereo body held in memory. The arithmetic shift is
 * the floor of the rule in the head (k a power of two); other k divide (to8) */
static int fast_read(struct sdec *d, signed char *out, int n)
{
    unsigned bps = (unsigned)d->bits / 8, fb = bps * (unsigned)d->chans, div = d->div, need = fb * div;
    int two = d->chans == 2, mono = d->mono, sh1 = shift_of(div), sh2 = shift_of(2 * div);
    int kind = d->bits == 8 ? (d->usign ? 0 : 1) : (d->big ? 3 : 2);
    int done = 0;
    while (done < n && d->left >= need) {
        int a = refill(d, need);
        if (a < 0)
            return fail(d, "read error");
        if ((unsigned)a < need)
            break;
        unsigned groups = (unsigned)a / need;
        if (groups > (unsigned)(n - done)) groups = (unsigned)(n - done);
        if (d->left != 0xFFFFFFFFu && groups > d->left / need) groups = d->left / need;
        const unsigned char *p = d->raw + d->rawp;
        switch (kind) {
        case 0: GROUP(U8) break;
        case 1: GROUP(S8) break;
        case 2: GROUP(LE16) break;
        default: GROUP(BE16) break;
        }
        d->rawp += (int)(groups * need);
        if (d->left != 0xFFFFFFFFu)
            d->left -= groups * need;
        done += (int)groups;
    }
    return done;
}

int sdec_read(struct sdec *d, signed char *out, int n)
{
    if (!d->fib && !d->body)
        return fast_read(d, out, n);
    int done = 0;
    while (done < n) {
        int sl = 0, sr = 0, v[2];
        unsigned k = 0;
        for (; k < d->div; k++) {
            int r = frame(d, v);
            if (r < 0)
                return r;
            if (!r)
                break;
            sl += v[0];
            sr += v[1];
        }
        if (k < d->div)                         /* a last partial group is dropped (less than 1/28,000 s) */
            break;
        if (d->mono) {
            if (d->chans == 1)
                *out++ = to8(sl, k);
            else
                *out++ = to8(sl + sr, 2 * k);
        } else {
            *out++ = to8(sl, k);
            *out++ = to8(sr, k);
        }
        done++;
    }
    return done;
}

void sdec_close(struct sdec *d)
{
    if (d->body)
        sdec_free(d->body);
    d->body = 0;
}
