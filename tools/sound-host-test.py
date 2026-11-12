#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# DESIGN D40 on the PC: play's file reader and modplay's replayer (user/snddec.c, user/modrep.c) built as they are
# (tools/sound-host-test.c) and checked against files of our own (tools/mksound.py, tools/mkmod.py). The decoder's
# bytes are compared with mksound's expectation; the replayer's periods, volumes and note starts tick by tick with
# values worked out by hand from ProTracker's documented effects (each check says how).
import os
import subprocess
import sys
import tempfile

os.environ['ASAN_OPTIONS'] = 'detect_leaks=0'    # the driver keeps its buffers to the end

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mksound as S            # noqa: E402
import mkmod as M              # noqa: E402

fails = 0
T = tempfile.mkdtemp(prefix='zenix-sound-host.', dir=os.environ.get('TMPDIR'))
EXE = os.path.join(T, 'sht')


def check(name, ok, info=''):
    global fails
    if callable(ok):
        try:
            ok = ok()
        except (IndexError, TypeError, KeyError) as e:
            info, ok = '%s (%s)' % (info, e), False
    print(('PASS ' if ok else 'FAIL ') + name + ('' if ok or not info else ': ' + str(info)))
    if not ok:
        fails += 1


def build():
    r = subprocess.run(['cc', '-Wall', '-Wextra', '-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-o', EXE, 'tools/sound-host-test.c', 'user/snddec.c', 'user/modrep.c'], capture_output=True, text=True)
    if r.returncode or r.stderr.strip():
        print(r.stderr)
        check('the host build, without warnings', False)
        sys.exit(1)


def dec(data, maxrate=28604, mono=0):
    f = os.path.join(T, 'in')
    o = os.path.join(T, 'out')
    open(f, 'wb').write(data)
    r = subprocess.run([EXE, 'dec', f, str(maxrate), str(mono), o], capture_output=True, text=True)
    if r.returncode:
        return ('crash', r.stderr[-300:]) + (None,) * 8, b''
    line = r.stdout.split()
    if line[0] == 'error':
        return ('error', ' '.join(line[1:])) + (None,) * 8, b''
    return tuple(int(x) for x in line), open(o, 'rb').read()


def sb(v):
    return bytes(x & 255 for x in v)


