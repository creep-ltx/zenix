#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The build's program links as zld command lines (tools/zld-host-test.sh, DESIGN D52 section 5).

Reads `make -n -B` output on standard input and prints, for every program the build links with link/user.ld or
link/pure.ld and turns into a .zx/.pzx with tools/mkzexe.py, one line:
    OUT.zx<TAB>zld argument<TAB>zld argument ...
with the same inputs in the same order, the same --defsym and --wrap options and the same stack size, so that
`zld -o NEW ARGS` must give OUT byte for byte. Kernel links (--delta, link/kernel.ld) are not programs: left out."""
import shlex
import sys

txt = sys.stdin.read().replace("\\\n", " ")
links = {}
progs = []
for line in txt.splitlines():
    try:
        a = shlex.split(line)
    except ValueError:
        continue
    if not a:
        continue
    if a[0].endswith("gcc") and "-T" in a and a[a.index("-T") + 1] in ("link/user.ld", "link/pure.ld") \
            and "-o" in a:
        out = a[a.index("-o") + 1]
        args = []
        skip = False
        for i, x in enumerate(a[1:], 1):
            if skip:
                skip = False
                continue
            if x in ("-o", "-T"):
                skip = True
                continue
            if x.startswith("-Wl,"):
                for w in x[4:].split(","):
                    if w.startswith(("--defsym=", "--wrap=")):
                        args.append(w)
                    elif w not in ("--build-id=none", "-q", "--gc-sections"):
                        sys.exit(f"{out}: linker option {w} has no zld counterpart")
                continue
            if x.startswith("-"):
                continue          # compiler flags (gcc passes none of them to ld here)
            args.append(x)
        links[out] = args
    elif len(a) >= 3 and a[0] == "tools/mkzexe.py" and "--delta" not in a:
        progs.append((a[1], a[2], a[3] if len(a) > 3 else "0"))
for elf, out, stack in progs:
    if elf not in links:
        sys.exit(f"{out}: no link command found for {elf}")
    print("\t".join([out] + links[elf] + ["-s", stack]))
