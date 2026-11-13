#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# zld on the PC (DESIGN D52 section 5, docs/man/zld.txt): user/zld.c built for the PC, then
#  - every program the build links with link/user.ld or link/pure.ld and tools/mkzexe.py (the command lines read from
#    `make -n -B`, tools/zld-links.py) linked again by zld from the same objects and archives: each must be the
#    build's .zx/.pzx byte for byte;
#  - links that must fail, each with its message naming the symbol and the object: an undefined reference, a
#    duplicate definition, a common symbol, a 16-bit PC-relative branch out of reach, a relocation a program cannot
#    carry, no crt0, --defsym's limits, ZEX2's refusals (tools/mkzexe.py's), a crt0 shorter than its header, a
#    program past 4 GB; and no output file is left by a failed link;
#  - the cases in tools/tests/zld (merged constants and string tails, empty aligned sections, --wrap, an archive with
#    two members of one name, ZEX2's GOT classes mixed) linked by ld + tools/mkzexe.py and by zld: the same bytes;
#  - damaged objects and archives (a fixed set of random byte changes and cuts, ZLD_FUZZ of each, default 400) given to
#    a build under AddressSanitizer and UBSan: each must link or be refused with a message, never crash.
# ZENIX_BUILD names the build directory (default build), which must be complete (make); ZLD_KEEP=DIR keeps the damaged
# inputs that failed there.
cd "$(dirname "$0")/.." || exit 1
B=${ZENIX_BUILD:-build}
P=${TOOLPREFIX:-$(command -v m68k-elf-gcc >/dev/null && echo m68k-elf- || echo "$HOME/opt/m68k-elf/bin/m68k-elf-")}
T=$(mktemp -d "${TMPDIR:-/tmp}/zenix-zld.XXXXXX")
trap 'rm -rf "$T"' EXIT
fail=0
check() { if [ "$1" = 0 ]; then echo "PASS $2"; else echo "FAIL $2${3:+: $3}"; fail=1; fi; }
cc -std=gnu99 -O2 -Wall -Wextra -Werror -o "$T/zld" user/zld.c 2>"$T/cc.log"
check $? "the PC build of user/zld.c" "$(head -3 "$T/cc.log")"
cc -std=gnu99 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -o "$T/zld-asan" user/zld.c 2>"$T/cc2.log"
check $? "the PC build of user/zld.c with AddressSanitizer and UBSan" "$(head -3 "$T/cc2.log")"
[ $fail = 0 ] || exit 1
# leak detection off: zld frees its tables by ending (the system takes a process's memory back)
export ASAN_OPTIONS=detect_leaks=0

# ---- every program of the build ----
make -n -B BUILD="$B" TOOLPREFIX="$P" all >"$T/make.txt" 2>"$T/make.err"
check $? "make -n -B (the build's commands)" "$(head -3 "$T/make.err")"
python3 tools/zld-links.py <"$T/make.txt" >"$T/links.tsv"
check $? "the program links found (tools/zld-links.py)"
n=0; same=0; diffs=""
while IFS=$'\t' read -r -a a; do
    out=${a[0]}
    n=$((n + 1))
    if "$T/zld" -o "$T/p.zx" "${a[@]:1}" 2>"$T/e.txt" && cmp -s "$T/p.zx" "$out"; then
        same=$((same + 1))
    else
        diffs="$diffs $(basename "$out")"
    fi
done <"$T/links.tsv"
[ "$n" -ge 150 ] && [ "$same" = "$n" ]
check $? "$same of $n programs linked by zld are the build's byte for byte" "differ:$diffs $(head -2 "$T/e.txt")"
for p in sh.pzx lua.zx libctest.pzx make-probe.zx; do
    grep -q "^$B/$p	" "$T/links.tsv"
    check $? "among them $p"
done

# ---- links that must fail ----
CF="-m68020-60 -msoft-float -ffreestanding -fno-builtin -nostdlib -O2 -ffunction-sections -fdata-sections"
mk() { printf '%s\n' "$2" >"$T/$1"; "${P}gcc" $CF ${3:-} -c "$T/$1" -o "$T/${1%.*}.o" 2>>"$T/gcc.log"; }
mk ok.c 'int main(void) { return 0; }'
mk undef.c 'void missing(void); int main(void) { missing(); return 0; }'
mk twice1.c 'int twice(void) { return 1; } int main(void) { return twice(); }'
mk twice2.c 'int twice(void) { return 2; }'
mk comm.c 'int shared_c; int main(void) { return shared_c; }' -fcommon
mk far1.S '	.section .text.main,"ax"
	.globl main
main:	bsr.w far
	rts'
mk far2.S '	.section .text.big,"ax"
	.globl far
	.skip 40000
far:	rts'
mk abs16.S '	.section .text.main,"ax"
	.globl main
