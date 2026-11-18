#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Ahiru in FS-UAE (roadmap block 3; ports/ahiru): the owner's file manager on the stock A1200. /bin/ahiru opens
# /tmp/a (a folder sub with keep.txt and a soft link, one.txt of mode 640, two.txt); termtest screen reads the
# console's grid back (CON_READ) at each step and then types the next keys into the console (CON_INPUT): j yy k l
# (one.txt yanked, into sub), pp (a copy), h jj dd kk l pp (two.txt cut and pasted into sub: a move), h j dD and y at
# the question (one.txt deleted), l jj E (Sumi on sub/one.txt: E opens /bin/sumi), Ctrl+W (Sumi ends, Ahiru is back),
# q; then a second start in /tmp/z, which holds a small lha archive (written with printf, packed -lh5-): l (into it,
# as into a folder), yy h pp (its member copied out, unpacked), q. Checked on the screen: the three columns (the parent, the folder, the preview of what the cursor is on), the
# title (the path and the name under the cursor), the sizes, the status line (AmigaOS's protection letters, the free
# space, the place in the list), the colours of directories, the highlight and links, the yank and cut tags, the
# question, Sumi's screen, the archive's card and its member; and on the disk: the copy byte for byte with its mode kept, the move, the delete, the link
# left as it was, the member unpacked with its date; the settings folder ($HOME/.config/Ahiru: the keys, the theme, the config, the places, mode 700) and
# $HOME/.config/FileColours. A third start: a zip's member folder named ".." pasted is refused, nothing written
# outside the target (FA-11). The memory with Ahiru open is printed (free and ps). Every wait is for an event (the
# grid, the shell's lines), none for a time.
cd "$(dirname "$0")/.." || exit 1
OUT=${ZENIX_OUT:-/tmp/zenix-ahiru}; mkdir -p $OUT
BD=${ZENIX_BUILD:-build}
make -s BUILD=$BD all >$OUT/build.log 2>&1 || { echo "FAIL the build"; tail -5 $OUT/build.log; exit 1; }
rm -f $BD/zenix.hdf                     # a fresh root: no settings or places from an earlier run
make -s BUILD=$BD $BD/zenix.hdf >>$OUT/build.log 2>&1 || { echo "FAIL the image"; tail -5 $OUT/build.log; exit 1; }
ZENIX_BUILD=$BD tools/run-test.sh fsuae/a1200-020.fs-uae 500 tools/tests/ahiru-test.txt 2>&1 | tr -d '\r' > $OUT/run.log
python3 - "$OUT/run.log" <<'PY'
import re, sys
sys.path.insert(0, "tools")
from screenlog import Log, rows, attrs, cursor
L = Log(sys.argv[1])
check, part = L.check, L.part
s = [part("scr%d" % i, "scr%d" % (i + 1)) for i in range(11)] + [part("scr11", "file")]
f = part("file", "adone")
r = [rows(x) for x in s]
a = [attrs(x) for x in s]
mid = lambda rr, i: rr.get(i, "")[11:39].rstrip()     # the middle column (the folder): its mark cell at 10, the name from 11
left = lambda rr, i: rr.get(i, "")[1:9].rstrip()      # the parent column
right = lambda rr, i: rr.get(i, "")[41:].rstrip()     # the preview (its mark cell at 40)
check(L.status("st") == 0, "ahiru ran and ended with status 0 (q)", L.status("st"))
r0, a0 = r[0], a[0]
check(len(r0) == 32 and r0.get(0) == " /tmp/a/sub" and mid(r0, 1) == "sub" and
      re.fullmatch(r"one\.txt +12", mid(r0, 2)) is not None and re.fullmatch(r"two\.txt +12", mid(r0, 3)) is not None,
      "/tmp/a in the middle column on the console's 32 rows: sub first, the files with their sizes; the title the "
      "path and the name under the cursor", [r0.get(i) for i in range(4)])
check(left(r0, 1) == "a" and re.fullmatch(r"keep\.txt +5", right(r0, 1)) is not None and
      re.fullmatch(r"link +7", right(r0, 2)) is not None,
      "the parent column (/tmp, the cursor on a) and the preview of sub (keep.txt, the soft link)",
      (r0.get(1), r0.get(2)))
st0 = r0.get(31, "")
check(st0.startswith(" ----rwed  ") and st0.endswith("free  1/3"),
      "the status line: AmigaOS's protection letters (a folder this user may use), the free space, 1 of 3", st0)
check(a0.get(0, "")[1:11] == "c" * 7 + "fff" and set(a0.get(2, "")[40:]) == {"e"} and a0.get(1, "")[10:39] == "0" * 29,
      "colours: the path as a directory (12), the name under the cursor bright (15), the soft link (14), the cursor "
      "row in reverse", (a0.get(0), a0.get(1), a0.get(2)))
check(r[1].get(0) == " /tmp/a/sub/keep.txt" and "yank 1" in r[1].get(31, ""),
      "j yy k l: one.txt yanked (the tag on the status line), into sub", (r[1].get(0), r[1].get(31)))
check(re.fullmatch(r"one\.txt +12", mid(r[2], 3)) is not None and r[2].get(31, "").endswith("free  1/3"),
      "pp: the copy of one.txt in sub", (r[2].get(3), r[2].get(31)))
check("cut 1" in r[3].get(31, "") and r[3].get(0) == " /tmp/a/sub/keep.txt",
      "h jj dd kk l: two.txt cut, back in sub", (r[3].get(0), r[3].get(31)))
