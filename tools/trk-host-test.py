#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# DESIGN D56 on the PC: the tracker player's replayers (user/trk.c, modrep.c, medrep.c, s3mrep.c, xmrep.c) and its
# mixer (user/trkmix.c), built as modplay builds them (tools/trk-host-test.c) and checked against modules of our own
# (tools/mktrk.py). Each replayer's registers tick by tick (period or rate, volume, panning, note starts, places) are
# compared with values worked out by hand from the format documents (each check says how); the mixer's bytes with a
# model of its arithmetic written here; damaged files of every format under ASan and UBSan.
import os
import random
import struct
import subprocess
import sys
import tempfile

os.environ['ASAN_OPTIONS'] = 'detect_leaks=1'
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mkmod as M              # noqa: E402
import mktrk as K              # noqa: E402

fails = 0
T = tempfile.mkdtemp(prefix='zenix-trk-host.', dir=os.environ.get('TMPDIR'))
EXE = os.path.join(T, 'tht')
CLOCK = 3546895


def check(name, ok, info=''):
    global fails
    if callable(ok):
        try:
            ok = ok()
        except (IndexError, TypeError, KeyError, ValueError) as e:
            info, ok = '%s (%s)' % (info, e), False
    print(('PASS ' if ok else 'FAIL ') + name + ('' if ok or not info else ': ' + str(info)))
    if not ok:
        fails += 1


def build():
    r = subprocess.run(['cc', '-Wall', '-Wextra', '-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-DTRK_HOST', '-o', EXE, 'tools/trk-host-test.c', 'user/trk.c', 'user/modrep.c', 'user/medrep.c',
                        'user/s3mrep.c', 'user/xmrep.c', 'user/trkmix.c'], capture_output=True, text=True)
    if r.returncode or r.stderr.strip():
        print(r.stderr)
        check('the host build, without warnings', False)
        sys.exit(1)


def run(data, ticks, extra=None):
    f = os.path.join(T, 'm.bin')
    open(f, 'wb').write(data)
    r = subprocess.run([EXE, 'trk', f, str(ticks)], capture_output=True, text=True)
    if r.returncode:
        return None, [r.stderr[-600:] + r.stdout[-200:]]
    lines = r.stdout.splitlines()
    if lines[0].startswith('error'):
        return lines[0].split(' ', 1), []
    kind, rest = lines[0][5:].split('|')
    head = [kind] + [int(v) for v in rest.split()]
    out = []
    for ln in lines[1:]:
        x = [int(v) for v in ln.split()]
        ch = [dict(zip(('flags', 'per', 'hz16', 'vol', 'pan', 'sp', 'start', 'len', 'loop', 'looplen'), x[6 + 10 * c:16 + 10 * c]))
              for c in range(head[1])]
        out.append({'pos': x[0], 'row': x[1], 'tick': x[2], 'ended': x[3], 'tnum': x[4], 'tden': x[5], 'ch': ch})
    return head, out


def col(t, key, c=0):
    return [x['ch'][c][key] for x in t]


def trig(t, c=0):
    return [x['ch'][c]['flags'] & 1 for x in t]


def hz(note_semis, c4=8363):
    return c4 * 16 * 2 ** (note_semis / 12)


# ---------------------------------------------------------------- MOD, 6 to 32 channels

def mods():
    smp = [(M.wave(64, 'sine'), 0, 32, 0, 64), (M.wave(1000, 'saw'), 0, 64, 0, 0)]
    cells = {(0, 0, 0): (1, 428, 0, 0), (0, 0, 5): (2, 214, 0xC, 20), (0, 1, 7): (1, 856, 0, 0x37)}
    h, t = run(K.modn(cells, smp, [0], 8), 12)
    check('8CHN: 8 channels, the patterns 2,048 bytes each (the bank after 1084 + 2048)',
          h[:3] == ['8-channel MOD', 8, 1] and h[4] == 0, h)
    check('8CHN: channel 5 starts C-3 (214) at volume 20, channel 0 C-2 (428) at 32',
          col(t, 'per', 5)[0] == 214 and col(t, 'vol', 5)[0] == 20 and col(t, 'per', 0)[0] == 428 and col(t, 'vol', 0)[0] == 32)
    check('8CHN: Paula\'s sides repeat: left, right, right, left',
          [t[0]['ch'][c]['pan'] for c in range(8)] == [0, 255, 255, 0, 0, 255, 255, 0])
    check('8CHN: channel 7\'s arpeggio 037 on C-1 (856): 856 720 570 on the next row',
          col(t, 'per', 7)[6:9] == [856, 720, 570], col(t, 'per', 7)[6:9])
    check('8CHN: the sample\'s place: 1084 + 2048 from the file\'s start', t[0]['ch'][0]['sp'] == 1084 + 2048, t[0]['ch'][0])
    h, t = run(K.modn({(0, 0, 15): (1, 428, 0, 0)}, smp, [0], 16), 1)
    check('16CH: sixteen channels, the last one plays', h[1] == 16 and t[0]['ch'][15]['flags'] & 1)
    h, t = run(K.modn({(0, 0, 5): (1, 428, 0, 0)}, smp, [0], 6), 1)
    check('6CHN: six channels', h[1] == 6 and t[0]['ch'][5]['per'] == 428)
    d = bytearray(K.modn({}, smp, [0], 8))
    d[1080:1084] = b'FLT8'
    h, _ = run(bytes(d), 1)
    check('FLT8 (StarTrekker\'s pattern pairs): refused, said', h[0] == 'error', h)
    h, t = run(M.module({(0, 0, 0): (1, 428, 0, 0)}, smp, [0]), 1)
    check('a 4-channel M.K. module goes to Paula as it is (native)', h[0] == 'ProTracker' and h[4] == 1, h)
    # review 2026-11-17: a looped sample plays up to its loop's end (ProTracker's length = loop start + loop length)
    lp = [(M.wave(200, 'saw'), 0, 64, 20, 20)]
    _, t = run(M.module({(0, 0, 0): (1, 428, 0, 0)}, lp, [0]), 1)
    c = t[0]['ch'][0]
    check('a sample of 200 bytes looped at 20 + 20: played 0..40, then the loop', (c['len'], c['loop'] - (c['sp'] and 0), c['looplen']) == (40, 20, 20), c)
    # review: a text file is not a 15-sample module; a 31-sample module with a tag not known is refused
    text = (b'This is a README, not a module. ' * 200)[:5000]
    h, _ = run(text, 1)
    check('a text file: not a module (the 15-sample format\'s headers checked)', h[0] == 'error', h)
    d = bytearray(M.module({(0, 0, 0): (1, 428, 0, 0)}, smp, [0]))
    d[1080:1084] = b'LARD'
    h, _ = run(bytes(d), 1)
    check('a tag not known (LARD): refused, said', h[0] == 'error' and 'tag' in h[1], h)


