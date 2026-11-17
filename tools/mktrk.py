#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Modules of our own for DESIGN D56's tests (no third-party music in the repository): N-channel MODs, MED/OctaMED
# (MMD0, MMD1, MMD2), Scream Tracker 3 and FastTracker 2 files, written from the formats' documents (the same ones the
# replayers were written from, docs/d56-tracker-player.md section 8). The samples are generated waves (tools/mkmod.py).
#   tools/mktrk.py DIR     writes the emulator test's modules into DIR: tune8.mod, tune.med, tune.s3m, tune.xm
import struct
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mkmod as M              # noqa: E402

NOTES = M.NOTES


def modn(cells, samples, order, nch, title=b'zenix test'):
    """an N-channel MOD (tag 'nCHN' or 'nnCH'): cells {(pattern, row, channel): (sample, period, effect, param)}"""
    tag = b'%dCHN' % nch if nch < 10 else b'%dCH' % nch
    base = bytearray(M.module({}, samples, order, title=title))
    head = base[:1080] + tag
    npat = max(order) + 1
    pats = bytearray(64 * 4 * nch * npat)
    for (p, r, c), (s, per, e, prm) in cells.items():
        o = p * 64 * 4 * nch + r * 4 * nch + c * 4
        pats[o:o + 4] = bytes([(s & 0xF0) | (per >> 8), per & 255, (s & 15) << 4 | e, prm])
    return bytes(head) + bytes(pats) + b''.join(s[0] for s in samples)


# ---- Scream Tracker 3 (TECH.DOC) ----

