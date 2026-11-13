#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# tcc on the PC (DESIGN D52 section 7, ports/tcc/README): the cross compiler the Makefile builds from ports/tcc
# (build/tcc/m68k-tcc), and the same source under AddressSanitizer and UBSan. Checked:
#  - every program of upstream's test suite (tests/tests2, the ones tools/tcc-test.sh runs) compiles to an object
#    under the sanitizers, never a report, and the object is the plain build's byte for byte;
#  - the compile-time tests (60_errors_and_warnings, 96_nodata_wanted: each "#if defined test_..." part alone, as
#    upstream's -dt does) give upstream's errors and warnings, line for line;
#  - an object is a big-endian 68k ELF relocatable file gcc's binutils read;
#  - tools/tcc-gen.py's ABI sources and 200 of its random programs compile (tools/tcc-test.sh runs them);
#  - 400 damaged sources (bytes of the test suite's files cut, doubled or changed; 3000 with LONG=1) are each
#    compiled or refused with a message and status 1 under the sanitizers: never a crash.
# Running the programs is tools/tcc-test.sh's (FS-UAE). Needs the build (make) and gcc's m68k binutils (TOOLPREFIX).
cd "$(dirname "$0")/.." || exit 1
B=${ZENIX_BUILD:-build}
P=${TOOLPREFIX:-$(command -v m68k-elf-gcc >/dev/null && echo m68k-elf- || echo "$HOME/opt/m68k-elf/bin/m68k-elf-")}
T=$(mktemp -d "${TMPDIR:-/tmp}/zenix-tcc.XXXXXX")
trap 'rm -rf "$T"' EXIT
fail=0
check() { if [ "$1" = 0 ]; then echo "PASS $2"; else echo "FAIL $2${3:+: $3}"; fail=1; fi; }
make -s BUILD="$B" "$B/tcc/m68k-tcc" "$B/devel/usr/lib/libc.a" >"$T/make.log" 2>&1
check $? "the cross compiler and the development files (make)" "$(tail -3 "$T/make.log")"
[ $fail = 0 ] || exit 1
S=$B/tcc/src
cc -O1 -g -w -fsanitize=address,undefined -fno-sanitize-recover=all -DTCC_TARGET_M68K -DONE_SOURCE=1 -I"$S" \
    -o "$T/tcc-asan" "$S/tcc.c" -lm 2>"$T/cc.log"
check $? "the cross compiler under AddressSanitizer and UBSan" "$(head -3 "$T/cc.log")"
[ $fail = 0 ] || exit 1
export ASAN_OPTIONS=detect_leaks=0
D=$B/devel/usr/lib/tcc
TCC="$B/tcc/m68k-tcc -B$D"
TA="$T/tcc-asan -B$D"
U=ports/tcc/upstream/tests/tests2
# the programs (tools/tcc-test.sh's list): compiled by both builds, the objects compared
n=0; bad=""
for c in $(tools/tcc-test.sh --list); do
    f=$U/$c.c
    n=$((n + 1))
    if ! $TA -w -c "$f" -o "$T/a.o" >"$T/a.log" 2>&1 || ! $TCC -w -c "$f" -o "$T/b.o" >/dev/null 2>&1 ||
       ! cmp -s "$T/a.o" "$T/b.o"; then
        bad="$bad $c"
    fi
done
check "$([ -z "$bad" ] && echo 0 || echo 1)" "the test suite's $n programs compile under the sanitizers, the same objects" "$bad"
# the compile-time tests, part by part
for c in 60_errors_and_warnings 96_nodata_wanted; do
    exp=$(grep -E "^$c\.c:[0-9]+: (error|warning): " "$U/$c.expect")
    got=$(for t in $($TCC -dt -E "$U/$c.c" 2>/dev/null | sed -n 's/^\[\(test_.*\)\]$/\1/p'); do
              $TA -D"$t" -c "$U/$c.c" -o "$T/d.o" 2>&1 | sed -e "s,^$U/,," -e 's,^tcc: ,,'
          done | grep -E "^$c\.c:[0-9]+: (error|warning): ")
    [ "$exp" = "$got" ]
    check $? "$c: upstream's errors and warnings, part by part ($(echo "$exp" | wc -l) lines)" \
          "$(diff <(echo "$exp") <(echo "$got") | head -4 | tr '\n' ' ')"
done
# the object format
printf 'int g = 5;\nint add(int a, int b) { return a + b * g; }\n' >"$T/o.c"
$TCC -c "$T/o.c" -o "$T/o.o" && "${P}readelf" -h "$T/o.o" >"$T/h.txt" 2>&1 &&
    grep -q 'big endian' "$T/h.txt" && grep -q 'REL (Relocatable' "$T/h.txt" && grep -q 'MC68000' "$T/h.txt" &&
    "${P}objdump" -d "$T/o.o" | grep -q 'mulsl'
