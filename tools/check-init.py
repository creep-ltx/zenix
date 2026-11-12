#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""DESIGN D45 (docs/d45-boot-memory.md section 5): nothing that runs after the boot may reach the spans the kernel gives
back to its pool - the boot's (.zinit code with stage0, .zinitd the embedded boot program) and the CD drive's (.zhwcd).
Reads the kernel's ELF (linked with -q: every relocation kept) and fails, naming the place, when
  1. a relocation outside a span points into it (a call, a jump, a pointer in data, an address taken), except the
     pairs in ALLOW; the CD span may not point into the boot's either;
  2. code in a span takes the address of span code other than to call or jump to it (a lea into an address register
     is allowed only when the register is then used for nothing but jsr/jmp through it: gcc's -Os call cse), also for
     PC-relative addresses with no relocation (read from the disassembly);
  3. the layout is not as link/kernel.ld makes it (alignment, the 32-byte pads, the blobs and atapi_run inside).
usage: tools/check-init.py KERNEL.elf   (TOOLPREFIX for objdump; the Makefile runs it on every kernel it links)"""
import os
import re
import struct
import subprocess
import sys

PFX = os.environ.get("TOOLPREFIX", "m68k-elf-")
SHF_WRITE, SHF_ALLOC, SHF_EXEC = 1, 2, 4
SHT_SYMTAB, SHT_RELA = 2, 4
STT_FUNC, STT_SECTION = 2, 3

# (the function that holds the place, the target symbol) allowed to cross into a span from outside it
ALLOW = {
    ("enter_kmain", "kmain"),           # start.S calls kmain once; nothing after it returns there
    ("ide_atapi", "atapi_run"),         # the CD span's one entry: ide_atapi checks cd_sel first (set only at boot)
}
BOUNDS = {"__zinit_start", "__zinit_end", "__zhwcd_start", "__zhwcd_end", "boot_blob", "boot_blob_end"}  # data, not code
# boot code that may take the address of the boot span's data (the embedded boot program): kmain hands it to
# proc_spawn_image, whose fill_mem copies it before returning (proc.c); a new use must be reviewed and added here
ALLOW_DATA = {("kmain", "boot_blob"), ("kmain", "boot_blob_end")}
CALLS = {0x4EB9, 0x4EF9, 0x61FF, 0x60FF, 0x6100, 0x6000, 0x4EBA, 0x4EFA}


class Elf:
    def __init__(self, path):
        d = self.d = open(path, "rb").read()
        if d[:4] != b"\x7fELF" or d[4] != 1 or d[5] != 2:
            sys.exit(f"check-init: {path}: not a 32-bit big-endian ELF file")
        shoff, = struct.unpack(">I", d[32:36])
        shentsize, shnum, shstrndx = struct.unpack(">HHH", d[46:52])
        self.secs = []
        for i in range(shnum):
            f = struct.unpack(">10I", d[shoff + i * shentsize:shoff + i * shentsize + 40])
            self.secs.append(dict(name_off=f[0], type=f[1], flags=f[2], addr=f[3], off=f[4], size=f[5], link=f[6],
                                  info=f[7], idx=i))
        st = self.secs[shstrndx]
        for s in self.secs:
            s["name"] = self.cstr(st["off"] + s["name_off"])
        self.syms = []
        for s in self.secs:
            if s["type"] == SHT_SYMTAB:
                strs = self.secs[s["link"]]["off"]
                for o in range(s["off"], s["off"] + s["size"], 16):
                    name, value, size, info, other, shndx = struct.unpack(">IIIBBH", d[o:o + 16])
                    self.syms.append(dict(name=self.cstr(strs + name), value=value, size=size, shndx=shndx,
                                          type=info & 15))

    def cstr(self, o):
        return self.d[o:self.d.index(b"\0", o)].decode("latin1")

    def sec(self, name):
        return next((s for s in self.secs if s["name"] == name), None)

    def word(self, a):
        for s in self.secs:
            if s["flags"] & SHF_ALLOC and s["addr"] <= a and a + 2 <= s["addr"] + s["size"] and s["type"] != 8:
                o = s["off"] + a - s["addr"]
                return struct.unpack(">H", self.d[o:o + 2])[0]
        return None

    def relas(self):
        for s in self.secs:
            if s["type"] != SHT_RELA or not (self.secs[s["info"]]["flags"] & SHF_ALLOC):
                continue
            for o in range(s["off"], s["off"] + s["size"], 12):
                off, info, addend = struct.unpack(">IIi", self.d[o:o + 12])
                yield self.secs[s["info"]], off, info & 0xFF, self.syms[info >> 8], addend


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    path = sys.argv[1]
    E = Elf(path)
    errs = []
    syms = {s["name"]: s for s in E.syms if s["name"]}

    def need(n):
        if n not in syms:
            sys.exit(f"check-init: {path}: no {n} (link/kernel.ld's spans missing?)")
        return syms[n]["value"]
    spans = {"boot": (need("__zinit_start"), need("__zinit_end")), "cd": (need("__zhwcd_start"), need("__zhwcd_end"))}
    secs = {"boot": [E.sec(".zinit"), E.sec(".zinitd")], "cd": [E.sec(".zhwcd")]}
    if None in secs["boot"] + secs["cd"]:
        sys.exit(f"check-init: {path}: a span's section is missing")
    span_of_sec = {s["idx"]: k for k, l in secs.items() for s in l}
    code = {"boot": (secs["boot"][0]["addr"], secs["boot"][0]["addr"] + secs["boot"][0]["size"]),
            "cd": (secs["cd"][0]["addr"], secs["cd"][0]["addr"] + secs["cd"][0]["size"])}
    st0 = (need("stage0_blob"), need("stage0_blob_end"))     # stage0's own code: runs under AmigaOS, before the kernel

    def span_at(a):
        return next((k for k, (lo, hi) in spans.items() if lo <= a < hi), None)

    def code_at(a):
        return next((k for k, (lo, hi) in code.items() if lo <= a < hi), None)

    # the functions and labels by address: who holds a place
    marks = sorted((s["value"], s["name"]) for s in E.syms
                   if s["name"] and s["type"] != STT_SECTION and s["shndx"] not in (0, 0xFFF1)
                   and not s["name"].startswith(("__zinit", "__zhwcd", "__bss", "_end")))
    import bisect
    addrs = [a for a, _ in marks]

    def holder(a):
        i = bisect.bisect_right(addrs, a) - 1
        return marks[i][1] if i >= 0 else "?"

    def tname(sym, add):
        if sym["type"] == STT_SECTION or not sym["name"]:
            return holder(sym["value"] + add)
        return sym["name"] + (f"+{add}" if add else "")

    # 3. the layout
    for k, (lo, hi) in spans.items():
        if lo & 15 or hi & 15:
            errs.append(f"the {k} span {lo:#x}..{hi:#x} is not 16-byte aligned")
        pad = E.d[secs[k][0]["off"]:secs[k][0]["off"] + 32]
        if secs[k][0]["addr"] != lo or pad != bytes(32):
            errs.append(f"the {k} span does not start with its 32 zero bytes")
        for s in secs[k]:
            if s["flags"] & SHF_WRITE:
                errs.append(f"{s['name']} is writable")
    for n, k in (("stage0_blob", "boot"), ("boot_blob", "boot"), ("atapi_run", "cd")):
        cand = [s for s in E.syms if s["name"] == n or s["name"].startswith(n + ".")]
        if not cand or span_at(cand[0]["value"]) != k:
            errs.append(f"{n} is not in the {k} span")

    # 1 and 2. the relocations
    nrel = 0
    for sec, off, typ, sym, add in E.relas():
        if typ == 0:
            continue                                    # R_68K_NONE
        nrel += 1
        t = sym["value"] + add
        tspan = span_of_sec.get(sym["shndx"]) if sym["type"] != STT_SECTION else None
        tspan = tspan or span_at(t)
        if sym["name"] in ("__zinit_end", "__zhwcd_end") and add == 0:
            tspan = None                                # a span's end bound: an address just after it
        pspan = span_at(off)
        if tspan and tspan != pspan and not (pspan == "boot" and tspan == "cd"):
            pair = (holder(off).split(".")[0], (sym["name"] or holder(t)).split(".")[0])
            w = E.word(off - 2)
            if pair not in ALLOW or not (w in CALLS or w is not None and (w & 0xF0FF) in (0x60FF, 0x6000)):
                errs.append(f"{holder(off)} at {off:#x} ({sec['name']}) refers to {tname(sym, add)} in the {tspan} "
                            f"span: it may run after the span is given back")
        if pspan and tspan == "boot" and not code_at(t) and not (st0[0] <= off < st0[1]) and \
                sym["name"] not in ("__zinit_start", "__zinit_end", "__zhwcd_start", "__zhwcd_end"):
            if (holder(off).split(".")[0], sym["name"]) not in ALLOW_DATA:
                errs.append(f"{holder(off)} at {off:#x} takes the address of the boot span's data {tname(sym, add)}: "
                            f"only the uses in ALLOW_DATA (reviewed) may")
            continue
        if pspan and code_at(t) and not (st0[0] <= off < st0[1]) and sym["name"] not in BOUNDS:
            w = E.word(off - 2)
            if w is not None and w in CALLS or w is not None and (w & 0xF0FF) in (0x60FF, 0x6000):
                continue
            if w is not None and (w & 0xF1FF) == 0x41F9:
                leas.append((off - 2, f"%a{(w >> 9) & 7}"))
                continue
            if w is not None and (w & 0xF1FF) == 0x203C:
                leas.append((off - 2, f"%d{(w >> 9) & 7}"))     # move.l #fn,%dN: the same cse through a data register
                continue
            errs.append(f"{holder(off)} at {off:#x} takes the address of span code {tname(sym, add)} "
                        f"(not a call or jump)")

    # 2, from the disassembly: PC-relative lea/pea of span code (no relocation inside one input section), and what each
    # lea'd register is used for
    dis = []
    for k in code:
        out = subprocess.run([PFX + "objdump", "-d", "-j", secs[k][0]["name"], path], capture_output=True, text=True,
                             check=True).stdout
        for line in out.splitlines():
            m = re.match(r"\s*([0-9a-f]+):\s+(?:[0-9a-f]{4} ?)+\s*\t?(\S.*)$", line)
            if m:
                dis.append((int(m.group(1), 16), m.group(2).strip().replace("%fp", "%a6").replace("%sp", "%a7")))
        dis.append((None, None))                        # a section's end
    at = {a: i for i, (a, _) in enumerate(dis) if a is not None}
    for i, (a, ins) in enumerate(dis):
        if a is None or st0[0] <= a < st0[1]:
            continue
        m = re.match(r"(lea|pea)\s+%pc@\(([0-9a-f]+)", ins)
        if m and code_at(int(m.group(2), 16)):
            if m.group(1) == "pea":
                errs.append(f"{holder(a)} at {a:#x} pushes the address of span code ({ins})")
            else:
                r = re.search(r",(%a[0-7])$", ins)
                leas.append((a, r.group(1)))
    # each register that holds span code's address, followed to the end of its function (linearly, every path at
    # once): it may only be called through, copied to an address register (followed in turn), or written over
    # each register that holds span code's address, followed to the end of its function (linearly): it may only be
    # called through, copied to an address register (followed in turn), or written over by a plain load (move, moveq,
    # lea, clr, a movem load); any other use (arithmetic, a store, a push, a return or a jump away with it) is an error
    def regs_of(lst):                                   # "%d2-%d4/%a2" -> {%d2, %d3, %d4, %a2}
        out = set()
        for part in lst.split("/"):
            m = re.fullmatch(r"%([ad])([0-7])(?:-%([ad])([0-7]))?", part.strip())
            if not m:
                continue
            if m.group(3):
                lo, hi = (0 if m.group(1) == "d" else 8) + int(m.group(2)), (0 if m.group(3) == "d" else 8) + int(m.group(4))
                out |= {("%d" if x < 8 else "%a") + str(x & 7) for x in range(lo, hi + 1)}
            else:
                out.add("%" + m.group(1) + m.group(2))
        return out
    for a, r in leas:
        i = at.get(a)
        if i is None:
            errs.append(f"{holder(a)} at {a:#x}: a load of span code's address outside the disassembly")
            continue
        held = {r}
        for b, ins in dis[i + 1:]:
            if b is None or holder(b) != holder(a) or not held:
                break
            op = ins.split()[0]
            if op in ("rts", "rtd", "rte", "rtr") or op in ("jra", "bra", "braw", "bral", "jmp") and "@" not in ins \
                    and not re.search(r"<" + re.escape(holder(a)) + r"(\+0x[0-9a-f]+)?>", ins):
                errs.append(f"{holder(a)} at {a:#x}: the address of span code is still in {', '.join(sorted(held))} "
                            f"at '{ins}' ({b:#x}): it may leave the function")
                break
            if op.startswith("movem"):
                src, dst = ins[len(op):].strip().rsplit(",", 1) if "," in ins else ("", "")
                if "@" in dst or "(" in dst:            # a store of the list
                    hit = regs_of(src) & held
                    if hit:
                        errs.append(f"{holder(a)} at {a:#x}: the address of span code in {sorted(hit)[0]} is stored "
                                    f"by '{ins}' at {b:#x}")
                        break
                else:
                    held -= regs_of(dst)                # a load: written over
                continue
            for reg in sorted(held):
                if reg not in ins:
                    continue
                args = ins[len(op):].strip()
                src = args.rsplit(",", 1)[0] if "," in args else ""
                m = re.fullmatch(rf"move[a]?l\s+{reg},(%a[0-7])", ins)
                if re.fullmatch(rf"(jsr|jmp)\s+{reg}@", ins):
                    pass
                elif m:
                    held.add(m.group(1))
                elif re.match(r"(move[aq]?[lwb]?|mov3ql|lea|clr[lwb]?|bfext[su]|bfffo)$", op) and args.endswith("," + reg) \
                        and reg not in src or re.fullmatch(rf"clr[lwb]?\s+{reg}", ins):
                    held.discard(reg)                   # written over: the address is gone from it
                else:
                    errs.append(f"{holder(a)} at {a:#x}: the address of span code in {reg} is used by '{ins}' "
                                f"at {b:#x}")
                    held.clear()
                    break

    if errs:
        for e in errs:
            print(f"check-init: {path}: {e}", file=sys.stderr)
        sys.exit(1)
    b, c = spans["boot"], spans["cd"]
    print(f"check-init: {os.path.basename(path)}: boot span {b[1] - b[0]} bytes, CD span {c[1] - c[0]} bytes; "
          f"{nrel} relocations, none into a span from outside")


leas = []
if __name__ == "__main__":
    main()
