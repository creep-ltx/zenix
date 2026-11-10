#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# ZINE's second offset table (docs/d29-zine.md 16.12): the function names user/zine/lvo.c (the RKRM's V37 tables)
# does not have, from the NDK 3.2's .fd files (names and numbers only; owner answer 4, 2026-10-30): exec, dos,
# graphics and intuition past their V37 ends, and the libraries ZINE has of its own beyond the four (utility, the
# math libraries, diskfont: 16.13). So `zine` names a call such as graphics -888 (SetChipRev) when it stops a program there.
#   tools/zine-ndk-names.py FDDIR OUT.c      write user/zine/lvo40.c
#   tools/zine-ndk-names.py check FDDIR      the committed lvo40.c is what FDDIR makes
# FDDIR: the NDK 3.2's FD folder (ZENIX_NDK_FD in the tests; the owner's private notes say where).
import os, re, sys

LIBS = [("exec", "exec.library"), ("dos", "dos.library"), ("graphics", "graphics.library"),
        ("intuition", "intuition.library"), ("utility", "utility.library"), ("mathffp", "mathffp.library"),
        ("mathtrans", "mathtrans.library"), ("mathieeedoubbas", "mathieeedoubbas.library"),
        ("mathieeedoubtrans", "mathieeedoubtrans.library"), ("mathieeesingbas", "mathieeesingbas.library"),
        ("mathieeesingtrans", "mathieeesingtrans.library"), ("diskfont", "diskfont.library")]
LAST = 1200                         # offsets past V40 (3.1.4/3.2 additions) are left out: ZINE reports V40
# the first offset lvo.c does not name (its V37 tables end there): the four libraries' names start after it
FROM = {"exec": 786, "dos": 990, "graphics": 828, "intuition": 732}


def read_fd(path):
    bias, out = 30, {}
    for line in open(path, encoding="latin-1"):
        line = line.strip()
        if line.startswith("##bias"):
            bias = int(line.split()[1])
        elif line.startswith("##") or line.startswith("*") or not line:
            continue
        else:
            m = re.match(r"([A-Za-z_][A-Za-z0-9_]*)\(", line)
            if m:
                if bias <= LAST:
                    out[bias] = m.group(1)
                bias += 6
    return out


def make(fd):
    o = ["/* SPDX-License-Identifier: GPL-2.0-only */",
         "/* Made by tools/zine-ndk-names.py from the NDK 3.2's .fd files: function names and offsets only (owner answer",
         " * 4). Do not edit: run the tool. Asked by `zine` when lvo.c (the RKRM's V37 tables) has no name. */",
         '#include "lvo.h"', ""]
    for short, lib in LIBS:
        t = read_fd(os.path.join(fd, short + "_lib.fd"))
        hi, lo = max(t), FROM.get(short, 30)
        names = "".join((t.get(off, "") + "\\0") for off in range(lo, hi + 6, 6))
        o.append(f"static const char n_{short}[] =")
        line = ""
        for part in names.split("\\0")[:-1]:
            if len(line) + len(part) + 2 > 104:
                o.append(f'    "{line}"')
                line = ""
            line += part + "\\0"
        o.append(f'    "{line}"')
        o[-1] += ";"
    o.append("")
    o.append("static const struct zl_lib t40[] = {")
    for short, lib in LIBS:
        t = read_fd(os.path.join(fd, short + "_lib.fd"))
        lo = FROM.get(short, 30)
        o.append(f'    {{ "{lib}", {lo}, {(max(t) - lo) // 6 + 1}, n_{short} }},')
    o.append("};")
    o.append("")
    o.append("const char *zl_name40(const char *lib, unsigned offset)")
    o.append("{")
    o.append("    for (unsigned i = 0; i < sizeof t40 / sizeof t40[0]; i++) {")
    o.append("        const struct zl_lib *l = &t40[i];")
    o.append("        const char *a = l->name, *b = lib;")
    o.append("        while (*a && *a == *b)")
    o.append("            a++, b++;")
    o.append("        if (*a || *b)")
    o.append("            continue;")
    o.append("        if (offset < l->first || (offset - l->first) % 6 || (offset - l->first) / 6 >= l->n)")
    o.append("            return 0;")
    o.append("        const char *p = l->names;")
    o.append("        for (unsigned k = (offset - l->first) / 6; k; k--)")
    o.append("            while (*p++)")
    o.append("                ;")
    o.append("        return *p ? p : 0;")
    o.append("    }")
    o.append("    return 0;")
    o.append("}")
    return "\n".join(o) + "\n"


if __name__ == "__main__":
    a = sys.argv[1:]
    if a[:1] == ["check"]:
        want = make(a[1])
        have = open(os.path.join(os.path.dirname(__file__), "..", "user", "zine", "lvo40.c")).read()
        if want != have:
            sys.exit("zine-ndk-names: user/zine/lvo40.c is not what the NDK's .fd files make: run the tool")
        print("zine-ndk-names: lvo40.c matches the NDK's .fd files")
    else:
        open(a[1], "w").write(make(a[0]))
