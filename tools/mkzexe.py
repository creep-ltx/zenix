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
"""
import re
import struct
import subprocess
import sys

PFX = "/home/creep/opt/m68k-elf/bin/m68k-elf-"
HDR_MAGIC_OFF = 4
STACK_DEFAULT = 0


def run(*a):
    return subprocess.run(a, check=True, capture_output=True, text=True).stdout


def main():
    elf, out = sys.argv[1:3]
    stack = int(sys.argv[3]) if len(sys.argv) > 3 else STACK_DEFAULT
    subprocess.run([PFX + "objcopy", "-O", "binary", elf, out + ".img"], check=True)
    img = bytearray(open(out + ".img", "rb").read())
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


main()