def s3m(cells, samples, order, nch=4, title=b'zenix s3m', speed=6, tempo=125, gvol=64, mv=0xB0, flags=0, chset=None):
    """cells {(pattern, row, channel): (note, ins, vol, cmd, info)}: note 'C-4' style as (octave << 4 | semitone),
    255 none, 254 off; vol 255 none; cmd 0 none, 1 = A ... samples: dicts with data (signed bytes), vol, c4, loop
    (begin, end) or None, name, b16 (data are 16-bit signed little-endian words when set)"""
    npat = max(order) + 1
    ordl = list(order) + [255]
    if len(ordl) % 2:
        ordl.append(255)
    h = bytearray(0x60)
    h[0:len(title)] = title[:27]
    h[28] = 0x1A
    h[29] = 16
    struct.pack_into('<HHHHHH', h, 32, len(ordl), len(samples), npat, flags, 0x1320, 2)
    h[44:48] = b'SCRM'
    h[48], h[49], h[50], h[51] = gvol, speed, tempo, mv
    cs = chset or [(i % 2) * 8 + i // 2 if i < 16 else 255 for i in range(32)]
    for i in range(32):
        h[64 + i] = cs[i] if i < nch or chset else 255
    body = bytearray(h) + bytes(ordl)
    ptr_at = len(body)
    body += b'\0' * (2 * (len(samples) + npat))

    def align():
        while len(body) % 16:
            body.append(0)

    ins_off, pat_off, smp_hdr = [], [], []
    for s in samples:
        align()
        ins_off.append(len(body) // 16)
        smp_hdr.append(len(body))
        body += b'\0' * 80
    for p in range(npat):
        align()
        pat_off.append(len(body) // 16)
        pd = bytearray()
        for r in range(64):
            for c in range(32):
                cell = cells.get((p, r, c))
                if not cell:
                    continue
                note, ins, vol, cmd, info = cell
                w = c
                b = bytearray()
                if note != 255 or ins:
                    w |= 32
                    b += bytes([note, ins])
                if vol != 255:
                    w |= 64
                    b.append(vol)
                if cmd:
                    w |= 128
                    b += bytes([cmd, info])
                pd += bytes([w]) + b
            pd.append(0)
        body += struct.pack('<H', len(pd)) + pd
    for i, s in enumerate(samples):
        align()
        at = len(body) // 16
        d = s['data']
        if s.get('b16'):
            raw = bytearray(d)
            for k in range(0, len(raw), 2):              # signed little-endian words -> unsigned (format 2)
                raw[k + 1] ^= 0x80
            body += raw
            n = len(d) // 2
        else:
            body += bytes(((x & 255) ^ 0x80) for x in d)        # unsigned bytes (format 2)
            n = len(d)
        o = smp_hdr[i]
        body[o] = 1
        body[o + 13] = at >> 16
        struct.pack_into('<H', body, o + 14, at & 0xFFFF)
        lb, le = s.get('loop') or (0, 0)
        struct.pack_into('<III', body, o + 16, n, lb, le)
        body[o + 28] = s.get('vol', 64)
        body[o + 31] = (1 if s.get('loop') else 0) | (4 if s.get('b16') else 0)
        struct.pack_into('<I', body, o + 32, s.get('c4', 8363))
        nm = s.get('name', b'smp%d' % (i + 1))
        body[o + 48:o + 48 + len(nm)] = nm
        body[o + 76:o + 80] = b'SCRS'
    for i, v in enumerate(ins_off + pat_off):
        struct.pack_into('<H', body, ptr_at + 2 * i, v)
    return bytes(body)


def s3mnote(octave, semi):
    return octave << 4 | semi


# ---- FastTracker 2 (XM 1.04) ----

def xm(patterns, instruments, order, nch=4, title=b'zenix xm', linear=True, speed=6, tempo=125, restart=0):
    """patterns: list of (rows, {(row, channel): (note, ins, vol, eff, prm)}) (note 1 = C-0 .. 96, 97 off; vol the
    volume column byte); instruments: dicts with name, samples (list of dicts: data signed bytes, or b16 words, vol,
    fine, pan, rel, loop (start, length) in samples, ping), map (96 sample numbers, default 0), venv/penv: (points,
    sustain, loop start, loop end, type), vib (type, sweep, depth, rate), fade"""
    h = bytearray(b'Extended Module: ' + title[:20].ljust(20, b'\0') + b'\x1a' + b'zenix mktrk'.ljust(20, b' '))
    h += struct.pack('<H', 0x104)
    h += struct.pack('<IHHHHHHHH', 276, len(order), restart, nch, len(patterns), len(instruments), 1 if linear else 0,
                     speed, tempo)
    h += bytes(order).ljust(256, b'\0')
    for rows, cells in patterns:
        pd = bytearray()
        for r in range(rows):
            for c in range(nch):
                cell = cells.get((r, c))
                if not cell:
                    pd.append(0x80)
                    continue
                flags, b = 0x80, bytearray()
                for k, v in enumerate(cell):
                    if v:
                        flags |= 1 << k
                        b.append(v)
                pd += bytes([flags]) + b
        h += struct.pack('<IBHH', 9, 0, rows, len(pd)) + pd
    for ins in instruments:
        smps = ins.get('samples', [])
        if not smps:
            h += struct.pack('<I', 29) + ins.get('name', b'')[:22].ljust(22, b'\0') + b'\0' + struct.pack('<H', 0)
            continue
        ih = bytearray(struct.pack('<I', 263) + ins.get('name', b'')[:22].ljust(22, b'\0') + b'\0' +
                       struct.pack('<HI', len(smps), 40))
        ih += bytes(ins.get('map', [0] * 96))
        for key in ('venv', 'penv'):
            pts = ins.get(key, ([], 0, 0, 0, 0))[0]
            ih += b''.join(struct.pack('<HH', x, y) for x, y in pts).ljust(48, b'\0')
        ve = ins.get('venv', ([], 0, 0, 0, 0))
        pe = ins.get('penv', ([], 0, 0, 0, 0))
        ih += bytes([len(ve[0]), len(pe[0]), ve[1], ve[2], ve[3], pe[1], pe[2], pe[3], ve[4], pe[4]])
        vt, vs, vd, vr = ins.get('vib', (0, 0, 0, 0))
        ih += bytes([vt, vs, vd, vr]) + struct.pack('<HH', ins.get('fade', 0), 0)
        ih += b'\0' * (263 - len(ih))
        sh, sd = bytearray(), bytearray()
        for s in smps:
            if s.get('b16'):
                words = s['data']
                old, enc = 0, bytearray()
                for w in words:
                    enc += struct.pack('<h', ((w - old + 32768) & 0xFFFF) - 32768)
                    old = w
                k = 2
            else:
                old, enc = 0, bytearray()
                for x in s['data']:
                    enc.append((x - old) & 255)
                    old = x
                k = 1
            ls, ll = s.get('loop', (0, 0))
            ty = (0 if not s.get('loop') else 2 if s.get('ping') else 1) | (16 if s.get('b16') else 0)
            sh += struct.pack('<IIIBbBBbB', len(enc), ls * k, ll * k, s.get('vol', 64), s.get('fine', 0), ty,
                              s.get('pan', 128), s.get('rel', 0), 0) + s.get('name', b'')[:22].ljust(22, b'\0')
            sd += enc
        h += ih + sh + sd
    return bytes(h)


# ---- MED / OctaMED (MMD0, MMD1; MMD2 with one play sequence and one section) ----

def med(blocks, seq, samples, ver=0, tempo=33, tempo2=6, flags=0x10, flags2=0, transp=0, names=None, songname=None,
        trkvol=None, master=64):
    """blocks: list of (tracks, lines, {(line, track): (note, ins, cmd, dat)}) (note 1 = C-1); seq: the play sequence
    (block numbers); samples: list of (data signed bytes, vol, rep bytes, replen bytes, transpose); names: the
    instruments' names (MMDInstrInfo)"""
    out = bytearray(52)
    out[0:4] = b'MMD%d' % ver

    def here():
        while len(out) % 2:
            out.append(0)
        return len(out)

    song = here()
    sg = bytearray(788)
    for i, (d, vol, rep, rl, tr) in enumerate(samples[:63]):
        struct.pack_into('>HHBBBb', sg, 8 * i, rep // 2, max(1, rl // 2), 0, 0, vol, tr)
    struct.pack_into('>HH', sg, 504, len(blocks), len(seq))
    if ver < 2:
        sg[508:508 + len(seq)] = bytes(seq)
        for i in range(16):
            sg[770 + i] = (trkvol or [64] * 16)[i]
    struct.pack_into('>HbBBB', sg, 764, tempo, transp, flags, flags2, tempo2)
    sg[786] = master
    sg[787] = len(samples)
    out += sg
    if ver >= 2:
        ntr = max(b[0] for b in blocks)
        tv = here()
        out += bytes(trkvol or [64] * max(16, ntr))
        tp = here()
        out += bytes(max(16, ntr))
        ps = here()
        out += b'seq'.ljust(32, b'\0') + struct.pack('>IIH', 0, 0, len(seq)) + b''.join(struct.pack('>H', b) for b in seq)
        pst = here()
        out += struct.pack('>I', ps)
        sct = here()
        out += struct.pack('>H', 0)
        struct.pack_into('>IIIHHI', out, song + 508, pst, sct, tv, ntr, 1, tp)
        struct.pack_into('>H', out, song + 506, 1)
    boffs = []
    for tracks, lines, cells in blocks:
        bo = here()
        boffs.append(bo)
        if ver == 0:
            out += bytes([tracks, lines - 1])
            for ln in range(lines):
                for t in range(tracks):
                    n, ins, cmd, dat = cells.get((ln, t), (0, 0, 0, 0))
                    out += bytes([(n & 0x3F) | (0x80 if ins & 16 else 0) | (0x40 if ins & 32 else 0),
                                  (ins & 15) << 4 | (cmd & 15), dat])
        else:
            out += struct.pack('>HHI', tracks, lines - 1, 0)
            for ln in range(lines):
                for t in range(tracks):
                    n, ins, cmd, dat = cells.get((ln, t), (0, 0, 0, 0))
                    out += bytes([n & 0x7F, ins & 0x3F, cmd, dat])
    ba = here()
    out += b''.join(struct.pack('>I', o) for o in boffs)
    soffs = []
    for d, vol, rep, rl, tr in samples:
        so = here()
        soffs.append(so)
        out += struct.pack('>Ih', len(d), 0) + bytes(x & 255 for x in d)
    sa = here()
    out += b''.join(struct.pack('>I', o) for o in soffs)
    ex = 0
    if names or songname:
        ii = here()
        for i in range(len(samples)):
            nm = (names[i] if names and i < len(names) else b'')[:39]
            out += nm.ljust(40, b'\0')
        sn = here()
        sname = (songname or b'') + b'\0'
        out += sname
        ex = here()
        e = bytearray(80)
        struct.pack_into('>IHH', e, 20, ii, len(samples), 40)
        struct.pack_into('>II', e, 44, sn, len(sname))
        out += e
    struct.pack_into('>IIIIIIIII', out, 4, len(out), song, 0, ba, 0, sa, 0, ex, 0)
    struct.pack_into('>h', out, 48, -1)
    return bytes(out)


# ---- the emulator test's tunes ----

def wave(n, kind):
    return [x - 256 if x > 127 else x for x in M.wave(n, kind)]


def tune8():
    smp = [(M.wave(32, 'saw'), 0, 64, 0, 32), (M.wave(16, 'square'), 0, 48, 0, 16), (M.wave(64, 'sine'), 0, 40, 0, 64)]
    cells = {}
    for p in range(2):
        for r in range(0, 64, 4):
            for c in range(8):
                if (r // 4 + c) % 3 == 0:
                    cells[(p, r, c)] = (1 + c % 3, NOTES[(c * 5 + r // 4) % 36], 0, 0)
    return modn(cells, smp, [0, 1], 8, title=b'zenix eight')


def tune_med():
    blk = {}
    for ln in range(0, 64, 4):
        blk[(ln, 0)] = (13 + (ln // 4) % 12, 1, 0, 0)
        blk[(ln, 1)] = (25, 2, 0xC, 0x40) if ln % 8 == 0 else (0, 0, 0, 0)
        blk[(ln, 2)] = (20, 3, 0, 0)
        blk[(ln, 3)] = (8, 1, 0, 0)
    return med([(4, 64, blk), (4, 64, blk)], [0, 1], [(wave(32, 'saw'), 64, 0, 32, 0), (wave(16, 'square'), 48, 0, 16, 0),
               (wave(64, 'sine'), 40, 0, 64, 0)], ver=1, names=[b'saw', b'square', b'sine'], songname=b'zenix med')


def tune_s3m():
    cells = {}
    for p in range(2):
        for r in range(0, 64, 4):
            for c in range(6):
                if (r // 4 + c) % 2 == 0:
                    cells[(p, r, c)] = (s3mnote(4 + c % 2, (r // 4 + c) % 12), 1 + c % 3, 48, 0, 0)
    smp = [{'data': wave(32, 'saw'), 'vol': 64, 'loop': (0, 32)}, {'data': wave(16, 'square'), 'vol': 48, 'loop': (0, 16)},
           {'data': wave(64, 'sine'), 'vol': 40, 'loop': (0, 64)}]
    return s3m(cells, smp, [0, 1], nch=6, title=b'zenix s3m')


def tune_xm():
    pats = []
    for p in range(2):
        cells = {}
        for r in range(0, 64, 4):
            for c in range(8):
                if (r // 4 + c) % 2 == 0:
                    cells[(r, c)] = (49 + (r // 4 + c) % 12, 1 + c % 2, 0x40, 0, 0)
        pats.append((64, cells))
    ins = [{'name': b'saw', 'samples': [{'data': wave(32, 'saw'), 'loop': (0, 32)}],
            'venv': ([(0, 64), (8, 32), (32, 16)], 1, 0, 0, 3), 'fade': 512},
           {'name': b'pingsine', 'samples': [{'data': wave(64, 'sine')[:32], 'loop': (0, 32), 'ping': True}]}]
    return xm(pats, ins, [0, 1], nch=8, title=b'zenix xm')


def busy_xm(nch):
    """every channel playing a long looped sample at once, a note every 4 rows: the mixer's measure (modplay -B)"""
    cells = {}
    for r in range(0, 64, 4):
        for c in range(nch):
            cells[(r, c)] = (37 + (r // 4 + c * 5) % 24, 1 + c % 2, 0x50, 0, 0)
    ins = [{'name': b'saw', 'samples': [{'data': wave(256, 'saw') * 8, 'loop': (0, 2048)}]},
           {'name': b'sine', 'samples': [{'data': wave(512, 'sine') * 4, 'loop': (0, 2048)}]}]
    return xm([(64, cells)], ins, [0], nch=nch, title=b'busy %d' % nch)


def busy_mod(nch):
    cells = {}
    for r in range(0, 64, 4):
        for c in range(nch):
            cells[(0, r, c)] = (1 + c % 2, NOTES[(r // 4 + c * 5) % 36], 0xC, 64)
    smp = [(M.wave(2048, 'saw'), 0, 64, 0, 2048), (M.wave(2048, 'sine'), 0, 64, 0, 2048)]
    return modn(cells, smp, [0], nch, title=b'busy %d' % nch) if nch != 4 else M.module(cells, smp, [0], title=b'busy 4')


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit('usage: tools/mktrk.py DIR')
    d = sys.argv[1]
    os.makedirs(d, exist_ok=True)
    for name, data in (('tune8.mod', tune8()), ('tune.med', tune_med()), ('tune.s3m', tune_s3m()), ('tune.xm', tune_xm())):
        open(os.path.join(d, name), 'wb').write(data)
    for n in (4, 8, 16, 32):
        open(os.path.join(d, 'busy%d.xm' % n), 'wb').write(busy_xm(n))
    open(os.path.join(d, 'busy8.mod'), 'wb').write(busy_mod(8))
