// SPDX-License-Identifier: GPL-2.0-only
#include "libz.h"
#include "miniz.h"

/* gzip [-d] [-c] [-k] [-1..-9] [file...]   (also gunzip = gzip -d, zcat = gzip -dc)
 * Compresses (default) or decompresses with deflate (RFC 1951) in a gzip wrapper (RFC 1952). With no file it
 * works from standard input to standard output. A file is replaced by file.gz (or the reverse) unless -c writes to
 * standard output or -k keeps the original. */
static u32 crc_table[256];
static void crc_init(void)
{
    for (u32 n = 0; n < 256; n++) { u32 c = n; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; crc_table[n] = c; }
}
static u32 crc_update(u32 crc, const unsigned char *p, u32 n)
{
    crc = ~crc;
    while (n--) crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static int out_fd = 1;
static unsigned char obuf[4096]; static u32 olen;
static int out_flush(void) { if (olen && sys_write(out_fd, obuf, olen) != (int)olen) return -1; olen = 0; return 0; }
static int out_put(const void *p, u32 n)
{
    const unsigned char *s = p;
    while (n) { u32 c = sizeof obuf - olen; if (c > n) c = n; memcpy(obuf + olen, s, c); olen += c; s += c; n -= c; if (olen == sizeof obuf && out_flush() < 0) return -1; }
    return 0;
}
static int deflate_out(const void *buf, int len, void *user) { (void)user; return out_put(buf, (u32)len) == 0; }

static char *slurp(int fd, u32 *len)
{
    u32 cap = 16384, n = 0; char *b = malloc(cap);
    for (;;) {
        if (n + 8192 > cap) { char *nb = malloc(cap * 2); if (!nb) return 0; memcpy(nb, b, n); b = nb; cap *= 2; }
        int r = sys_read(fd, b + n, 8192); if (r <= 0) break; n += (u32)r;
    }
    *len = n; return b;
}

static int compress_fd(int in, int level, u32 mtime)
{
    static tdefl_compressor *d;
    if (!d) d = malloc(sizeof *d);
    static const mz_uint probes[10] = { 0, 4, 8, 16, 32, 64, 128, 256, 512, 1000 };
    mz_uint flags = probes[level < 1 ? 1 : level > 9 ? 9 : level] | (level <= 3 ? TDEFL_GREEDY_PARSING_FLAG : 0);
    unsigned char hdr[10] = { 0x1f, 0x8b, 8, 0, (unsigned char)mtime, (unsigned char)(mtime >> 8), (unsigned char)(mtime >> 16), (unsigned char)(mtime >> 24), 0, 3 };
    out_put(hdr, 10);
    tdefl_init(d, deflate_out, 0, (int)flags);
    unsigned char buf[4096]; u32 crc = 0, total = 0; int n;
    while ((n = sys_read(in, buf, sizeof buf)) > 0) {
        crc = crc_update(crc, buf, (u32)n); total += (u32)n;
        if (tdefl_compress_buffer(d, buf, (size_t)n, TDEFL_NO_FLUSH) == TDEFL_STATUS_PUT_BUF_FAILED) return -1;
    }
    if (tdefl_compress_buffer(d, 0, 0, TDEFL_FINISH) == TDEFL_STATUS_PUT_BUF_FAILED) return -1;
    unsigned char tr[8] = { (unsigned char)crc, (unsigned char)(crc >> 8), (unsigned char)(crc >> 16), (unsigned char)(crc >> 24),
                            (unsigned char)total, (unsigned char)(total >> 8), (unsigned char)(total >> 16), (unsigned char)(total >> 24) };
    out_put(tr, 8);
    return out_flush();
}

static int decompress_buf(const unsigned char *in, u32 len)
{
    static unsigned char *dict; static tinfl_decompressor *dec;
    if (!dict) { dict = malloc(TINFL_LZ_DICT_SIZE); dec = malloc(sizeof *dec); }
    u32 pos = 0;
    while (pos < len) {                                         /* one member after another */
        if (len - pos < 18 || in[pos] != 0x1f || in[pos + 1] != 0x8b || in[pos + 2] != 8) { if (pos) break; fprintf(2, "gzip: not in gzip format\n"); return -1; }
        unsigned flg = in[pos + 3]; pos += 10;
        if (flg & 4) { u32 xl = in[pos] | (u32)in[pos + 1] << 8; pos += 2 + xl; }
        if (flg & 8) { while (pos < len && in[pos++]) ; }
        if (flg & 16) { while (pos < len && in[pos++]) ; }
        if (flg & 2) pos += 2;
        tinfl_init(dec);
        size_t dofs = 0; u32 crc = 0, total = 0;
        for (;;) {
            size_t in_n = len - pos, out_n = TINFL_LZ_DICT_SIZE - dofs;
            tinfl_status st = tinfl_decompress(dec, in + pos, &in_n, dict, dict + dofs, &out_n, 0);
            pos += (u32)in_n;
            if (out_n) { if (out_put(dict + dofs, (u32)out_n) < 0) return -1; crc = crc_update(crc, dict + dofs, (u32)out_n); total += (u32)out_n; }
            dofs = (dofs + out_n) & (TINFL_LZ_DICT_SIZE - 1);
            if (st == TINFL_STATUS_DONE) break;
            if (st < 0) { fprintf(2, "gzip: data error\n"); return -1; }
            if (st == TINFL_STATUS_NEEDS_MORE_INPUT && pos >= len) { fprintf(2, "gzip: unexpected end of file\n"); return -1; }
        }
        if (len - pos < 8) { fprintf(2, "gzip: missing trailer\n"); return -1; }
        u32 wcrc = in[pos] | (u32)in[pos + 1] << 8 | (u32)in[pos + 2] << 16 | (u32)in[pos + 3] << 24;
        u32 wlen = in[pos + 4] | (u32)in[pos + 5] << 8 | (u32)in[pos + 6] << 16 | (u32)in[pos + 7] << 24;
        pos += 8;
        if (wcrc != crc || wlen != total) { fprintf(2, "gzip: crc error\n"); return -1; }
    }
    return out_flush();
}

int main(int argc, char **argv)
{
    const char *me = strrchr(argv[0], '/'); me = me ? me + 1 : argv[0];
    int decomp = !strcmp(me, "gunzip") || !strcmp(me, "zcat"), to_stdout = !strcmp(me, "zcat"), keep = 0, level = 6, i = 1, rc = 0;
    crc_init();
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *p = argv[i] + 1; *p; p++) { if (*p == 'd') decomp = 1; else if (*p == 'c') to_stdout = 1; else if (*p == 'k') keep = 1; else if (*p >= '1' && *p <= '9') level = *p - '0'; else if (*p == 'f' || *p == 'v' || *p == 'q') ; else { fprintf(2, "gzip: unknown option -%c\n", *p); return 2; } }
    }
    if (i >= argc) {                                            /* standard input to standard output */
        out_fd = 1;
        if (decomp) { u32 n; char *b = slurp(0, &n); rc = b ? decompress_buf((unsigned char *)b, n) : -1; }
        else rc = compress_fd(0, level, sys_time());
        return rc < 0;
    }
    for (; i < argc; i++) {
        const char *f = argv[i]; int in = sys_open(f, O_RDONLY);
        if (in < 0) { fprintf(2, "gzip: %s: %s\n", f, zwhy("cannot open")); rc = 1; continue; }
        char outname[160]; u32 fl = strlen(f), have_gz = fl > 3 && !strcmp(f + fl - 3, ".gz");
        if (decomp) { if (have_gz) { strcpy(outname, f); outname[fl - 3] = 0; } else if (fl > 4 && !strcmp(f + fl - 4, ".tgz")) { strcpy(outname, f); strcpy(outname + fl - 4, ".tar"); } else { fprintf(2, "gzip: %s: unknown suffix\n", f); sys_close(in); rc = 1; continue; } }
        else { if (have_gz) { fprintf(2, "gzip: %s already has .gz suffix\n", f); sys_close(in); rc = 1; continue; } snprintf(outname, sizeof outname, "%s.gz", f); }
        out_fd = to_stdout ? 1 : sys_open(outname, O_WRONLY | O_CREAT | O_TRUNC);
        if (out_fd < 0) { fprintf(2, "gzip: %s: %s\n", outname, zwhy("cannot write")); sys_close(in); rc = 1; continue; }
        olen = 0; int r;
        if (decomp) { u32 n; char *b = slurp(in, &n); r = b ? decompress_buf((unsigned char *)b, n) : -1; }
        else { struct ustat st; u32 mt = sys_fstat(in, &st) == 0 ? st.mtime : 0; r = compress_fd(in, level, mt); }
        sys_close(in);
        if (!to_stdout) { sys_close(out_fd); if (r < 0) sys_unlink(outname); else if (!keep) sys_unlink(f); }
        if (r < 0) rc = 1;
    }
    return rc;
}
