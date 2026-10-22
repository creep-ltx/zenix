/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* Amiga floppy track format: MFM coding and the standard 11-sector track.
 *
 * Pure functions on memory (no hardware), so they are tested on the host (tools/mfm-test.sh).
 * Format (ROM Kernel Manual: Devices, "Commodore-Amiga disk format"): per sector, in MFM, 1088 bytes:
 *   2 x 0xAAAA (MFM of two zero bytes), 2 x 0x4489 (sync: A1 without a clock bit),
 *   header info long 0xFF TT SS GG (format, track, sector, sectors until the gap),
 *   16 bytes label, header checksum, data checksum, 512 bytes data.
 * Every block is coded as ALL ITS ODD BITS FIRST, then all its EVEN BITS (each as MFM longs): the odd long of
 * a data long d is (d >> 1) & 0x55555555, the even one d & 0x55555555; decoding is ((odd & m) << 1) | (even & m).
 * MFM: between two data bits a clock bit is 1 only if both neighbours are 0. In a long, data bits sit at the even
 * bit positions (mask 0x55555555) and clock bits at the odd ones. Checksums: the XOR of the MFM longs of the
 * block, & 0x55555555, coded as a one-long block.
 */
typedef unsigned char mfm_u8;
typedef unsigned short mfm_u16;
typedef unsigned int mfm_u32;

#define MFM_SECTORS     11
#define MFM_SECTOR_WORDS 544                    /* 1088 bytes */
#define MFM_TRACK_WORDS  (MFM_SECTORS * MFM_SECTOR_WORDS)
#define MFM_M           0x55555555u

/* the MFM long of data bits "x" (only even bit positions set), the previous long ended in data bit "prev_bit" */
static inline mfm_u32 mfm_clock(mfm_u32 x, mfm_u32 prev_bit)
{
    mfm_u32 c = ~((x << 1) | (x >> 1) | (prev_bit << 31)) & 0xAAAAAAAAu;
    return x | c;
}

/* encode n data longs as an odd block then an even block (2n MFM longs); *prev = low bit of the last long
 * emitted before (updated); returns the XOR of the data parts (the checksum accumulator) */
static inline mfm_u32 mfm_encode_block(mfm_u32 *dst, const mfm_u32 *src, int n, mfm_u32 *prev)
{
    mfm_u32 sum = 0;
    for (int i = 0; i < n; i++) {
        mfm_u32 x = (src[i] >> 1) & MFM_M;
        dst[i] = mfm_clock(x, *prev & 1);
        *prev = dst[i];
        sum ^= x;
    }
    for (int i = 0; i < n; i++) {
        mfm_u32 x = src[i] & MFM_M;
        dst[n + i] = mfm_clock(x, *prev & 1);
        *prev = dst[n + i];
        sum ^= x;
    }
    return sum;
}

/* decode 2n MFM longs (odd block, even block) into n data longs; returns the XOR of the raw longs & 0x55555555 */
static inline mfm_u32 mfm_decode_block(mfm_u32 *dst, const mfm_u32 *raw, int n)
{
    mfm_u32 sum = 0;
    for (int i = 0; i < n; i++) {
        dst[i] = ((raw[i] & MFM_M) << 1) | (raw[n + i] & MFM_M);
        sum ^= raw[i] ^ raw[n + i];
    }
    return sum & MFM_M;
}

/* One sector into "words" (544 big-endian 16-bit MFM words, as the disk DMA delivers them); *prev = the low bit of
 * the MFM long before (carried from sector to sector). "gap" = sectors until the end of the write (11 - sector
 * for a track written in order). The two leading zero bytes are coded as one long so that their first clock bit
 * follows the rule (it is 0 when the previous data bit was 1). */
