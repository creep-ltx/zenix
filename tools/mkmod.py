#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# ProTracker modules of our own for DESIGN D40's tests (no third-party music in the repository), written from the
# format's documentation: a 20-byte title, 31 sample headers of 30 bytes (name, length in words, finetune nibble,
# volume, loop start and length in words), the song length, 127, 128 order bytes, "M.K.", the patterns (64 rows x 4
# channels x 4 bytes), the samples. The samples are generated waves.
#   tools/mkmod.py OUT     writes the emulator test's module: a short tune that uses every channel
import math
import struct
import sys

C2 = 428                        # ProTracker's C-2, finetune 0
NOTES = [856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453, 428, 404, 381, 360, 339, 320, 302, 285, 269, 254,
         240, 226, 214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113]


def wave(n, kind):
    if kind == 'sine':
        return bytes(int(round(100 * math.sin(2 * math.pi * i / n))) & 255 for i in range(n))
    if kind == 'square':
        return bytes((90 if i < n // 2 else -90) & 255 for i in range(n))
    if kind == 'saw':
        return bytes(int(-100 + 200 * i / n) & 255 for i in range(n))
    if kind == 'noise':
        x, out = 12345, bytearray()
        for _ in range(n):
            x = (x * 1103515245 + 12345) & 0x7FFFFFFF
            out.append((x >> 16) & 255)
        out[0] = out[1] = 0
        return bytes(out)
    raise ValueError(kind)


def module(cells, samples, order, title=b'zenix test', old=False):
    """cells: {(pattern, row, channel): (sample, period, effect, param)};
    samples: list of (data bytes, finetune 0..15, volume, loop start bytes, loop length bytes) for samples 1..;
    order: the positions' patterns. old: the 15-sample format without a tag"""
    ns = 15 if old else 31
    h = bytearray(title[:20].ljust(20, b'\x00'))
    for i in range(ns):
        if i < len(samples):
            d, ft, vol, ls, ll = samples[i]
            if old:
                ls_field = ls           # the old format's loop start counts bytes
            else:
                ls_field = ls // 2
            h += b'smp%02d' % (i + 1) + b'\x00' * 17 + struct.pack('>HBBHH', len(d) // 2, ft, vol, ls_field, max(1, ll // 2))
        else:
            h += b'\x00' * 22 + struct.pack('>HBBHH', 0, 0, 0, 0, 1)
    h += bytes([len(order), 127]) + bytes(order).ljust(128, b'\x00')
    if not old:
        h += b'M.K.'
    npat = max(order) + 1
    pats = bytearray(1024 * npat)
    for (p, r, c), (s, per, e, prm) in cells.items():
        o = p * 1024 + r * 16 + c * 4
        pats[o:o + 4] = bytes([(s & 0xF0) | (per >> 8), per & 255, (s & 15) << 4 | e, prm])
    return bytes(h) + bytes(pats) + b''.join(s[0] for s in samples)


def demo():
    """the emulator test's tune: 2 positions, a bass on 0, a lead with arpeggio and vibrato on 1, a pad on 2 with a
    volume slide, a noise hat on 3; speed 6, 125 BPM, then F 96 (150 BPM) in the second pattern"""
    smp = [(wave(32, 'saw'), 0, 64, 0, 32), (wave(16, 'square'), 0, 48, 0, 16), (wave(64, 'sine'), 0, 40, 0, 64),
           (wave(1024, 'noise'), 0, 32, 0, 0)]
    cells = {}
    bass = [0, 0, 7, 5]
    for p in range(2):
        for r in range(0, 64, 4):
            cells[(p, r, 0)] = (1, NOTES[bass[(r // 16) % 4]], 0, 0)
            cells[(p, r, 3)] = (4, NOTES[24], 0xC, 20 if r % 8 else 40)
        for r in range(0, 64, 8):
            cells[(p, r, 1)] = (2, NOTES[12 + (r // 8) % 5], 0, 0x37 if p == 0 else 0)
            cells[(p, r + 4, 1)] = (0, 0, 4, 0x46)
        cells[(p, 0, 2)] = (3, NOTES[19], 0, 0)
        cells[(p, 32, 2)] = (0, 0, 0xA, 0x02)
    cells[(1, 0, 0)] = (1, NOTES[0], 0xF, 0x96)
    return module(cells, smp, [0, 1], title=b'zenix sound test')


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit('usage: tools/mkmod.py OUT')
    open(sys.argv[1], 'wb').write(demo())
