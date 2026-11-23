#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Set the kernel's default boot options in a kernel file (DESIGN D41 section 13).

usage: tools/kopts.py KERNEL [MMU=ON|MMU=OFF|MMU=AUTO|MMU=STRICT|MMU=D41]

The kernel keeps its defaults in a long after the eight bytes "ZKOPTDEF" (kernel/mmu.c, kopt_default); a bit given at
boot in bootinfo's kopts (zenixboot MMU=ON|OFF) wins over it. A floppy's bootblock passes no options, so this is how a
floppy (and the emulator tests, tools/mmu-test.sh) starts with memory protection on or off. Every build holds MMU=AUTO
(D41 section 19): on where the CPU is known to have an MMU, an LC or EC 68040/68060 left alone. MMU=ON: every CPU whose
MMU passes the boot's probe, the LC and EC parts tried too. MMU=OFF: off unless zenixboot MMU=ON asks. Without a word,
it says what the file holds. The marker must be in the file exactly once. A kopts.py older than 2026-11-17 knows only
bit 4: its MMU=OFF leaves a newer kernel on AUTO, so use the tool of the kernel's own tree.
MMU=STRICT and MMU=D41 (D60 section 12) set their bit (32, 64; each clears the other's) in what the file holds: old servers get no lend; D41's
system processes (the way back for the first hardware runs). MMU=ON, OFF or AUTO clears both."""
import struct
import sys

MARK = b"ZKOPTDEF"
KOPT_MMU_ON = 4
KOPT_MMU_AUTO = 16
KOPT_MMU_STRICT = 32
KOPT_MMU_D41 = 64


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
        if word == "MMU=STRICT":
            v = (v & ~KOPT_MMU_D41) | KOPT_MMU_STRICT
        elif word == "MMU=D41":
            v = (v & ~KOPT_MMU_STRICT) | KOPT_MMU_D41
        else:
            v &= ~(KOPT_MMU_ON | KOPT_MMU_AUTO | KOPT_MMU_STRICT | KOPT_MMU_D41)
            if word == "MMU=ON":
                v |= KOPT_MMU_ON
            elif word == "MMU=AUTO":
                v |= KOPT_MMU_AUTO
            elif word != "MMU=OFF":
                sys.exit(f"kopts: {sys.argv[2]}: say MMU=ON, MMU=OFF, MMU=AUTO, MMU=STRICT or MMU=D41")
        data[at:at + 4] = struct.pack(">I", v)
        open(path, "wb").write(data)
    how = ("on wherever the MMU works" if v & KOPT_MMU_ON else
           "on where the CPU is known to have an MMU" if v & KOPT_MMU_AUTO else "off")
    how += "; MMU=D41" if v & KOPT_MMU_D41 else "; MMU=STRICT" if v & KOPT_MMU_STRICT else ""
    print(f"kopts: {path}: memory protection {how} by default (0x{v:x})")


if __name__ == "__main__":
    main()
