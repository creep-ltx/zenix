#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The C library in FS-UAE (DESIGN D27 section 11): /tests/libctest's checks (formats, sscanf, strtol, qsort, fopen's
# modes, O_EXCL and O_NONBLOCK apart, fseek, ungetc, directories, malloc stress and a bad free, setjmp, time and
# time zones, CLOCK_MONOTONIC, UTF-8, the error texts, atexit; step 3's signals, poll and select, sockets on the
# loopback, libzenix's helpers, getpw* and getgr*, glob, regcomp, posix_spawn with file actions, envp and a process
# group, wait, system and popen), then what needs the shell around it: stdout line-buffered on the console and fully
# buffered into a pipe, EPIPE ending the program with 141, the prompt out before stdin's read waits (typed after it
# appeared), the stream buffers inside the allocator's first chunk, and the terminal with typed keys (TIOCGWINSZ, raw
# and back, ^C a byte without ISIG, a line read with ECHO off and nothing on the console's grid, the C library's line
# editing, getpass). Then a second run (fast RAM: two at once): the ZEX2 build (/tests/libctestpure, libc-pure.a and
# the port's 64-bit division) on consoles 2 and 3 at once, every check passing in both, one copy of its code (free
# and ps show it shared).
cd "$(dirname "$0")/.." || exit 1
OUT=${ZENIX_OUT:-/tmp/zenix-libc}; mkdir -p $OUT
BD=${ZENIX_BUILD:-build}
make -s BUILD=$BD all >$OUT/build.log 2>&1 || { echo "FAIL the build"; tail -5 $OUT/build.log; exit 1; }
ZENIX_BUILD=$BD tools/run-test.sh fsuae/a1200-020.fs-uae 600 tools/tests/libc-test.txt 2>&1 | tr -d '\r' > $OUT/run.log
ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenix-serial}-p ZENIX_BUILD=$BD tools/run-test.sh fsuae/a1200-020-fast.fs-uae 600 \
    tools/tests/libc-pure-test.txt 2>&1 | tr -d '\r' | sed 's/\x1b\[[0-9;:]*[mK]//g' > $OUT/pure.log
CK=$(python3 -c 'import struct, sys; print(struct.unpack(">I", open(sys.argv[1], "rb").read()[8:12])[0] // 1024)' $BD/libctest.pzx)
python3 - "$OUT/run.log" "$OUT/pure.log" "$CK" <<'PY'
import re, sys
log = open(sys.argv[1], errors="replace").read()
plog = open(sys.argv[2], errors="replace").read()
ck = int(sys.argv[3])
bad = 0
def check(ok, what, detail=""):
    global bad
    print(("PASS " if ok else "FAIL ") + what + ("" if ok else ": " + str(detail)))
    bad += not ok
a = log.split("==adone")[0]
fails = re.findall(r"^FAIL .*$", a, re.M)
oks = re.findall(r"^ok .*$", a, re.M)
m = re.search(r"^libctest: (\d+) checks, (\d+) failed$", a, re.M)
check(m is not None and m.group(2) == "0" and not fails and len(oks) == int(m.group(1)),
      "libctest: every check (%d)" % len(oks), fails or (m and m.group(0)) or a[-400:])
check(re.search(r"^atexit ran$", a, re.M) is not None and "st=0" in a, "atexit ran at the end, status 0", a[-200:])
check("pointer malloc did not give out" in a, "a bad free is reported on standard error")
b = log.split("==adone")[-1].split("==bdone")[0]
lines = [l for l in b.splitlines() if l in ("printf line", "dprintf line")]
check(lines == ["printf line", "dprintf line", "dprintf line", "printf line"],
      "stdout: line-buffered on the console, fully buffered into a pipe", lines)
c = log.split("==bdone")[-1].split("==cdone")[0]
check(re.search(r"^ep=141$", c, re.M) is not None, "a write after the reader went: status 141", c[-200:])
d = log.split("==cdone")[-1].split("==ddone")[0]
check(re.search(r"read: typed line", d) is not None, "the prompt was out before the read waited (typed after it)", d[-300:])
check(re.search(r"first chunk: yes", d) is not None, "stdin's and stdout's buffers inside the first 4 KB chunk", d[-200:])
e = log.split("==ddone")[-1].split("==edone")[0]
efails = re.findall(r"^FAIL .*$", e, re.M)
m = re.search(r"^libctest tty: (\d+) checks, (\d+) failed$", e, re.M)
check(m is not None and m.group(2) == "0" and not efails, "the terminal: every check (%s)" % (m.group(1) if m else "?"),
      efails or e[-400:])
w = re.search(r"^winsize (\d+)x(\d+)$", e, re.M)
check(w is not None and w.group(1) == "80" and w.group(2) in ("32", "25"), "TIOCGWINSZ: 80 x 32 (PAL) or 25 (NTSC)", w and w.group(0))
check("hidden words" not in e and "pw123" not in e, "nothing typed with ECHO off reached the console's output")
def fv(mark):
    m = re.search(r"^" + mark + r"$.*?shared code: (\d+) KB, sharing saves (\d+) KB", plog, re.M | re.S)
    return (int(m.group(1)), int(m.group(2))) if m else (-1, -1)
b0, t2, af = fv("==pbase"), fv("==ptwo"), fv("==pafter")
check(t2[0] - b0[0] in (ck, ck + 1) and t2[1] - b0[1] in (ck - 1, ck, ck + 1),
      "two libctestpure at once: one copy of its code (%d KB), the second one saved" % ck, (b0, t2))
two = plog.split("==ptwo")[-1].split("==pafter")[0]
n = len(re.findall(r"^ +\d+ +\d+ +\d+ +-?\d+ [a-z]+ +\d+ +%d libctestpure$" % ck, two, re.M))
check(n == 2, "ps: both run the shared code", n)
check(af == b0, "both ended: the copy is freed", (b0, af))
res = plog.split("==pafter")[-1]
oks = re.findall(r"^(\d+)$", res, re.M)
sums = re.findall(r"^libctest: (\d+) checks, (\d+) failed$", res, re.M)
sts = re.findall(r"^st=(\d+)$", res, re.M)
check(len(sums) == 2 and all(f == "0" for _, f in sums) and oks == [c for c, _ in sums] and sts == ["0", "0"] and
      not re.search(r"^FAIL ", res, re.M), "the ZEX2 build: every check on both consoles", res[-600:])
mz = re.search(r"^libctest: (\d+) checks", a, re.M)
check(mz is not None and [c for c, _ in sums] == [mz.group(1)] * 2, "... the same checks as the ZEXE build", (sums, mz and mz.group(0)))
sys.exit(1 if bad else 0)
PY