main:	rts
	.word main'
check $? "the test objects compile" "$(head -3 "$T/gcc.log")"
L="$B/crt0.o"
Z="$B/libz.uo $("${P}gcc" -mcpu=68060 -msoft-float -print-libgcc-file-name)"
must_fail() {   # NAME EXPECTED-TEXT ARGS...
    local name=$1 want=$2
    shift 2
    rm -f "$T/bad.zx"
    "$T/zld-asan" -o "$T/bad.zx" "$@" >"$T/out.txt" 2>&1
    local st=$?
    [ $st = 1 ] && grep -qF -- "$want" "$T/out.txt" && [ ! -e "$T/bad.zx" ]
    check $? "$name: refused, no output, the message names it" "status $st: $(head -2 "$T/out.txt")"
}
"$T/zld-asan" -o "$T/ok.zx" $L "$T/ok.o" $Z 2>"$T/out.txt"
check $? "a small program links" "$(head -2 "$T/out.txt")"
must_fail "undefined reference" "$T/undef.o: undefined reference to \`missing'" $L "$T/undef.o" $Z
must_fail "duplicate definition" "multiple definition of \`twice': $T/twice1.o and $T/twice2.o" $L "$T/twice1.o" "$T/twice2.o" $Z
must_fail "common symbol" "$T/comm.o: shared_c is a common symbol" $L "$T/comm.o" $Z
must_fail "PC-relative overflow" "$T/far1.o: R_68K_PC16 at .text.main+0x2: far is out of reach" $L "$T/far1.o" "$T/far2.o" $Z
must_fail "a relocation a program cannot carry" "$T/abs16.o: unsupported relocation R_68K_16" $L "$T/abs16.o" $Z
must_fail "no crt0" "no _start" "$T/ok.o" $Z
must_fail "not an object" "not an ELF object or an archive" $L "$T/undef.c" $Z
must_fail "a missing library" "cannot find libnothere.a" $L "$T/ok.o" -lnothere
must_fail "--defsym with an expression" "only a symbol or a number" --defsym=a=b+4 $L "$T/ok.o" $Z
must_fail "--defsym to an undefined symbol" "--defsym a: undefined symbol \`nowhere'" --defsym=a=nowhere $L "$T/ok.o" $Z

