#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# DESIGN D53 (docs/d53-shebang.md): scripts started by their #! line, in FS-UAE on the minimum machine (an A1200 with
# 4 MB of fast RAM). rootfs/tests/shebang holds the scripts: an sh script, a Lua script, `#!/bin/env lua`, a
# `#!/bin/make -f` makefile, `#!/bin/echo` with one argument holding spaces (what the interpreter is given, in order),
# a 127-byte line; each one's $0 / arg[0] and arguments are checked. Then the refusals: a 128-byte line, a missing
# interpreter, no x bit, an interpreter that is a script, a file with neither magic nor #! (sh FILE still runs it), and
# the kernel log's lines. Then scripts started not by the shell's own spawn: a make recipe, xargs, env, find -exec.
# Then a script over 256 KB (the program size limit is not a script's), xargs's full runs into a script (room left
# for the #! line's words), and the memory after 400 script starts, 300 of them refused (nothing kept). Last: the
# process's name is the script's, and a set-user-id script owned by uid 1000 still runs as root.
cd "$(dirname "$0")/.." || exit 1
OUT=${ZENIX_OUT:-/tmp/zenix-shebang}; mkdir -p "$OUT"
BD=${ZENIX_BUILD:-build}
make -s BUILD=$BD all >"$OUT/build.log" 2>&1 || { echo "FAIL the build"; tail -5 "$OUT/build.log"; exit 1; }
ZENIX_BUILD=$BD tools/run-test.sh fsuae/a1200-020-fast.fs-uae 1200 tools/tests/shebang-test.txt 2>&1 | tr -d '\r' > "$OUT/run.log"
python3 - "$OUT/run.log" <<'PY'
import re, sys
sys.path.insert(0, "tools")
from screenlog import Log
L = Log(sys.argv[1])
check, part = L.check, L.part
def has(t, line, what):
    check(re.search(r"^" + re.escape(line) + r"$", t, re.M) is not None, what, t[-700:])
r = part("run", "rend")
has(r, "sh0=./args.sh n=2 1=a1 2=b2", "an sh script: $0 is the path it was started by, then its arguments")
has(r, "s1=0", "the sh script's status")
has(r, "lua0=./args.lua n=2 1=l1 2=l2", "a Lua script: arg[0] is its path, arg[1] and arg[2] its arguments")
has(r, "env0=./envlua n=1 1=e1", "#!/bin/env lua: env finds lua on $PATH")
has(r, "make ran V=7", "#!/bin/make -f: make reads the script as its makefile, V=7 from the command line")
has(r, "s4=0", "make's status")
has(r, "one  two ./echo1 x y", "#!/bin/echo  one  two  : one argument with its inner spaces, then the path, then x y")
has(r, "a" * 115 + " ./long127", "a 127-byte #! line is taken")
has(r, "s5=0", "its status")
b = part("bad", "bend")
def said(st, name, line, what):
    check(L.status(st) == 126 and re.search(r"^" + re.escape(line) + r"$", b, re.M) is not None, what + " (126)", b[-700:])
said("l8", "l8", "sh: ./long128: #! line over 127 bytes", "a 128-byte #! line is refused, the shell says why")
said("mi", "mi", "sh: ./missing: #! interpreter /tests/shebang/no-such-interpreter: no such file or directory",
     "a missing interpreter: the shell names it")
said("nx", "nx", "sh: ./nox.sh: permission denied", "no x bit: permission denied")
said("ne", "ne", "sh: ./nested: #! interpreter /tests/shebang/args.sh: not a Zenix program (a script, an AmigaOS program?)",
     "an interpreter that is a script is refused")
said("cr", "cr", "sh: ./crlf: its #! line ends in a carriage return (a DOS file?)", "a CRLF script: the shell says so")
said("nm", "nm", "sh: ./nomagic: not a Zenix program", "neither magic nor #!: refused as before")
check(L.status("sn") == 0 and "this file has no magic and no shebang line" in b, "sh FILE still runs it", b[-600:])
check("exec: ./long128 refused: #! line over 127 bytes" in b, "the kernel log names the long line", b[-800:])
v = part("via", "vend")
has(v, "sh0=/tests/shebang/args.sh n=2 1=rec1 2=rec2", "a make recipe starts a script")
has(v, "mk=0", "make's status")
has(v, "sh0=/tests/shebang/args.sh n=2 1=x1 2=x2", "xargs starts a script")
has(v, "sh0=/tests/shebang/args.sh n=1 1=v1 2=", "env starts a script")
has(v, "one  two /tests/shebang/echo1 q", "find -exec starts a script")
check(L.status("xa") == 0 and L.status("en") == 0 and L.status("fi") == 0, "their statuses", v[-400:])
g = part("big", "heap")
check(re.search(r"^big-ran$", g, re.M) is not None and L.status("bg") == 0 and re.search(r" 3\d{5} .*/tmp/big\.sh$", g, re.M),
      "a script over 256 KB runs (the program size limit is not a script's)", g[-500:])
counts = [int(x) for x in re.findall(r"^count n=(\d+)$", g, re.M)]
check(sum(counts) == 600 and L.status("xc") == 0 and len(counts) >= 3 and max(counts) <= 252,
      "xargs gives a script all 600 words in full runs of at most 252 (%s)" % counts, g[-500:])
h = part("heap", "gend")
fr = [int(x) for x in re.findall(r"^fast RAM free: (\d+) KB$", h, re.M)]
shm = [int(x) for x in re.findall(r"^ +\d+ +\d+ +\d+ +-?\d+ +\w+ +(\d+) +\d+ sh$", h, re.M)]
check(len(fr) == 2 and len(shm) == 2 and abs((fr[0] + shm[0]) - (fr[1] + shm[1])) <= 1,
      "400 script starts, 300 of them refused, give all memory back (fast RAM free %s, the test's shell %s KB: the "
      "shell's own work area grows for the long loop line and stays, the rest must be the same)" % (fr, shm), h[-700:])
p = part("ps", "pend")
check(re.search(r"^-rwsr-xr-x .*/tmp/psname\.sh$", p, re.M) is not None, "the script is set-user-id", p[-500:])
check(re.search(r" psname\.sh$", p, re.M) is not None, "ps names the process after the script", p[-500:])
check(re.search(r"^uid=0\(root\)", p, re.M) is not None and L.status("ps") == 0,
      "a set-user-id script owned by uid 1000 runs as root (the bit is ignored on scripts)", p[-500:])
L.waits()
L.done()
PY
