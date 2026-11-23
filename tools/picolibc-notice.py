#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The picolibc licence notice cut to the files Zenix vendors (libc/picolibc).

usage: picolibc-notice.py PICOLIBC_DIR OUT

COPYING.picolibc is Debian's copyright file for all of picolibc: 400 "Files:" stanzas, most for other
machines' headers, tests and scripts that are not in libc/picolibc. A binary carries the notices of the code
that is in it, so this keeps the header, every stanza with a file that is in the vendored tree (or a pattern
matching one), and the licence texts those stanzas name. Used by the demo floppy, where every byte is packed.
"""
import glob
import os
import re
import sys


def main() -> None:
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    root, out = sys.argv[1:3]
    stanzas = open(os.path.join(root, "COPYING.picolibc"), encoding="utf-8").read().strip("\n").split("\n\n")
    kept, lic_used, lic_text = [], set(), {}
    for s in stanzas:
        if s.startswith("License:"):
            lic_text[s.split("\n")[0][8:].strip()] = s
        elif s.startswith("Files:"):
            lines = s.split("\n")
            paths = [lines[0][6:].strip()]
            for l in lines[1:]:
                if not l.startswith(" "):
                    break
                paths.append(l.strip())
            if any(p == "*" or glob.glob(os.path.join(root, p)) for p in paths if p):
                kept.append(s)
                m = re.search(r"^License:(.*)$", s, re.M)
                lic_used.update(re.split(r"\s+(?:and|or)\s+", m.group(1).strip()) if m else [])
        else:
            kept.append(s)
            m = re.search(r"^License:(.*)$", s, re.M)
            lic_used.update(re.split(r"\s+(?:and|or)\s+", m.group(1).strip()) if m else [])
    missing = [n for n in lic_used if n not in lic_text]
    if missing:
        sys.exit(f"picolibc-notice.py: licence text not found: {missing}")
    texts = [lic_text[n] for n in sorted(lic_used)]
    open(out, "w", encoding="utf-8").write("\n\n".join(kept + texts) + "\n")


if __name__ == "__main__":
    main()
