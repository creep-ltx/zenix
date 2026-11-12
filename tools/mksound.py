#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Sound files of our own for DESIGN D40's tests (no third-party sound in the repository): WAV, 8SVX and AIFF writers
# from the formats' descriptions, a tone generator, and what play's reader (user/snddec.c) must make of each file,
# computed here from the rule in that file's head: a sample made 16-bit (8-bit << 8, unsigned 8-bit less 128 first),
# k of them added up give floor((s + 128k) / 256k), at most 127.
#   tools/mksound.py DIR   writes the emulator test's files into DIR (tools/sound-test.sh)
import math
import struct
import sys


def tone(n, hz, rate, amp=0.8, bits=16, phase=0.0):
    """n samples of a sine, as signed integers of the given width"""
    top = (1 << (bits - 1)) - 1
    return [int(round(top * amp * math.sin(2 * math.pi * hz * i / rate + phase))) for i in range(n)]


def wav(chans, rate, bits, frames, extensible=False, extra_chunk=True):
    """frames: a list of tuples (one value a channel), signed; 8-bit written unsigned as WAV has it"""
    data = bytearray()
    for fr in frames:
        for v in fr:
            data += struct.pack('<B', v + 128) if bits == 8 else struct.pack('<h', v)
    align = chans * bits // 8
    if extensible:
        fmt = struct.pack('<HHIIHHHHIH14s', 0xFFFE, chans, rate, rate * align, align, bits, 22, bits, 0, 1,
                          b'\x00\x00\x00\x00\x10\x00\x80\x00\x00\xaa\x00\x38\x9b\x71')
    else:
        fmt = struct.pack('<HHIIHH', 1, chans, rate, rate * align, align, bits)
    body = b'WAVE' + b'fmt ' + struct.pack('<I', len(fmt)) + fmt
    if extra_chunk:                     # a chunk the reader must skip, odd-sized (padded)
        body += b'LIST' + struct.pack('<I', 5) + b'abcde\x00'
    body += b'data' + struct.pack('<I', len(data)) + bytes(data)
    return b'RIFF' + struct.pack('<I', len(body)) + body


def fib_pack(vals):
    """8SVX Fibonacci delta: a pad byte, the first value, then two codes a byte (the nearest delta each step)"""
    tab = [-34, -21, -13, -8, -5, -3, -2, -1, 0, 1, 2, 3, 5, 8, 13, 21]
    x = vals[0]
    out = bytearray([0, vals[0] & 255])
    codes = []
    for v in vals:
        best = min(range(16), key=lambda c: abs(((x + tab[c] + 128) & 255) - 128 - v))
        x = ((x + tab[best] + 128) & 255) - 128
        codes.append(best)
    if len(codes) & 1:
        codes.append(8)
    for i in range(0, len(codes), 2):
        out.append(codes[i] << 4 | codes[i + 1])
    return bytes(out)


def fib_unpack(packed):
    tab = [-34, -21, -13, -8, -5, -3, -2, -1, 0, 1, 2, 3, 5, 8, 13, 21]
    x = struct.unpack('b', packed[1:2])[0]
    out = []
    for b in packed[2:]:
        for c in (b >> 4, b & 15):
            x = ((x + tab[c] + 128) & 255) - 128
            out.append(x)
    return out


def svx(rate, left, right=None, vol=0x10000, fib=False, octaves=1, extra=b''):
    """8-bit signed lists; right: a stereo body (left half, then right); octaves > 1: more data after the first"""
    def chan(v):
        return fib_pack(v) if fib else bytes(x & 255 for x in v)
    body = chan(left) + (chan(right) if right is not None else b'')
    n = len(left)
    vhdr = struct.pack('>IIIHBBI', n, 0, 0, rate, octaves, 1 if fib else 0, vol)
    f = b'8SVX' + b'VHDR' + struct.pack('>I', len(vhdr)) + vhdr
    f += b'NAME' + struct.pack('>I', 3) + b'abc\x00'
    if right is not None:
        f += b'CHAN' + struct.pack('>II', 4, 6)
    f += b'BODY' + struct.pack('>I', len(body) + len(extra)) + body + extra + (b'\x00' if (len(body) + len(extra)) & 1 else b'')
    return b'FORM' + struct.pack('>I', len(f)) + f


