#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""DESIGN D45: tools/check-init.py must refuse what it is there to refuse. From a good kernel ELF it makes damaged
copies and expects each to fail with the right words: a resident call retargeted at boot code (rule 1); a call inside
the boot span turned into a pea of boot code (rule 2); the span's pad made non-zero (rule 3); and for the MMU span
(section 13) a resident call retargeted into it, mmu_arm's store sent elsewhere than mmu_ops, boot code taking the
live table's address, resident code writing mmu_ops, a call inside it made a pea, a span address handed to a call in a
scratch register, the trap guard loading another function's address. The good ELF must pass.
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
zfuncs = {x["value"] for x in E.syms if x["type"] == ci.STT_FUNC and lo + 32 <= x["value"] < hi}  # boot functions
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
# D45 section 13, the MMU span. 9: a resident call (jsr abs.l) retargeted at a span function, as a hook without the table
mu = E.sec(".zhwmmu")
d = bytearray(E.d)
for o, off, info in relas(".text"):
    t = E.syms[info >> 8]
    if info & 0xFF == 1 and E.word(off - 2) == 0x4EB9 and t["name"] and not lo <= t["value"] < hi:
        d[o + 4:o + 8] = struct.pack(">I", idx["mmu_fault_"] << 8 | 1)
        break
open(f"{tmp}/k9.elf", "wb").write(d)
run(f"{tmp}/k9.elf", "refers to mmu_fault_ in the mmu span")
# 10: mmu_arm's store of the live table's address sent somewhere other than mmu_ops (here: mmu_on)
d = bytearray(E.d)
arm = next(x for x in E.syms if x["name"] == ci.ARM)
for o, off, info in relas(".zhwmmu"):
    if arm["value"] <= off < arm["value"] + 16 and E.word(off - 6) == 0x23FC:
        d[o + 4:o + 8] = struct.pack(">I", idx["mmu_on"] << 8 | 1)
        d[o + 8:o + 12] = struct.pack(">i", 0)
        break
open(f"{tmp}/k10.elf", "wb").write(d)
run(f"{tmp}/k10.elf", "refers to mmu_ops_on: only mmu_arm")
# 10b: boot code (allowed to call into the MMU span) taking the live table's address: a pea in .zinit retargeted at it
d = bytearray(E.d)
for o, off, info in relas(".zinit"):
    t = E.syms[info >> 8]
    if E.word(off - 2) == 0x4879 and not lo <= t["value"] < hi:
        d[o + 4:o + 8] = struct.pack(">I", idx["mmu_ops_on"] << 8 | 1)
        d[o + 8:o + 12] = struct.pack(">i", 0)
        break
open(f"{tmp}/k10b.elf", "wb").write(d)
run(f"{tmp}/k10b.elf", "refers to mmu_ops_on: only mmu_arm")
# 10c: resident code writing mmu_ops (a load of it, movea.l mmu_ops,%aN, made a move.l %aN,mmu_ops)
d = bytearray(E.d)
for o, off, info in relas(".text"):
    if E.syms[info >> 8]["name"] == "mmu_ops" and E.word(off - 2) is not None and (E.word(off - 2) & 0xF1FF) == 0x2079:
        tx = E.sec(".text")
        p = tx["off"] + off - 2 - tx["addr"]
        d[p:p + 2] = struct.pack(">H", 0x23C8 | (E.word(off - 2) >> 9 & 7))
        break
open(f"{tmp}/k10c.elf", "wb").write(d)
run(f"{tmp}/k10c.elf", "uses mmu_ops other than to load it")
# 11: a call inside the MMU span (jsr (d16,pc), PC-relative, no relocation) made a pea of the same function
# (found in the disassembly, outside the trap guard's functions: the rule must hold for C code too)
d = bytearray(E.d)
dis = subprocess.run([ci.PFX + "objdump", "-d", "-j", ".zhwmmu", elf], capture_output=True, text=True).stdout
fns = sorted((x["value"], x["name"]) for x in E.syms if x["name"] and x["type"] != ci.STT_SECTION and
             mu["addr"] <= x["value"] < mu["addr"] + mu["size"])