def decoders():
    # 8-bit mono WAV at 11,025 Hz: unchanged
    t = S.tone(1000, 1000, 11025, bits=8)
    h, out = dec(S.wav(1, 11025, 8, [(v,) for v in t]))
    check('WAV 8-bit mono: the header', h[:4] == (1, 11025, 1, 8) and h[5:8] == (1, 11025, 1), h)
    check('WAV 8-bit mono: every byte', out == sb(S.expect([(v,) for v in t], 8)))
    # 16-bit stereo 44.1 kHz: halved to 22,050, stereo kept
    a, b = S.tone(3001, 440, 44100), S.tone(3001, 660, 44100, phase=1.0)
    fr = list(zip(a, b))
    h, out = dec(S.wav(2, 44100, 16, fr))
    check('WAV 16-bit stereo 44.1 kHz: two to one, 22,050 Hz, stereo', h[5:8] == (2, 22050, 0) and h[8] == 1500, h)
    check('WAV 16-bit stereo: every byte (averaged pairs, rounded)', out == sb(S.expect(fr, 16, maxrate=28604, rate=44100)))
    h, out = dec(S.wav(2, 44100, 16, fr), mono=1)
    check('WAV 16-bit stereo mixed to mono', h[7] == 1 and out == sb(S.expect(fr, 16, maxrate=28604, rate=44100, mono=True)))
    # 48 kHz, WAVE_FORMAT_EXTENSIBLE
    t = S.tone(2400, 1000, 48000)
    h, out = dec(S.wav(1, 48000, 16, [(v,) for v in t], extensible=True))
    check('WAV_EXTENSIBLE 48 kHz: 24,000 Hz', h[5:7] == (2, 24000), h)
    check('WAV_EXTENSIBLE: every byte', out == sb(S.expect([(v,) for v in t], 16, maxrate=28604, rate=48000)))
    # 96 kHz: four to one
    t = S.tone(400, 1000, 96000)
    h, out = dec(S.wav(1, 96000, 16, [(v,) for v in t]))
    check('96 kHz: four to one', h[5:7] == (4, 24000) and out == sb(S.expect([(v,) for v in t], 16, maxrate=28604, rate=96000)), h)
    # extremes: full scale both ways (rounding never above 127)
    fr = [(32767,), (-32768,), (127,), (128,), (-129,), (0,)]
    h, out = dec(S.wav(1, 8000, 16, fr))
    check('16-bit full scale and rounding', out == sb([127, -128, 0, 1, -1, 0]), list(out))
    # refusals
    bad = bytearray(S.wav(1, 8000, 16, fr))
    bad[20:22] = b'\x02\x00'                    # ADPCM
    h, _ = dec(bytes(bad))
    check('compressed WAV refused', h[0] == 'error', h)
    h, _ = dec(S.wav(3, 8000, 16, [(1, 2, 3)]))
    check('three channels refused', h[0] == 'error' and 'two' in h[1], h)
    whole = S.wav(1, 8000, 16, [(v,) for v in S.tone(100, 300, 8000)])
    for cut in (14, 30, 40, 50):
        h, _ = dec(whole[:cut])
        check('WAV cut at %d bytes: an error, no crash' % cut, h[0] == 'error', h)
    h, out = dec(whole[:-11])
    check('WAV with its data cut: what is there', h[8] == 94 if isinstance(h[0], int) else False, h)
    # 8SVX plain, packed, stereo, several octaves
    v = S.tone(999, 523, 8363, bits=8)
    h, out = dec(S.svx(8363, v, vol=0x8000))
    check('8SVX: rate, volume 32', h[:2] == (2, 8363) and h[4] == 32, h)
    check('8SVX: every byte', out == sb(v))
    pk = S.fib_pack(v)
    h, out = dec(S.svx(8363, v, fib=True))
    check('8SVX Fibonacci delta: every byte', out == sb(S.fib_unpack(pk)[:len(out)]) and len(out) >= len(v), (len(out), len(v)))
    r = S.tone(999, 330, 8363, bits=8)
    h, out = dec(S.svx(8363, v, right=r))
    check('8SVX stereo (CHAN 6): left and right', h[2] == 2 and out == sb([x for p in zip(v, r) for x in p]), h)
    h, out = dec(S.svx(8363, v, right=r, fib=True))
    lu, ru = S.fib_unpack(S.fib_pack(v)), S.fib_unpack(S.fib_pack(r))
    check('8SVX stereo packed', out == sb([x for p in zip(lu, ru) for x in p]), (len(out), len(lu)))
    h, out = dec(S.svx(8363, v, octaves=2, extra=b'\x7f' * 2000))
    check('8SVX of two octaves: the first only', out == sb(v), (len(out), len(v)))
    # AIFF, AIFF-C NONE and sowt, 8-bit AIFF
    t = S.tone(1601, 750, 16000)
    for kind in (None, b'NONE', b'sowt'):
        h, out = dec(S.aiff(1, 16000, 16, [(x,) for x in t], aifc=kind))
        check('AIFF%s: 16,000 Hz, every byte' % ('-C ' + kind.decode() if kind else ''),
              h[:2] == (3, 16000) and out == sb(S.expect([(x,) for x in t], 16)), h)
    t8 = S.tone(500, 750, 22050, bits=8)
    h, out = dec(S.aiff(2, 22050, 8, list(zip(t8, t8[::-1]))))
    check('AIFF 8-bit stereo', out == sb([x for p in zip(t8, t8[::-1]) for x in p]), h)
    check('AIFF rates as 80-bit numbers: 8000, 11025, 22050, 44100, 48000',
          all(dec(S.aiff(1, rr, 8, [(0,)] * 4))[0][1] == rr for rr in (8000, 11025, 22050, 44100, 48000)))
    bad = S.aiff(1, 16000, 16, [(1,)] * 10, aifc=b'ima4')
    check('compressed AIFF-C refused', dec(bad)[0][0] == 'error')
    # garbage: never a crash
    import random
    rnd = random.Random(7)
    crashes = 0
    for i in range(300):
        base = [S.wav(2, 22050, 16, [(1, 2)] * 50), S.svx(8000, [3] * 60, fib=bool(i & 1)), S.aiff(1, 8000, 16, [(5,)] * 40)][i % 3]
        b = bytearray(base)
        for _ in range(rnd.randint(1, 6)):
            b[rnd.randrange(len(b))] = rnd.randrange(256)
        if rnd.random() < 0.3:
            b = b[:rnd.randrange(len(b))]
        h, _ = dec(bytes(b))
        crashes += h[0] == 'crash'
    check('300 damaged files: no crash (ASan, UBSan)', crashes == 0, crashes)