static inline void mfm_encode_sector(mfm_u16 *words, int track, int sector, int gap, const mfm_u8 *data, mfm_u32 *prev)
{
    mfm_u32 l[MFM_SECTOR_WORDS / 2];
    mfm_u32 d[128], z[4] = { 0, 0, 0, 0 };
    mfm_u32 info = 0xFF000000u | (mfm_u32)track << 16 | (mfm_u32)sector << 8 | (mfm_u32)gap;
    mfm_u32 dsum = 0;
    for (int i = 0; i < 128; i++) {
        d[i] = (mfm_u32)data[4 * i] << 24 | (mfm_u32)data[4 * i + 1] << 16 | (mfm_u32)data[4 * i + 2] << 8 | data[4 * i + 3];
        dsum ^= ((d[i] >> 1) & MFM_M) ^ (d[i] & MFM_M);
    }
    l[0] = mfm_clock(0, *prev & 1);                         /* 00 00 */
    l[1] = 0x44894489u;                                     /* two sync words; the last data bit of A1 is 1 */
    *prev = l[1];
    mfm_u32 hs = mfm_encode_block(l + 2, &info, 1, prev);   /* info: 2 longs */
    hs ^= mfm_encode_block(l + 4, z, 4, prev);              /* label: 8 longs */
    mfm_u32 hdr = hs & MFM_M;
    mfm_encode_block(l + 12, &hdr, 1, prev);                /* header checksum: 2 longs */
    mfm_encode_block(l + 14, &dsum, 1, prev);               /* data checksum: 2 longs */
    mfm_encode_block(l + 16, d, 128, prev);                 /* data: 256 longs */
    for (int i = 0; i < MFM_SECTOR_WORDS / 2; i++) {
        words[2 * i] = (mfm_u16)(l[i] >> 16);
        words[2 * i + 1] = (mfm_u16)l[i];
    }
}

/* A whole track as the disk is written: "gapwords" words of MFM zeros, then the 11 sectors in order. Returns the
 * number of words written. data = 11 * 512 bytes. */
static inline int mfm_encode_track(mfm_u16 *words, int gapwords, int track, const mfm_u8 *data)
{
    mfm_u32 prev = 0;
    int n = 0;
    for (int i = 0; i < gapwords; i++, n++) {
        words[n] = (i & 1) ? 0xAAAA : (mfm_u16)(mfm_clock(0, prev & 1) >> 16);
        prev = words[n];
    }
    for (int s = 0; s < MFM_SECTORS; s++, n += MFM_SECTOR_WORDS)
        mfm_encode_sector(words + n, track, s, MFM_SECTORS - s, data + 512 * s, &prev);
    return n;
}

/* Find the sectors of track "track" in a raw read of nwords words (a bit more than one revolution, starting at
 * a sync). Decoded data goes to out (11 * 512 bytes). Returns a bit mask of the sectors found with valid header
 * and data checksums. */
static inline int mfm_decode_track(const mfm_u16 *raw, int nwords, int track, mfm_u8 *out)
{
    int mask = 0;
    for (int i = 0; i < nwords;) {
        if (raw[i] != 0x4489) {
            i++;
            continue;
        }
        int j = i;
        while (j < nwords && raw[j] == 0x4489)
            j++;
        if (j + (MFM_SECTOR_WORDS - 4) > nwords)
            break;
        mfm_u32 l[270];
        for (int k = 0; k < 270; k++)
            l[k] = (mfm_u32)raw[j + 2 * k] << 16 | raw[j + 2 * k + 1];
        mfm_u32 info, hsum, dsum, tmp;
        mfm_u32 hcalc = 0;
        mfm_decode_block(&info, l, 1);
        for (int k = 0; k < 10; k++)
            hcalc ^= l[k];
        hcalc &= MFM_M;
        mfm_decode_block(&hsum, l + 10, 1);
        mfm_decode_block(&dsum, l + 12, 1);
        int fmt = (int)(info >> 24), trk = (int)((info >> 16) & 0xFF), sec = (int)((info >> 8) & 0xFF);
        if (fmt == 0xFF && hcalc == hsum && trk == track && sec < MFM_SECTORS) {
            mfm_u32 dcalc = 0;
            for (int k = 14; k < 270; k++)
                dcalc ^= l[k];
            dcalc &= MFM_M;
            if (dcalc == dsum) {
                mfm_u32 d[128];
                mfm_decode_block(d, l + 14, 128);
                for (int k = 0; k < 128; k++) {
                    out[512 * sec + 4 * k] = (mfm_u8)(d[k] >> 24);
                    out[512 * sec + 4 * k + 1] = (mfm_u8)(d[k] >> 16);
                    out[512 * sec + 4 * k + 2] = (mfm_u8)(d[k] >> 8);
                    out[512 * sec + 4 * k + 3] = (mfm_u8)d[k];
                }
                mask |= 1 << sec;
                i = j + 540;                                /* a whole sector: its data cannot hold a sync */
                continue;
            }
        }
        (void)tmp;
        i = j;          /* no good sector here (a stray sync in the gap, a damaged one): the next sync may be close */
    }
    return mask;
}
