#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# make in FS-UAE (DESIGN D52 section 4, ports/pdpmake): /bin/make on the minimum machine (an A1200 with 4 MB of fast
# RAM). rootfs/tests/make/run.sh runs make over the Makefiles beside it (dependencies and timestamps with a rebuild
# skipped because the target is newer, suffix rules, macros, -n -k -i -q -s -t -e, a failing command and its exit
# status, errors, include, a recursive make, a chain of 100 targets and of 100 macros) and its output must be the
# PC's, line for line (tools/make-host-test.sh builds the same pinned source for the PC and runs the same script:
# the reference). Then the stack probe build (/tests/make/makeprobe) over the two deep chains, a bad option's status,
# and the memory with a make running (emulator figures).
cd "$(dirname "$0")/.." || exit 1
OUT=${ZENIX_OUT:-/tmp/zenix-make}; mkdir -p "$OUT"
BD=${ZENIX_BUILD:-build}
MAKE_EXPECTED="$OUT/expected.txt" tools/make-host-test.sh >"$OUT/host.log" 2>&1 ||
    { echo "FAIL the PC's reference run (tools/make-host-test.sh)"; cat "$OUT/host.log"; exit 1; }
make -s BUILD=$BD all >"$OUT/build.log" 2>&1 || { echo "FAIL the build"; tail -5 "$OUT/build.log"; exit 1; }
ZENIX_BUILD=$BD tools/run-test.sh fsuae/a1200-020-fast.fs-uae 2400 tools/tests/make-test.txt 2>&1 | tr -d '\r' > "$OUT/run.log"
python3 - "$OUT/run.log" "$OUT/expected.txt" <<'PY'
import re, sys
sys.path.insert(0, "tools")
from screenlog import Log
L = Log(sys.argv[1])
check, part = L.check, L.part
want = open(sys.argv[2], encoding="utf-8").read().splitlines()
got = part("mout", "mend").splitlines()
check(L.status("mst") == 0, "run.sh ended with status 0", L.status("mst"))
diff = [(i + 1, w, g) for i, (w, g) in enumerate(zip(want, got)) if w != g]
check(len(got) == len(want) and not diff, "run.sh's %d lines are the PC's, line for line" % len(want),
      (len(got), len(want), diff[:5]))
pr = part("mend", "pend")
probes = re.findall(r"^makeprobe: stack (\d+) bytes deep(.*)$", pr, re.M)
stack = int(re.search(r"^STACK_make := (\d+)$", open("Makefile").read(), re.M).group(1))
check(len(probes) == 2 and not any(x for _, x in probes) and max(int(a) for a, _ in probes) + 4096 <= stack,
      "the deepest stack of the two chains (%s bytes) leaves 4 KB of STACK_make (%d) free" %
      (", ".join(a for a, _ in probes), stack), pr[-500:])
check("overflowed its user stack" not in L.text, "the stack guard never fired",
      re.findall(r".*overflowed its user stack.*", L.text))
check(L.status("ust") == 2 and L.status("zst") == 2, "a missing option argument and an unknown option: status 2",
      (L.status("ust"), L.status("zst")))
check(L.status("xst") == 2 and re.search(r"^run by sh$", L.text, re.M) is not None,
      "a recipe's exit 5 is make's status 2 (and it ran)", L.status("xst"))
m = part("mem", "mmend")
ps = re.findall(r"^ +\d+ +\d+ +\d+ +-?\d+ +\w+ +(\d+) +(\d+) make$", m, re.M)
fr = re.findall(r"^(?:chip RAM|fast RAM|RAM) free: (\d+) KB$", m, re.M)
check(len(ps) == 1, "ps while a make runs", m[-600:])
if len(ps) == 1:
    print("     (memory, A1200 with 4 MB fast RAM, emulator figures: a running make owns %s KB, its shared code %s KB; "
          "free before it: %s KB)" % (ps[0][0], ps[0][1], " / ".join(fr)))
L.waits()
L.done()
PY
