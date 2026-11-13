#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The ZINE trace build's memory dump (docs/d29-zine.md 16.20, 16.21) from a serial log into binaries.

usage: tools/zine-dump.py LOG OUTDIR [TEXT...]     one OUTDIR/m_<address>.bin per piece of memory read whole, a line
                                                   per piece and per gap, where each TEXT is found
       tools/zine-dump.py --test                   the decoder's own test (tools/host/zine-test.sh runs it)

The dump is sent twice ('G 00000001', 'G 00000002' before each pass). Each 'D' line is an address, 8 longs and a
check long (each of the nine rotated left by one into it, as user/zine/blob.c's zb_dump). A line whose check fails
(a gap on the serial line joined two halves) is dropped; a line lost in one pass is taken from the other. The 'H'
lines (a hunk's address and size) say what the dump should hold: the gaps left in them are listed."""
import os
import re
import sys

LINE = re.compile(r"D ([0-9a-f]{8})((?: [0-9a-f]{8}){8}) ([0-9a-f]{8})\s*$")
HUNK = re.compile(r"H ([0-9a-f]{8}) ([0-9a-f]{8})\s*$")


def check(words):
    c = 0
    for w in words:
        c = ((c << 1 | c >> 31) & 0xFFFFFFFF) ^ w
    return c


def parse(lines):
    """(memory {address: 32 bytes}, hunks [(address, size)], bad lines, lines per pass)"""
    mem, hunks, bad, npass, cur = {}, [], 0, {}, 0
    for l in lines:
        l = l.rstrip("\r\n")
        if l.startswith("G "):
            cur = int(l[2:10], 16) if re.fullmatch(r"G [0-9a-f]{8}\s*", l) else cur
            continue
        m = HUNK.match(l)
        if m:
            h = (int(m.group(1), 16), int(m.group(2), 16))
            if h not in hunks:
                hunks.append(h)
            continue
        if not l.startswith("D "):
            continue
        m = LINE.match(l)
        if not m:
            bad += 1
            continue
        words = [int(m.group(1), 16)] + [int(x, 16) for x in m.group(2).split()]
        if check(words) != int(m.group(3), 16):
            bad += 1
            continue
        mem[words[0]] = b"".join(w.to_bytes(4, "big") for w in words[1:])
        npass[cur] = npass.get(cur, 0) + 1
    return mem, hunks, bad, npass


def pieces(mem):
    runs = []
    for a in sorted(mem):
        if runs and runs[-1][0] + len(runs[-1][1]) == a:
            runs[-1][1].extend(mem[a])
        else:
            runs.append([a, bytearray(mem[a])])
    return runs


def gaps(mem, hunks):
    """[(address, bytes)] of each hunk's lines not read whole in either pass"""
    out = []
    for a, n in hunks:
        lo, hi = a & ~31, a + n
        g = None
        for x in range(lo, hi, 32):
            if x in mem:
                if g is not None:
                    out.append((g, x - g))
                    g = None
            elif g is None:
                g = x
        if g is not None:
            out.append((g, hi - g))
    return out


def dline(a, words):
    v = [a] + words
    return "D " + " ".join(f"{w:08x}" for w in v + [check(v)])


def selftest():
    words = [[(0x11111111 * (i + 1) + k) & 0xFFFFFFFF for k in range(8)] for i in range(6)]
    full = [dline(0x1000 + 32 * i, w) for i, w in enumerate(words)]
    joined = full[1][:40] + full[2][40:]           # a gap inside a line: the halves of two lines, still 10 words
    log = ["H 00001000 000000c0", "G 00000001", full[0], joined, full[3], full[4], full[5],
           "G 00000002", full[0], full[1], full[2], full[3][:30], "H 00001000 000000c0"]
    mem, hunks, bad, npass = parse(log)
    ok = True
    def want(what, cond):
        nonlocal ok
        print(("PASS " if cond else "FAIL ") + what)
        ok &= cond
    want("a joined line fails its check and is dropped (2 bad: it and a cut one)", bad == 2)
    want("each line taken from the pass that has it whole (6 of 6)", sorted(mem) == [0x1000 + 32 * i for i in range(6)])
    want("the data is the dumped data", all(mem[0x1000 + 32 * i] == b"".join(x.to_bytes(4, "big") for x in w)
                                            for i, w in enumerate(words)))
    want("one hunk, its repeat not counted twice", hunks == [(0x1000, 0xC0)])
    want("no gap left in it", gaps(mem, hunks) == [])
    del mem[0x1040]
    want("a line missing in both passes is a gap of 32 bytes at its address", gaps(mem, hunks) == [(0x1040, 32)])
    want("pieces split at the gap", [(a, len(b)) for a, b in pieces(mem)] == [(0x1000, 64), (0x1060, 96)])
    print("zine-dump: self-test " + ("passed" if ok else "FAILED"))
    return 0 if ok else 1


def main():
    if sys.argv[1:] == ["--test"]:
        return selftest()
    if len(sys.argv) < 3:
        print(__doc__.split("\n\n")[1])
        return 2
    log, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    mem, hunks, bad, npass = parse(open(log, encoding="latin1"))
    runs = pieces(mem)
    for a, b in runs:
        with open(os.path.join(out, f"m_{a:08x}.bin"), "wb") as f:
            f.write(b)
        print(f"piece {a:08x} {len(b):8d} bytes")
    g = gaps(mem, hunks)
    for a, n in g:
        print(f"gap   {a:08x} {n:8d} bytes (in neither pass)")
    print(f"{len(mem)} lines whole ({', '.join(f'pass {p}: {n}' for p, n in sorted(npass.items()))}), {bad} damaged "
          f"dropped, {len(g)} gaps in the hunks")
    for t in sys.argv[3:]:
        for a, b in runs:
            for m in re.finditer(re.escape(t.encode("latin1")), b):
                print(f"'{t}' at {a + m.start():08x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
