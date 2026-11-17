#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# tcc in FS-UAE (DESIGN D52 section 7, ports/tcc/README, docs/man/tcc.txt), on the minimum machine (an A1200 with 4 MB
# of fast RAM). Built on the PC and run on Zenix (the files in /root/tcc of the test's copy of the root image):
#  - upstream's test suite (tests/tests2: the programs in LIST below), each built by the cross compiler and by gcc
#    (-O1, the Zenix build's flags): a test passes when tcc's program prints upstream's .expect (with the compile's
#    own warnings first, as upstream runs them) or, where upstream's expectation is a little-endian PC's or glibc's,
#    exactly what gcc's program prints;
#  - the ABI test (tools/tcc-gen.py abi): 150 functions defined by tcc and called by gcc, and the other three
#    pairings, each must say "abi: 0 bad";
#  - 20 random programs (tools/tcc-gen.py fuzz), tcc's and gcc -fwrapv's builds printing the same;
#  - /bin/tcc on Zenix, with the development files (build/devel) in the image: its objects for hello.c, a random
#    program and user/zld.c are the cross compiler's byte for byte; "tcc -o" links with zld and the programs run;
#    ps while it compiles zld.c (its memory, emulator figures); a deep nesting is refused with its message.
# "tcc-test.sh --list" prints LIST (tools/tcc-host-test.sh compiles the same programs). Needs debugfs and e2fsck.
cd "$(dirname "$0")/.." || exit 1
# the programs of tests/tests2 run here. Not run: the compile-time tests (60, 96: tools/tcc-host-test.sh); upstream's
# skip (34); other CPUs' asm (85, 98, 99, 127, 138-141, 145); bounds checking and backtraces, not built (112-117, 126,
# 132); shared libraries, threads, TLS (106, 113, 124, 144, 146, 148); MS bitfields (95_bitfields_ms: gcc's m68k-elf
# has no such layout); undefined weak symbols, which zld refuses (104_inline); C11 atomics, which need a helper
# library not built (125, 136, 151); sys/mman.h (119: no mmap); glibc's headers (33: NULL from assert.h) and its
# on_exit (128)
SKIP="60_errors_and_warnings 96_nodata_wanted 34_array_assignment 85_asm-outside-function 98_al_ax_extend
           99_fastcall 127_asm_goto 138_arm64_encoding 139_arm64_errors 140_arm64_extasm 141_riscv_asm
           145_winarm64_interlocked 112_backtrace 113_btdll 114_bound_signal 115_bound_setjmp 116_bound_setjmp2
           117_builtins 126_bound_global 132_bound_test 106_versym 124_atomic_counter 144_tls 146_tls_extern
           148_linker_symbols 95_bitfields_ms 104_inline 125_atomic_misc 136_atomic_gcc_style 151_atomic_header
           119_random_stuff 33_ternary_op 128_run_atexit"
LIST=$(cd ports/tcc/upstream/tests/tests2 && for c in [0-9]*_*.c; do
    n=${c%.c}
    case $n in *+*) continue ;; esac
    [ -f "$n.expect" ] || continue
    case " $(echo $SKIP) " in *" $n "*) continue ;; esac
    echo "$n"
done)
[ "$1" = --list ] && { echo $LIST; exit 0; }
OUT=${ZENIX_OUT:-/tmp/zenix-tcc}; mkdir -p "$OUT"
BD=${ZENIX_BUILD:-build}
P=${TOOLPREFIX:-$(command -v m68k-elf-gcc >/dev/null && echo m68k-elf- || echo "$HOME/opt/m68k-elf/bin/m68k-elf-")}
make -s BUILD=$BD all "$BD/tcc/m68k-tcc" "$BD/devel/usr/lib/libc.a" >"$OUT/build.log" 2>&1 ||
    { echo "FAIL the build"; tail -5 "$OUT/build.log"; exit 1; }