# ---- small cases against ld + tools/mkzexe.py (tools/tests/zld: what the build's programs do not use) ----
LG=${Z#* }
for f in tools/tests/zld/*.S; do
    "${P}gcc" -m68020-60 -c "$f" -o "$T/$(basename "${f%.S}").o" 2>>"$T/gcc.log" || echo "FAIL $f: $(tail -2 "$T/gcc.log")"
done
mkdir -p "$T/d1" "$T/d2"
cp "$T/dup1.o" "$T/d1/averyveryverylongmembername.o"; cp "$T/dup2.o" "$T/d2/averyveryverylongmembername.o"
"${P}ar" q "$T/dup.a" "$T/d1/averyveryverylongmembername.o" "$T/d2/averyveryverylongmembername.o" 2>/dev/null
"${P}ranlib" "$T/dup.a"
vs_ld() {   # NAME user|pure OPTIONS -- FILES...
    local name=$1 lay=$2 crt lib opts=()
    shift 2
    while [ "$1" != -- ]; do opts+=("$1"); shift; done
    shift
    if [ "$lay" = user ]; then crt=$B/crt0.o lib=$B/libz.uo; else crt=$B/crt0-pure.o lib=$B/libz.po; fi
    local wl=()
    for o in "${opts[@]}"; do wl+=("-Wl,$o"); done
    "${P}gcc" -m68020-60 -msoft-float -nostdlib -nostartfiles -T link/$lay.ld -Wl,--build-id=none -Wl,-q \
        -Wl,--gc-sections "${wl[@]}" -o "$T/$name.elf" "$crt" "$@" "$lib" $LG >"$T/ld.txt" 2>&1 &&
        TOOLPREFIX=$P python3 tools/mkzexe.py "$T/$name.elf" "$T/$name.ref" 0 >>"$T/ld.txt" 2>&1 &&
        "$T/zld-asan" "${opts[@]}" -o "$T/$name.zx" "$crt" "$@" "$lib" $LG >"$T/out.txt" 2>&1 &&
        cmp -s "$T/$name.ref" "$T/$name.zx"
    check $? "$name: zld's program is ld + mkzexe.py's byte for byte" "$(tail -2 "$T/ld.txt") $(head -2 "$T/out.txt")"
}
vs_ld "merged constants and strings across objects" user -- "$T/c1.o" "$T/c2.o"
vs_ld "string tails merged, a label at a merged section's end" user -- "$T/m0.o" "$T/m2.o"
vs_ld "an empty 64-aligned .rodata" user -- "$T/t1.o"
vs_ld "an empty 64-aligned .init_array" user -- "$T/t1b.o"
vs_ld "a referenced empty 64-aligned section" user -- "$T/t1c.o"
vs_ld "--wrap" user --wrap=foo -- "$T/wr.o"
vs_ld "an archive with two members of one name" user -- "$T/dm.o" "$T/dup.a"
vs_ld "ZEX2: GOT8O, GOT16O and GOT32O mixed over two objects" pure -- "$T/g2.o" "$T/g3.o"
vs_ld "ZEX2: the same, the objects the other way round" pure -- "$T/g3.o" "$T/g2.o"
LP="$B/crt0-pure.o"
ZP="$B/libz.po $LG"
must_fail "ZEX2: a writable section in the shared part" "writable section .rodata.cnt in the shared part" $LP "$T/wsec.o" $ZP
must_fail "ZEX2: an absolute address in the code" "a relocation in the shared part" $LP "$T/abs32.o" $ZP
must_fail "ZEX2: a PC-relative reference from code to data" "a PC-relative reference from the shared part to the data part" $LP "$T/pcx.o" $ZP
must_fail "ZEX2: 8-bit table offsets past 32 entries" "reached with 8-bit offsets (at most 32)" $LP "$T/g4.o" $ZP
must_fail "a crt0 shorter than the ZEXE header" "shorter than the ZEXE header" "$T/tiny.o" "$T/ok.o" $Z
must_fail "a crt0 shorter than the ZEX2 header" "shorter than the ZEX2 header" "$T/tiny2.o" "$T/ok.o" $ZP
must_fail "a program past 4 GB" "the program is larger than 4 GB" $L "$T/big.o" $Z

# ---- damaged objects and archives, under the sanitizers ----
"${P}ar" rcs "$T/small.a" "$T/twice2.o" "$T/ok.o" 2>/dev/null
cp "$T/twice1.o" "$T/m.o"
N=${ZLD_FUZZ:-400}
python3 - "$T" "$N" "$L" "$Z" >"$T/fz.txt" 2>"$T/fz.err" <<'EOF'
import os, random, subprocess, sys
T, N, crt0, Z = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4].split()
rng = random.Random(52)
bad = 0
kinds = {"ok": 0, "refused": 0}
srcs = [(crt0, "c.o"), (os.path.join(T, "undef.o"), "u.o"), (os.path.join(T, "small.a"), "s.a"), (Z[0], "z.o")]
for i in range(N):
    src, name = srcs[i % len(srcs)]
    d = bytearray(open(src, "rb").read())
    if rng.random() < 0.15:
        d = d[:rng.randrange(len(d))]
    else:
        for _ in range(rng.randint(1, 8)):
            if rng.random() < 0.6:
                p = rng.randrange(min(len(d), 1200))   # the headers, section and symbol tables of small objects
            else:
                p = rng.randrange(len(d))
            d[p] = rng.randrange(256) if rng.random() < 0.7 else d[p] ^ (1 << rng.randrange(8))
    f = os.path.join(T, "fz-" + name)
    open(f, "wb").write(d)
    args = [crt0 if name != "c.o" else f, os.path.join(T, "m.o") if name != "u.o" else f]
    args += [f if name in ("s.a", "z.o") else Z[0], Z[1]]
    try:
        r = subprocess.run([os.path.join(T, "zld-asan"), "-o", os.path.join(T, "fz.zx")] + args,
                           capture_output=True, text=True, errors="replace", timeout=20)
    except subprocess.TimeoutExpired:
        r = None
    if r is None:
        bad += 1
        os.rename(f, f + ".hang%d" % i)
        sys.stderr.write("case %d (%s): no answer in 20 s\n" % (i, name))
    elif r.returncode == 0:
        kinds["ok"] += 1
    elif r.returncode == 1 and r.stderr.startswith("zld: ") and "Sanitizer" not in r.stderr and "runtime error" not in r.stderr:
        kinds["refused"] += 1
    else:
        bad += 1
        if bad <= 3:
            sys.stderr.write("case %d (%s): status %d: %s\n" % (i, name, r.returncode, r.stderr[-600:]))
            os.rename(f, f + ".case%d" % i)
print("fuzz %d %d %d" % (kinds["ok"], kinds["refused"], bad))
EOF
read -r _ fok fref fbad <"$T/fz.txt"
[ -n "$ZLD_KEEP" ] && cp "$T"/fz-*.hang* "$T"/fz-*.case* "$ZLD_KEEP" 2>/dev/null
[ "${fbad:-1}" = 0 ] && [ $((fok + fref)) = "$N" ]
check $? "$N damaged inputs: $fok linked, $fref refused with a message, none crashed" "$(head -5 "$T/fz.err")"
exit $fail
