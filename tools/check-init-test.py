#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""DESIGN D45: tools/check-init.py must refuse what it is there to refuse. From a good kernel ELF it makes damaged
copies and expects each to fail with the right words: a resident call retargeted at boot code (rule 1); a call inside
the boot span turned into a pea of boot code (rule 2); the span's pad made non-zero (rule 3). The good ELF must pass.
usage: tools/check-init-test.py KERNEL.elf SCRATCH_DIR"""
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
ci = __import__("check-init")
elf, tmp = sys.argv[1:3]
CHECK = [sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "check-init.py")]
E = ci.Elf(elf)
idx = {s["name"]: i for i, s in enumerate(E.syms) if s["name"]}
fails = 0


def run(path, want):
    global fails
    r = subprocess.run(CHECK + [path], capture_output=True, text=True)
    ok = (r.returncode == 0) if want is None else (r.returncode != 0 and want in r.stderr)
    print(("PASS " if ok else "FAIL ") + (f"refused: {want}" if want else "the good kernel passes"))
    if not ok:
        print(r.stdout + r.stderr)
        fails += 1


def relas(secname):
    s = E.sec(".rela" + secname)
    for o in range(s["off"], s["off"] + s["size"], 12):
        off, info, add = struct.unpack(">IIi", E.d[o:o + 12])
        yield o, off, info


run(elf, None)
zi = E.sec(".zinit")
lo, hi = zi["addr"], zi["addr"] + zi["size"]
# 1: a call in resident code (jsr abs.l of a resident function) retargeted at kmain
d = bytearray(E.d)
for o, off, info in relas(".text"):
    t = E.syms[info >> 8]
    if info & 0xFF == 1 and E.word(off - 2) == 0x4EB9 and t["name"] and not lo <= t["value"] < hi:
        d[o + 4:o + 8] = struct.pack(">I", idx["kmain"] << 8 | 1)
        break
open(f"{tmp}/k1.elf", "wb").write(d)
run(f"{tmp}/k1.elf", "refers to kmain in the boot span")
# 2: a jsr of boot code inside the boot span made a pea (the address pushed, kept who knows where)
d = bytearray(E.d)
for o, off, info in relas(".zinit"):
    t = E.syms[info >> 8]
    if E.word(off - 2) == 0x4EB9 and lo <= t["value"] < hi:
        p = zi["off"] + off - 2 - zi["addr"]
        d[p:p + 2] = b"\x48\x79"
        break
open(f"{tmp}/k2.elf", "wb").write(d)
run(f"{tmp}/k2.elf", "(not a call or jump)")
# 4-6: gcc's call cse (move.l #boot_fn,%dN ... movea.l %dN,%aM; jsr (%aM)) with the copy to %aM replaced by: a push of
# the address, an rts with the address still in the register (returned), an addq on it (an address kept, moved on)
def cse_patch(word_of_n):
    d = bytearray(E.d)
    for o, off, info in relas(".zinit"):
        w = E.word(off - 2)
        t = E.syms[info >> 8]
        if w is not None and (w & 0xF1FF) == 0x203C and t["name"] not in ci.BOUNDS and \
                lo <= t["value"] + struct.unpack(">i", E.d[o + 8:o + 12])[0] < hi:
            n = (w >> 9) & 7
            for a in range(off + 4, off + 200, 2):
                v = E.word(a)
                if v is not None and (v & 0xF1F8) == 0x2040 and (v & 7) == n:
                    p = zi["off"] + a - zi["addr"]
                    d[p:p + 2] = struct.pack(">H", word_of_n(n))
                    return d
    return None
for k, (f, want, what) in enumerate(((lambda n: 0x2F00 | n, "is used by", "pushed"),
                                     (lambda n: 0x4E75, "may leave the function", "returned"),
                                     (lambda n: 0x5480 | n, "is used by", "moved on by addq"))):
    d = cse_patch(f)
    if d is None:
        print(f"SKIP no move.l #function,%dN in this build's boot span (the {what} case)")
        continue
    open(f"{tmp}/k4{k}.elf", "wb").write(d)
    run(f"{tmp}/k4{k}.elf", want)
# 7: a boot function other than kmain taking the boot program's address (a pea of a string retargeted at boot_blob)
d = bytearray(E.d)
for o, off, info in relas(".zinit"):
    t = E.syms[info >> 8]
    if E.word(off - 2) == 0x4879 and not lo <= t["value"] < hi and \
            not any(s["name"] == "kmain" and s["value"] <= off < s["value"] + s["size"] for s in E.syms):
        d[o + 4:o + 8] = struct.pack(">I", idx["boot_blob"] << 8 | 1)
        d[o + 8:o + 12] = struct.pack(">i", 0)
        break
open(f"{tmp}/k7.elf", "wb").write(d)
run(f"{tmp}/k7.elf", "the boot span's data boot_blob")
# 8: the one allowed call into the span (start.S's jsr kmain) made a pea of kmain
d = bytearray(E.d)
for o, off, info in relas(".text"):
    if E.syms[info >> 8]["name"] == "kmain" and E.word(off - 2) == 0x4EB9:
        tx = E.sec(".text")
        p = tx["off"] + off - 2 - tx["addr"]
        d[p:p + 2] = b"\x48\x79"
        break
open(f"{tmp}/k8.elf", "wb").write(d)
run(f"{tmp}/k8.elf", "refers to kmain in the boot span")
# 3: the pad's first byte not zero
d = bytearray(E.d)
d[zi["off"]] = 1
open(f"{tmp}/k3.elf", "wb").write(d)
run(f"{tmp}/k3.elf", "does not start with its 32 zero bytes")
sys.exit(1 if fails else 0)
