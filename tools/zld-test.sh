#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# zld in FS-UAE (DESIGN D52 section 5, docs/man/zld.txt): /bin/zld on the minimum machine (an A1200 with 4 MB of fast
# RAM) links three programs from objects and archives the test writes into /root/zld of its copy of the root image:
# libctest (a C library program: crt0.o, its object, the development package's libc.a and libgcc.a found with -L and
# -l, the --defsym options of the Makefile), hello and sh (programs with shared code: crt0-pure.o, their objects,
# libz). Checked: each program Zenix linked is the PC's zld output from the same files byte for byte, and runs (hello
# prints its lines and ends with its status 7, the linked sh runs a command, libctest's every check passes); a link
# with an undefined symbol is refused with status 1 and leaves no file; zld's memory while it links sh (ps, emulator figures) and the times
# (emulator figures, not hardware speed). The objects are the build's with their debug sections stripped (as the
# development package's libraries are); the PC's reference is zld on the PC (tools/zld-host-test.sh proves that one
# against ld + tools/mkzexe.py). Needs debugfs and e2fsck (e2fsprogs).
cd "$(dirname "$0")/.." || exit 1
OUT=${ZENIX_OUT:-/tmp/zenix-zld}; mkdir -p "$OUT"
BD=${ZENIX_BUILD:-build}
P=${TOOLPREFIX:-$(command -v m68k-elf-gcc >/dev/null && echo m68k-elf- || echo "$HOME/opt/m68k-elf/bin/m68k-elf-")}
make -s BUILD=$BD all >"$OUT/build.log" 2>&1 || { echo "FAIL the build"; tail -5 "$OUT/build.log"; exit 1; }
F=$OUT/files; rm -rf "$F"; mkdir -p "$F"
cc -std=gnu99 -O2 -o "$OUT/zld-pc" user/zld.c || { echo "FAIL the PC build of user/zld.c"; exit 1; }
L=$BD/devel/usr/lib
cp "$L/crt0.o" "$L/crt0-pure.o" "$L/libc.a" "$L/libgcc.a" "$F/"
for p in libctest.co:libctest.o hello.po:hello.o sh.po:sh.o libz.po:libz.o libz.uo:libzu.o; do
    "${P}objcopy" --strip-debug "$BD/${p%%:*}" "$F/${p##*:}" || { echo "FAIL objcopy $p"; exit 1; }
done
printf 'void missing(void); int main(void) { missing(); return 0; }\n' >"$OUT/undef.c"
"${P}gcc" -m68020-60 -msoft-float -ffreestanding -nostdlib -O2 -c "$OUT/undef.c" -o "$F/undef.o" ||
    { echo "FAIL undef.o"; exit 1; }
# the PC's links, from the same files (the same command lines as the serial script's)
(cd "$F" && "$OUT/zld-pc" -s 16384 --defsym=vfprintf=__l_vfprintf --defsym=vfscanf=__l_vfscanf -o "$OUT/pc-libctest" \
    crt0.o libctest.o -L . -lc -lgcc && "$OUT/zld-pc" -s 16384 -o "$OUT/pc-hello" crt0-pure.o hello.o libz.o libgcc.a &&
    "$OUT/zld-pc" -s 24576 -o "$OUT/pc-shx" crt0-pure.o sh.o libz.o libgcc.a) || { echo "FAIL the PC's links"; exit 1; }
H=$BD/zenix.hdf
rm -f "$H"; make -s BUILD=$BD "$H" >>"$OUT/build.log" 2>&1 || { echo "FAIL the root image"; exit 1; }   # a fresh one
dfs() { debugfs -w -R "$1" "$H" >/dev/null 2>&1; }
dfs "mkdir /root/zld"
for f in "$F"/*; do
    dfs "write $f /root/zld/$(basename "$f")"
done
e2fsck -fn "$H" >"$OUT/e2fsck-before.log" 2>&1 || { echo "FAIL the image after writing the files"; exit 1; }
ZENIX_BUILD=$BD tools/run-test.sh fsuae/a1200-020-fast.fs-uae 3600 tools/tests/zld-test.txt 2>&1 | tr -d '\r' >"$OUT/run.log"
for n in libctest hello shx bad; do
    rm -f "$OUT/z-$n"
    debugfs -R "dump /root/zld/$n $OUT/z-$n" "$H" >/dev/null 2>&1
done
e2fsck -fn "$H" >"$OUT/e2fsck.log" 2>&1; est=$?
python3 - "$OUT" "$est" <<'PY'
import os, re, sys
sys.path.insert(0, "tools")
from screenlog import Log
out, est = sys.argv[1], int(sys.argv[2])
L = Log(os.path.join(out, "run.log"))
check = L.check
t = L.text
for prog, st, n in (("libctest", "cst", "C library program, archives found with -l"), ("hello", "hst", "shared code"),
                    ("shx", "sst", "sh, shared code")):
    check(L.status(st) == 0, "zld linked %s on Zenix (%s), status 0" % (prog, n), L.status(st))
    z, pc = os.path.join(out, "z-" + prog), os.path.join(out, "pc-" + prog)
    same = os.path.exists(z) and open(z, "rb").read() == open(pc, "rb").read()
    check(same, "%s as Zenix linked it is the PC's byte for byte (%d bytes)" % (prog, os.path.getsize(pc)),
          os.path.getsize(z) if os.path.exists(z) else "missing")
check(L.status("ust") == 1 and re.search(r"^zld: undef\.o: undefined reference to `missing'$", t, re.M) is not None
      and not os.path.exists(os.path.join(out, "z-bad")),
      "an undefined symbol: status 1, the message names it and the object, no file left", L.status("ust"))
check(L.status("hrun") == 7 and re.search(r"^hello from pid \d+, round 2$", t, re.M) is not None,
      "the linked hello runs (its three lines, its status 7)", L.status("hrun"))
check(L.status("srun") == 0 and re.search(r"^sh says 42$", t, re.M) is not None, "the linked sh runs a command",
      L.status("srun"))
m = re.search(r"^libctest: (\d+) checks, (\d+) failed$", t, re.M)
check(L.status("lrun") == 0 and m is not None and m.group(2) == "0" and int(m.group(1)) > 100,
      "the linked libctest passes all its checks (%s)" % (m.group(1) if m else "?"), (L.status("lrun"), m and m.group(0)))
check(est == 0, "the host's e2fsck finds the root clean", est)
check("overflowed its user stack" not in t, "the stack guard never fired", re.findall(r".*overflowed.*", t))
ps = [int(a) for a in re.findall(r"^ +\d+ +\d+ +\d+ +-?\d+ +\w+ +(\d+) +\d+ zld$", t, re.M)]
fr = re.findall(r"^(?:chip RAM|fast RAM|RAM) free: (\d+) KB$", t, re.M)
times = re.findall(r"^real\s+(\S+)$", t, re.M)
check(len(ps) > 0, "ps saw zld while it linked sh", t[-400:])
if ps:
    print("     (emulator figures, A1200 with 4 MB fast RAM: zld linking sh owned at most %d KB in %d samples; free "
          "before: %s KB; times: libctest %s, hello %s)" % (max(ps), len(ps), " / ".join(fr[:2]),
                                                           times[0] if times else "?", times[1] if len(times) > 1 else "?"))
L.waits()
L.done()
PY
st=$?
# the copy of the build's image is the test's: put it back as the build makes it
rm -f "$H"; make -s BUILD=$BD "$H" >/dev/null 2>&1
exit $st