BD=$(cd "$BD" && pwd); OUT=$(cd "$OUT" && pwd)       # (the compiles below run in other directories)
# three runs, as the files do not fit one copy of the root image: run 1 tcc's builds of the test suite's, the ABI
# test's and the random programs (gzip'ed, unpacked one at a time), run 3 gcc's builds of the same, run 2 the
# development files, /bin/tcc's sources and Zenix's own programs
F=$OUT/files1; F2=$OUT/files2; F3=$OUT/files3; rm -rf "$F" "$F2" "$F3"
mkdir -p "$F/t" "$F/out" "$F3/g" "$F3/out" "$F2/out" "$F2/src" "$F2/u"
U=ports/tcc/upstream/tests/tests2
DV=$BD/devel
cc -std=gnu99 -O2 -o "$OUT/zld-pc" user/zld.c || { echo "FAIL the PC build of user/zld.c"; exit 1; }
TC() { TMPDIR="$OUT" TCC_ZLD="$OUT/zld-pc" "$BD/tcc/m68k-tcc" -B"$DV/usr/lib/tcc" "$@"; }
INC=""; for d in "" /pico/gen /pico/m68k /pico/stdio /pico/locale /pico /pico/include /pico/libm; do
    INC="$INC -I$DV/usr/include$d"; done
LIBGCC=$("${P}gcc" -mcpu=68060 -msoft-float -print-libgcc-file-name)
GC() { # gcc's build of a C program: GC OUT FLAGS... FILES...
    local o=$1 a fl=() objs=() k=0; shift
    for a in "$@"; do case $a in *.c) ;; *) fl+=("$a") ;; esac; done
    for a in "$@"; do
        case $a in *.c)
            k=$((k + 1))
            "${P}gcc" -m68020-60 -msoft-float -ffreestanding -fno-builtin -nostdlib -fno-common -w $INC "${fl[@]}" \
                -c "$a" -o "$o.$k.o" || return 1
            objs+=("$o.$k.o") ;;
        esac
    done
    "$OUT/zld-pc" -o "$o" -s 65536 --defsym=vfprintf=__d_vfprintf --defsym=vfscanf=__d_vfscanf "$DV/usr/lib/crt0.o" \
        "${objs[@]}" "$DV/usr/lib/libc.a" "$LIBGCC" && rm -f "${objs[@]}"
}
fail=0; err() { echo "FAIL $1"; fail=1; }
# the test suite's programs; run.sh runs each on Zenix (a timeout, its arguments) into out/
echo 'cd /root/tcc' >"$F/run.sh"
echo 'cd /root/tcc' >"$F3/run.sh"
run1() { # run1 DIR/NAME ARGS: unpack the program, run it, its output and status into out/DIR-NAME (t: run 1, g: 3)
    local r=$F/run.sh; [ "${1%%/*}" = g ] && r=$F3/run.sh
    echo "gzip -dc $1.gz >p; chmod 755 p; timeout 60 ./p $2 >out/${1/\//-} 2>&1; echo \"[status \$?]\" >>out/${1/\//-}; rm p" >>"$r"
}
for n in $LIST; do
    a=""; [ "$n" = 31_args ] && a="arg1 arg2 arg3 arg4 arg5"
    [ "$n" = 46_grep ] && { a="'[^* ]*[:a:d: ]+\\:\\*-/: \$' 46_grep.c"; cp "$U/46_grep.c" "$F/"; cp "$U/46_grep.c" "$F3/"; }
    fl=""; [ "$n" = 76_dollars_in_identifiers ] && fl=-fdollars-in-identifiers
    ex=""; [ "$n" = 120_alias ] && ex=120+_alias.c       # (two files)
    # 101_cleanup: one function with 65,536 cleanup variables, 1.9 MB of code: over the 256 KB the loader took until
    # 2026-11-13 (both builds were refused alike and the test "passed"); tcc's frame for it needs more than 64 KB
    st=65536; [ "$n" = 101_cleanup ] && st=1048576
    (cd "$U" && TC $fl -Wl,-s,$st "$n.c" $ex -o "$F/t/$n") >"$OUT/cc-$n.log" 2>&1 || err "tcc built $n: $(head -2 "$OUT/cc-$n.log")"
    # gcc's build is the reference where upstream's expectation is not this machine's; some tests are tcc's own C
    # (old-style definitions, its extensions) that gcc 16 refuses: those have none
    GC "$F3/g/$n" -O1 -std=gnu17 $fl "$U/$n.c" ${ex:+"$U/$ex"} >"$OUT/gcc-$n.log" 2>&1 || rm -f "$F3/g/$n"
    for w in t g; do run1 "$w/$n" "$a"; done
