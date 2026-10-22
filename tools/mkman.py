#!/usr/bin/env python3
"""Make a manual page for each program from the comment at the top of its source file (user/NAME.c).
A hand-written page in rootfs/usr/man/ wins: it is copied in afterwards and replaces the generated one.
usage: mkman.py OUTDIR program..."""
import os
import re
import sys
import textwrap

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "user")
SKIP = {"boot", "diskd", "init", "badptr", "crash", "spin"}      # internal or test programs
for prog in sys.argv[2:]:
    if prog in SKIP:
        continue
    name = re.sub(r"^(mkfs|fsck)_", r"\1.", prog)
    src = os.path.join(root, prog + ".c")
    if not os.path.exists(src):
        continue
    text = open(src, encoding="latin-1").read()
    m = re.search(r"/\*(.*?)\*/", text, re.S)
    if not m:
        continue
    body = m.group(1)
    lines = [re.sub(r"^\s*\*?\s?", "", l).rstrip() for l in body.strip().splitlines()]
    while lines and not lines[0]:
        lines.pop(0)
    summary = lines[0] if lines else name
    page = [f"{name.upper()}(1)\t\t\tZenix manual\n", "NAME", f"    {name}\n", "SYNOPSIS AND DESCRIPTION"]
    for l in lines:
        page.append("    " + l if l else "")
    page.append("")
    page.append("SEE ALSO\n    sh(1), less(1), man(1)\n")
    open(os.path.join(out, name + ".txt"), "w").write("\n".join(page))
    # first line used by man -k
    open(os.path.join(out, name + ".txt"), "a").close()
