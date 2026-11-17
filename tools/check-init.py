#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""DESIGN D45 (docs/d45-boot-memory.md section 5): nothing that runs after the boot may reach the spans the kernel gives
back to its pool - the boot's (.zinit code with stage0, .zinitd the embedded boot program), the CD drive's (.zhwcd) and
memory protection's (.zhwmmu, section 13: reached only through the table mmu_ops).
Reads the kernel's ELF (linked with -q: every relocation kept) and fails, naming the place, when
  1. a relocation outside a span points into it (a call, a jump, a pointer in data, an address taken), except the
     pairs in ALLOW (none for the MMU span); the CD and MMU spans may not point into the boot's or each other;
  2. code in a span takes the address of span code other than to call or jump to it (a lea into an address register
     is allowed only when the register is then used for nothing but jsr/jmp through it: gcc's -Os call cse), also for
     PC-relative addresses with no relocation (read from the disassembly), nor hand it to another function in a scratch
     register; span functions' addresses are held only by the MMU span's table mmu_ops_on, which only mmu_arm refers
     to (move.l #mmu_ops_on,mmu_ops), and only mmu_arm and mmu_disarm write mmu_ops (everyone else loads it);
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
BOUNDS = {"__zinit_start", "__zinit_end", "__zhwcd_start", "__zhwcd_end", "__zhwmmu_start", "__zhwmmu_end", "boot_blob",
          "boot_blob_end"}  # data, not code
# D45 section 13: the MMU span's register routines (kernel/mmu.c's asm) hand resume addresses to the trap guard mt_on in
# %a0 (kept in mt_pc) and mt_on puts mt_trap into four vectors; mt_off puts them back and only mt_trap reads mt_pc, all
# within one call with interrupts masked. In these functions only a PC-relative lea of a label inside the same function
# (a resume address) or, in mt_on, of mt_trap is accepted, and not followed (reviewed); any other load of span code's
# address there is an error.
ALLOW_HAND = {"mmu_regs_load", "mmu_regs_save", "mmu_probe_hw", "mmu_fpu", "mt_on"}
GUARD_TRAP = ("mt_on", "mt_trap")
# ... and since these functions' loads are not followed, their bytes (the asm from mmu_regs_load to the end of
# mmu_flush, the relocated fields read as zero) are pinned: an edit of the asm fails the build until it is reviewed and
# the new digest written here (review of revision 3's code, C5). The allowance holds only for the functions of these
# names inside that pinned range in the MMU span, never for a namesake elsewhere.
GUARD_FIRST, GUARD_LAST = "mmu_regs_load", "mmu_flush"
GUARD_SHA = "8fd515637e6e2c7d"
ARM, DISARM, OPS, ON = "mmu_arm", "mmu_disarm", "mmu_ops", "mmu_ops_on"   # D45 section 13: the table and its writers
# mmu_on is set only by mmu_arm (with the table: armed exactly while it is set, so the span is kept whenever a copy of a
# slot could exist) and cleared only by mmu_before_reset_; everyone else only tests or loads it (C3)
ON_FLAG, ON_WRITERS = "mmu_on", {"mmu_arm", "mmu_before_reset_"}
SCRATCH = {"%d0", "%d1", "%a0", "%a1"}
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
    spans = {"boot": (need("__zinit_start"), need("__zinit_end")), "cd": (need("__zhwcd_start"), need("__zhwcd_end")),
             "mmu": (need("__zhwmmu_start"), need("__zhwmmu_end"))}
    secs = {"boot": [E.sec(".zinit"), E.sec(".zinitd")], "cd": [E.sec(".zhwcd")],
            "mmu": [E.sec(".zhwmmu"), E.sec(".zhwmmud")]}
    if None in secs["boot"] + secs["cd"] + secs["mmu"]:
        sys.exit(f"check-init: {path}: a span's section is missing")
    span_of_sec = {s["idx"]: k for k, l in secs.items() for s in l}
    code = {k: (secs[k][0]["addr"], secs[k][0]["addr"] + secs[k][0]["size"]) for k in secs}
    ops = (need(OPS), need(OPS) + syms[OPS]["size"])
    on = (need(ON), need(ON) + syms[ON]["size"])
    st0 = (need("stage0_blob"), need("stage0_blob_end"))     # stage0's own code: runs under AmigaOS, before the kernel

    def span_at(a):
        return next((k for k, (lo, hi) in spans.items() if lo <= a < hi), None)

    def code_at(a):
        return next((k for k, (lo, hi) in code.items() if lo <= a < hi), None)

    # the functions and labels by address: who holds a place
    marks = sorted((s["value"], s["name"]) for s in E.syms
                   if s["name"] and s["type"] != STT_SECTION and s["shndx"] not in (0, 0xFFF1)
                   and not s["name"].startswith(("__zinit", "__zhwcd", "__zhwmmu", "__bss", "_end")))
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
    for n, k in (("stage0_blob", "boot"), ("boot_blob", "boot"), ("atapi_run", "cd"), (ARM, "mmu"),
                 ("mmu_regs_load", "mmu"), ("mt_trap", "mmu"), ("mmu_fault_", "mmu"), (ON, "mmu")):
        cand = [s for s in E.syms if s["name"] == n or s["name"].startswith(n + ".")]
        if not cand or span_at(cand[0]["value"]) != k:
            errs.append(f"{n} is not in the {k} span")
    if span_at(ops[0]) or ops[1] - ops[0] != 4:
        errs.append(f"{OPS} is in a span (or is not one pointer)")
    if not (secs["mmu"][1]["addr"] <= on[0] and on[1] <= secs["mmu"][1]["addr"] + secs["mmu"][1]["size"]):
        errs.append(f"{ON} is not in the MMU span's data section .zhwmmud")

    rel_at = {off: (sym, add) for _, off, typ, sym, add in E.relas() if typ}
    for n in (ARM, DISARM, OPS, ON, ON_FLAG, GUARD_FIRST, GUARD_LAST) + tuple(ALLOW_HAND) + tuple(ON_WRITERS):
        if sum(1 for x in E.syms if x["name"] == n) != 1:
            errs.append(f"{n}: a name the check trusts is not defined exactly once")
    g0 = need(GUARD_FIRST)
    g1 = next((v for v, _ in marks if v > need(GUARD_LAST)), code["mmu"][1])
    if not (code["mmu"][0] <= g0 < g1 <= code["mmu"][1]):
        errs.append(f"the trap guard's asm ({GUARD_FIRST}..{GUARD_LAST}) is not in the MMU span's code")
    import hashlib
    gb = bytearray(E.d[secs["mmu"][0]["off"] + g0 - code["mmu"][0]:secs["mmu"][0]["off"] + g1 - code["mmu"][0]])
    for o in rel_at:
        if g0 <= o < g1:
            gb[o - g0:o - g0 + 4] = bytes(4)
    sha = hashlib.sha256(bytes(gb)).hexdigest()[:16]
    if sha != GUARD_SHA:
        errs.append(f"the trap guard's code changed ({GUARD_FIRST}..{GUARD_LAST}, digest {sha}): review it against "
                    f"ALLOW_HAND's rule, then write the new digest into GUARD_SHA")

    def guard(a):                                       # inside the pinned trap guard
        return g0 <= a < g1 and holder(a) in ALLOW_HAND
    zd = (secs["mmu"][1]["addr"], secs["mmu"][1]["addr"] + secs["mmu"][1]["size"])
    flag = (need(ON_FLAG), need(ON_FLAG) + 1)
    # 1 and 2. the relocations
    nrel = 0
    for sec, off, typ, sym, add in E.relas():
        if typ == 0:
            continue                                    # R_68K_NONE
        nrel += 1
        t = sym["value"] + add
        tspan = span_of_sec.get(sym["shndx"]) if sym["type"] != STT_SECTION else None
        tspan = tspan or span_at(t)
        if sym["name"] in ("__zinit_end", "__zhwcd_end", "__zhwmmu_end") and add == 0:
            tspan = None                                # a span's end bound: an address just after it
        pspan = span_at(off)
        if tspan and tspan != pspan and not (pspan == "boot" and tspan in ("cd", "mmu")):
            pair = (holder(off).split(".")[0], (sym["name"] or holder(t)).split(".")[0])
            w = E.word(off - 2)
            if pair not in ALLOW or not (w in CALLS or w is not None and (w & 0xF0FF) in (0x60FF, 0x6000)):
                errs.append(f"{holder(off)} at {off:#x} ({sec['name']}) refers to {tname(sym, add)} in the {tspan} "
                            f"span: it may run after the span is given back")
        if pspan and tspan == "boot" and not code_at(t) and not (st0[0] <= off < st0[1]) and \
                sym["name"] not in BOUNDS - {"boot_blob", "boot_blob_end"}:
            if (holder(off).split(".")[0], sym["name"]) not in ALLOW_DATA:
                errs.append(f"{holder(off)} at {off:#x} takes the address of the boot span's data {tname(sym, add)}: "
                            f"only the uses in ALLOW_DATA (reviewed) may")
            continue
        if pspan and not sec["flags"] & SHF_EXEC and code_at(t):    # span data holding span code's address (C2)
            if not (on[0] <= off < on[1] and code_at(t) == "mmu"):
                errs.append(f"{holder(off)} at {off:#x} ({sec['name']}, data) holds the address of span code "
                            f"{tname(sym, add)}: only {ON}'s entries may")
            continue
        if flag[0] <= t < flag[1] and holder(off) not in ON_WRITERS:
            w = E.word(off - 2)                         # tst.b abs.l, or move.b abs.l,%dN
            if w is None or w != 0x4A39 and (w & 0xF1FF) != 0x1039:
                errs.append(f"{holder(off)} at {off:#x} writes (or uses) {ON_FLAG}: only {', '.join(sorted(ON_WRITERS))} "
                            f"may write it")
        if zd[0] <= t < zd[1] and not (sym["name"] == "__zhwmmu_end" and add == 0):
            # the MMU span's data (the live table): only mmu_arm's move.l #mmu_ops_on,mmu_ops
            d = rel_at.get(off + 4)
            if holder(off) != ARM or E.word(off - 2) != 0x23FC or not d or not ops[0] <= d[0]["value"] + d[1] < ops[1]:
                errs.append(f"{holder(off)} at {off:#x} refers to {ON}: only {ARM}'s move.l #{ON},{OPS} may")
            continue
        if ops[0] <= t < ops[1] and holder(off) not in (ARM, DISARM, OPS):
            w = E.word(off - 2)                         # everyone else only loads the pointer (movea.l/move.l abs.l)
            if w is None or (w & 0xF1FF) not in (0x2079, 0x2039):
                errs.append(f"{holder(off)} at {off:#x} uses {OPS} other than to load it: only {ARM} and {DISARM} "
                            f"write it")
        if pspan == "mmu" and on[0] <= off < on[1] and code_at(t) == "mmu":
            continue                                    # the live table's entries: the span's functions
        pad = code_at(t) and t < secs[code_at(t)][0]["addr"] + 32
        if pspan and code_at(t) and not (st0[0] <= off < st0[1]) and not (sym["name"] in BOUNDS and pad):
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
    functions = {s["value"]: True for s in E.syms if s["type"] == STT_FUNC}
    for i, (a, ins) in enumerate(dis):
        if a is None or st0[0] <= a < st0[1]:
            continue
        m = re.match(r"(lea|pea)\s+%pc@\(([0-9a-f]+)", ins)
        if m and code_at(int(m.group(2), 16)):
            if m.group(1) == "pea":
                errs.append(f"{holder(a)} at {a:#x} pushes the address of span code ({ins})")
            elif guard(a):                              # the trap guard's resume addresses: not followed (pinned)
                tgt = holder(int(m.group(2), 16))
                if tgt != holder(a) and (holder(a), tgt) != GUARD_TRAP:
                    errs.append(f"{holder(a)} at {a:#x}: the trap guard loads {tgt}'s address ({ins}): only its own "
                                f"resume labels (and mt_on: mt_trap) are accepted")
            else:
                r = re.search(r",(%a[0-7])$", ins)
                leas.append((a, r.group(1)))
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
    # every path from the load is followed (a worklist over the function's branches, each place once per set of held
    # registers): a write over the register on one path does not clear it on another (review of revision 3, A4)
    cond = re.compile(r"(?:[bj](?:eq|ne|cc|cs|hi|ls|ge|lt|gt|le|pl|mi|vc|vs|hs|lo)[swl]?|db\w+)$")
    for a, r in leas:
        i = at.get(a)
        if i is None:
            errs.append(f"{holder(a)} at {a:#x}: a load of span code's address outside the disassembly")
            continue
        fn, seen, work, bad = holder(a), set(), [(i + 1, frozenset({r}))], False
        while work and not bad:
            j, h = work.pop()
            held = set(h)
            while held and not bad:
                b, ins = dis[j]
                if b is None or holder(b) != fn or (j, frozenset(held)) in seen:
                    break
                seen.add((j, frozenset(held)))
                j += 1
                op = ins.split()[0]
                tm = re.search(r"([0-9a-f]+) <", ins)
                tgt = int(tm.group(1), 16) if tm else None
                if op in ("jsr", "jbsr", "bsrs", "bsrw", "bsrl", "bsr", "trap", "trapv", "illegal"):  # a call: the
                    # scratch registers are the callee's (a trap's too)
                    through = re.fullmatch(r"jsr\s+(%a[0-7])@", ins)
                    through = through and through.group(1) in held     # through the held register: no hand-over
                    if not through and held & SCRATCH:
                        errs.append(f"{fn} at {a:#x}: the address of span code in "
                                    f"{', '.join(sorted(held & SCRATCH))} is handed to '{ins}' at {b:#x}")
                        bad = True
                    elif through or tgt is not None and functions.get(tgt):
                        held -= SCRATCH                 # a C function (or the held one) clobbers the scratch registers
                    continue
                inside = tgt is not None and holder(tgt) == fn and tgt in at
                if op == "jmp" and "@" in ins and not re.fullmatch(r"jmp\s+(%a[0-7])@", ins) or \
                        op == "jmp" and re.fullmatch(r"jmp\s+(%a[0-7])@", ins) and ins.split()[1][:-1] not in held \
                        and held:              # a computed jump (a jump table): its targets are not followed (C6)
                    errs.append(f"{fn} at {a:#x}: the address of span code is still in {', '.join(sorted(held))} "
                                f"at the computed jump '{ins}' ({b:#x})")
                    bad = True
                    continue
                if cond.match(op) and tgt is not None and not inside:
                    errs.append(f"{fn} at {a:#x}: the address of span code is still in {', '.join(sorted(held))} "
                                f"at '{ins}' ({b:#x}): a branch out of the function")
                    bad = True
                    continue
                if op in ("rts", "rtd", "rte", "rtr") or op in ("jra", "bra", "bras", "braw", "bral", "jmp") \
                        and "@" not in ins and not inside:
                    errs.append(f"{fn} at {a:#x}: the address of span code is still in {', '.join(sorted(held))} "
                                f"at '{ins}' ({b:#x}): it may leave the function")
                    bad = True
                    continue
                if op in ("jra", "bra", "bras", "braw", "bral", "jmp") and inside:
                    j = at[tgt]                         # an unconditional jump inside the function
                    continue
                if cond.match(op) and inside:
                    work.append((at[tgt], frozenset(held)))     # the branch taken; the fall-through goes on here
                if op.startswith("movem"):
                    src, dst = ins[len(op):].strip().rsplit(",", 1) if "," in ins else ("", "")
                    if "@" in dst or "(" in dst:        # a store of the list
                        hit = regs_of(src) & held
                        if hit:
                            errs.append(f"{fn} at {a:#x}: the address of span code in {sorted(hit)[0]} is stored "
                                        f"by '{ins}' at {b:#x}")
                            bad = True
                    else:
                        held -= regs_of(dst)            # a load: written over
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
                    elif re.match(r"(movel|moveal|moveq|mov3ql|lea|clrl|bfext[su]|bfffo)$", op) and \
                            args.endswith("," + reg) and reg not in src or re.fullmatch(rf"clrl\s+{reg}", ins):
                        # (only a whole-register write: a .w or .b one leaves the address's other half, C6)
                        held.discard(reg)               # written over on this path: the address is gone from it
                    else:
                        errs.append(f"{fn} at {a:#x}: the address of span code in {reg} is used by '{ins}' "
                                    f"at {b:#x}")
                        bad = True
                        break

    if errs:
        for e in errs:
            print(f"check-init: {path}: {e}", file=sys.stderr)
        sys.exit(1)
    b, c, u = spans["boot"], spans["cd"], spans["mmu"]
    print(f"check-init: {os.path.basename(path)}: boot span {b[1] - b[0]} bytes, CD span {c[1] - c[0]} bytes, MMU span "
          f"{u[1] - u[0]} bytes; "
          f"{nrel} relocations, none into a span from outside")


leas = []
if __name__ == "__main__":
    main()