done
# the ABI test
mkdir -p "$OUT/abi" && python3 tools/tcc-gen.py abi "$OUT/abi" || err "tools/tcc-gen.py abi"
(cd "$OUT/abi" && TC -w -c def.c -o def-t.o && TC -w -c call.c -o call-t.o) || err "tcc compiled the ABI test"
for f in def call; do
    "${P}gcc" -m68020-60 -msoft-float -ffreestanding -fno-builtin -O2 -w $INC -c "$OUT/abi/$f.c" -o "$OUT/abi/$f-g.o" ||
        err "gcc compiled the ABI test"
done
for d in t g; do for c in t g; do
    TC -Wl,-s,65536 -o "$F/t/abi-$d$c" "$OUT/abi/def-$d.o" "$OUT/abi/call-$c.o" || err "linked abi-$d$c"
    run1 "t/abi-$d$c" ""
done; done
# random programs
for s in $(seq 1 20); do
    python3 tools/tcc-gen.py fuzz "$s" >"$OUT/fz$s.c" && TC -w -Wl,-s,65536 "$OUT/fz$s.c" -o "$F/t/fz$s" &&
        GC "$F3/g/fz$s" -O1 -fwrapv "$OUT/fz$s.c" || err "built fz$s"
    for w in t g; do run1 "$w/fz$s" ""; done