def ext80(rate):
    e = int(math.floor(math.log2(rate)))
    m = int(rate * (1 << (63 - e)))
    return struct.pack('>HQ', 16383 + e, m)


def aiff(chans, rate, bits, frames, aifc=None):
    """aifc: None (AIFF), b'NONE' or b'sowt' (little-endian 16-bit)"""
    data = bytearray()
    for fr in frames:
        for v in fr:
            if bits == 8:
                data += struct.pack('b', v)
            else:
                data += struct.pack('<h' if aifc == b'sowt' else '>h', v)
    comm = struct.pack('>hIh', chans, len(frames), bits) + ext80(rate)
    if aifc:
        comm += aifc + b'\x00'          # a compression name: an empty pstring, padded
    f = (b'AIFC' if aifc else b'AIFF')
    if aifc:
        f += b'FVER' + struct.pack('>II', 4, 0xA2805140)
    f += b'COMM' + struct.pack('>I', len(comm)) + comm + (b'\x00' if len(comm) & 1 else b'')
    f += b'SSND' + struct.pack('>III', 8 + 4 + len(data), 4, 0) + b'\x00' * 4 + bytes(data)
    return b'FORM' + struct.pack('>I', len(f)) + f


def expect(frames, bits, unsigned8=False, maxrate=None, rate=None, mono=False):
    """what snddec makes of frames (signed values as written): a list of output bytes (interleaved when stereo)"""
    chans = len(frames[0])
    div = 1
    while maxrate and rate // div > maxrate:
        div += 1
    def v16(v):
        return v << 8 if bits == 8 else v
    out = []
    for g in range(len(frames) // div):
        grp = frames[g * div:(g + 1) * div]
        if mono or chans == 1:
            s = sum(v16(fr[c]) for fr in grp for c in range(chans))
            k = div * chans
            out.append(min(127, (s + 128 * k) // (256 * k)))
        else:
            for c in range(2):
                s = sum(v16(fr[c]) for fr in grp)
                out.append(min(127, (s + 128 * div) // (256 * div)))
    return out


def emu_files(d):
    """the emulator test's files: a 1 kHz tone at 11,025 Hz 8-bit mono WAV (2 s), a 16-bit stereo 44.1 kHz WAV (2 s:
    play takes it at 22,050 Hz), an 8SVX of 0.5 s at 8,363 Hz, an AIFF 16-bit mono at 16 kHz, and a 10 s WAV for Ctrl-C"""
    import os
    os.makedirs(d, exist_ok=True)
    t = tone(22050, 1000, 11025, bits=8)
    open(os.path.join(d, 'tone8.wav'), 'wb').write(wav(1, 11025, 8, [(v,) for v in t]))
    a, b = tone(88200, 440, 44100), tone(88200, 660, 44100)
    open(os.path.join(d, 'st16.wav'), 'wb').write(wav(2, 44100, 16, list(zip(a, b))))
    open(os.path.join(d, 'tone.8svx'), 'wb').write(svx(8363, tone(4181, 523, 8363, bits=8)))
    open(os.path.join(d, 'tone.aiff'), 'wb').write(aiff(1, 16000, 16, [(v,) for v in tone(8000, 750, 16000)]))
    t = tone(110250, 300, 11025, bits=8)
    open(os.path.join(d, 'long.wav'), 'wb').write(wav(1, 11025, 8, [(v,) for v in t]))


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit('usage: tools/mksound.py DIR')
    emu_files(sys.argv[1])
