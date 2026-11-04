/* SPDX-License-Identifier: GPL-2.0-only */
/* The demo floppy's unpacking (DESIGN D25 revision 3, section 6): the table that says where the packed stream is,
 * and the stream fed to miniz's tinfl a piece at a time. No AmigaOS calls: boot/demoload.c (the stage 2) uses it on
 * the Amiga, tools/demoload-host-test.c on the PC with the same source (damaged streams, a fuzzer, the sanitizers).
 *
 * The disk (made by tools/mkdemo.py; not frozen: only this loader reads it):
 *   byte 0       the bootblock (boot/demoboot.S): +8 the stage 2's size on the disk, +1020 its memory size
 *   byte 1024    the stage 2 (a ZEXE the bootblock relocates), padded to 512
 *   then         the table, one 512-byte sector (below)
 *   then         the stream (zlib: deflate with a 2-byte header and an Adler-32 of everything unpacked), from a
 *                sector boundary to the end of what is used of the disk
 * Unpacked, the stream is the kernel (kernel.bin, padded with zeros to kern_len) followed by the RAM disk's image
 * (an ext2 filesystem, image_len bytes): one buffer, as the bootblock's hand-over wants it (boot/handoff.S). */
#pragma once

#define DT_MAGIC 0x5A444D31u            /* "ZDM1" */
#define DT_SECTOR 512
#define DT_DISK_MAX 1802240u            /* an HD floppy; a DD one is 901120 */
#define DT_KERN_MAX (1u << 20)          /* sizes far beyond anything real are refused before any allocation */
#define DT_IMAGE_MAX (16u << 20)

/* the table's sector: big-endian u32s */
#define DT_O_MAGIC 0
#define DT_O_VERSION 4                  /* 1 */
#define DT_O_STREAM_OFF 8               /* the stream's first byte on the disk (a multiple of 512) */
#define DT_O_STREAM_LEN 12              /* its length in bytes (the last sector padded with zeros) */
#define DT_O_KERN_LEN 16                /* the kernel's part of the unpacked buffer (a multiple of 512) */
#define DT_O_KERN_MEM 20                /* the memory the kernel needs at its home (its ZEXE header's mem_size) */
#define DT_O_IMAGE_LEN 24               /* the RAM disk's image (a multiple of 1024) */
#define DT_O_DISK_LEN 28                /* the disk's size (901120 for DD, 1802240 for HD) */
#define DT_O_TRACK 32                   /* a track's bytes: 5632 (DD, 11 sectors) or 11264 (HD, 22) */
#define DT_TRACK_MAX 11264u
#define DT_O_SUM 508                    /* the sum of the other 127 longs, negated: the 128 add up to 0 */

typedef unsigned char du8;
typedef unsigned int du32;

struct dtab {
    du32 stream_off, stream_len, kern_len, kern_mem, image_len, disk_len, track;
};

/* Checks a table sector (512 bytes); fills t and returns 0, or returns why it is refused. Every range is checked
 * here: the stream lies after the table and inside the disk, the sizes are in bounds and multiples of what they
 * must be, kern_mem holds kern_len's image. */
const char *dt_check(const du8 *sector, du32 table_off, struct dtab *t);

/* The unpacking. The state holds tinfl's (about 11 KB): allocate it, do not put it on a small stack. */
struct dunpack;
unsigned du_state_size(void);
void du_start(struct dunpack *u, du8 *out, du32 out_len, du32 in_len);
/* Feeds the next n bytes of the stream (in order, every byte once; the pieces' sizes are free). Returns 0 while more
 * is wanted, 1 when the stream ended exactly at in_len with exactly out_len bytes out and its Adler-32 right, or a
 * negative number when it is refused (du_why says why); after 1 or an error, later calls return the same. */
int du_feed(struct dunpack *u, const du8 *in, du32 n);
const char *du_why(int code);
du32 du_out_done(const struct dunpack *u);   /* bytes unpacked so far */

/* After a whole stream: the parts look like what they must be (the ZEXE kernel and its mem_size, ext2's magic). */
const char *du_check_parts(const du8 *out, const struct dtab *t);