for line in dis.splitlines():
    f = line.split()
    if len(f) >= 4 and f[1] == "4eba" and f[3] == "jsr":
        a = int(f[0].rstrip(":"), 16)
        if max((v, n) for v, n in fns if v <= a)[1] not in ci.ALLOW_HAND:
            p = mu["off"] + a - mu["addr"]
            d[p:p + 2] = b"\x48\x7a"
            break
open(f"{tmp}/k11.elf", "wb").write(d)
run(f"{tmp}/k11.elf", "pushes the address of span code")
# 12: the register routines' trap guard without its allowance: a resume address handed to mt_on in %a0 is refused
import contextlib
import io
err = io.StringIO()
ci.ALLOW_HAND = ci.ALLOW_HAND - {"mmu_probe_hw"}
ci.leas.clear()
sys.argv = ["check-init.py", elf]
with contextlib.redirect_stderr(err):
    try:
        ci.main()
        code = 0
    except SystemExit as e:
        code = e.code
ok = code not in (0, None) and "mmu_probe_hw" in err.getvalue() and "is handed to" in err.getvalue()
print(("PASS " if ok else "FAIL ") + "refused: span code's address handed to a call in a scratch register (mmu_probe_hw)")
if not ok:
    print(err.getvalue())
    fails += 1
# 13: a resume address of the trap guard (lea label(%pc),%a0 in mmu_probe_hw) retargeted at another span function
d = bytearray(E.d)
pr = next(x["value"] for x in E.syms if x["name"] == "mmu_probe_hw")
other = next(x["value"] for x in E.syms if x["name"] == "tflip")
for line in dis.splitlines():
    f = line.split()
    if len(f) >= 4 and f[1] == "41fa" and int(f[0].rstrip(":"), 16) >= pr:
        a = int(f[0].rstrip(":"), 16)
        p = mu["off"] + a + 2 - mu["addr"]
        d[p:p + 2] = struct.pack(">h", other - (a + 2))
        break
open(f"{tmp}/k13.elf", "wb").write(d)
run(f"{tmp}/k13.elf", "the trap guard loads tflip's address")
# Review of revision 3's code (C1-C6): each false pass a reviewer built, now refused.
# C1: a span's start bound plus an offset into its code (boot code storing kmain's address as __zinit_start + n)
d = bytearray(E.d)
km = next(x["value"] for x in E.syms if x["name"] == "kmain")
for o, off, info in relas(".zinit"):
    t = E.syms[info >> 8]
    if E.word(off - 2) == 0x4879 and not lo <= t["value"] < hi:
        d[o + 4:o + 8] = struct.pack(">I", idx["__zinit_start"] << 8 | 1)
        d[o + 8:o + 12] = struct.pack(">i", km - E.syms[idx["__zinit_start"]]["value"])
        break
open(f"{tmp}/kc1.elf", "wb").write(d)
run(f"{tmp}/kc1.elf", "takes the address of span code __zinit_start+")
# C2: span data outside the live table holding span code's address (mmu_ops_on's size cut by 4 in the symbol table:
# its last entry is then data of nobody's)
d = bytearray(E.d)
st = next(x for x in E.secs if x["type"] == ci.SHT_SYMTAB)
o = st["off"] + idx["mmu_ops_on"] * 16
d[o + 8:o + 12] = struct.pack(">I", struct.unpack(">I", d[o + 8:o + 12])[0] - 4)
open(f"{tmp}/kc2.elf", "wb").write(d)
run(f"{tmp}/kc2.elf", "holds the address of span code")
# C3: resident code writing mmu_on (a tst.b mmu_on made a clr.b mmu_on)
d = bytearray(E.d)
for o, off, info in relas(".text"):
    if E.syms[info >> 8]["name"] == "mmu_on" and E.word(off - 2) == 0x4A39:
        tx = E.sec(".text")
        p = tx["off"] + off - 2 - tx["addr"]
        d[p:p + 2] = b"\x42\x39"
        break
