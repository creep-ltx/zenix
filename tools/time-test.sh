#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The wall clock across the tick count's wrap (audit 2026-10-26 F8), in FS-UAE: a test build whose tick count starts
# 40 s before its 32-bit wrap (DIAG=-DDIAG_TICKS0, the wrap that comes after 497 days of uptime), the clock set to
# 2040-01-01 (above 2^31: SYS_time read unsigned), then `date +%s` and `uptime` every 4 s for 80 s. Checked: uptime
# starts at 497 days and goes past the tick count's wrap without starting again at 0 (F9; the old uptime read the tick
# count and showed 0:00:0x after the wrap), the time never goes back, and it moves 4-6 s between two readings (the old
# kernel jumped back 497 days there). The host side of the same code: tools/clock-host-test.sh.
cd "$(dirname "$0")/.." || exit 1
OUT=${ZENIX_OUT:-/tmp/zenix-time}; mkdir -p $OUT
NB=$OUT/build
make -s BUILD=$NB DIAG=-DDIAG_TICKS0=0xFFFFF05Fu all >$OUT/build.log 2>&1 || { echo "FAIL the DIAG_TICKS0 build"; tail -5 $OUT/build.log; exit 1; }
ZENIX_BUILD=$NB tools/run-test.sh fsuae/a1200-020-fast.fs-uae 300 tools/tests/time-test.txt 2>&1 | tr -d '\r' > $OUT/run.log
python3 - "$OUT/run.log" <<'PY'
import re, sys
log = open(sys.argv[1], errors="replace").read()
bad = 0
def check(ok, what, detail=""):
    global bad
    print(("PASS " if ok else "FAIL ") + what + ("" if ok else ": " + str(detail)))
    bad += not ok
t = [int(x) for x in re.findall(r"^t=(\d+)$", log, re.M)]
up = [int(h) * 3600 + int(m) * 60 + int(s) for h, m, s in re.findall(r"^up (\d+):(\d\d):(\d\d)$", log, re.M)]
WRAP = 0x100000000 // 100           # 42,949,672 s: the tick count wraps after this many seconds
check(len(t) == 20, "20 readings of date +%s", len(t))
check(len(up) == 21 and up[0] >= WRAP - 45 and up[0] < WRAP, "uptime starts 497 days in, just before the tick count's wrap", up[:1])
check(bool(up) and up[-1] > WRAP + 10, "the tick count wrapped during the run (uptime past 2^32 ticks)", up[-1:])
du = [b - a for a, b in zip(up, up[1:])]
check(bool(du) and all(0 <= x <= 7 for x in du), "uptime never starts again at 0 across the wrap (F9)", du)
check(bool(t) and t[0] >= 2208988800, "2040-01-01 set: SYS_time above 2^31 read unsigned", t[:1])
d = [b - a for a, b in zip(t, t[1:])]
check(bool(d) and all(3 <= x <= 7 for x in d), "the time goes on across the wrap: 3-7 s between readings 4 s apart, never back", d)
sys.exit(1 if bad else 0)
PY
