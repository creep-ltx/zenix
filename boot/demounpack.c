/* SPDX-License-Identifier: GPL-2.0-only */
/* The demo floppy's unpacking: boot/demounpack.h says what each part does. Everything read from the disk is
 * untrusted (CLAUDE.md): the table is range-checked before anything is allocated, tinfl is given the exact end of the
 * output buffer, and the stream must end exactly where the table says, with exactly the unpacked size and its
 * Adler-32 right. */
#include "demounpack.h"
#include "miniz.h"
#include "miniz_tinfl.h"

struct dunpack {
    tinfl_decompressor d;
    du8 *out;
    du32 out_len, out_pos, in_len, in_pos;
    int result;                         /* 0 going on, 1 done, < 0 refused */
};

static du32 be32(const du8 *p) { return (du32)p[0] << 24 | (du32)p[1] << 16 | (du32)p[2] << 8 | p[3]; }

const char *dt_check(const du8 *s, du32 table_off, struct dtab *t)
{
    du32 sum = 0;
    for (int i = 0; i < DT_SECTOR; i += 4) sum += be32(s + i);
    if (be32(s + DT_O_MAGIC) != DT_MAGIC) return "the table is not there (not a Zenix demo disk?)";
    if (sum) return "the table's checksum is wrong";
    if (be32(s + DT_O_VERSION) != 1) return "the table is of a version this loader does not know";
    t->stream_off = be32(s + DT_O_STREAM_OFF);
    t->stream_len = be32(s + DT_O_STREAM_LEN);
    t->kern_len = be32(s + DT_O_KERN_LEN);
    t->kern_mem = be32(s + DT_O_KERN_MEM);
    t->image_len = be32(s + DT_O_IMAGE_LEN);
    t->disk_len = be32(s + DT_O_DISK_LEN);
    t->track = be32(s + DT_O_TRACK);
    if (t->track != 5632 && t->track != 11264) return "the table's track size is wrong";
    if (t->disk_len > DT_DISK_MAX || t->disk_len % DT_SECTOR) return "the table's disk size is wrong";
    if (t->stream_off % DT_SECTOR || t->stream_off < table_off + DT_SECTOR || t->stream_off > t->disk_len)
        return "the table's stream offset is wrong";
    if (t->stream_len < 6 || t->stream_len > t->disk_len - t->stream_off) return "the table's stream size is wrong";
    if (t->kern_len < 512 || t->kern_len > DT_KERN_MAX || t->kern_len % DT_SECTOR) return "the table's kernel size is wrong";
    if (t->kern_mem < 64 || t->kern_mem > 4 * DT_KERN_MAX) return "the table's kernel memory size is wrong";
    if (t->image_len < 2048 || t->image_len > DT_IMAGE_MAX || t->image_len % 1024) return "the table's image size is wrong";
    return 0;
}

unsigned du_state_size(void) { return sizeof(struct dunpack); }

void du_start(struct dunpack *u, du8 *out, du32 out_len, du32 in_len)
{
    tinfl_init(&u->d);
    u->out = out;
    u->out_len = out_len;
    u->out_pos = 0;
    u->in_len = in_len;
    u->in_pos = 0;
    u->result = 0;
}

#define DU_CUT     (-1)
#define DU_LONG    (-2)
#define DU_SHORT   (-3)
#define DU_TRAIL   (-4)
#define DU_SUM     (-5)
#define DU_BAD     (-6)

const char *du_why(int code)
{
    switch (code) {
    case DU_CUT:   return "the packed data ends too early";
    case DU_LONG:  return "the packed data unpacks to more than the table says";
    case DU_SHORT: return "the packed data unpacks to less than the table says";
    case DU_TRAIL: return "the packed data ends before the table says it does";
    case DU_SUM:   return "the unpacked data's checksum (Adler-32) is wrong";
    default:       return "the packed data is damaged";
    }
}

du32 du_out_done(const struct dunpack *u) { return u->out_pos; }

int du_feed(struct dunpack *u, const du8 *in, du32 n)
{
    if (u->result) return u->result;
    if (n > u->in_len - u->in_pos) n = u->in_len - u->in_pos;     /* the last sector's padding is not the stream */
    for (;;) {
        int last = u->in_pos + n == u->in_len;
        size_t in_sz = n, out_sz = u->out_len - u->out_pos;
        mz_uint32 flags = TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF |
                          (last ? 0 : TINFL_FLAG_HAS_MORE_INPUT);
        tinfl_status st = tinfl_decompress(&u->d, in, &in_sz, u->out, u->out + u->out_pos, &out_sz, flags);
        if (in_sz > n || out_sz > u->out_len - u->out_pos) return u->result = DU_BAD;   /* never: tinfl's contract */
        in += in_sz; n -= (du32)in_sz; u->in_pos += (du32)in_sz; u->out_pos += (du32)out_sz;
        if (st == TINFL_STATUS_DONE) {
            if (u->in_pos != u->in_len) return u->result = DU_TRAIL;
            if (u->out_pos != u->out_len) return u->result = DU_SHORT;
            return u->result = 1;
        }
        if (st == TINFL_STATUS_ADLER32_MISMATCH) return u->result = DU_SUM;
        if (st == TINFL_STATUS_HAS_MORE_OUTPUT) return u->result = DU_LONG;
        if (st == TINFL_STATUS_NEEDS_MORE_INPUT) {
            if (last) return u->result = DU_CUT;
            if (!n) return 0;
            if (!in_sz && !out_sz) return u->result = DU_BAD;  /* no progress with input left: never loop for ever */
            continue;
        }
        return u->result = last && st == TINFL_STATUS_FAILED_CANNOT_MAKE_PROGRESS ? DU_CUT : DU_BAD;
    }
}

const char *du_check_parts(const du8 *out, const struct dtab *t)
{
    if (out[4] != 'Z' || out[5] != 'E' || out[6] != 'X' || out[7] != 'E') return "the kernel is not a ZEXE image";
    /* as zenixboot's kernel_ok: the image, its memory, the relocations inside the part, stage0 inside the image */
    du32 image = be32(out + 8), mem = be32(out + 12), nrel = be32(out + 16), reloff = be32(out + 20), st0 = be32(out + 28);
    if (image < 32 || image > t->kern_len || mem < image || mem != t->kern_mem || reloff < image ||
        reloff > t->kern_len || nrel > (t->kern_len - reloff) / 4 || st0 >= image || (st0 & 1))
        return "the kernel's header is wrong";
    const du8 *sb = out + t->kern_len + 1024;
    if (sb[56] != 0x53 || sb[57] != 0xEF) return "the RAM disk is not an ext2 filesystem";
    /* ext2 is little-endian: s_blocks_count at +4, s_log_block_size at +24 */
    du32 blocks = (du32)sb[4] | (du32)sb[5] << 8 | (du32)sb[6] << 16 | (du32)sb[7] << 24;
    du32 log = (du32)sb[24] | (du32)sb[25] << 8 | (du32)sb[26] << 16 | (du32)sb[27] << 24;
    if (log > 2 || blocks > t->image_len / (1024u << log)) return "the RAM disk's filesystem is bigger than its image";
    return 0;
}