open(f"{tmp}/kc3.elf", "wb").write(d)
run(f"{tmp}/kc3.elf", "writes (or uses) mmu_on")
# C5: the trap guard's asm edited (one byte of mt_on): its loads are not followed, so its bytes are pinned
d = bytearray(E.d)
mo = next(x["value"] for x in E.syms if x["name"] == "mt_on")
p = mu["off"] + mo - mu["addr"]
d[p + 1] ^= 1
open(f"{tmp}/kc5.elf", "wb").write(d)
run(f"{tmp}/kc5.elf", "the trap guard's code changed")


# C6 and A4, on the disassembly the tool reads (subprocess.run wrapped, in this process): after a boot function's load
# of span code's address into %dN (gcc's call cse), the next three instructions replaced
def fake_case(what, want, three):
    real_run = subprocess.run
    def fake_run(cmd, **kw):
        r = real_run(cmd, **kw)
        if cmd[0].endswith("objdump") and ".zinit" in cmd:
            lines = r.stdout.splitlines()
            for k, line in enumerate(lines):
                m = __import__("re").match(r"\s*[0-9a-f]+:\s+(?:203c [0-9a-f ]+\s+movel #(\d+),%(d[0-7])|"
                                           r"4[13579bdf]fa [0-9a-f]{4}\s+lea %pc@\(([0-9a-f]+) <[^>]*>\),%(a[0-7]))$", line)
                fa = m and (int(m.group(1)) if m.group(1) else int(m.group(3), 16))
                if m and fa in zfuncs and all(__import__("re").match(r"\s*[0-9a-f]+:", x) for x in lines[k + 1:k + 4]):
                    ad = [lines[k + n].split(":")[0].strip() for n in (1, 2, 3)]
                    for n, (word, ins) in enumerate(three(ad, m.group(2) or m.group(4))):
                        lines[k + 1 + n] = f"   {ad[n]}:\t{word}      \t{ins}"
                    break
            r.stdout = "\n".join(lines) + "\n"
        return r
    global fails
    ci.subprocess.run = fake_run
    ci.leas.clear()
    err = io.StringIO()
    with contextlib.redirect_stderr(err):
        try:
            ci.main()
            code = 0
        except SystemExit as e:
            code = e.code
    ci.subprocess.run = real_run
    ok = code not in (0, None) and want in err.getvalue()
    print(("PASS " if ok else "FAIL ") + "refused: " + what)
    if not ok:
        print(err.getvalue())
        fails += 1


ci.ALLOW_HAND = ci.ALLOW_HAND | {"mmu_probe_hw"}
fake_case("a write over the held register skipped by a branch, then rts (the linear reading's false pass)",
          "may leave the function",
          lambda ad, r: [("6700 0000", f"beqs {ad[2]} <x>"), ("203c 0000 0000", f"movel #0,%{r}"), ("4e75", "rts")])
fake_case("a held register at a computed jump (a jump table's targets are not followed)", "at the computed jump",
          lambda ad, r: [("4efb 0002", "jmp %pc@(2,%d0:w)"), ("0004", "orib #0,%d0"), ("4e75", "rts")])
fake_case("only half of the held register written (a .w move), then rts", "is used by",
          lambda ad, r: [("3000", f"move{'a' if r[0] == 'a' else ''}w #0,%{r}"), ("4e75", "rts"), ("4e71", "nop")])
fake_case("a conditional branch out of the function while held", "a branch out of the function",
          lambda ad, r: [("6700 0000", "beqs 0 <__zinit_start>"), ("4e71", "nop"), ("4e75", "rts")])
# 3: the pad's first byte not zero
d = bytearray(E.d)
d[zi["off"]] = 1
open(f"{tmp}/k3.elf", "wb").write(d)
run(f"{tmp}/k3.elf", "does not start with its 32 zero bytes")
sys.exit(1 if fails else 0)
