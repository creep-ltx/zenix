#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The text-UI library in FS-UAE (roadmap block 3; ports/tui): /tests/tuitest on the stock A1200 opens the console
# through the old tui_* names, draws its widgets (a title bar, a box with rounded corners, Latin-1 and UTF-8 text in
# UTF-8 since DESIGN D28, colours, attributes, a wide character, the full block, the last cell, the cursor) and reads the
# console's grid back (CON_READ) to check them; only changed cells are sent and TuiRedraw sends all; then the keys:
# the single ones this script types after each prompt (a letter, a Latin-1 key, ^C as a key, a lone Esc) and the
# escape sequences tuitest puts into the console's input at once (an arrow, F1, Ctrl+Right, Shift+Tab), a timed wait, TuiPoll, suspending for a command and TuiKeyAfter's key, resuming (drawn again), and the
# terminal given back (cooked, the cursor shown, the screen cleared).
cd "$(dirname "$0")/.." || exit 1
OUT=${ZENIX_OUT:-/tmp/zenix-tui}; mkdir -p $OUT
BD=${ZENIX_BUILD:-build}
make -s BUILD=$BD all >$OUT/build.log 2>&1 || { echo "FAIL the build"; tail -5 $OUT/build.log; exit 1; }
ZENIX_BUILD=$BD tools/run-test.sh fsuae/a1200-020.fs-uae 300 tools/tests/tui-test.txt 2>&1 | tr -d '\r' > $OUT/run.log
python3 - "$OUT/run.log" <<'PY'
import re, sys
log = open(sys.argv[1], errors="replace").read()
bad = 0
def check(ok, what, detail=""):
    global bad
    print(("PASS " if ok else "FAIL ") + what + ("" if ok else ": " + str(detail)))
    bad += not ok
r = log.split("==tdone")[0]
r = r[r.rfind("tuitest: the results"):] if "tuitest: the results" in r else r
fails = re.findall(r"^FAIL .*$", r, re.M)
oks = re.findall(r"^ok .*$", r, re.M)
m = re.search(r"^tuitest: (\d+) checks, (\d+) failed$", r, re.M)
check(m is not None and m.group(2) == "0" and not fails and len(oks) == int(m.group(1)),
      "tuitest: every check (%d)" % len(oks), fails or (m and m.group(0)) or r[-600:])
check(re.search(r"^st=0$", r, re.M) is not None, "tuitest's status 0", r[-200:])
ev = re.search(r"^events: (.*)$", r, re.M)
check(ev is not None and ev.group(1).split() == "a UP F1 C-RIGHT U+00E5 C-c ESC S-TAB q".split(),
      "the keys as events", ev and ev.group(1))
check("tuitest: a command's output" in log, "a command's output while suspended")
mem = re.search(r"^memory: (\d+) bytes while open, (\d+) after$", r, re.M)
check(mem is not None and int(mem.group(2)) < int(mem.group(1)), "its memory: less after closing than while open",
      mem and mem.group(0))
if mem: print("     (memory: %s bytes while open, %s after)" % (mem.group(1), mem.group(2)))
check("serial-tap: wait timed out" not in log and "the run ended during a wait" not in log,
      "every prompt came", re.findall(r"FAIL: serial-tap.*", log))
sys.exit(1 if bad else 0)
PY