# ---------------------------------------------------------------- Scream Tracker 3

def S(cells, smp=None, **kw):
    smp = smp or [{'data': K.wave(64, 'sine'), 'vol': 32, 'loop': (0, 64)}, {'data': K.wave(2000, 'saw'), 'vol': 64},
                  {'data': K.wave(64, 'square'), 'vol': 40, 'c4': 16726, 'loop': (16, 48)}]
    return K.s3m(cells, smp, kw.pop('order', [0]), **kw)


C4 = K.s3mnote(4, 0)


def s3ms():
    # ST3's rate: 14,317,056 / period, the period 8363 x 16 x (1712 >> 4) / C4SPD: C-4 at 8363 is 1712, 133,804 / 16 Hz
    h, t = run(S({(0, 0, 0): (C4, 1, 255, 0, 0), (0, 0, 1): (K.s3mnote(5, 0), 3, 20, 0, 0)}), 2)
    check('S3M: loads, 4 channels, Scream Tracker 3', h[:2] == ['Scream Tracker 3', 4], h)
    check('S3M C-4 at C4SPD 8363: 14,317,056 x 16 / 1712 = 133,804 (1/16 Hz)', col(t, 'hz16')[0] == 133804, col(t, 'hz16'))
    check('S3M C-5 at C4SPD 16,726: period 1712 / 2 x 8363 / 16726 = 428: 535,212', col(t, 'hz16', 1)[0] == 14317056 * 16 // 428,
          col(t, 'hz16', 1))
    check('S3M: the volume column (20) over the sample\'s (40); the sample\'s (32) without one',
          col(t, 'vol', 1)[0] == 20 and col(t, 'vol')[0] == 32)
    check('S3M: the loop 16..48 of sample 3: start 0, length 48, loop 16 + 32', [t[0]['ch'][1][k] for k in ('start', 'len', 'loop', 'looplen')] == [0, 48, 16, 32],
          t[0]['ch'][1])
    check('S3M: channels 0 and 2 left (0-7: 48), 1 and 3 right (8-15: 208)', [t[0]['ch'][c]['pan'] for c in range(4)] == [48, 208, 48, 208])
    # D04 at speed 6: 4 a tick after the first; DF2: 2 on the first only; D0F the normal slide by 15
    _, t = run(S({(0, 0, 0): (C4, 2, 255, 4, 0x04), (0, 1, 0): (255, 0, 255, 4, 0xF2), (0, 2, 0): (255, 0, 255, 4, 0x20)}), 18)
    check('S3M D04 / DF2 / D20: 64 60 56 52 48 44, 42 x 6, 42 44 46 48 50 52', col(t, 'vol') == [64, 60, 56, 52, 48, 44] + [42] * 6 +
          [42, 44, 46, 48, 50, 52], col(t, 'vol'))
    _, t = run(S({(0, 0, 0): (C4, 2, 255, 4, 0x02), (0, 1, 0): (255, 0, 255, 4, 0)}), 12)
    check('S3M D00 takes the last value (02)', col(t, 'vol')[6:] == [54, 52, 50, 48, 46, 44], col(t, 'vol'))
    # F02 slides up: period - 8 a tick; E fine EF1: + 4 once; EE1 extra fine: + 1 once
    _, t = run(S({(0, 0, 0): (C4, 2, 255, 6, 0x02), (0, 1, 0): (255, 0, 255, 5, 0xF1), (0, 2, 0): (255, 0, 255, 5, 0xE1)}), 18)
    pers = [1712 - 8 * i for i in range(6)] + [1672 + 4] * 6 + [1677] * 6
    check('S3M F02 (up 8 a tick), EF1 (down 4 once), EE1 (down 1 once): the periods', col(t, 'hz16') == [229072896 // p for p in pers],
          [229072896 // max(1, v) if v else 0 for v in col(t, 'hz16')][:18])
    # G04 from C-4 to D-4 (1524): 16 a tick
    _, t = run(S({(0, 0, 0): (C4, 2, 255, 0, 0), (0, 1, 0): (K.s3mnote(4, 2), 0, 255, 7, 4)}), 12)
    pers = [1712] * 6 + [1712, 1696, 1680, 1664, 1648, 1632]
    check('S3M G04 toward D-4: 1712 1696 ... (16 a tick, no new start)', col(t, 'hz16') == [229072896 // p for p in pers] and trig(t)[6:] == [0] * 6)
    # H48: the sine x 8 >> 5 in ST3's periods: 0, 97x8>>5 = 24, 180x8>>5 = 45, 235 -> 58, 255 -> 63
    _, t = run(S({(0, 0, 0): (C4, 2, 255, 8, 0x48)}), 6)
    pers = [1712, 1712, 1712 + 24, 1712 + 45, 1712 + 58, 1712 + 63]
    check('S3M H48 vibrato: 1712, then + 0 24 45 58 63', col(t, 'hz16') == [229072896 // p for p in pers], col(t, 'hz16'))
    # J37 on C-4: C-4, D#4 (1440), G-4 (1140)
    _, t = run(S({(0, 0, 0): (C4, 2, 255, 10, 0x37)}), 6)
    check('S3M J37: C-4 D#4 G-4 C-4 D#4 G-4', col(t, 'hz16') == [229072896 // p for p in (1712, 1440, 1140) * 2], col(t, 'hz16'))
    # A03, T96 (150): ticks of 5 / 300 s
    _, t = run(S({(0, 0, 0): (C4, 2, 255, 1, 3), (0, 0, 1): (255, 0, 255, 20, 150)}), 7)
    check('S3M A03: three ticks a row', [x['row'] for x in t] == [0, 0, 0, 1, 1, 1, 2])
    check('S3M T96: 150 BPM, a tick 5 / 300 s from the next', (t[1]['tnum'], t[1]['tden']) == (5, 300), t[1])
    # V20 global volume halves the output
    _, t = run(S({(0, 0, 0): (C4, 2, 255, 22, 32)}), 1)
    check('S3M V20 (32): volume 64 heard as 32', col(t, 'vol') == [32])
    # SD2 delays the start by two ticks; SC3 cuts at the third
    _, t = run(S({(0, 0, 0): (C4, 2, 255, 19, 0xD2)}), 6)
    check('S3M SD2: the note starts on tick 2', trig(t) == [0, 0, 1, 0, 0, 0], trig(t))
    _, t = run(S({(0, 0, 0): (C4, 2, 255, 19, 0xC3)}), 6)
    check('S3M SC3: the volume 0 from tick 3', col(t, 'vol') == [64, 64, 64, 0, 0, 0], col(t, 'vol'))
    # Q02: retrig every second tick; Q42 the volume falls 8 each time
    _, t = run(S({(0, 0, 0): (C4, 2, 255, 17, 0x42)}), 6)
    check('S3M Q42: started on 0, 2, 4, the volume 64, 56, 48', trig(t) == [1, 0, 1, 0, 1, 0] and col(t, 'vol') == [64, 64, 56, 56, 48, 48],
          (trig(t), col(t, 'vol')))
    # O02: 512 bytes in
    _, t = run(S({(0, 0, 0): (C4, 2, 255, 15, 2)}), 1)
    check('S3M O02: start 512, length 2000 - 512', (t[0]['ch'][0]['start'], t[0]['ch'][0]['len']) == (512, 1488), t[0]['ch'][0])
    # orders: 254 skipped, 255 the end; B and C
    _, t = run(S({(0, 63, 0): (255, 0, 255, 0, 0)}, order=[0, 254, 1, 255]), 64 * 6 * 2 + 1)
    check('S3M order 254 skipped: the second position played is 2', t[64 * 6]['pos'] == 2 and t[64 * 12]['pos'] == 0 and t[64 * 12 - 1]['ended'] == 1)
    _, t = run(S({(0, 1, 0): (255, 0, 255, 3, 0x12)}, order=[0, 1]), 13)
    check('S3M C12 (decimal): the next pattern, row 12', (t[12]['pos'], t[12]['row']) == (1, 12), (t[12]['pos'], t[12]['row']))
    _, t = run(S({(0, 1, 0): (255, 0, 255, 2, 0)}, order=[0, 1]), 13)
    check('S3M B00, back: ended, position 0 row 0', t[11]['ended'] == 1 and (t[12]['pos'], t[12]['row']) == (0, 0))
    # SB0 / SB2: rows 0 1 0 1 0 1 2
    _, t = run(S({(0, 0, 0): (C4, 2, 255, 19, 0xB0), (0, 1, 0): (255, 0, 255, 19, 0xB2)}), 42)
    check('S3M SB0 / SB2 pattern loop: rows 0 1 0 1 0 1 2', [x['row'] for x in t[::6]] == [0, 1, 0, 1, 0, 1, 2])
    # key off (254) stops the sample
    _, t = run(S({(0, 0, 0): (C4, 2, 255, 0, 0), (0, 1, 0): (254, 0, 255, 0, 0)}), 7)
    check('S3M ^^ (254): the channel stops', t[6]['ch'][0]['flags'] & 16)
    # 16-bit and signed (format 1) samples; mono
    h, t = run(S({(0, 0, 0): (C4, 1, 255, 0, 0)}, smp=[{'data': struct.pack('<4h', 0x4000, -0x4000, 0x7F00, 0), 'b16': True,
                                                       'vol': 64}]), 1)
    check('S3M 16-bit sample: 4 samples (the bytes halved)', t[0]['ch'][0]['len'] == 4, t[0]['ch'][0])
    h, t = run(S({(0, 0, 0): (C4, 2, 255, 0, 0)}, mv=0x30), 1)
    check('S3M mono (master volume\'s bit 7 clear): every channel in the middle', all(t[0]['ch'][c]['pan'] == 128 for c in range(4)))
    d = bytearray(S({}))
    d[44:48] = b'SCRX'
    h, _ = run(bytes(d), 1)
    check('not SCRM: not an S3M (falls to the MOD reader, which refuses it or reads it as one)', h is not None)


# ---------------------------------------------------------------- FastTracker 2

def X(pats, ins=None, **kw):
    ins = ins or [{'name': b'sine', 'samples': [{'data': K.wave(64, 'sine'), 'loop': (0, 64), 'vol': 48}]},
                  {'name': b'saw', 'samples': [{'data': K.wave(2000, 'saw'), 'vol': 64}]}]
    if isinstance(pats, dict):
        pats = [(64, pats)]
    return K.xm(pats, ins, kw.pop('order', [0]), **kw)


C4X = 49        # XM's note 49 is C-4 (1 = C-0)


def xms():
    h, t = run(X({(0, 0): (C4X, 1, 0, 0, 0), (0, 1): (C4X + 12, 2, 0x30, 0, 0)}), 2)
    check('XM: loads, 4 channels, linear', h[:2] == ['FastTracker 2', 4], h)
    # linear: period 7680 - 48 x 64 = 4608, rate 8363 x 2^0 = 8363 Hz: 133,808 / 16
    check('XM linear C-4: 8363 Hz (133,808)', col(t, 'hz16')[0] == 133808, col(t, 'hz16'))
    check('XM linear C-5: 16,726 Hz', col(t, 'hz16', 1)[0] == 267616, col(t, 'hz16', 1))
    check('XM: the volume column 0x30 sets 32; the sample\'s 48 without one', col(t, 'vol', 1)[0] == 32 and col(t, 'vol')[0] == 48)
    check('XM: the sample\'s panning (128)', t[0]['ch'][0]['pan'] == 128)
    h, t = run(X({(0, 0): (C4X, 1, 0, 0, 0), (0, 1): (C4X + 12, 2, 0, 0, 0)}, linear=False), 1)
    check('XM Amiga C-4: period 856 x 32 >> 4 = 1712, 8363 x 1712 / 1712 Hz', col(t, 'hz16')[0] == 229079296 // 1712, col(t, 'hz16'))
    check('XM Amiga C-5: period 856', col(t, 'hz16', 1)[0] == 229079296 // 856, col(t, 'hz16', 1))
    # finetune +16 (a 1/8 semitone, 8 periods): 8363 x 2^(8/768)
    ins = [{'samples': [{'data': K.wave(64, 'sine'), 'fine': 16, 'rel': 12}]}]
    _, t = run(X({(0, 0): (C4X, 1, 0, 0, 0)}, ins), 1)
    check('XM finetune +16 and relative note +12: C-5 and an eighth: %d' % round(hz(12 + 1 / 8)),
          abs(col(t, 'hz16')[0] - hz(12 + 1 / 8)) <= 16, col(t, 'hz16'))
    # 1xx: the period - 4 x xx a tick; 2xx +; separate memories
    _, t = run(X({(0, 0): (C4X, 2, 0, 1, 2), (1, 0): (0, 0, 0, 1, 0)}), 12)
    pers = [4608 - 8 * i for i in range(6)] + [4568 - 8 * i for i in range(6)]
    expect = [round(8363 * 16 * 2 ** ((4608 - p) / 768)) for p in pers]
    got = col(t, 'hz16')
    check('XM 102 then 100 (its memory): the period - 8 a tick, the rate from it (within 1/20,000)', all(abs(a - b) <= b // 20000 for a, b in zip(got, expect)),
          (got, expect))
    # A0F/A20: slide down 15, up 2; Cxx; vol column slides -1 / +1 a tick
    _, t = run(X({(0, 0): (C4X, 2, 0, 0xA, 0x0F), (1, 0): (0, 0, 0x71, 0, 0)}), 12)
    check('XM A0F: 64 49 34 19 4 0, then the column +1: 0 1 2 3 4 5', col(t, 'vol') == [64, 49, 34, 19, 4, 0, 0, 1, 2, 3, 4, 5], col(t, 'vol'))
    # a volume envelope: (0, 64), (4, 32), (8, 32) with sustain at point 1; key off (97); fadeout 4096 a tick
    env = {'samples': [{'data': K.wave(64, 'sine'), 'loop': (0, 64), 'vol': 64}], 'venv': ([(0, 64), (4, 32), (8, 0)], 1, 0, 0, 3),
           'fade': 2048}
    _, t = run(X({(0, 0): (C4X, 1, 0, 0, 0), (3, 0): (97, 0, 0, 0, 0)}, [env]), 24)
    v = col(t, 'vol')
    check('XM volume envelope 64 -> 32 over 4 frames, held at the sustain point: 64 56 48 40 32 32 ...', v[:12] == [64, 56, 48, 40] + [32] * 8, v[:12])
    # key off at tick 18: the frame moves on (32 -> 0 over 4), faded by 2048 / 65536 a tick: 32 x 63488 >> 16 = 31,
    # then 24 x 61440 >> 16 = 22, 16 x 59392 >> 16 = 14, 8 x 57344 >> 16 = 7, 0
    check('XM key off: on from the sustain point, faded by 2048 a tick: 31 22 14 7 0', v[18:23] == [31, 22, 14, 7, 0], v[18:24])
    # no envelope: the key off silences the note
    _, t = run(X({(0, 0): (C4X, 1, 0, 0, 0), (1, 0): (97, 0, 0, 0, 0)}), 7)
    check('XM key off without an envelope: volume 0', col(t, 'vol')[6] == 0, col(t, 'vol'))
    # a panning envelope (0, 64): 128 + (64-32) x (128 - 0) / 32 = 255
    penv = {'samples': [{'data': K.wave(64, 'sine'), 'loop': (0, 64)}], 'penv': ([(0, 64)], 0, 0, 0, 1)}
    _, t = run(X({(0, 0): (C4X, 1, 0, 0, 0)}, [penv]), 1)
    check('XM panning envelope at 64: hard right (255)', t[0]['ch'][0]['pan'] == 255, t[0]['ch'][0])
    # ping-pong: 32 bytes forward then 32 back, in a copy of its own
    pp = {'samples': [{'data': list(range(32)), 'loop': (8, 24), 'ping': True}]}
    _, t = run(X({(0, 0): (C4X, 1, 0, 0, 0)}, [pp]), 1)
    c = t[0]['ch'][0]
    check('XM ping-pong 8+24: a copy, played 0..80 with the loop 8 + 48', c['sp'] == -1 and (c['start'], c['len'], c['loop'], c['looplen']) == (0, 56, 8, 48), c)
    # 9xx: with a note only, and its memory
    _, t = run(X({(0, 0): (C4X, 2, 0, 9, 2), (1, 0): (C4X, 2, 0, 9, 0), (2, 0): (0, 0, 0, 9, 5)}), 13)
    check('XM 902 then 900 (its memory): start 512 both; 905 without a note: nothing',
          (t[0]['ch'][0]['start'], t[6]['ch'][0]['start']) == (512, 512) and trig(t)[12] == 0)
    # Fxx: speed below 32, BPM from 32; Gxx global volume
    _, t = run(X({(0, 0): (C4X, 2, 0, 0xF, 3), (0, 1): (0, 0, 0, 0xF, 0x96), (0, 2): (0, 0, 0, 16, 0x20)}), 4)
    check('XM F03 and F96: three ticks a row, 150 BPM', [x['row'] for x in t] == [0, 0, 0, 1] and (t[1]['tnum'], t[1]['tden']) == (5, 300))
    check('XM G20: the global volume 32 halves every channel', col(t, 'vol')[0] == 32, col(t, 'vol'))
    # 16 channels; 16-bit samples become their high bytes
    h, t = run(X({(0, 15): (C4X, 1, 0, 0, 0)}, nch=16), 1)
    check('XM 16 channels', h[1] == 16 and t[0]['ch'][15]['flags'] & 1)
    w16 = {'samples': [{'data': [0x4000] * 64, 'b16': True, 'loop': (0, 64), 'vol': 8}]}
    h, t = run(X({(0, 0): (C4X, 1, 0, 0, 0)}, [w16], nch=1), 1)
    check('XM 16-bit sample: 64 samples, loop 0 + 64', (t[0]['ch'][0]['len'], t[0]['ch'][0]['looplen']) == (64, 64), t[0]['ch'][0])
    f = os.path.join(T, 'w16.xm')
    open(f, 'wb').write(X({(0, 0): (C4X, 1, 0, 0, 0)}, [w16], nch=1))
    o = os.path.join(T, 'w16.pcm')
    subprocess.run([EXE, 'mix', f, '8000', '64', o], capture_output=True)
    pcm = open(o, 'rb').read()
    # 0x4000's high byte 64, volume 8; the gain for one channel 2 x 127 x 256 / 2032 = 32: 64 x 8 x 32 / 256 = 64 in
    # the middle sum, so 64 on both sides
    check('XM 16-bit through the mixer: every byte 64 left and right', len(pcm) == 128 and set(pcm) == {64}, set(pcm))
    # B and D, E6 loops, EE delay
    _, t = run(X([(64, {(1, 0): (0, 0, 0, 0xD, 0x12)}), (64, {})], order=[0, 1]), 13)
    check('XM D12 (decimal): the next position, row 12', (t[12]['pos'], t[12]['row']) == (1, 12), (t[12]['pos'], t[12]['row']))
    _, t = run(X([(32, {}), (16, {})], order=[0, 1]), 6 * 48 + 1)
    check('XM patterns of 32 and 16 rows: position 1 after 32 rows, the end after 48', t[32 * 6]['pos'] == 1 and t[48 * 6]['pos'] == 0 and t[48 * 6 - 1]['ended'] == 1)
    _, t = run(X({(0, 0): (C4X, 2, 0, 0xE, 0x60), (1, 0): (0, 0, 0, 0xE, 0x62)}), 42)
    check('XM E60 / E62: rows 0 1 0 1 0 1 2', [x['row'] for x in t[::6]] == [0, 1, 0, 1, 0, 1, 2])
    _, t = run(X({(0, 0): (C4X, 2, 0, 0xE, 0x58), (1, 0): (C4X, 2, 0, 0xE, 0x59)}), 7)
    check('XM E58: finetune none (8363 Hz); E59: +16 (an eighth of a semitone up)', col(t, 'hz16')[0] == 133808 and
          abs(col(t, 'hz16')[6] - hz(1 / 8)) <= 16, col(t, 'hz16'))
    _, t = run(X({(0, 0): (C4X, 2, 0, 0xE, 0xD3)}), 6)
    check('XM ED3: the note on tick 3', trig(t) == [0, 0, 0, 1, 0, 0], trig(t))
    h, _ = run(X({}, nch=4)[:200], 1)
    check('XM cut in its patterns: refused', h[0] == 'error', h)


# ---------------------------------------------------------------- MED / OctaMED

def MD(cells, ver=0, ntr=4, lines=64, smp=None, **kw):
    smp = smp or [(K.wave(64, 'sine'), 32, 0, 64, 0), (K.wave(1000, 'saw'), 64, 0, 0, 0)]
    return K.med([(ntr, lines, cells)], kw.pop('seq', [0]), smp, ver=ver, **kw)


def meds():
    for ver in (0, 1, 2):
        h, t = run(MD({(0, 0): (13, 1, 0, 0), (0, 1): (25, 2, 0xC, 0x32)}, ver=ver, flags=0), 2)
        check('MMD%d: loads, 4 tracks, Paula as it is (native)' % ver, h[1] == 4 and h[4] == 1, h)
        check('MMD%d: C-2 (13) is 428, C-3 (25) 214; C32 decimal is 32' % ver,
              col(t, 'per')[0] == 428 and col(t, 'per', 1)[0] == 214 and col(t, 'vol', 1)[0] == 32 and col(t, 'vol')[0] == 32,
              (col(t, 'per'), col(t, 'vol', 1)))
    _, t = run(MD({(0, 0): (13, 2, 0xC, 0x20)}, flags=0x10), 1)
    check('MED FLAG_VOLHEX: C20 is 32', col(t, 'vol') == [32])
    # tempo 33 (SPD): 33 / (50 x 33) s, ProTracker's 20 ms; BPM mode 125 x 4 lines: 10 / 500 s
    _, t = run(MD({}), 1)
    check('MED tempo 33: a tick 33 / 1650 s (20 ms)', (t[0]['tnum'], t[0]['tden']) == (33, 1650), t[0])
    _, t = run(MD({}, tempo=125, flags2=0x20 | 3), 1)
    check('MED BPM 125, 4 lines a beat: a tick 10 / 500 s', (t[0]['tnum'], t[0]['tden']) == (10, 500), t[0])
    _, t = run(MD({(0, 0): (13, 2, 9, 3)}, tempo=125, flags2=0x20 | 3), 7)
    check('MED 0903: three ticks a line', [x['row'] for x in t] == [0, 0, 0, 1, 1, 1, 2])
    # 0D01: down 1 every tick, the first too; with FLAG_STSLIDE not on the first
    _, t = run(MD({(0, 0): (13, 2, 0xD, 0x01)}, flags=0x10), 6)
    check('MED 0D01: 63 62 61 60 59 58 (every tick)', col(t, 'vol') == [63, 62, 61, 60, 59, 58], col(t, 'vol'))
    _, t = run(MD({(0, 0): (13, 2, 0xD, 0x01)}, flags=0x30), 6)
    check('MED 0D01 with FLAG_STSLIDE: 64 63 62 61 60 59', col(t, 'vol') == [64, 63, 62, 61, 60, 59], col(t, 'vol'))
    # 01 02: the period - 2 a tick (every tick)
    _, t = run(MD({(0, 0): (13, 2, 1, 2)}), 3)
    check('MED 0102: 426 424 422', col(t, 'per') == [426, 424, 422], col(t, 'per'))
    # 03 portamento toward D-2 (381) at 8: no start
    _, t = run(MD({(0, 0): (13, 2, 0, 0), (1, 0): (15, 0, 3, 8)}, flags=0x30), 12)
    check('MED 0308 (ST slides): 428 420 412 404 396 388, no new start', col(t, 'per')[6:] == [428, 420, 412, 404, 396, 388] and trig(t)[6:] == [0] * 6,
          col(t, 'per')[6:])
    # 04 vibrato: MED's depth twice ProTracker's: sine x depth >> 6
    _, t = run(MD({(0, 0): (13, 2, 4, 0x48)}), 6)
    check('MED 0448 vibrato: 428, then + 0 12 22 29 31 (twice 0x48 in ProTracker)', col(t, 'per') == [428, 428, 440, 450, 457, 459], col(t, 'per'))
    _, t = run(MD({(0, 0): (13, 2, 0x14, 0x48)}, ver=1), 6)
    check('MED 1448 (ProTracker\'s): 428 428 434 439 442 443', col(t, 'per') == [428, 428, 434, 439, 442, 443], col(t, 'per'))
    # 0F00 the next block; 0FFE stops; 0FFF cuts the note
    _, t = run(MD({(1, 0): (0, 0, 0xF, 0)}, seq=[0, 0]), 13)
    check('MED 0F00: the next block after line 1', (t[12]['pos'], t[12]['row']) == (1, 0))
    _, t = run(MD({(1, 0): (0, 0, 0xF, 0xFE)}), 30)
    check('MED 0FFE: the song stops', len(t) == 7, len(t))
    _, t = run(MD({(0, 0): (13, 2, 0, 0), (1, 0): (0, 0, 0xF, 0xFF)}), 7)
    check('MED 0FFF: the note stops', t[6]['ch'][0]['flags'] & 16)
    # 1F20: delay two ticks; 1F02 retrig every second; 19 offset; 16 loop; 18 cut
    _, t = run(MD({(0, 0): (13, 2, 0x1F, 0x20)}, ver=1), 6)
    check('MED 1F20: the note on tick 2', trig(t) == [0, 0, 1, 0, 0, 0], trig(t))
    _, t = run(MD({(0, 0): (13, 2, 0x1F, 0x02)}, ver=1), 6)
    check('MED 1F02: on ticks 0 2 4', trig(t) == [1, 0, 1, 0, 1, 0], trig(t))
    _, t = run(MD({(0, 0): (13, 2, 0x19, 2)}, ver=1), 1)
    base = t[0]['ch'][0]['start'] - 512
    check('MED 1902: 512 bytes in, 488 left', t[0]['ch'][0]['len'] == 488, t[0]['ch'][0])
    _, t = run(MD({(0, 0): (13, 2, 0x16, 0), (1, 0): (0, 0, 0x16, 2)}, ver=1), 42)
    check('MED 1600 / 1602 loop: lines 0 1 0 1 0 1 2', [x['row'] for x in t[::6]] == [0, 1, 0, 1, 0, 1, 2])
    _, t = run(MD({(0, 0): (13, 2, 0x18, 3)}, ver=1), 6)
    check('MED 1803: silent from tick 3', col(t, 'vol') == [64, 64, 64, 0, 0, 0], col(t, 'vol'))
    # the play transpose (+12), the instrument's (-12: back)
    _, t = run(MD({(0, 0): (13, 1, 0, 0)}, transp=12), 1)
    check('MED play transpose +12: C-2 plays C-3 (214)', col(t, 'per') == [214])
    # track volume 32 and master 32: a quarter
    _, t = run(MD({(0, 0): (13, 2, 0, 0)}, trkvol=[32] * 16, master=32), 1)
    check('MED track volume 32 and master 32: 64 heard as 16', col(t, 'vol') == [16], col(t, 'vol'))
    # MMD0's instrument bits: instrument 17 = x (16) + 1
    smp = [(K.wave(64, 'sine'), 10 + i, 0, 64, 0) for i in range(17)]
    _, t = run(MD({(0, 0): (13, 17, 0, 0)}, smp=smp), 1)
    check('MMD0 instrument 17 (its bit 4 in the note byte): volume 26', col(t, 'vol') == [26], col(t, 'vol'))
    # 8 tracks: mixed
    h, t = run(MD({(0, 7): (13, 1, 0, 0)}, ver=1, ntr=8), 1)
    check('MMD1 of 8 tracks: 8 channels, mixed (not native)', h[1] == 8 and h[4] == 0 and t[0]['ch'][7]['flags'] & 1)
    # names and the song's name from the expansion
    f = os.path.join(T, 'n.med')
    open(f, 'wb').write(MD({}, names=[b'first', b'second'], songname=b'a song'))
    r = subprocess.run([EXE, 'cells', f, '0', '0'], capture_output=True, text=True)
    check('MED cells of an empty line', r.stdout.strip() == '--- .. .. ...|' * 4, r.stdout)
    # a synthetic instrument is silent
    d = bytearray(MD({(0, 0): (13, 1, 0, 0)}))
    sa = struct.unpack('>I', d[24:28])[0]
    i0 = struct.unpack('>I', d[sa:sa + 4])[0]
    d[i0 + 4:i0 + 6] = b'\xff\xff'
    _, t = run(bytes(d), 1)
    check('MED synthetic instrument: silent', t[0]['ch'][0]['flags'] & 16)


# ---------------------------------------------------------------- the mixer against a model of its arithmetic

def mixmodel(regs, data, nch, rate, frames):
    """the mixer as D56 section 5 says: each channel's 16.16 place stepping through its 8-bit bytes, sample x vol x
    gain / 256 added into the left, right or middle sum, left + middle and right + middle clipped to 8 bits"""
    kper = (CLOCK << 16) // rate
    half = max(1, nch // 2)
    amp = max(1, 2 * 127 * 256 // (half * 2032))
    ch = [None] * nch
    out = bytearray()
    acc = 0
    sb = lambda b: b - 256 if b > 127 else b      # noqa: E731
    for tick in regs:
        if frames <= 0:
            break
        for c, v in enumerate(tick['ch']):
            st = ch[c] or {'on': False, 'x': 0, 'p': 0, 'n': 0, 'lp': 0, 'ln': 0, 'step': 0, 'vol': 0, 'side': 0}
            if v['flags'] & 16:
                st['on'] = False
            if v['flags'] & 1:
                st.update(on=True, p=v['sp'] + v['start'], n=v['len'], x=0)
                if not st['n']:
                    st.update(p=v['sp'] + v['loop'], n=v['looplen'], on=bool(v['looplen']))
            if v['flags'] & 9:
                st.update(lp=v['sp'] + v['loop'], ln=v['looplen'])
            st['step'] = kper // v['per'] if v['per'] > 0 else ((v['hz16'] << 12) // rate)
            st['vol'] = min(64, max(0, v['vol']))
            st['side'] = 0 if v['pan'] < 64 else 1 if v['pan'] > 192 else 2
            ch[c] = st
        acc += rate * tick['tnum']
        n = acc // tick['tden']
        acc -= n * tick['tden']
        n = min(frames, n)
        for _ in range(n):
            s = [0, 0, 0]
            for st in ch:
                if not st or not st['on']:
                    continue
                while st['on'] and (st['x'] >> 16) >= st['n']:
                    st['x'] -= st['n'] << 16
                    st['p'], st['n'] = st['lp'], st['ln']
                    if st['n'] < 1:
                        st['on'] = False
                    elif (st['x'] >> 16) >= st['n']:
                        st['x'] = 0
                if not st['on']:
                    continue
                if st['vol']:
                    b = sb(data[st['p'] + (st['x'] >> 16)])
                    s[st['side']] += int(b * st['vol'] * amp / 256)
                st['x'] += st['step']
            for side in (0, 1):
                out.append(max(-128, min(127, s[side] + s[2])) & 255)
        frames -= n
    return bytes(out)


def mixer():
    for name, data in (('the 8-channel tune', K.tune8()), ('the busy 8-channel MOD', K.busy_mod(8))):
        f = os.path.join(T, 'mx.mod')
        open(f, 'wb').write(data)
        ticks = 40
        h, regs = run(data, ticks)
        o = os.path.join(T, 'mx.pcm')
        for rate in (11025, 16000, 22050):
            acc, frames = 0, -1
            for x in regs:
                acc += rate * x['tnum']
                frames += acc // x['tden']
                acc %= x['tden']
            r = subprocess.run([EXE, 'mix', f, str(rate), str(frames), o], capture_output=True, text=True)
            got = open(o, 'rb').read() if not r.returncode else b''
            want = mixmodel(regs, data, h[1], rate, frames)
            diff = next((i for i in range(min(len(got), len(want))) if got[i] != want[i]), None)
            check('the mixer, %s at %d Hz: %d frames byte for byte as the model' % (name, rate, frames),
                  len(got) == len(want) and diff is None, (len(got), len(want), diff, r.stderr[-300:]))


# ---------------------------------------------------------------- damaged files

def damaged():
    rnd = random.Random(11)
    bases = [('MOD8', K.tune8()), ('MED', K.tune_med()), ('MMD2', K.med([(4, 16, {(0, 0): (13, 1, 0x19, 3)})], [0], [(K.wave(64, 'sine'), 64, 0, 64, 0)], ver=2)),
             ('S3M', K.tune_s3m()), ('XM', K.tune_xm()), ('XM16', K.busy_xm(16))]
    f = os.path.join(T, 'dmg')
    o = os.path.join(T, 'dmg.pcm')
    for name, base in bases:
        crashes, refused, info = 0, 0, ''
        for i in range(150):
            b = bytearray(base)
            for _ in range(rnd.randint(1, 30)):
                b[rnd.randrange(len(b))] = rnd.randrange(256)
            if rnd.random() < 0.3:
                b = b[:rnd.randrange(64, len(b))]
            if rnd.random() < 0.2:                         # a pointer or count made large
                k = rnd.randrange(0, min(len(b) - 4, 1200))
                b[k:k + 4] = struct.pack('>I', rnd.choice([0x7FFFFFFF, 0xFFFFFFFF, len(b) - 2, 0x10000]))
            open(f, 'wb').write(bytes(b))
            r = subprocess.run([EXE, 'trk', f, '300'], capture_output=True, text=True, timeout=60)
            if r.returncode:
                crashes += 1
                info = info or r.stderr[-400:] + r.stdout[-100:]
            elif r.stdout.startswith('error'):
                refused += 1
            else:
                r = subprocess.run([EXE, 'mix', f, '16000', '3000', o], capture_output=True, text=True, timeout=60)
                if r.returncode:
                    crashes += 1
                    info = info or r.stderr[-400:]
        check('150 damaged %s files: no crash, no place outside the file (ASan, UBSan; %d refused)' % (name, refused), crashes == 0, info)


build()
mods()
s3ms()
xms()
meds()
mixer()
damaged()
print('trk-host-test: %s' % ('ok' if not fails else '%d failed' % fails))
sys.exit(1 if fails else 0)
