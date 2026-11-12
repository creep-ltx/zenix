#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Set the kernel's default boot options in a kernel file (DESIGN D41 section 13).

usage: tools/kopts.py KERNEL [MMU=ON|MMU=OFF]

The kernel keeps its defaults in a long after the eight bytes "ZKOPTDEF" (kernel/mmu.c, kopt_default); a bit given at
boot in bootinfo's kopts (zenixboot MMU=ON|OFF) wins over it. A floppy's bootblock passes no options, so this is how a
floppy (and the emulator tests, tools/mmu-test.sh) starts with memory protection on. Without a word, it says what the
file holds. The marker must be in the file exactly once."""
import struct
import sys

MARK = b"ZKOPTDEF"
KOPT_MMU_ON = 4


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    path = sys.argv[1]
    data = bytearray(open(path, "rb").read())
    at = data.find(MARK)
    if at < 0 or data.find(MARK, at + 1) >= 0:
        sys.exit(f"kopts: {path}: the marker {MARK.decode()} is not there exactly once")
    at += len(MARK)
    (v,) = struct.unpack(">I", data[at:at + 4])
    if len(sys.argv) == 3:
        word = sys.argv[2].upper()
        if word == "MMU=ON":
            v |= KOPT_MMU_ON
        elif word == "MMU=OFF":
            v &= ~KOPT_MMU_ON
        else:
            sys.exit(f"kopts: {sys.argv[2]}: say MMU=ON or MMU=OFF")
        data[at:at + 4] = struct.pack(">I", v)
        open(path, "wb").write(data)
    print(f"kopts: {path}: memory protection {'on' if v & KOPT_MMU_ON else 'off'} by default (0x{v:x})")


if __name__ == "__main__":
    main()
