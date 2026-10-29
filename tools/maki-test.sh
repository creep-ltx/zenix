#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Maki in FS-UAE (roadmap block 3; ports/maki): the owner's pager on the stock A1200. /bin/maki opens a file of 300
# lines; termtest screen reads the console's grid back (CON_READ) once Maki has drawn it, then types into the console
# (CON_INPUT): Space (a page on), a search for "line 25" (the first match at the top, every match marked), q. Then a
# pipe whose second line comes 2 s later (the screen fills without a key: the wait wakes for the pipe), a directory
# (its listing, as ls -l), and Maki's output into a pipe (written out as cat would, byte for byte). The settings
# folder ($HOME/.config/Maki) holds the keys file, $HOME/.config/FileColours the colours; the memory with Maki open is
# printed (free and ps). Every wait is for an event (the grid, the shell's lines), none for a time.
cd "$(dirname "$0")/.." || exit 1
OUT=${ZENIX_OUT:-/tmp/zenix-maki}; mkdir -p $OUT
BD=${ZENIX_BUILD:-build}
make -s BUILD=$BD all >$OUT/build.log 2>&1 || { echo "FAIL the build"; tail -5 $OUT/build.log; exit 1; }
rm -f $BD/zenix.hdf                     # a fresh root: no settings from an earlier run
make -s BUILD=$BD $BD/zenix.hdf >>$OUT/build.log 2>&1 || { echo "FAIL the image"; tail -5 $OUT/build.log; exit 1; }
ZENIX_BUILD=$BD tools/run-test.sh fsuae/a1200-020.fs-uae 400 tools/tests/maki-test.txt 2>&1 | tr -d '\r' > $OUT/run.log
python3 - "$OUT/run.log" <<'PY'
import re, sys
sys.path.insert(0, "tools")
from screenlog import Log, rows, attrs, cursor
L = Log(sys.argv[1])
check, part = L.check, L.part
s0, s1, s2, s3 = (part("scr%d" % i, "scr%d" % (i + 1)) for i in range(4))
s4, f = part("scr4", "file"), part("file", "mdone")
r0, r1, r2, r3, r4 = rows(s0), rows(s1), rows(s2), rows(s3), rows(s4)
check([L.status(n) for n in ("st", "st2", "st3")] == [0, 0, 0],
      "maki ran and ended with status 0 three times (q): a file, a pipe, a directory",
      [L.status(n) for n in ("st", "st2", "st3")])
check(len(r0) == 32 and all(r0.get(i) == "line %d" % (i + 1) for i in range(31)),
      "the file on the console's 32 rows: lines 1-31 above the status line", r0)
st0 = r0.get(31, "")
check(st0.startswith(" /tmp/m.txt  line 1-31/300  9%") and st0.endswith("Latin-1  h help") and
      set(attrs(s0).get(31, "")) == {"7"}, "the status line: the name, lines 1-31 of 300, Latin-1, inverse", st0)
check(r1.get(0) == "line 32" and r1.get(30) == "line 62" and r1.get(31, "").startswith(" /tmp/m.txt  line 32-62/300"),
      "Space: a page on (lines 32-62)", (r1.get(0), r1.get(30), r1.get(31)))
a2 = attrs(s2)
check(r2.get(0) == "line 250" and r2.get(30) == "line 280" and r2.get(31, "").startswith(" /tmp/m.txt  line 250-280/300"),
      "/line 25 and Return: the first match (line 250) at the top", (r2.get(0), r2.get(31)))
check(all(a2.get(i, "").startswith("7777777.") for i in range(10)) and 10 not in a2,
      "every match on the screen marked (lines 250-259, the 7 letters of 'line 25'), none else", a2)
check(r3.get(0) == "first" and r3.get(1) == "second" and r3.get(31, "").startswith(" (pipe)  line 1-2/2  (END)"),
      "a pipe: the line written 2 s later shown without a key (the wait woke for the pipe), then its end",
      (r3.get(0), r3.get(1), r3.get(31)))
check(re.search(r"^row \d\d\|[-hspa]{4}[-r][-w][-e][-d] +2592 \d\d-[A-Z][a-z][a-z]-\d\d \d\d:\d\d m\.txt\|$", s4, re.M)
      is not None and r4.get(31, "").startswith(" /tmp  line 1-"),
      "a directory: its listing as ls -l (AmigaOS's bits, the size, the date, the name)", s4[:900])
check(re.search(r"^300$", f, re.M) is not None and re.search(r"^same=yes$", L.text, re.M) is not None,
      "output into a pipe: the file written out as cat would, byte for byte", f[:200])
check(re.search(r"^drwx------ .*Maki$", f, re.M) is not None and re.search(r"^-rw-r--r-- .* Maki\.keys$", f, re.M)
      is not None and re.search(r"^-rw-r--r-- .*\.config/FileColours$", f, re.M) is not None,
      "the settings: $HOME/.config/Maki (this user's only) with the keys, $HOME/.config/FileColours", f[:600])
L.memory(f, "maki", "Maki")
L.waits()
L.done()
PY
