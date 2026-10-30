#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# ctop in FS-UAE (roadmap block 3; ports/ctop): the owner's btop-like monitor on the stock A1200. termtest screen reads
# the console's grid back (CON_READ) once ctop has drawn it and then types the next key into the console (CON_INPUT):
# the four boxes (the CPU's line from the kernel's log, chip memory in use, the processes, the root's space among the
# disks, "no network" while netd is not running), h (the keys), Space (back), e (only the Shells: none on Zenix), q;
# then with netd running (netstat starts it) and -d 0.5: the net box's rates and totals from the interfaces' counters
# (bsdsocket.library's calls, ports/ctop/zenix), q; a bad delay and an extra word (status 10, said), ? (the usage,
# status 0); the keys file written ($HOME/.config/ctop.keys); the memory with ctop open printed (free and ps). Every
# wait is for an event (the grid, the shell's lines), none for a time.
cd "$(dirname "$0")/.." || exit 1
OUT=${ZENIX_OUT:-/tmp/zenix-ctop}; mkdir -p $OUT
BD=${ZENIX_BUILD:-build}
make -s BUILD=$BD all >$OUT/build.log 2>&1 || { echo "FAIL the build"; tail -5 $OUT/build.log; exit 1; }
rm -f $BD/zenix.hdf                     # a fresh root: no keys file from an earlier run
make -s BUILD=$BD $BD/zenix.hdf >>$OUT/build.log 2>&1 || { echo "FAIL the image"; tail -5 $OUT/build.log; exit 1; }
ZENIX_BUILD=$BD tools/run-test.sh fsuae/a1200-020.fs-uae 400 tools/tests/ctop-test.txt 2>&1 | tr -d '\r' > $OUT/run.log
python3 - "$OUT/run.log" <<'PY'
import re, sys
sys.path.insert(0, "tools")
from screenlog import Log, rows
L = Log(sys.argv[1])
check, part = L.check, L.part
s = [part("c%d" % i, "c%d" % (i + 1)) for i in range(4)] + [part("c4", "file")]
f = part("file", "cdone")
r = [rows(x) for x in s]
col = lambda rr, a, b: "\n".join(rr.get(i, "")[a:b] for i in range(32))
check([L.status(n) for n in ("st", "st2", "st5")] == [0, 0, 0], "ctop ran and ended with status 0 (q, q, ?)",
      [L.status(n) for n in ("st", "st2", "st5")])
r0 = r[0]
check(len(r0) == 32 and "cpu" in r0.get(0, "") and "1000ms" in r0.get(0, "") and "68020" in r0.get(1, ""),
      "the cpu box on the console's 32 rows: the refresh interval, the processor from the kernel's log",
      (r0.get(0), r0.get(1)))
c0 = col(r0, 44, 80)
check(re.search(r"chip \d+%", c0) is not None and re.search(r"Tasks \d+, \d+ running/ready", c0) is not None,
      "the cpu panel: chip memory in use, the processes and how many run or are ready", c0)
m0 = col(r0, 0, 36)
check(re.search(r"Chip: .* 1\.7M", m0) is not None and re.search(r"Used: .* \d+% \d", m0) is not None,
      "the mem box: chip memory's size (the kernel's boot log: 1,812 KB) and what is in use", m0)
check(re.search(r"disks", m0) is not None and re.search(r"^│/ +\S+ +\d+% +8\.0M│$", m0, re.M) is not None,
      "the disks: the root (/) with its meter, its use and its size (8 MB)", m0)
check("no bsdsocket.library:" in m0 and "no network" in m0, "the net box while netd is not running: no network", m0)
p0 = col(r0, 36, 80)
check(re.search(r"Program:", p0) is not None and re.search(r"- ctop +0 run ", p0) is not None and
      re.search(r"- diskd +10 wai ", p0) is not None and re.search(r"- init +0 wai ", p0) is not None and
      re.search(r"\[idle\] +0 rdy ", p0) is not None and re.search(r"\d+/\d+─╯$", r0.get(31, "")) is not None,
      "the proc box: every process (ctop running, diskd at nice -10 as priority 10, the kernel's idle loop), "
      "the count", p0)
check(r[1].get(0, "").startswith(" ctop 0.1b1") and any(x.strip().startswith("q <Esc> <C-c>") for x in r[1].values())
      and "CPU% is sampled" in r[1].get(31, ""), "h: the keys as bound (the help screen)", (r[1].get(0), r[1].get(31)))
check(re.search(r"Tasks \d+", col(r[2], 44, 80)) is not None, "Space: back to the boxes", r[2].get(4))
check(r[3].get(31, "").endswith("0/0─╯") and "ctop" not in col(r[3], 36, 80),
      "e: only the Shells (none on Zenix: an empty list)", r[3].get(31))
r4 = r[4]
m4 = col(r4, 0, 36)
check("500ms" in r4.get(0, "") and re.search(r"v 0B/s", m4) is not None and re.search(r"\^ 0B/s", m4) is not None and
      m4.count("Total 0B") == 2 and re.search(r"- netd +0 wai ", col(r4, 36, 80)) is not None,
      "with netd running and -d 0.5: the net box's rates and totals (the interfaces' counters, none but the "
      "loopback's here), netd among the processes", m4)
check(L.status("st3") == 10 and re.search(r"^ctop: x: not a delay in seconds$", L.text, re.M) is not None and
      L.status("st4") == 10 and re.search(r"^ctop: extra: unexpected argument$", L.text, re.M) is not None and
      re.search(r"^usage: ctop \[-d SECONDS\]$", L.text, re.M) is not None,
      "a bad delay and an extra word: said, status 10; ?: the usage", (L.status("st3"), L.status("st4")))
check(re.search(r"^-rw-r--r-- .*\.config/ctop\.keys$", f, re.M) is not None and
      re.search(r"^; ctop's keys: KEY ACTION", f, re.M) is not None,
      "the keys file written at the first start ($HOME/.config/ctop.keys, the built-in keys)", f[:400])
L.memory(f, "ctop", "ctop")
L.waits()
L.done()
PY
