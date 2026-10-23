/* SPDX-License-Identifier: GPL-2.0-only */
/* Host test of kernel/mfm.h: encode a track, check it is valid MFM bit by bit (no two 1 bits in a row, no run of more
 * than three 0 bits except around the sync words), decode it back from every possible starting sync and from a
 * "raw read" that starts in the middle of the track and wraps (as a real DMA read does). tools/mfm-test.sh */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/mfm.h"

static int bad;
#define EXPECT_TRUE(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); bad++; } else printf("ok:   %s\n", msg); } while (0)

int main(void)
{
    static mfm_u8 data[11 * 512], back[11 * 512];
    for (int i = 0; i < (int)sizeof data; i++)
        data[i] = (mfm_u8)(i * 7 + (i >> 8) * 13 + 1);
    static mfm_u16 trk[6400];
    int n = mfm_encode_track(trk, 224, 37, data);
    EXPECT_TRUE(n == 224 + 11 * 544 + MFM_TAIL_WORDS, "track length: gap + 11 sectors of 544 words + the filler word");
    /* the filler word (HRM p.247: Paula loses the last three bits of a write): an MFM zero whose last 3 bits are 0,
     * clock bit right after the last data bit of sector 10 */
    EXPECT_TRUE((trk[n - 1] & 7) == 0 && (trk[n - 1] == 0xAAA8 || trk[n - 1] == 0x2AA8) &&
                (trk[n - 1] == 0x2AA8) == (trk[n - 2] & 1), "write tail: $AAA8 / $2AA8 after sector 10");
    {   /* with its last three bits lost (and whatever was on the disk there), sector 10 still decodes */
        static mfm_u16 cut[6400];
        memcpy(cut, trk, n * sizeof *cut);
        cut[n - 1] ^= 0x0007;
        memset(back, 0, sizeof back);
        EXPECT_TRUE(mfm_decode_track(cut, n, 37, back) == 0x7FF, "write tail: losing its last 3 bits costs no sector");
        cut[n - 2] ^= 0x0007;                               /* without the tail the data's last bits would be lost */
        memset(back, 0, sizeof back);
        EXPECT_TRUE(mfm_decode_track(cut, n, 37, back) == 0x3FF, "without the tail: losing 3 bits breaks sector 10");
    }

    /* MFM validity, over the whole bit stream, ignoring the sync words (0x4489 breaks the rule on purpose) */
    int ones_adjacent = 0, long_zero = 0, run = 0, prevbit = 0;
    for (int w = 0; w < n; w++) {
        if (trk[w] == 0x4489) { run = 0; prevbit = 1; continue; }
        for (int b = 15; b >= 0; b--) {
            int bit = (trk[w] >> b) & 1;
            if (bit && prevbit) ones_adjacent++;
            if (!bit) { if (++run > 3) long_zero++; } else run = 0;
            prevbit = bit;
        }
    }
    EXPECT_TRUE(ones_adjacent == 0, "no two adjacent 1 bits (clock rule)");
    EXPECT_TRUE(long_zero == 0, "no run of more than three 0 bits");

    memset(back, 0, sizeof back);
    EXPECT_TRUE(mfm_decode_track(trk, n, 37, back) == 0x7FF && !memcmp(back, data, sizeof data), "decode: all 11 sectors, data identical");
    memset(back, 0, sizeof back);
    EXPECT_TRUE(mfm_decode_track(trk, n, 36, back) == 0, "decode: a wrong track number is rejected");

    /* a raw read as the hardware does it: starts at a sync in the middle of the track, runs past the end of the
     * written data into the next revolution (here simulated by wrapping) */
    static mfm_u16 raw[6656];
    int start = 224 + 5 * 544 + 2;                          /* the first sync word of sector 5 */
    for (int i = 0; i < 6656; i++)
        raw[i] = trk[(start + i) % n];
    memset(back, 0, sizeof back);
    EXPECT_TRUE(mfm_decode_track(raw, 6656, 37, back) == 0x7FF && !memcmp(back, data, sizeof data), "decode: a read that starts mid-track and wraps");

    /* a damaged bit in sector 3's data: that sector must be dropped, the others kept */
    static mfm_u16 dmg[6400];
    memcpy(dmg, trk, sizeof trk);
    dmg[224 + 3 * 544 + 300] ^= 0x0100;
    memset(back, 0, sizeof back);
    int m = mfm_decode_track(dmg, n, 37, back);
    EXPECT_TRUE(m == (0x7FF & ~(1 << 3)), "decode: a damaged sector fails its checksum, the other ten are found");

    /* a stray sync in the gap, 262 words before sector 0, followed by a broken header (seen in FS-UAE on track
     * 46): the decoder must not skip a whole sector's length past it and lose sector 0 */
    static mfm_u16 gap[6656];
    memcpy(gap, trk, n * sizeof *gap);
    {
        int g = 0;                                          /* build: [stray sync + 260 junk words][the 11 sectors] */
        gap[g++] = 0x4489;
        for (int i = 0; i < 261; i++)
            gap[g++] = trk[224 + 5 * 544 + 2 + 1 + i];      /* the first part of sector 5, header and all */
        memcpy(gap + g, trk + 224, 11 * 544 * sizeof *gap);
        memset(back, 0, sizeof back);
        EXPECT_TRUE(mfm_decode_track(gap, g + 11 * 544, 37, back) == 0x7FF && !memcmp(back, data, sizeof data),
                    "decode: a stray sync in the gap does not hide the sector after it");
    }

    /* the DMA started inside the first sync word (hand-off note: about 1 read in 1000): the buffer begins with
     * sector 4's header, no sync in front; it must still be found */
    {
        static mfm_u16 nos[6656];
        int st = 224 + 4 * 544 + 4;                         /* sector 4's header info, after both syncs */
        for (int i = 0; i < 6656; i++)
            nos[i] = trk[(st + i) % n];
        memset(back, 0, sizeof back);
        EXPECT_TRUE(mfm_decode_track(nos, 6656, 37, back) == 0x7FF && !memcmp(back, data, sizeof data),
                    "decode: a read that starts right after the sync words (no sync in front of the first sector)");
    }

    /* round trip of every sector with an all-ones and all-zero pattern (clock rule at the extremes) */
    memset(data, 0xFF, sizeof data);
    n = mfm_encode_track(trk, 224, 0, data);
    memset(back, 0, sizeof back);
    EXPECT_TRUE(mfm_decode_track(trk, n, 0, back) == 0x7FF && !memcmp(back, data, sizeof data), "round trip of an all-ones track");
    memset(data, 0x00, sizeof data);
    n = mfm_encode_track(trk, 224, 159, data);
    memset(back, 0xEE, sizeof back);
    EXPECT_TRUE(mfm_decode_track(trk, n, 159, back) == 0x7FF && !memcmp(back, data, sizeof data), "round trip of an all-zero track (track 159)");

    printf(bad ? "FAILED\n" : "all ok\n");
    return bad != 0;
}