def run(data, ticks):
    f = os.path.join(T, 'm.mod')
    open(f, 'wb').write(data)
    r = subprocess.run([EXE, 'mod', f, str(ticks)], capture_output=True, text=True)
    if r.returncode:
        return None, [r.stderr[-400:]]
    lines = r.stdout.splitlines()
    head = lines[0].split()
    out = []
    for ln in lines[1:]:
        x = [int(v) for v in ln.split()]
        ch = [x[5 + 7 * c:12 + 7 * c] for c in range(4)]
        out.append({'pos': x[0], 'row': x[1], 'tick': x[2], 'ended': x[3], 'len': x[4], 'ch': ch})
    return head, out


SMP = [(M.wave(64, 'sine'), 0, 32, 0, 64), (M.wave(1000, 'saw'), 0, 64, 0, 0), (M.wave(64, 'square'), 1, 40, 16, 32)]


def song(rows, order=(0,), smp=SMP):
    """rows: {(pattern, row, channel): (sample, period, effect, param)}"""
    return M.module(rows, smp, list(order))


def per(t, c=0):
    return [x['ch'][c][0] for x in t]


def vol(t, c=0):
    return [x['ch'][c][1] for x in t]


def trig(t, c=0):
    return [x['ch'][c][2] & 1 for x in t]