done
cp "$F3/g/fz1" "$F2/src/g-fz1"      # (run 2 compares /bin/tcc's build of fz1.c with it)
for f in "$F"/t/* "$F3"/g/*; do gzip -9 "$f"; done
R=$F2/run.sh
echo 'cd /root/tcc' >"$R"
# Zenix's own programs built by tcc: libctest (the C library's checks; the integer printf, as the Makefile links it)
# and coreutils over libz (their inline asm system calls: user/usys.h), each run beside /bin's (gcc's) on the same input
F=$F2
(TC -w -Wl,--defsym=vfprintf=__l_vfprintf -Wl,--defsym=vfscanf=__l_vfscanf user/libctest.c -o "$F/libctest") \
    >"$OUT/cc-libctest.log" 2>&1 || err "tcc built libctest: $(head -2 "$OUT/cc-libctest.log")"
echo 'cd /root/tcc; /root/tcc/libctest >out/libctest 2>&1; echo "[status $?]" >>out/libctest' >>"$R"   # (absolute: it runs itself by argv[0] after a chdir)
printf 'pear\napple\nzoo\nbanana\napple\n12 + 30\n' >"$F/in.txt"
UTILS="cat:in.txt wc:in.txt sort:in.txt sort:-r,in.txt uniq:-c,in.txt rev:in.txt tac:in.txt nl:in.txt head:-n,2,in.txt
       tail:-n,2,in.txt cut:-c1-3,in.txt seq:-w,1,3,20 echo:a,b,c basename:/a/b/c.txt,.txt tr:a-z,A-Z fold:-w,3,in.txt
       printf:%s-%d\\n,x,42 expr:12,+,30 expr:apple,:,ap* grep:-n,an,in.txt sed:s/a/A/g,in.txt"
for u in $UTILS; do
    p=${u%%:*}
    [ -f "$F/u/$p" ] && continue
    ex=""; case $p in expr|sed|grep) ex="user/regex.c" ;; esac
    TZ() { "$BD/tcc/m68k-tcc" -B"$DV/usr/lib/tcc" -nostdinc -I"$DV/usr/lib/tcc/include" -w "$@"; }
    objs=""
    for c in user/$p.c $ex; do
        TZ -c "$c" -o "$OUT/u-$(basename "$c" .c).o" || err "tcc compiled $c"
        objs="$objs $OUT/u-$(basename "$c" .c).o"
    done
    "$OUT/zld-pc" -o "$F/u/$p" -s 32768 "$BD/crt0.o" $objs "$BD/libz.uo" "$LIBGCC" || err "zld linked $p (tcc's)"
done
i=0
for u in $UTILS; do
    p=${u%%:*}; a=$(echo "${u#*:}" | tr ',' ' ')
    i=$((i + 1))
    echo "./u/$p $a <in.txt >out/u-$i 2>&1; echo \"[status \$?]\" >>out/u-$i; /bin/$p $a <in.txt >out/b-$i 2>&1; echo \"[status \$?]\" >>out/b-$i" >>"$R"
done
echo "$i" >"$OUT/nutils"
# (Lua built by tcc, 387 KB, runs in tools/lua-test.sh)
# for /bin/tcc on Zenix: the sources, and the PC's objects from the same command lines in a directory of the same name
cat >"$F/src/hello.c" <<'EOF'
#include <stdio.h>
#include <string.h>
struct pt { short x; char tag[3]; double w; };
static struct pt mid(struct pt a, struct pt b) { struct pt r = { (short)((a.x + b.x) / 2), "mi", (a.w + b.w) / 2 }; return r; }
int main(int argc, char **argv)
{
    struct pt a = { 10, "aa", 1.5 }, b = { 31, "bb", 2.25 }, m = mid(a, b);
    long long big = 1;
    int i;
    for (i = 0; i < 39; i++) big *= 3;
    printf("hello from tcc on zenix: %d %s %.3f %lld %zu\n", m.x, m.tag, m.w, big % 1000003, strlen(argv[0]));
    return 7;
}
EOF
cp "$OUT/fz1.c" "$F/src/fz1.c"; cp user/zld.c "$F/src/zld.c"
python3 -c "n=400; print('int f(int x) { return ' + '('*n + 'x' + '+1)'*n + '; }')" >"$F/src/deep.c"
mkdir -p "$OUT/pcsrc/src" && cp "$F/src"/*.c "$OUT/pcsrc/src/"
(cd "$OUT/pcsrc/src" && for f in hello fz1 zld; do TC -w -c $f.c -o pc-$f.o || exit 1; done) || err "the PC's objects"
# the two runs, each on a fresh root image
H=$BD/zenix.hdf
[ $fail = 0 ] || exit 1
est=0
for run in 1 3 2; do
    rm -f "$H"; make -s BUILD=$BD "$H" >>"$OUT/build.log" 2>&1 || { echo "FAIL the root image"; exit 1; }
    # 24 MB instead of 16: the files below and 101_cleanup's 1.9 MB program unpacked no longer fit beside today's
    # programs (2026-11-17: "no space left on device", the program cut short and refused)
    truncate -s 24M "$H" && resize2fs -f "$H" >>"$OUT/build.log" 2>&1 || { echo "FAIL growing the root image"; exit 1; }
    FF=$OUT/files$run
    {
        if [ $run = 2 ]; then   # the development files tcc uses (not the shared-code ones)
            for d in $(cd "$DV" && find . -mindepth 1 -type d ! -path './usr/lib/zenix*' | sort); do
                debugfs -R "stat ${d#.}" "$H" 2>&1 | grep -q 'Type: directory' || echo "mkdir ${d#.}"; done
            for f in $(cd "$DV" && find . -type f ! -name '*-pure*' ! -path './usr/lib/zenix/*' | sort); do
                echo "write $DV/${f#./} ${f#.}"; done
        fi
        echo "mkdir /root/tcc"
        for d in $(cd "$FF" && find . -mindepth 1 -type d | sort); do echo "mkdir /root/tcc/${d#./}"; done
        for f in $(cd "$FF" && find . -type f | sort); do echo "write $FF/${f#./} /root/tcc/${f#./}"; done
    } >"$OUT/debugfs$run.cmd"
    debugfs -w -f "$OUT/debugfs$run.cmd" "$H" >"$OUT/debugfs$run.log" 2>&1
    if grep -q 'Could not\|^[a-z]*: [A-Z]' "$OUT/debugfs$run.log" || ! e2fsck -fn "$H" >"$OUT/e2fsck-before$run.log" 2>&1; then
        echo "FAIL run $run's files on the image (debugfs$run.log)"; exit 1
    fi
    sc=tools/tests/tcc-test.txt; [ $run != 2 ] && sc=tools/tests/tcc-test1.txt
    ZENIX_BUILD=$BD tools/run-test.sh fsuae/a1200-020-fast.fs-uae 7200 $sc 2>&1 | tr -d '\r' >"$OUT/run$run.log"
    rm -rf "$OUT/z$run"; mkdir -p "$OUT/z$run"
    debugfs -R "rdump /root/tcc/out $OUT/z$run" "$H" >/dev/null 2>&1
    if [ $run = 2 ]; then
        for f in hello.o fz1.o zld.o; do debugfs -R "dump /root/tcc/src/$f $OUT/z2/$f" "$H" >/dev/null 2>&1; done
    fi
    e2fsck -fn "$H" >"$OUT/e2fsck$run.log" 2>&1 || est=$?
done
python3 - "$OUT" "$est" "$U" "$LIST" <<'PY'
import os, re, sys
sys.path.insert(0, "tools")
from screenlog import Log
out, est, up, names = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4].split()
L1 = Log(os.path.join(out, "run1.log"))
L3 = Log(os.path.join(out, "run3.log"))
L = Log(os.path.join(out, "run2.log"))
check, t = L.check, L.text
z = os.path.join(out, "z1", "out")
z2 = os.path.join(out, "z2", "out")
z3 = os.path.join(out, "z3", "out")
def rd(p):
    return open(p, encoding="utf-8", errors="surrogateescape").read() if os.path.exists(p) else None
def run(w, n):
    s = rd(os.path.join(z if w == "t" else z3, "%s-%s" % (w, n)))
    if s is None:
        return None, None
    m = re.search(r"\[status (\d+)\]\n?$", s)
    return (s[:m.start()] if m else s), (int(m.group(1)) if m else None)
exact, asgcc, bad = 0, [], []
for n in names:
    cc = re.sub(r"^.*/", "", rd(os.path.join(out, "cc-%s.log" % n)) or "", flags=re.M)
    tout, tst = run("t", n)
    gout, gst = run("g", n)
    if tout is None:
        bad.append(n + " (no output)")
        continue
    exp = rd(os.path.join(up, n + ".expect"))
    if exp is not None and (cc + tout).split() == exp.split():
        exact += 1
    elif gout is not None and tout == gout and tst == gst:
        asgcc.append(n)
    else:
        bad.append(n)