check $? "an object: big-endian 68k ELF, relocatable, read by binutils (muls.l in add)"
# the inline assembler (ports/tcc/zenix/m68k-asm.c): each line assembled at file scope gives GNU as's bytes for it
# (the forms with more than one encoding are written so gas and tcc pick the same), and forms gas refuses are refused
cat >"$T/asm.txt" <<'ASM'
move.l %d0,-(%sp)
move.l (%sp)+,%d7
move.w %sr,%d1
move.l %sp,%d2
move.b (%a0)+,(%a1)+
move.l 8(%a6),%d0
move.l -4(%a6),12(%a5)
move.l (8,%a0,%d1.l*4),%d0
move.w (2,%a0,%a1.w),%d3
movea.l %d0,%a0
moveq #-5,%d4
lea 100(%a2),%a3
pea 16(%a6)
add.l %d1,%d0
add.w (%a0),%d2
add.l %d0,4(%a1)
adda.l %d0,%a1
addq.l #8,%d3
subq.w #1,%a2
sub.l #300,%d6
cmp.l %d1,%d0
cmpa.l %a0,%a1
eor.l %d2,%d3
not.l %d0
neg.b %d1
clr.w 6(%a0)
tst.l %d5
ext.l %d0
extb.l %d1
swap %d2
lsl.l #3,%d0
lsr.l %d1,%d2
asr.w #8,%d3
rol.l #1,%d4
muls.l %d1,%d0
mulu.w (%a0),%d2
divs.l #10,%d3
divu.l %d1,%d4
divsl.l %d1,%d2:%d3
divs.w %d1,%d0
trap #0
illegal
nop
rts
link %a6,#-8
unlk %a6
movem.l %d2-%d4/%a2,-(%sp)
movem.l (%sp)+,%d2-%d4/%a2
btst #3,%d0
bset %d1,(%a0)
exg %d0,%a1
seq %d0
jsr (%a1)
jmp 4(%a0)
ASM
bad=""
while IFS= read -r i; do
    printf '__asm__(".text\\n\\t%s\\n");\n' "$i" >"$T/a.c"
    printf '\t.text\n\t%s\n' "$i" >"$T/a.s"
    "$B/tcc/m68k-tcc" -B"$D" -c "$T/a.c" -o "$T/a.o" >"$T/a.log" 2>&1 &&
        "${P}as" -m68020 "$T/a.s" -o "$T/g.o" 2>>"$T/a.log" &&
        "${P}objcopy" -O binary -j .text "$T/a.o" "$T/a.bin" && "${P}objcopy" -O binary -j .text "$T/g.o" "$T/g.bin" &&
        cmp -s "$T/a.bin" "$T/g.bin" || bad="$bad [$i]"
done <"$T/asm.txt"
check "$([ -z "$bad" ] && echo 0 || echo 1)" "inline asm: $(wc -l <"$T/asm.txt") instruction forms, GNU as's bytes" "$bad"
bad=""
for i in 'and.l %d0,%a0' 'st #1' 'addq.b #1,%a0' 'ext.b %d0' 'movem.l %d0-%d1,(%a0)+' 'movem.l -(%a0),%d0-%d1' \
         'muls.l %d1,%d2:%d3' 'move.b %a0,%d0' 'bset #3,%a0' 'clr.l %a0' 'tst.b %a0' 'move.l %d0,#4' 'frob %d0' \
         'moveq #300,%d0' 'trap #16' 'move.l (%a0' 'move.l %q9,%d0' '.long 100-1b'; do
    printf '__asm__(".text\\n1:\\t%s\\n");\n' "$i" >"$T/a.c"
    $TA -c "$T/a.c" -o "$T/a.o" >"$T/a.log" 2>&1
    st=$?
    { [ $st = 1 ] && grep -q 'error' "$T/a.log"; } || bad="$bad [$i]($st)"
done
check "$([ -z "$bad" ] && echo 0 || echo 1)" "inline asm: 18 wrong forms refused with a message (under the sanitizers)" "$bad"
# the generated programs
mkdir -p "$T/abi" && python3 tools/tcc-gen.py abi "$T/abi" && $TA -w -c "$T/abi/def.c" -o "$T/abi/d.o" &&
    $TA -w -c "$T/abi/call.c" -o "$T/abi/c.o"
check $? "the ABI test's sources (tools/tcc-gen.py abi) compile"
bad=""
for s in $(seq 1 200); do
    python3 tools/tcc-gen.py fuzz "$s" >"$T/f.c" && $TA -w -c "$T/f.c" -o "$T/f.o" >"$T/f.log" 2>&1 || bad="$bad $s"
done
check "$([ -z "$bad" ] && echo 0 || echo 1)" "200 random programs (tools/tcc-gen.py fuzz) compile under the sanitizers" "$bad"
# damaged sources: compiled or refused, never a crash
N=400; [ -n "$LONG" ] && N=3000
python3 - "$U" "$T/m" "$N" <<'PY'
import os, random, sys
src, out, n = sys.argv[1], sys.argv[2], int(sys.argv[3])
os.makedirs(out, exist_ok=True)
files = sorted(f for f in os.listdir(src) if f.endswith('.c'))
random.seed(52)
for i in range(n):
    b = bytearray(open(os.path.join(src, random.choice(files)), 'rb').read())
    for _ in range(random.randint(1, 6)):
        k = random.randint(0, max(0, len(b) - 1)); m = random.randint(1, 40)
        r = random.random()
        if r < 0.4:
            del b[k:k + m]
        elif r < 0.7:
            b[k:k] = b[k:k + m]
        else:
            b[k:k + 1] = bytes([random.choice(b'{}()[];,*&|^<>=+-/%#"\'\\\n 0123456789abcxyz_.?:!~\x00\xff')])
    open(os.path.join(out, '%04d.c' % i), 'wb').write(b)
PY
bad=""
for f in "$T"/m/*.c; do
    timeout 20 $TA -w -I"$U" -c "$f" -o "$T/m.o" >"$T/m.log" 2>&1
    st=$?
    if [ $st -gt 1 ] || grep -q -E 'AddressSanitizer|runtime error|internal compiler error' "$T/m.log"; then
        bad="$bad $(basename "$f")($st)"
        cp "$f" "$T/m.log" "${TMPDIR:-/tmp}/" 2>/dev/null
    fi
done
check "$([ -z "$bad" ] && echo 0 || echo 1)" "$N damaged sources: compiled or refused, never a crash" "$bad"
exit $fail