check(re.fullmatch(r"two\.txt +12", mid(r[4], 4)) is not None and left(r[4], 3) == "" and
      left(r[4], 2) == "one.txt" and r[4].get(31, "").endswith("free  1/4") and "cut" not in r[4].get(31, ""),
      "pp: two.txt moved into sub (and gone from /tmp/a, the parent column)", [r[4].get(i) for i in (2, 3, 4, 31)])
check(r[5].get(31, "").strip() == 'delete "one.txt"? (y)es (n)o' and r[5].get(0) == " /tmp/a/one.txt",
      "h j dD: the question, on one.txt", (r[5].get(0), r[5].get(31)))
check(mid(r[6], 1) == "sub" and mid(r[6], 2) == "" and r[6].get(31, "").endswith("free  1/1"),
      "y: one.txt deleted, sub alone in /tmp/a", [r[6].get(i) for i in (1, 2, 31)])
check(r[7].get(0, "").startswith("Sumi  one.txt") and r[7].get(1) == "  1 hello ahiru" and
      r[7].get(30, "").startswith(" /tmp/a/sub/one.txt"),
      "l jj E: Sumi (/bin/sumi) on sub/one.txt, in the terminal", [r[7].get(i) for i in (0, 1, 30)])
check(r[8].get(0) == " /tmp/a/sub/one.txt" and r[8].get(31, "").endswith("free  3/4") and
      re.fullmatch(r"keep\.txt +5", mid(r[8], 1)) is not None,
      "Ctrl+W: Sumi ended, Ahiru drawn again where it was", (r[8].get(0), r[8].get(31)))
check(L.status("st2") == 0, "a second start (/tmp/z) ended with status 0 (q)", L.status("st2"))
check(r[9].get(0) == " /tmp/z/a.lha" and right(r[9], 1) == "LhA archive" and right(r[9], 2) == "1 members, 68 unpacked",
      "an lha archive: its preview card (the kind, the members, the size unpacked)", (r[9].get(0), r[9].get(1), r[9].get(2)))
check(r[10].get(0) == " /tmp/z/a.lha/in.txt" and re.fullmatch(r"in\.txt +68", mid(r[10], 1)) is not None and
      right(r[10], 1) == "in the archive",
      "l: inside the archive as in a folder (the core's arcfs: its member, its size, the card)", (r[10].get(0), r[10].get(1)))
check(re.fullmatch(r"in\.txt +68", mid(r[11], 2)) is not None and r[11].get(31, "").endswith("free  1/2"),
      "yy h pp: the member copied out (unpacked: lh5) into /tmp/z", (r[11].get(2), r[11].get(31)))
check(re.search(r"^-rw-r--r-- 1 root root 68 Oct 30  2026 in\.txt$", f, re.M) is not None and
      re.search(r"^(inside the archive, packed by lha\n){2}", f, re.M) is not None,
      "the member on the disk byte for byte, with the date the archive gave it (SetFileDate)", f[:900])
check(re.search(r"^/tmp/a:\ndrwx\S+ +\d+ root root +\d+ .* sub\n\n", f, re.M) is not None,
      "on the disk: /tmp/a holds only sub (one.txt deleted, two.txt moved)", f[:300])
check(re.search(r"^-rw-r----- 1 root root 12 .* one\.txt$", f, re.M) is not None and
      re.search(r"^-rw-r--r-- 1 root root 12 .* two\.txt$", f, re.M) is not None and
      re.search(r"^-rw-r--r-- 1 root root +5 .* keep\.txt$", f, re.M) is not None and
      re.search(r"^lrwxrwxrwx 1 root root +7 .* link -> one\.txt$", f, re.M) is not None,
      "sub: the copy with its mode kept (640), the moved file, keep.txt and the link untouched", f[:600])
check(re.search(r"^hello ahiru\nsecond file\nkept\n", f, re.M) is not None and re.search(r"^one\.txt$", f, re.M),
      "the copied and moved files byte for byte, the link's target", f[:700])
check(re.search(r"^drwx------ .*\.config/Ahiru$", f, re.M) is not None and
      all(re.search(r"^-rw-r--r-- .* Ahiru\.%s$" % n, f, re.M) for n in ("config", "keys", "places", "theme")) and
      re.search(r"^-rw-r--r-- .* \S*\.config/FileColours$", f, re.M) is not None,
      "the settings: $HOME/.config/Ahiru (this user's only) with the config, the keys, the places and the theme; "
      "$HOME/.config/FileColours", f[:1200])
# FA-11 (B3-6): a zip whose member is "../esc.txt" (a folder named ".." at its top): picked and pasted into /tmp/y/t,
# it would land in /tmp/y; the paste is refused with a message and nothing is written
y = [rows(part("y%d" % i, "y%d" % (i + 1))) for i in range(2)] + [rows(part("y2", "yfile"))]
yf = part("yfile", "ydone")
check(L.status("st3") == 0, "a third start (/tmp/y/t) ended with status 0 (q)", L.status("st3"))
check(y[1].get(0) == " /tmp/y/t/d.zip/..", "l: inside the zip, its member folder named '..' under the cursor", y[1].get(0))
check(any("names no file here - nothing pasted" in v for v in y[2].values()),
      "yy h pp: the paste of a member named '..' is refused with a message", y[2])
check(re.search(r"^/tmp/y:$", yf, re.M) is not None and re.search(r"^/tmp/y/t:$", yf, re.M) is not None and "esc.txt" not in yf,
      "nothing written outside the target: no esc.txt in /tmp/y (or in the target)", yf[:300])
L.memory(f, "ahiru", "Ahiru")
L.waits()
L.done()
PY
