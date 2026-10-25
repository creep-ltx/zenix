#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Turn a linked ELF (linked at address 0, with -Wl,-q) into a ZEXE image.

ZEXE = flat image + relocation table, loadable at any address:
  +0   bra.w to the entry code
  +4   'ZEXE'
  +8   u32 image_size   bytes of code+data in the file
  +12  u32 mem_size     bytes of memory the image needs (image + bss)
  +16  u32 nreloc
  +20  u32 reloc_off    file offset of the relocation table
  +24  u32 stack_size   stack the program wants (0 = loader default)
  +28  u32 stage0   file offset of position-independent code the bootblock calls (0 = none)
After the image: nreloc big-endian u32 offsets of 32-bit words that hold
link-time (base 0) addresses; the loader adds the load address to each.
Only R_68K_32 needs relocating; PC-relative references stay valid.

ZEX2 (DESIGN D13, docs/d13-shared-code.md section 4): a program with shared code. Linked with link/pure.ld from
objects built with -msep-data (tools/purecc.sh): code and constants first (the shared part, one copy for every
process running the program), then, from text_size (16-aligned), the data part: the address table .got, .got.plt,
.data and .bss. The code reaches every global through the table, whose start the kernel puts in a5.
  +0   bra.w to the entry code
  +4   'ZEX2'
  +8   u32 text_size  bytes of the shared part (file offsets 0..text_size; also the link address of the data part)
  +12  u32 data_size  bytes of the data image in the file, at offset text_size (table + .data)
  +16  u32 mem_size   bytes of the data part in memory (data image + bss)
  +20  u32 nrel_code  relocations whose target is in the shared part (the loader adds the code's address)
  +24  u32 nrel_data  relocations whose target is in the data part (it adds data address - text_size)
  +28  u32 reloc_off  file offset of the two lists: nrel_code, then nrel_data big-endian u32 link addresses of
                      32-bit words, all inside the data image (stage 1: no relocation in the shared part)
  +32  u32 stack_size
  +36  u32 reserved (0)
Every relocation is classified by the section of its target symbol, never by its value: the table's slots through
the R_68K_GOT16O/GOT32O relocations in the code that name them (a static -q link lists no .rela.got, so this tool adds
one relocation per slot). Refused, with the symbol named: a relocation placed in the shared part (stage 1), a
PC-relative reference between the parts (shared-to-data), a value equal to text_size (ambiguous), an undefined weak
or an absolute symbol, a table slot no relocation names.
"""
import os
import re
import struct
import subprocess
import sys

PFX = os.environ.get("TOOLPREFIX", "m68k-elf-")    # the Makefile exports it; default: m68k-elf-objcopy from PATH
HDR_MAGIC_OFF = 4
STACK_DEFAULT = 0


def run(*a):
    return subprocess.run(a, check=True, capture_output=True, text=True).stdout


def main():
    elf, out = sys.argv[1:3]
    stack = int(sys.argv[3]) if len(sys.argv) > 3 else STACK_DEFAULT
    subprocess.run([PFX + "objcopy", "-O", "binary", elf, out + ".img"], check=True)
    img = bytearray(open(out + ".img", "rb").read())
    if img[4:8] == b"ZEX2":
        return main_v2(elf, out, stack, img)
    if img[4:8] != b"ZEXE":
        sys.exit("no ZEXE header at offset 4 (start.S header missing)")

    syms = {}
    for line in run(PFX + "nm", elf).splitlines():
        p = line.split()
        if len(p) == 3:
            syms[p[2]] = int(p[0], 16)
    bss_end = syms["__bss_end"]

    relocs = []
    cur = None
    for line in run(PFX + "readelf", "-r", "-W", elf).splitlines():
        m = re.match(r"Relocation section '(\S+)'", line)
        if m:
            cur = m.group(1)
            continue
        m = re.match(r"([0-9a-f]{8})\s+[0-9a-f]+\s+(R_68K_\w+)", line)
        if not m:
            continue
        off, typ = int(m.group(1), 16), m.group(2)
        if cur and cur.startswith((".rela.debug", ".rela.eh_frame", ".rela.comment")):
            continue                        # not loaded: its offsets are not addresses
        if typ == "R_68K_32":
            if off + 4 > len(img):
                continue                    # inside bss: nothing to patch
            relocs.append(off)
        elif typ in ("R_68K_PC32", "R_68K_PC16", "R_68K_PC8"):
            pass
        else:
            sys.exit(f"unsupported relocation {typ} at {off:#x} in {cur}")
    relocs.sort()

    while len(img) % 4:
        img.append(0)
    image_size = len(img)
    reloc_off = image_size
    table = b"".join(struct.pack(">I", r) for r in relocs)
    total = image_size + len(table)
    mem_size = max(bss_end, total)
    mem_size = (mem_size + 3) & ~3
    img[8:12] = struct.pack(">I", image_size)
    img[12:16] = struct.pack(">I", mem_size)
    img[16:20] = struct.pack(">I", len(relocs))
    img[20:24] = struct.pack(">I", reloc_off)
    img[24:28] = struct.pack(">I", stack)
    img[28:32] = struct.pack(">I", syms.get("stage0_blob", 0))
    open(out, "wb").write(bytes(img) + table)
    print(f"{out}: image {image_size}B, {len(relocs)} relocs, mem {mem_size}B")


# ---- ZEX2 ----
R_NAMES = {0: "NONE", 1: "32", 2: "16", 3: "8", 4: "PC32", 5: "PC16", 6: "PC8", 7: "GOT32", 8: "GOT16", 9: "GOT8",
           10: "GOT32O", 11: "GOT16O", 12: "GOT8O", 13: "PLT32", 14: "PLT16", 15: "PLT8"}
SHF_WRITE, SHF_ALLOC = 1, 2
SHT_SYMTAB, SHT_RELA, SHT_NOBITS = 2, 4, 8
SHN_UNDEF, SHN_ABS = 0, 0xFFF1


class Elf:
    """just enough of a 32-bit big-endian ELF: sections, symbols, RELA relocations"""

    def __init__(self, path):
        d = self.d = open(path, "rb").read()
        if d[:4] != b"\x7fELF" or d[4] != 1 or d[5] != 2:
            sys.exit(f"{path}: not a 32-bit big-endian ELF file")
        shoff, = struct.unpack(">I", d[32:36])
        shentsize, shnum, shstrndx = struct.unpack(">HHH", d[46:52])
        self.secs = []
        for i in range(shnum):
            f = struct.unpack(">10I", d[shoff + i * shentsize:shoff + i * shentsize + 40])
            self.secs.append(dict(name_off=f[0], type=f[1], flags=f[2], addr=f[3], off=f[4], size=f[5], link=f[6],
                                  info=f[7], entsize=f[9], idx=i))
        st = self.secs[shstrndx]
        for s in self.secs:
            s["name"] = self.cstr(st["off"] + s["name_off"])
        self.syms = []
        for s in self.secs:
            if s["type"] == SHT_SYMTAB:
                strs = self.secs[s["link"]]["off"]
                for o in range(s["off"], s["off"] + s["size"], 16):
                    name, value, size, info, other, shndx = struct.unpack(">IIIBBH", d[o:o + 16])
                    self.syms.append(dict(name=self.cstr(strs + name), value=value, shndx=shndx, bind=info >> 4,
                                          type=info & 15))

    def cstr(self, o):
        return self.d[o:self.d.index(b"\0", o)].decode("latin1")

    def sec(self, name):
        return next((s for s in self.secs if s["name"] == name), None)

    def relas(self):
        """(section the relocations apply to, offset, type, symbol, addend) for every loaded section"""
        for s in self.secs:
            if s["type"] != SHT_RELA or not (self.secs[s["info"]]["flags"] & SHF_ALLOC):
                continue
            for o in range(s["off"], s["off"] + s["size"], 12):
                off, info, addend = struct.unpack(">IIi", self.d[o:o + 12])
                yield self.secs[s["info"]], off, info & 0xFF, self.syms[info >> 8], addend


def main_v2(elf, out, stack, img):
    E = Elf(elf)
    syms = {s["name"]: s["value"] for s in E.syms if s["name"]}
    text_size, bss_end = syms.get("__text_end"), syms.get("__bss_end")
    if text_size is None or bss_end is None or text_size % 16:
        sys.exit(f"{elf}: no 16-aligned __text_end / no __bss_end (not linked with link/pure.ld)")
    got = E.sec(".got")
    got_lo, got_hi = (got["addr"], got["addr"] + got["size"]) if got else (text_size, text_size)
    if got and got_lo != text_size:
        sys.exit(f"{elf}: the table (.got) must start the data part ({got_lo:#x}, not {text_size:#x})")
    errs = []

    def part_of(a):
        return "code" if a < text_size else "data" if a <= bss_end else None

    for s in E.secs:                                    # every loaded section lies wholly in one part
        if not (s["flags"] & SHF_ALLOC) or s["size"] == 0:
            continue
        lo, hi = s["addr"], s["addr"] + s["size"]
        if hi <= text_size:
            if s["flags"] & SHF_WRITE:
                errs.append(f"writable section {s['name']} in the shared part")
        elif lo < text_size or hi > bss_end:
            errs.append(f"section {s['name']} ({lo:#x}-{hi:#x}) is not inside one part")

    def target(sym, addend, what):
        """the part a relocation's target symbol lies in (by its section), or None after naming why not"""
        name = sym["name"]
        if not name:                            # a section symbol: the symbol at the target, else section+offset
            at = (sym["value"] + addend) & 0xFFFFFFFF
            near = [t for t in E.syms if t["name"] and t["shndx"] == sym["shndx"] and t["type"] in (0, 1, 2)
                    and t["value"] <= at]          # (a PC-relative addend carries the PC's offset: the nearest below)
            if near:
                t = max(near, key=lambda t: t["value"])
                name = t["name"] + (f"+{at - t['value']:#x}" if at != t["value"] else "")
            else:
                sec = E.secs[sym["shndx"]] if sym["shndx"] < len(E.secs) else None
                name = f"{sec['name']}+{at - sec['addr']:#x}" if sec else "?"
        if sym["shndx"] == SHN_UNDEF:
            errs.append(f"{what}: undefined (weak) symbol {name}: its address would be 0")
            return None, name
        if sym["shndx"] >= len(E.secs):         # SHN_ABS, SHN_COMMON ...
            errs.append(f"{what}: absolute symbol {name}")
            return None, name
        s = E.secs[sym["shndx"]]
        return ("code" if s["addr"] + s["size"] <= text_size and s["addr"] < text_size else "data"), name

    def word(a):
        return struct.unpack(">I", img[a:a + 4])[0] if a + 4 <= len(img) else None

    rel = {"code": [], "data": []}

    def add(place, part, val, name, what):
        if val == text_size:
            errs.append(f"{what}: value {val:#x} equals text_size (ambiguous) ({name})")
        elif part == "code" and not val < text_size:
            errs.append(f"{what}: {name} is in the shared part but the value {val:#x} lies outside it")
        elif part == "data" and not text_size < val <= bss_end:
            errs.append(f"{what}: {name} is in the data part but the value {val:#x} lies outside it")
        else:
            rel[part].append(place)

    slots = {}
    for sec, off, typ, sym, addend in E.relas():
        tname = "R_68K_" + R_NAMES.get(typ, str(typ))
        what = f"{tname} at {off:#x} in {sec['name']}"
        if typ == 0:
            continue
        if typ == 1:                                    # R_68K_32: an address
            part, name = target(sym, addend, what)
            if part is None:
                continue
            if off < text_size:
                errs.append(f"{what}: a relocation in the shared part ({name}{'; shared-to-data' if part == 'data' else ''})")
                continue
            if got_lo <= off < got_hi:
                errs.append(f"{what}: a listed relocation inside the table")
                continue
            if off + 4 > len(img):
                continue                                # inside bss: nothing to patch
            if off & 1:
                errs.append(f"{what}: at an odd address")
                continue
            val = word(off)
            if val != (sym["value"] + addend) & 0xFFFFFFFF:
                errs.append(f"{what}: the word holds {val:#x}, not {name} + {addend}")
                continue
            add(off, part, val, name, what)
        elif typ in (4, 5, 6):                          # PC-relative: only inside one part
            part, name = target(sym, addend, what)
            if part is not None and part != part_of(off):
                errs.append(f"{what}: a PC-relative reference from the {part_of(off)} part to {name} in the {part} part"
                            f"{' (shared-to-data)' if part == 'data' else ''}")
        elif typ in (10, 11, 12):                       # a table slot, named by the code that reads it
            part, name = target(sym, addend, what)
            n = {10: 4, 11: 2, 12: 1}[typ]
            if off + n > len(img):
                errs.append(f"{what}: outside the image")
                continue
            disp = int.from_bytes(img[off:off + n], "big", signed=True)
            slot = got_lo + disp
            if not (got_lo <= slot < got_hi) or slot & 3:
                errs.append(f"{what}: names table slot {disp} outside the table")
                continue
            if part is None:
                continue
            val = word(slot)
            if val != (sym["value"] + addend) & 0xFFFFFFFF:
                errs.append(f"{what}: table slot {slot:#x} holds {val:#x}, not {name} + {addend}")
                continue
            if slot in slots and slots[slot][0] != part:
                errs.append(f"{what}: table slot {slot:#x} is named for both parts")
            slots[slot] = (part, name, val)
        elif typ in (13, 14, 15):                       # a call: PC-relative, into code
            part, name = target(sym, addend, what)
            if part is not None and part != "code":
                errs.append(f"{what}: a call to {name} in the data part")
        else:
            errs.append(f"{what}: unsupported relocation")
    for slot in range(got_lo, got_hi, 4):
        if slot not in slots:
            errs.append(f"table slot {slot:#x} (holding {word(slot):#x}) is named by no relocation: cannot classify it")
            continue
        part, name, val = slots[slot]
        add(slot, part, val, name, f"table slot {slot:#x}")
    gp = E.sec(".got.plt")
    if gp and any(img[gp["addr"]:gp["addr"] + gp["size"]]):
        errs.append(".got.plt is not all zero (no lazy binding here)")
    if errs:
        for e in errs[:20]:
            print(f"{elf}: {e}", file=sys.stderr)
        if len(errs) > 20:
            print(f"{elf}: ... and {len(errs) - 20} more", file=sys.stderr)
        os.unlink(out + ".img")
        sys.exit(f"{elf}: cannot be a ZEX2 program ({len(errs)} problems)")

    if len(img) < text_size:
        img += bytes(text_size - len(img))
    data = img[text_size:]
    while len(data) % 4:
        data.append(0)
    mem_size = (bss_end - text_size + 3) & ~3
    mem_size = max(mem_size, len(data))
    rc, rd = sorted(rel["code"]), sorted(rel["data"])
    reloc_off = text_size + len(data)
    hdr = struct.pack(">8I", text_size, len(data), mem_size, len(rc), len(rd), reloc_off, stack, 0)
    body = bytearray(img[:text_size])
    body[8:40] = hdr
    table = b"".join(struct.pack(">I", r) for r in rc + rd)
    open(out, "wb").write(bytes(body) + bytes(data) + table)
    os.unlink(out + ".img")
    print(f"{out}: ZEX2 shared {text_size}B, data {len(data)}B, mem {mem_size}B, "
          f"{len(rc)} relocs to code, {len(rd)} to data, {len(slots)} table slots")


main()
