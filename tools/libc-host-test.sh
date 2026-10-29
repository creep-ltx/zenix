#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The C library on the PC (DESIGN D27 section 11): every constant the port's headers give (E*, O_*, S_IF*, SIG*,
# PATH_MAX, NSIG ...) equals the kernel's, read from both through the cross compiler's preprocessor; libc.a and
# libc-pure.a pass tools/check-68020-60.sh and hold no FPU instruction (one build for every 68020-68060, -msoft-float).
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
CC=${TOOLPREFIX:-$HOME/opt/m68k-elf/bin/m68k-elf-}gcc
OD=${TOOLPREFIX:-$HOME/opt/m68k-elf/bin/m68k-elf-}objdump
make -s BUILD=$BD $BD/libc.a $BD/libc-pure.a >/dev/null 2>&1 || { echo "FAIL libc.a and libc-pure.a do not build"; exit 1; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
pass=1
INC="-Ilibc/zenix/include -I$BD/libc-inc -Ilibc/picolibc/newlib/libc/machine/m68k -Ilibc/picolibc/newlib/libc/tinystdio -Ilibc/picolibc/newlib/libc/locale -Ilibc/picolibc -Ilibc/picolibc/newlib/libc/include"
printf '#include <errno.h>\n#include <fcntl.h>\n#include <signal.h>\n#include <sys/stat.h>\n#include <limits.h>\n#include <unistd.h>\n#include <stdio.h>\n' > $T/libc.c
printf '#include "kernel/kernel.h"\n#include "kernel/fsproto.h"\n' > $T/kern.c
printf '#include "kernel/errno.h"\n' > $T/errno.c
printf '#include "user/usys.h"\n#include "user/libz.h"\n' > $T/usys.c
$CC -m68020-60 -ffreestanding $INC -dM -E $T/libc.c > $T/libc.dm || { echo "FAIL the port's headers do not preprocess"; exit 1; }
$CC -m68020-60 -ffreestanding -I. -dM -E $T/kern.c > $T/kern.dm || { echo "FAIL the kernel's headers do not preprocess"; exit 1; }
$CC -m68020-60 -ffreestanding -I. -dM -E $T/errno.c > $T/errno.dm || { echo "FAIL kernel/errno.h does not preprocess"; exit 1; }
$CC -m68020-60 -ffreestanding -I. -dM -E $T/usys.c > $T/usys.dm || { echo "FAIL usys.h does not preprocess"; exit 1; }
python3 -I - "$T" <<'PY' || pass=0
import re, sys
T = sys.argv[1]
def defs(f):
    d = {}
    for l in open(f):
        m = re.match(r'#define (\w+) (.*)$', l)
        if m: d[m.group(1)] = m.group(2).strip()
    return d
def value(d, name, depth=0):
    if name not in d or depth > 20: return None
    e = d[name]
    e = re.sub(r'/\*.*?\*/', '', e)
    e = re.sub(r'\b[A-Za-z_]\w*\b', lambda m: str(value(d, m.group(0), depth + 1)), e)
    e = re.sub(r'\b0([0-7]+)\b', r'0o\1', e).replace('u', '').replace('U', '')
    try: return eval(e, {}, {})
    except Exception: return None
libc, kern, usys, errs = defs(T + "/libc.dm"), defs(T + "/kern.dm"), defs(T + "/usys.dm"), defs(T + "/errno.dm")
names = [n for n in errs if re.match(r'^E[A-Z0-9]+$', n)]
names += "O_RDONLY O_WRONLY O_RDWR O_CREAT O_TRUNC O_APPEND O_NONBLOCK S_IFMT S_IFDIR S_IFREG S_IFLNK S_IFSOCK".split()
names += [n for n in kern if re.match(r'^SIG[A-Z0-9]+$', n) and n not in ("SIGBIT", "SIGFRAME_FIRST")]
names += ["NSIG"]
bad = []
for n in sorted(set(names)):
    k, c = value(kern, n), value(libc, n)
    if k is None or c != k: bad.append("%s: kernel %s, C library %s" % (n, k, c))
# what only the programs' header (user/usys.h, libz.h) has: the signals the kernel never raises, O_EXCL, PATH_MAX
for n in "SIGILL SIGTRAP SIGABRT SIGBUS SIGFPE SIGSEGV O_EXCL SIG_BLOCK SIG_UNBLOCK SIG_SETMASK SA_RESTART".split():
    u, c = value(usys, n), value(libc, n)
    if u is None or c != u: bad.append("%s: usys.h %s, C library %s" % (n, u, c))
if value(usys, "ZPATH_MAX") != value(libc, "PATH_MAX"): bad.append("PATH_MAX: %s, libz's ZPATH_MAX %s" % (value(libc, "PATH_MAX"), value(usys, "ZPATH_MAX")))
if value(kern, "NFD") != value(libc, "OPEN_MAX"): bad.append("OPEN_MAX %s, the kernel's NFD %s" % (value(libc, "OPEN_MAX"), value(kern, "NFD")))
if value(libc, "ENOTSUP") != value(libc, "EOPNOTSUPP"): bad.append("ENOTSUP is not EOPNOTSUPP")
n = len(set(names)) + 14
print(("PASS" if not bad else "FAIL") + " the C library's constants equal the kernel's (%d names)" % n + ("" if not bad else ": " + "; ".join(bad)))
sys.exit(1 if bad else 0)
PY
for a in $BD/libc.a $BD/libc-pure.a; do
    TOOLPREFIX=${TOOLPREFIX:-$HOME/opt/m68k-elf/bin/m68k-elf-} tools/check-68020-60.sh $a >/dev/null &&
        echo "PASS $(basename $a): no 64-bit multiply or divide (check-68020-60)" || { echo "FAIL $(basename $a): check-68020-60"; pass=0; }
    fpu=$($OD -d --no-show-raw-insn $a | grep -E '^ *[0-9a-f]+:[[:space:]]+f(mov|add|sub|mul|div|cmp|tst|sqrt|abs|neg|int|b|db|s[a-z]|save|restore|trap|nop|get|scale|rem|mod|sin|cos|tan|a|e|l|t)[a-z.]*[[:space:]]')
    [ -z "$fpu" ] && echo "PASS $(basename $a): no FPU instruction (-msoft-float)" || { echo "FAIL $(basename $a): FPU instructions"; echo "$fpu" | head -3; pass=0; }
done
[ $pass = 1 ]
