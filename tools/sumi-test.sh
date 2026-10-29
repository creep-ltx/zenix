#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Sumi in FS-UAE (roadmap block 3; ports/sumi): the owner's text editor on the stock A1200. /bin/sumi opens a file
# of two lines; termtest screen reads the console's grid back (CON_READ) once Sumi has drawn it, then types into the
# console (CON_INPUT) a Tab, a line of text with a Latin-1 letter, Shift+Tab as raw key codes (the keyboard's ESC [ Z:
# Sumi unindents the line, the tab is gone again), Enter and Ctrl+S; a second read once "saved s.txt" is
# on the grid, then Ctrl+W quits. The file is compared byte for byte, its mode kept, the settings folder
# ($HOME/.config/Sumi) holds the keys file and where the file was left, and a second start puts the cursor back
# there; a UTF-8 file (DESIGN D28) shown, a letter typed as UTF-8, saved byte for byte. The memory with Sumi open is printed (free and ps). Every wait is for an event (the grid, the shell's
# lines), none for a time.
cd "$(dirname "$0")/.." || exit 1
OUT=${ZENIX_OUT:-/tmp/zenix-sumi}; mkdir -p $OUT
BD=${ZENIX_BUILD:-build}
make -s BUILD=$BD all >$OUT/build.log 2>&1 || { echo "FAIL the build"; tail -5 $OUT/build.log; exit 1; }
rm -f $BD/zenix.hdf                     # a fresh root: no settings or places from an earlier run
make -s BUILD=$BD $BD/zenix.hdf >>$OUT/build.log 2>&1 || { echo "FAIL the image"; tail -5 $OUT/build.log; exit 1; }
ZENIX_BUILD=$BD tools/run-test.sh fsuae/a1200-020.fs-uae 400 tools/tests/sumi-test.txt 2>&1 | tr -d '\r' > $OUT/run.log
python3 - "$OUT/run.log" <<'PY'
import re, sys
sys.path.insert(0, "tools")
from screenlog import Log, rows, attrs, cursor
L = Log(sys.argv[1])
log, check, part = L.text, L.check, L.part
s0, s1, s2, f = part("scr0", "scr1"), part("scr1", "scr2"), part("scr2", "scr3"), part("file", "sdone")
s3, s4 = part("scr3", "scr4"), part("scr4", "file")
r0, r1, r2 = rows(s0), rows(s1), rows(s2)
check(re.search(r"st=0$", log, re.M) is not None and re.search(r"st2=0$", log, re.M) is not None,
      "sumi ran and ended with status 0, twice (Ctrl+W)", re.findall(r"st2?=\S*", log))
check(len(r0) == 32 and r0.get(0, "").startswith("Sumi  s.txt") and r0.get(1) == "  1 first line" and
      r0.get(2) == "  2 second" and r0.get(3) == "  ~" and r0.get(29) == "  ~",
      "the file on the console's 32 rows: the tab, the lines numbered, ~ below the end", r0)
st0 = r0.get(30, "")
check(st0.startswith(" /tmp/s.txt") and st0.endswith("line 1/2  col 1   Latin-1   text"),
      "the status line: the name, line 1/2, column 1, Latin-1, plain text", st0)
check(cursor(s0) == (1, 4, True), "the cursor on the first character of line 1, shown", cursor(s0))
a0 = attrs(s0)
check(a0.get(2, "")[:4] == "8888" and a0.get(1, "")[:4] == "88.8" and a0.get(2, "")[4:14] == "." * 10 and
      set(a0.get(30, "")) == {"7"}, "colours: the line numbers dim (8) but the cursor line's, the text plain, the "
      "status line inverse", (a0.get(1), a0.get(2), a0.get(30)))
check(r1.get(1) == "  1 Hello Zenix \xe5" and r1.get(2) == "  2 first line" and r1.get(3) == "  3 second" and
      r1.get(31) == "saved s.txt" and r1.get(30, "").endswith("line 2/3  col 1   Latin-1   text") and
      "[changed]" not in r1.get(30, ""), "Tab, typed text, a Latin-1 letter, Shift+Tab (unindent), Enter and Ctrl+S: shown and saved", r1)
check(cursor(s1) == (2, 4, True), "the cursor after Enter: line 2, column 1", cursor(s1))
fu, fs = (re.split(r"^[0-9a-f]{8}$", f, maxsplit=1, flags=re.M) + [""])[:2]   # hexdump of u.txt, then of s.txt
hx = re.findall(r"^[0-9a-f]{8}  ((?:[0-9a-f]{2} {1,2})+)", fs, re.M)
data = bytes(int(b, 16) for line in hx for b in line.split())
check(data == b"Hello Zenix \xe5\nfirst line\nsecond\n", "the file on disk, byte for byte (Latin-1 kept)", data)
check(re.search(r"^-rw-r----- 1 root root 32 .* /tmp/s.txt$", f, re.M) is not None,
      "its mode kept (640) through the save's new copy", f[:400])
check(re.search(r"^-rw-r--r-- .* Sumi\.keys$", f, re.M) is not None and
      re.search(r"^-rw-r--r-- .* Sumi\.places$", f, re.M) is not None and re.search(r"^1 0 /tmp/s\.txt$", f, re.M),
      "the settings folder $HOME/.config/Sumi: the keys written at the first start, where the file was left", f)
check(re.search(r"^drwx------ .*Sumi$", f, re.M) is not None, "the settings folder is this user's only (700)", f[:600])
check(r2.get(1) == "  1 Hello Zenix \xe5" and r2.get(30, "").endswith("line 2/3  col 1   Latin-1   text") and
      cursor(s2) == (2, 4, True), "a second start: the cursor back where it was left", (r2.get(30), cursor(s2)))
# DESIGN D28: a UTF-8 file shown in UTF-8 on the console, a letter typed as UTF-8 (the keyboard's), saved as UTF-8
r3, r4 = rows(s3), rows(s4)
check(r3.get(1) == "  1 b\u00e5r \u2500" and "UTF-8" in r3.get(30, ""),
      "a UTF-8 file (a Swedish letter, a box line) shown as it is, the status line says UTF-8", (r3.get(1), r3.get(30)))
check(r4.get(1) == "  1 \u00f6b\u00e5r \u2500" and r4.get(31) == "saved u.txt", "a letter typed as UTF-8 and saved", r4)
hu = re.search(r"^00000000  ((?:[0-9a-f]{2} {1,2})+)", fu, re.M)
check(hu is not None and hu.group(1).split() == "c3 b6 62 c3 a5 72 20 e2 94 80 0a".split(),
      "the UTF-8 file on disk, byte for byte", hu and hu.group(1))
check(L.status("st3") == 0, "sumi ended with status 0 (the UTF-8 file)", L.status("st3"))
L.memory(f, "sumi", "Sumi")
L.waits()
L.done()
PY