check(not bad, "the test suite: %d programs print upstream's expectation, %d gcc's program's output (%s)"
      % (exact, len(asgcc), " ".join(asgcc)), bad)
for p in ("tt", "tg", "gt", "gg"):
    s = rd(os.path.join(z, "t-abi-" + p)) or ""
    check(re.search(r"^abi: 0 bad$", s, re.M) is not None and "[status 0]" in s,
          "ABI: functions by %s called by %s" % ({"t": "tcc", "g": "gcc"}[p[0]], {"t": "tcc", "g": "gcc"}[p[1]]),
          s[-300:])
s = rd(os.path.join(z2, "libctest")) or ""
m = re.search(r"^libctest: (\d+) checks, (\d+) failed$", s, re.M)
check(m is not None and m.group(2) == "0" and int(m.group(1)) > 100 and "[status 0]" in s,
      "libctest built by tcc: every check passes (%s)" % (m.group(1) if m else "?"),
      re.findall(r"^FAIL.*", s, re.M)[:5] or s[-200:])
nu = int(open(os.path.join(out, "nutils")).read())
ud = [i for i in range(1, nu + 1) if rd(os.path.join(z2, "u-%d" % i)) is None
      or rd(os.path.join(z2, "u-%d" % i)) != rd(os.path.join(z2, "b-%d" % i))]