def replayer():
    C2, D2, E2 = 428, 381, 340
    h, t = run(song({(0, 0, 0): (1, C2, 0, 0x37)}), 6)
    check('load: 31 samples, 1 position, 1 pattern, the bank after 1084 + 1024 bytes',
          h[1:5] == ['31', '1', '1', '2108'], h)
    # arpeggio 037 on C-2: C-2, D#2 (3 up), G-2 (7 up) from finetune 0's table
    check('0 37 arpeggio: 428 360 285 428 360 285', per(t) == [428, 360, 285, 428, 360, 285], per(t))
    check('a note starts on tick 0 only', trig(t) == [1, 0, 0, 0, 0, 0])
    check('the sample\'s volume (32)', vol(t) == [32] * 6)
    check('the sample\'s place: start 0, 64 bytes, looped 0+64', t[0]['ch'][0][3:] == [0, 64, 0, 64], t[0]['ch'][0])
    # 1xx / 2xx: no change on tick 0, then the step each tick; 2xx stops at 856 (C-1)
    _, t = run(song({(0, 0, 0): (1, C2, 1, 2), (0, 1, 0): (0, 0, 1, 2)}), 12)
    check('1 02 portamento up: 428 426 ... 418, on through the next row', per(t) == [428 - 2 * i for i in range(6)] + [418 - 2 * i for i in range(6)], per(t))
    _, t = run(song({(0, 0, 0): (1, 808, 2, 0x20)}), 6)
    check('2 20 portamento down from 808: stops at 856', per(t) == [808, 840, 856, 856, 856, 856], per(t))
    _, t = run(song({(0, 0, 0): (1, 120, 1, 5)}), 4)
    check('1 05 from 120: stops at 113 (B-3)', per(t) == [120, 115, 113, 113], per(t))
    # 3xx toward D-2 at 4 a tick: no new note; 3 00 goes on with the speed kept; it stops at the target
    _, t = run(song({(0, 0, 0): (1, C2, 0, 0), (0, 1, 0): (1, D2, 3, 4), (0, 2, 0): (0, 0, 3, 0), (0, 3, 0): (0, 0, 3, 0)}), 24)
    # (no step on a row's tick 0: five steps a row at speed 6)
    check('3 04 tone portamento C-2 to D-2: 428 424 ... 408, 408 ... 388, 388 384 then 381 held',
          per(t)[6:24] == [428, 424, 420, 416, 412, 408, 408, 404, 400, 396, 392, 388, 388, 384, 381, 381, 381, 381], per(t)[6:24])
    check('tone portamento starts no note', trig(t)[6:] == [0] * 18)
    # 4xy vibrato: speed 4, depth 8; the sine table's 0, 97, 180, 235, 255 x 8 / 128
    _, t = run(song({(0, 0, 0): (1, C2, 4, 0x48)}), 6)
    check('4 48 vibrato: 428 428 434 439 442 443', per(t) == [428, 428, 434, 439, 442, 443], per(t))
    _, t = run(song({(0, 0, 0): (1, C2, 4, 0x48), (0, 1, 0): (0, 0, 4, 0)}), 12)
    # (tick 0 plain; the position goes on from 20: sine 235, 180, 97, 0, then 97 below the note)
    check('4 00 goes on where it was: 428 442 439 434 428 422', per(t)[6:] == [428, 442, 439, 434, 428, 422], per(t)[6:])
    # 7xy tremolo on volume 32: x 8 / 64
    _, t = run(song({(0, 0, 0): (1, C2, 7, 0x48)}), 6)
    check('7 48 tremolo: 32 32 44 54 61 63', vol(t) == [32, 32, 44, 54, 61, 63], vol(t))
    # Axy volume slide, Cxx, 5 and 6
    _, t = run(song({(0, 0, 0): (1, C2, 0xA, 0x20), (0, 1, 0): (0, 0, 0xA, 0x05)}), 12)
    check('A 20 up 2 a tick, A 05 down 5 a tick', vol(t) == [32, 34, 36, 38, 40, 42, 42, 37, 32, 27, 22, 17], vol(t))
    _, t = run(song({(0, 0, 0): (1, C2, 0xA, 0xF0)}), 6)
    check('A F0 stops at 64', vol(t) == [32, 47, 62, 64, 64, 64], vol(t))
    _, t = run(song({(0, 0, 0): (1, C2, 0xC, 0x50)}), 2)
    check('C 50: 64 at most', vol(t) == [64, 64])
    _, t = run(song({(0, 0, 0): (1, C2, 0, 0), (0, 1, 0): (1, D2, 3, 8), (0, 2, 0): (0, 0, 5, 0x03)}), 18)
    check('5 03: the portamento goes on (388 to 381) and the volume falls by 3', per(t)[12:] == [388] + [381] * 5 and vol(t)[12:] == [32, 29, 26, 23, 20, 17], (per(t)[12:], vol(t)[12:]))
    # E-commands
    _, t = run(song({(0, 0, 0): (1, C2, 0xE, 0x14), (0, 1, 0): (0, 0, 0xE, 0x23)}), 12)
    check('E1 4 / E2 3 fine portamento on tick 0 only', per(t) == [424] * 6 + [427] * 6, per(t))
    _, t = run(song({(0, 0, 0): (1, C2, 0xE, 0xA5), (0, 1, 0): (0, 0, 0xE, 0xB9)}), 12)
    check('EA 5 / EB 9 fine volume', vol(t) == [37] * 6 + [28] * 6, vol(t))
    _, t = run(song({(0, 0, 0): (1, C2, 0xE, 0xC2)}), 6)
    check('EC 2: cut on tick 2', vol(t) == [32, 32, 0, 0, 0, 0], vol(t))
    _, t = run(song({(0, 0, 0): (1, C2, 0xE, 0xD3)}), 6)
    check('ED 3: the note starts on tick 3', trig(t) == [0, 0, 0, 1, 0, 0] and per(t)[3] == 428, (trig(t), per(t)))
    _, t = run(song({(0, 0, 0): (1, C2, 0xE, 0x92)}), 6)
    check('E9 2: started on ticks 0, 2 and 4', trig(t) == [1, 0, 1, 0, 1, 0], trig(t))
    _, t = run(song({(0, 0, 0): (1, C2, 0xE, 0x51)}), 1)
    check('E5 1: finetune 1, C-2 is 425', per(t) == [425], per(t))
    _, t = run(song({(0, 0, 0): (3, C2, 0, 0)}), 1)
    check('a sample of finetune 1: C-2 is 425', per(t) == [425], per(t))
    _, t = run(song({(0, 0, 0): (1, C2, 0xE, 0x31), (0, 1, 0): (1, 404, 3, 6)}), 12)
    # (the slide's period 422, 416, ... shown as the first note of the table at or above its pitch: 404)
    check('E3 1 glissando: the slide shown in semitones: 428, then 404',
          per(t)[6:] == [428, 404, 404, 404, 404, 404], per(t)[6:])
    # 9xx sample offset: 2 x 256 bytes into the 1000-byte saw
    _, t = run(song({(0, 0, 0): (2, C2, 9, 2)}), 1)
    check('9 02: start 576 + 512, length 1000 - 512', t[0]['ch'][0][3:5] == [64 + 512, 488], t[0]['ch'][0])
    check('a sample without a loop: loop length 0 (the silent word)', t[0]['ch'][0][6] == 0, t[0]['ch'][0])
    _, t = run(song({(0, 0, 0): (2, C2, 9, 9)}), 1)
    check('9 09 past the end: its loop (none: silence)', t[0]['ch'][0][4] == 0, t[0]['ch'][0])
    # an instrument without a note: volume and the next loop (MOD_LOOP 8), no start
    _, t = run(song({(0, 0, 0): (1, C2, 0, 0), (0, 1, 0): (3, 0, 0, 0)}), 7)
    check('an instrument alone: its volume and its loop for the next round, no new start',
          vol(t)[6] == 40 and t[6]['ch'][0][2] == 8 and t[6]['ch'][0][5:] == [64 + 1000 + 16, 32], t[6]['ch'][0])
    # speed, tempo, order, breaks and jumps
    _, t = run(song({(0, 0, 0): (1, C2, 0xF, 3)}), 9)
    check('F 03: three ticks a row', [x['row'] for x in t] == [0, 0, 0, 1, 1, 1, 2, 2, 2])
    _, t = run(song({(0, 0, 0): (1, C2, 0xF, 150)}), 3)
    check('F 96 (150 BPM): 709379 x 2.5 / 125 = 14187 E-clock ticks, then 11822 from the next tick',
          [x['len'] for x in t] == [14187, 11822, 11822], [x['len'] for x in t])
    _, t = run(song({(0, 2, 0): (0, 0, 0xD, 0x12)}, order=(0, 1)), 24)
    check('D 12 (decimal): position 1, row 12', [(x['pos'], x['row']) for x in t[18:24:6]] == [(1, 12)], [(x['pos'], x['row']) for x in t[12:24:6]])
    _, t = run(song({(0, 1, 0): (0, 0, 0xB, 2)}, order=(0, 1, 0, 1)), 18)
    check('B 02: position 2, row 0', (t[12]['pos'], t[12]['row']) == (2, 0), (t[12]['pos'], t[12]['row']))
    _, t = run(song({(0, 1, 0): (0, 0, 0xB, 0)}, order=(0, 1)), 13)
    check('B 00, a jump back: the song has ended', t[11]['ended'] == 1 and (t[12]['pos'], t[12]['row']) == (0, 0), t[11])
    _, t = run(song({(0, 63, 0): (0, 0, 0, 0)}, order=(0, 1)), 64 * 6 * 2 + 1)
    check('the order list played to its end: ended, back at position 0', t[64 * 12 - 1]['ended'] == 1 and t[64 * 12]['pos'] == 0 and t[64 * 6]['pos'] == 1)
    _, t = run(song({(0, 0, 0): (1, C2, 0xE, 0x60), (0, 1, 0): (0, 0, 0xE, 0x62)}), 6 * 7)
    check('E6 0 / E6 2 pattern loop: rows 0 1 0 1 0 1 2', [x['row'] for x in t[::6]] == [0, 1, 0, 1, 0, 1, 2], [x['row'] for x in t[::6]])
    _, t = run(song({(0, 0, 0): (1, C2, 0xE, 0xE2), (0, 1, 0): (1, D2, 0, 0)}), 24)
    check('EE 2: the row three times, its note once; the next row after 18 ticks',
          [x['row'] for x in t[::6]] == [0, 0, 0, 1] and sum(trig(t)[:18]) == 1 and t[18]['ch'][0][0] == D2, [x['row'] for x in t[::6]])
    _, t = run(song({(0, 0, 0): (1, C2, 0xA, 0x10), (0, 0, 1): (0, 0, 0xE, 0xE1)}), 12)
    check('EE 1 with A 10: the slide goes on through the repeated row (tick 0 too)', vol(t) == [32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43], vol(t))
    _, t = run(song({(0, 3, 0): (0, 0, 0xF, 0)}), 30)
    check('F 00: the song stops', t[18]['ended'] == 1)
    # the old 15-sample format, and refusals
    old = M.module({(0, 0, 0): (1, C2, 0, 0)}, [(M.wave(64, 'sine'), 0, 30, 8, 32)], [0], old=True)
    h, t = run(old, 1)
    check('15-sample module: loads, its loop start in bytes', h[1:5] == ['15', '1', '1', '1624'] and t[0]['ch'][0][5:] == [8, 32], (h, t[0]['ch'][0]))
    # audit 2026-11-12 A6-1: the old format's loop start in bytes may be odd; Paula's places are even (sndd refuses others)
    odd = M.module({(0, 0, 0): (1, C2, 0, 0)}, [(M.wave(64, 'sine'), 0, 30, 9, 32)], [0], old=True)
    h, t = run(odd, 1)
    check('15-sample module with an odd loop start: rounded down to even (8)', t[0]['ch'][0][5:] == [8, 32], (h, t[0]['ch'][0]))
    # A6-2: finetune 7's B-3 is 108, below the server's 113: held at 113, not dropped (modplay sent no period then)
    _, t = run(song({(0, 0, 0): (4, 113, 0, 0)}, smp=SMP + [(M.wave(64, 'sine'), 7, 32, 0, 64)]), 1)
    check('finetune 7, B-3 (108): held at the server\'s lowest period, 113', per(t) == [113], per(t))
    eight = bytearray(song({}))
    eight[1080:1084] = b'8CHN'
    h, _ = run(bytes(eight), 1)
    check('an 8-channel module refused', h[0] == 'error', h)
    h, _ = run(song({})[:1500], 1)
    check('a cut module: refused (patterns past the end)', h[0] == 'error', h)
    short = song({(0, 0, 0): (2, C2, 0, 0)})[:2108 + 64 + 100]
    h, t = run(short, 1)
    check('a cut sample: what is there', t[0]['ch'][0][4] == 100, t[0]['ch'][0])
    import random
    rnd = random.Random(3)
    crashes = 0
    base = M.demo()
    for i in range(200):
        b = bytearray(base)
        for _ in range(rnd.randint(1, 40)):
            b[rnd.randrange(len(b))] = rnd.randrange(256)
        h, t = run(bytes(b), 400)
        crashes += h is None
        if t and h is not None and h[0] != 'error':
            bank = int(h[5])
            for x in t:
                for c in x['ch']:
                    if c[3] + c[4] > bank or (c[6] and c[5] + c[6] > bank) or not 0 <= c[1] <= 64:
                        crashes += 1
                    # what sndd's frame_ok demands (A6-3): even places and lengths, a period 0 (none) or 113..4095
                    if (c[3] | c[4] | c[5] | c[6]) & 1 or not (c[0] == 0 or 113 <= c[0] <= 4095):
                        crashes += 1
    check('200 damaged modules, 400 ticks each: no crash, every place inside the bank and even, periods 113-4095, '
          'volumes 0-64', crashes == 0, crashes)
    h, t = run(base, 64 * 6 * 2)
    check('the emulator test\'s tune: two positions, every channel starts notes',
          h is not None and all(sum(trig(t, c)) > 0 for c in range(4)) and t[-1]['pos'] == 1)


build()
decoders()
replayer()
print('sound-host-test: %s' % ('ok' if not fails else '%d failed' % fails))
sys.exit(1 if fails else 0)
