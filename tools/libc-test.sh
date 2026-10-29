#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The C library in FS-UAE (DESIGN D27 section 11): /tests/libctest's checks (formats, sscanf, strtol, qsort, fopen's
# modes, O_EXCL and O_NONBLOCK apart, fseek, ungetc, directories, malloc stress and a bad free, setjmp, time and
# time zones, CLOCK_MONOTONIC, UTF-8, the error texts, atexit), then what needs the shell around it: stdout
# line-buffered on the console and fully buffered into a pipe, EPIPE ending the program with 141, the prompt out
# before stdin's read waits (typed after it appeared), and the stream buffers inside the allocator's first chunk.
cd "$(dirname "$0")/.." || exit 1
OUT=${ZENIX_OUT:-/tmp/zenix-libc}; mkdir -p $OUT
BD=${ZENIX_BUILD:-build}
make -s BUILD=$BD all >$OUT/build.log 2>&1 || { echo "FAIL the build"; tail -5 $OUT/build.log; exit 1; }
ZENIX_BUILD=$BD tools/run-test.sh fsuae/a1200-020.fs-uae 600 tools/tests/libc-test.txt 2>&1 | tr -d '\r' > $OUT/run.log
python3 - "$OUT/run.log" <<'PY'
import re, sys
log = open(sys.argv[1], errors="replace").read()
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
sys.exit(1 if bad else 0)
PY