check(not ud, "%d runs of 20 programs over libz (cat, wc, sort, sed, grep, expr ...) built by tcc: the output and "
      "status of /bin's (gcc's)" % nu, ud)
fz = [s for s in range(1, 21) if rd(os.path.join(z, "t-fz%d" % s)) != rd(os.path.join(z3, "g-fz%d" % s))
      or not rd(os.path.join(z, "t-fz%d" % s))]
check(not fz, "20 random programs: tcc's and gcc's print the same", fz)
for f in ("hello", "fz1", "zld"):
    a, b = os.path.join(out, "z2", f + ".o"), os.path.join(out, "pcsrc", "src", "pc-%s.o" % f)
    same = os.path.exists(a) and open(a, "rb").read() == open(b, "rb").read()
    check(same, "tcc on Zenix compiled %s.c: the PC's object byte for byte (%d bytes)" % (f, os.path.getsize(b)),
          os.path.getsize(a) if os.path.exists(a) else "missing")
for st, what in (("hst", "hello.c"), ("fst", "fz1.c"), ("zst", "zld.c")):
    check(L.status(st) == 0, "tcc -c %s on Zenix: status 0" % what, L.status(st))
check(L.status("hrun") == 7 and re.search(r"^hello from tcc on zenix: 20 mi 1\.875 \d+ \d+$", t, re.M) is not None,
      "tcc -o hello hello.c on Zenix (zld, -lc -lgcc): it runs, its line and status 7", L.status("hrun"))
check(L.status("frun") == 0 and L.status("fcmp") == 0, "tcc -o fz1 on Zenix: the program prints what gcc's does",
      (L.status("frun"), L.status("fcmp")))
check(L.status("dst") == 1 and re.search(r"^deep\.c:1: error: nested too deeply for tcc's stack$", t, re.M) is not None,
      "400 levels of brackets: refused with the stack's message, status 1", L.status("dst"))
check(est == 0, "the host's e2fsck finds the root clean", est)
check("overflowed its user stack" not in t + L1.text + L3.text, "the stack guard never fired",
      re.findall(r".*overflowed.*", t + L1.text + L3.text))
check("no space left" not in t + L1.text + L3.text, "the image had room", re.findall(r".*no space left.*", t + L1.text + L3.text)[:3])
ps = [int(a) for a in re.findall(r"^ +\d+ +\d+ +\d+ +-?\d+ +\w+ +(\d+) +\d+ tcc$", t, re.M)]
fr = re.findall(r"^(?:chip RAM|fast RAM|RAM) free: (\d+) KB$", t, re.M)
times = re.findall(r"^real\s+(\S+)$", t, re.M)
check(len(ps) > 0, "ps saw tcc while it compiled zld.c", t[-400:])
if ps:
    print("     (emulator figures, A1200 with 4 MB fast RAM: tcc compiling zld.c (2,200 lines) owned at most %d KB in %d "
          "samples; free before: %s KB; times: hello.c %s, fz1.c %s)" % (max(ps), len(ps), " / ".join(fr[:2]),
          times[0] if times else "?", times[1] if len(times) > 1 else "?"))
L1.waits()
L3.waits()
L.waits()
L.bad += L1.bad + L3.bad
L.done()
PY
