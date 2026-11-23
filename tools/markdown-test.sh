#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Markdown in FS-UAE (roadmap block 3; ports/markdown): the owner's Markdown viewer and editor on the stock A1200.
# /bin/markdown opens a .md file with two headings, bold, italic and code in a line, a list with a nested item, a
# numbered list, a C code block, a quote and 20 quotes inside each other; termtest screen reads the view back (CON_READ) once it is drawn, then
# types e (the text to edit, Sumi's editor), a second read once the Markdown source is on the grid, then Ctrl+W
# quits. Checked: the rendering (the headings with their colour, the emphasis marks gone and the words coloured,
# bullets, the numbers, the code block in its box, the quote's bar), the status and key lines, the source in the
# editor, the settings folder ($HOME/.config/markdown: the keys, where the file was left). The memory with Markdown
# open is printed (free and ps). Every wait is for an event (the grid, the shell's lines), none for a time.
cd "$(dirname "$0")/.." || exit 1
OUT=${ZENIX_OUT:-/tmp/zenix-markdown}; mkdir -p $OUT
BD=${ZENIX_BUILD:-build}
make -s BUILD=$BD all >$OUT/build.log 2>&1 || { echo "FAIL the build"; tail -5 $OUT/build.log; exit 1; }
rm -f $BD/zenix.hdf                     # a fresh root: no settings or places from an earlier run
make -s BUILD=$BD $BD/zenix.hdf >>$OUT/build.log 2>&1 || { echo "FAIL the image"; tail -5 $OUT/build.log; exit 1; }
ZENIX_BUILD=$BD tools/run-test.sh fsuae/a1200-020.fs-uae 400 tools/tests/markdown-test.txt 2>&1 | tr -d '\r' > $OUT/run.log
python3 - "$OUT/run.log" <<'PY'
import re, sys
sys.path.insert(0, "tools")
from screenlog import Log, rows, attrs, cursor
L = Log(sys.argv[1])
check, part = L.check, L.part
s0, s1, f = part("scr0", "scr1"), part("scr1", "file"), part("file", "mdone")
r0, r1, a0 = rows(s0), rows(s1), attrs(s0)
check(L.status("st") == 0, "markdown ran and ended with status 0 (Ctrl+W)", L.status("st"))
check(len(r0) == 32 and r0.get(0, "").startswith("Markdown  t.md"), "the view on the console's 32 rows, the tab's name",
      r0.get(0))
check(r0.get(1) == "   Big Title" and a0.get(1, "")[2:13] == "c" * 11 and r0.get(5) == "  Second" and
      a0.get(5, "")[2:8] == "c" * 6 and re.fullmatch(r"  \u2500{76}", r0.get(6, "")),
      "the headings: the # marks gone, coloured, the second one underlined", (r0.get(1), a0.get(1), r0.get(6)))
check(r0.get(3) == "  Some bold and italic text with code." and a0.get(3, "")[7:11] == "7777" and
      a0.get(3, "")[33:37] == "bbbb" and a0.get(3, "")[:7] == "." * 7,
      "emphasis: the marks gone, the bold word bold, the code coloured, plain text plain", (r0.get(3), a0.get(3)))
check([r0.get(i) for i in (8, 9, 10, 12, 13)] == ["  \u2022 one", "  \u2022 two", "    - nested", "  1. first", "  2. second"]
      and a0.get(8, "")[2] == "6" and a0.get(12, "")[2:4] == "66",
      "lists: bullets (another for the nested item) and numbers, coloured", [r0.get(i) for i in range(8, 14)])
check(re.fullmatch(r"  \u256d\u2500 c \u2500{70}\u256e", r0.get(15, "")) is not None and
      re.fullmatch(r"  \u2502 int x = 1; {63}\u2502", r0.get(16, "")) is not None and
      re.fullmatch(r"  \u2570\u2500{74}\u256f", r0.get(17, "")) is not None and a0.get(16, "")[4:7] == "666",
      "the code block in a box named by its language, its C coloured", [r0.get(i) for i in (15, 16, 17)])
check(r0.get(19) == "  \u2502 a quote", "the quote behind its bar", r0.get(19))
check(r0.get(21, "").replace(" ", "") == "\u2502" * 20 + "deep", "20 quotes inside each other (the renderer's recursion, "
      "upstream's 32 KB stack)", r0.get(21))
check(r0.get(30, "").startswith(" /tmp/t.md  > Big Title") and r0.get(30, "").endswith("all  view") and
      set(a0.get(30, "")) == {"7"} and r0.get(31, "").startswith("Space Page  Tab Link"),
      "the status line (the name, the heading above, view) and the keys' line", (r0.get(30), r0.get(31)))
check(r1.get(1) == "  1 # Big Title" and r1.get(3) == "  3 Some **bold** and *italic* text with `code`." and
      r1.get(14) == " 14 ```c" and r1.get(30, "").endswith("line 1/20  col 1   Latin-1   Markdown") and
      r1.get(31, "").startswith("^S Save  ^E View"), "e: the source to edit (Sumi's editor, the Markdown colours)",
      (r1.get(1), r1.get(3), r1.get(30)))
check(re.search(r"^drwx------ .*markdown$", f, re.M) is not None and
      re.search(r"^-rw-r--r-- .* keys$", f, re.M) is not None and re.search(r"^0 0 /tmp/t\.md$", f, re.M),
      "the settings: $HOME/.config/markdown (this user's only), the keys, where the file was left", f[:600])
L.memory(f, "markdown", "Markdown")
L.waits()
L.done()
PY
