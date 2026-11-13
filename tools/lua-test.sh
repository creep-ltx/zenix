#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Lua in FS-UAE (DESIGN D52 section 9, ports/lua): /bin/lua on the minimum machine (an A1200 with 4 MB of fast RAM).
# rootfs/tests/lua/zenix.lua's output must be the PC's, line for line (tools/lua-host-test.sh builds the same pinned
# source for the PC and runs it: the reference); cstack.lua drives every C recursion a script can make to Lua's own
# limit, and each must end in Lua's "C stack overflow", never in the kernel's stack guard (the stack is fixed, no
# MMU); then the command line: -v, -e, a script from standard input with arguments, a script file with arguments,
# the exit status of an error, a missing file and os.exit, -i (the prompt, a line typed). The memory with a Lua
# running is printed (emulator figures).
cd "$(dirname "$0")/.." || exit 1
OUT=${ZENIX_OUT:-/tmp/zenix-lua}; mkdir -p "$OUT"
BD=${ZENIX_BUILD:-build}
LUA_EXPECTED="$OUT/expected.txt" tools/lua-host-test.sh >"$OUT/host.log" 2>&1 ||
    { echo "FAIL the PC's reference run (tools/lua-host-test.sh)"; cat "$OUT/host.log"; exit 1; }
make -s BUILD=$BD all >"$OUT/build.log" 2>&1 || { echo "FAIL the build"; tail -5 "$OUT/build.log"; exit 1; }
ZENIX_BUILD=$BD tools/run-test.sh fsuae/a1200-020-fast.fs-uae 2400 tools/tests/lua-test.txt 2>&1 | tr -d '\r' > "$OUT/run.log"
python3 - "$OUT/run.log" "$OUT/expected.txt" <<'PY'
import re, sys
sys.path.insert(0, "tools")
from screenlog import Log
L = Log(sys.argv[1])
check, part = L.check, L.part
want = open(sys.argv[2], encoding="utf-8").read().splitlines()
got = part("zout", "zend").splitlines()
check(L.status("zst") == 0, "zenix.lua ended with status 0", L.status("zst"))
diff = [(i + 1, w, g) for i, (w, g) in enumerate(zip(want, got)) if w != g]
check(len(got) == len(want) and not diff, "zenix.lua's %d lines are the PC's, line for line" % len(want),
      (len(got), len(want), diff[:5]))
cs = part("cout", "cend")
cases = re.findall(r"^(\w+)\t(.*)\tok$", cs, re.M)
check(L.status("cst") == 0 and len(cases) >= 16 and all(e == "C stack overflow" for _, e in cases) and
      "cstack done" in cs, "every C recursion (%d) stopped by Lua's own limit" % len(cases),
      [c for c in cases if c[1] != "C stack overflow"] or cs[-400:])
check("overflowed its user stack" not in L.text, "the stack guard never fired",
      re.findall(r".*overflowed its user stack.*", L.text))
check(re.search(r"^Lua 5\.4\.9  Copyright \(C\) 1994-2026 Lua\.org, PUC-Rio$", L.text, re.M) is not None,
      "lua -v")
check(re.search(r"^2\t2\tlua$", L.text, re.M) is not None, "lua -e: the chunk run, arg as upstream makes it")
check(re.search(r"^fmt=0\.1\|5e-324$", L.text, re.M) is not None, "the C library's %.17g and a subnormal: the "
      "shortest exact digits (picolibc's Ryu; docs/limits.md)", re.findall(r"^fmt=.*", L.text, re.M))
check(re.search(r"^from a pipe\tx\ty$", L.text, re.M) is not None and L.status("pst") == 0,
      "a script from standard input (-) with its arguments")
check(re.search(r"^lua: \(command line\):1: boom$", L.text, re.M) is not None and L.status("est") == 1 and
      "stack traceback:" in L.text, "an error: the message, a traceback, status 1")
check(re.search(r"^lua: cannot open /no/such/file\.lua", L.text, re.M) is not None and L.status("nst") == 1,
      "a missing script: said, status 1")
check(L.status("xst") == 7, "os.exit(7): status 7", L.status("xst"))
check(re.search(r"^args\tone\ttwo$", L.text, re.M) is not None and L.status("ast") == 0,
      "a script file with its arguments")
check(re.search(r"^42$", L.text, re.M) is not None, "lua -i: the file run first, then a line typed at the prompt")
m = part("mem", "mend")
ps = re.findall(r"^ +\d+ +\d+ +\d+ +-?\d+ +\w+ +(\d+) +(\d+) lua$", m, re.M)
fr = re.findall(r"^(?:chip RAM|fast RAM|RAM) free: (\d+) KB$", m, re.M)
check(len(ps) == 2, "ps while a Lua runs (one bare, one with 100 KB of strings)", m[-600:])
if len(ps) == 2:
    print("     (memory, A1200 with 4 MB fast RAM, emulator figures: a bare lua owns %s KB (its code, its 128 KB stack, its "
          "heap: a plain program has no shared part), %s KB with 100 KB of strings in its heap; free before and with "
          "it: %s KB)" % (ps[0][0], ps[1][0], " / ".join(fr)))
L.waits()
L.done()
PY
