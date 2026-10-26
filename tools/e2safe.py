#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# What a power cut may leave on a Zenix ext2 volume (audit 2026-10-26 F5): ext2 has no journal, so a cut inside a
# request that writes several blocks leaves a half-done change. Zenix orders those writes so that only leaks can be
# left: a link count one too high, an inode or blocks marked in use that nothing names or holds, an unnamed inode or
# directory, free counts off by a few. Every leak is space lost until fsck.ext2 (or the PC's e2fsck) gives it back;
# none can lose or mix up data. This reads the output of `e2fsck -fn` and prints every finding that is NOT such a
# leak, one a line: a block or inode in use but marked free, a count below its names, a name of a deleted inode, two
# files sharing a block, anything this script does not know. Nothing printed: at worst leaks.
# usage: tools/e2safe.py E2FSCK-LOG          (exit 0: at worst leaks, 1: something else)
import re
import sys

LEAK = [r"^Pass \d", r"^e2fsck \d", r"^$", r"^Fix\? no$", r"^Clear\? no$", r"^Connect to /lost\+found\? no$",
        r"^Unattached (zero-length )?inode \d+", r"^Unconnected directory inode \d+",
        r"^'\.\.' in .* \(\d+\) is .* \(\d+\), should be <The NULL inode> \(0\)\.",
        r"^Free (blocks|inodes) count wrong", r"^Directories count wrong for group",
        r"^Padding at end of (block|inode) bitmap is not set",
        r"^Inode \d+, i_(size|blocks) is \d+, should be \d+\.",
        r"^Inode \d+ ref count is \d+, should be \d+\.", r"^(Block|Inode) bitmap differences:",
        r"\*{10} WARNING: Filesystem still has errors \*{10}$", r"^\S+: \d+/\d+ files"]


def problems(text):
    bad = []
    lines = [l.strip() for l in text.split("\n")]
    # an unnamed directory's ".." still counts at its parent: the parent's count may be that many below e2fsck's
    dotdot = {}
    for m in re.finditer(r"'\.\.' in .* \((\d+)\) is .* \((\d+)\), should be <The NULL inode>", text):
        dotdot[int(m.group(2))] = dotdot.get(int(m.group(2)), 0) + 1
    for l in lines:
        if not any(re.search(p, l) for p in LEAK):
            bad.append("unknown: " + l)
            continue
        m = re.match(r"^Inode (\d+) ref count is (\d+), should be (\d+)\.", l)
        if m and int(m.group(2)) < int(m.group(3)) - dotdot.get(int(m.group(1)), 0):
            bad.append("a count below its names: " + l)
        m = re.match(r"^(Block|Inode) bitmap differences:(.*)", l)
        if m and "+" in m.group(2):
            bad.append("in use but marked free: " + l)
        m = re.match(r"^Inode \d+, i_size is (\d+), should be (\d+)\.", l)
        if m and int(m.group(1)) > int(m.group(2)):
            bad.append("a size past the file's blocks: " + l)
    return bad


if __name__ == "__main__":
    found = problems(open(sys.argv[1], errors="replace").read())
    for b in found:
        print(b)
    sys.exit(1 if found else 0)
