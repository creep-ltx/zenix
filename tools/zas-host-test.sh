#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# zas on the PC (DESIGN D52 section 6, docs/man/zas.txt): user/zas.c built for the PC, then
#  - the corpus: the whole build made again in a scratch folder with an `as` that keeps a copy of everything it is
#    given (gcc's output for every C file, every .S after cpp) and runs GNU as; zas assembles each of them with the
#    same options and must give the build's object: byte for byte, or byte for byte once objcopy --strip-debug has
#    taken the debug sections from both (zas makes none);
#  - the instruction table: tools/zas-table.py's lines (every mnemonic, size and operand combination of a pool that
#    covers the addressing modes) for eight CPU settings: zas must refuse exactly the lines GNU as refuses, and give
#    its object for the lines it takes (whole, and in pieces of 200 lines: a difference is shown by line);
#  - the cases in tools/tests/zas (branch and operand relaxation near and far, directives, macros, conditionals,
#    expressions, relocations): gas's object;
#  - sources that must fail, each with its message, status 1 and no object left;
#  - damaged sources (ZAS_FUZZ of them, default 400; random byte changes, cut and doubled lines) given to a build
#    under AddressSanitizer and UBSan: each assembles or is refused with status 1, never a crash.
# ZENIX_BUILD names the build directory the corpus copies its settings from (default build).
cd "$(dirname "$0")/.." || exit 1
P=${TOOLPREFIX:-$(command -v m68k-elf-gcc >/dev/null && echo m68k-elf- || echo "$HOME/opt/m68k-elf/bin/m68k-elf-")}
T=$(mktemp -d "${TMPDIR:-/tmp}/zenix-zas.XXXXXX")
trap '[ -n "$ZAS_KEEP_T" ] || rm -rf "$T"' EXIT
fail=0
check() { if [ "$1" = 0 ]; then echo "PASS $2"; else echo "FAIL $2${3:+: $3}"; fail=1; fi; }
cc -std=gnu99 -O2 -Wall -Wextra -Werror -o "$T/zas" user/zas.c 2>"$T/cc.log"
check $? "the PC build of user/zas.c" "$(head -3 "$T/cc.log")"
cc -std=gnu99 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -o "$T/zas-asan" user/zas.c 2>"$T/cc2.log"
check $? "the PC build of user/zas.c with AddressSanitizer and UBSan" "$(head -3 "$T/cc2.log")"
GCC=$(command -v "${P}gcc")
AS=$("$GCC" -print-prog-name=as)
[ -x "$AS" ] || AS=$(command -v "${P}as")
[ $fail = 0 ] && [ -x "$AS" ] || { echo "FAIL no GNU as (${P}as)"; exit 1; }
export ASAN_OPTIONS=detect_leaks=0     # zas frees its tables by ending (the system takes a process's memory back)

# ---- the corpus: every assembler input of the build ----
W=$T/wrap
mkdir -p "$W" "$T/corpus"
for t in "$(dirname "$GCC")"/"$(basename "${P}")"*; do
    ln -s "$t" "$W/$(basename "$t")"
done
rm -f "$W/$(basename "${P}")gcc"
cat >"$W/$(basename "${P}")gcc" <<EOF
#!/usr/bin/env bash
exec "$GCC" -B"$W/" "\$@"
EOF
cat >"$W/as" <<EOF
#!/usr/bin/env bash
# the build's assembler: a copy of the input and the options, then GNU as
n=\$(mktemp "$T/corpus/s.XXXXXXXX")
in=""; for a in "\$@"; do case \$a in *.s|*.S) in=\$a ;; esac; done
[ -n "\$in" ] && cp "\$in" "\$n.s" && printf '%s\n' "\$@" >"\$n.args"
exec "$AS" "\$@"
EOF
chmod +x "$W/$(basename "${P}")gcc" "$W/as"
make -s -j"${ZAS_JOBS:-4}" BUILD="$T/build" TOOLPREFIX="$W/$(basename "${P}")" all >"$T/make.log" 2>&1
check $? "the build, every assembler input kept" "$(tail -3 "$T/make.log")"
python3 - "$T" "${P}objcopy" "$AS" <<'EOF'
import concurrent.futures, glob, os, re, subprocess, sys
T, objcopy, AS = sys.argv[1:4]
def nodebug(src, dst):
    """the source without its debugging data (.loc, .cfi_*, .file N, the .debug_* sections): then no objcopy is needed
    and the two objects are compared as written"""
    out, skip = [], False
    for l in open(src, encoding="latin1"):
        w = l.split()
        if w and w[0] in (".section", ".pushsection", ".text", ".data", ".bss", ".previous", ".popsection"):
            skip = w[0] in (".section", ".pushsection") and len(w) > 1 and w[1].startswith(".debug")
        if skip or (w and (w[0] in (".loc", ".loc_mark_labels") or w[0].startswith(".cfi_") or
                           (w[0] == ".file" and len(w) > 1 and w[1][0].isdigit()))):
            continue
        out.append(l)
    open(dst, "w", encoding="latin1").write("".join(out))
def one(s):
    a = open(s[:-2] + ".args").read().split("\n")
    out, opts, skip = None, [], False
    for i, x in enumerate(a):
        if skip:
            skip = False
        elif x == "-o":
            out, skip = a[i + 1], True
        elif x and not x.endswith((".s", ".S")):
            opts.append(x)
    z = s[:-2] + ".z.o"
    r = subprocess.run([T + "/zas"] + opts + ["-o", z, s], capture_output=True, text=True)
    if r.returncode:
        return ("refused", out, r.stderr.strip().split("\n")[0])
    g = open(out, "rb").read()
    if open(z, "rb").read() == g:
        return ("same", out, "")
    subprocess.run([objcopy, "--strip-debug", out, z + ".g"], check=True)
    subprocess.run([objcopy, "--strip-debug", z, z + ".z"], check=True)
    if open(z + ".g", "rb").read() != open(z + ".z", "rb").read():
        return ("differ", out, "")
    nd = s[:-2] + ".nd.s"
    nodebug(s, nd)
    nopts = [o for o in opts if not o.startswith("--gdwarf")]   # (gas would make line information for a .S itself)
    subprocess.run([AS] + nopts + ["-o", z + ".ng", nd], check=True)
    subprocess.run([T + "/zas"] + nopts + ["-o", z + ".nz", nd], check=True)
    if open(z + ".ng", "rb").read() != open(z + ".nz", "rb").read():
        return ("differ", out, "(without its debugging data)")
    return ("stripped", out, "")
files = glob.glob(T + "/corpus/s.*.s")
with concurrent.futures.ThreadPoolExecutor(8) as ex:
    res = list(ex.map(one, files))
n = {k: [r for r in res if r[0] == k] for k in ("same", "stripped", "refused", "differ")}
ok = len(files) > 1500 and not n["refused"] and not n["differ"]
print(("PASS " if ok else "FAIL ") + "the build's %d assembler inputs: %d objects byte for byte; the other %d byte for "
      "byte once objcopy --strip-debug has run over both, and byte for byte as written when the source's debugging "
      "data is left out for both" % (len(files), len(n["same"]), len(n["stripped"])))
for k in ("refused", "differ"):
    for r in n[k][:5]:
        print("     %s: %s %s" % (k, r[1], r[2]))
sys.exit(0 if ok else 1)
EOF
[ $? = 0 ] || fail=1

# ---- the instruction table ----
for g in int mmu fpu branch; do
    python3 tools/zas-table.py $g >"$T/table-$g.s"
done
python3 - "$T" "$AS" <<'EOF'
import os, re, subprocess, sys
T, AS = sys.argv[1:3]
ZAS = T + "/zas"
CONFIGS = ["-m68000", "-m68010", "-m68020", "-m68030", "-m68040", "-m68060", "-m68040 -mno-float",
           "-m68020 -m68881 -m68851"]
HDR = ["\t.text", "loc:\tnop", "loc0:"]
def refused(cmd, f):
    r = subprocess.run(cmd + ["-o", f + ".o", f], capture_output=True, text=True)
    return {int(m.group(1)) for l in r.stderr.split("\n") if "arning" not in l
            for m in [re.search(r"\.s:(\d+): ", l)] if m}
def obj(cmd, lines, name):
    f = "%s/%s.s" % (T, name)
    open(f, "w").write("\n".join(lines) + "\n")
    if os.path.exists(f + ".o"):
        os.unlink(f + ".o")
    subprocess.run(cmd + ["-o", f + ".o", f], capture_output=True)
    return open(f + ".o", "rb").read() if os.path.exists(f + ".o") else None
bad = 0
total = taken = 0
for g in ("int", "mmu", "fpu", "branch"):
    f = "%s/table-%s.s" % (T, g)
    src = open(f).read().split("\n")
    for c in CONFIGS:
        cfg = c.split()
        G, Z = refused([AS] + cfg, f), refused([ZAS] + cfg, f)
        good = [l for i, l in enumerate(src, 1) if i not in G and l.startswith("\t") and l != "\t.text"]
        total += len([l for l in src if l.startswith("\t")])
        taken += len(good)
        diff = []
        if obj([AS] + cfg, HDR + good, "wg") != obj([ZAS] + cfg, HDR + good, "wz"):
            for k in range(0, len(good), 200):
                ch = HDR + good[k:k + 200]
                if obj([AS] + cfg, ch, "cg") != obj([ZAS] + cfg, ch, "cz"):
                    diff += [l for l in good[k:k + 200] if obj([AS] + cfg, HDR + [l], "lg") != obj([ZAS] + cfg, HDR + [l], "lz")][:3]
                if len(diff) > 5:
                    break
            diff = diff or ["(the whole file differs)"]
        if G != Z or diff:
            bad = 1
            print("FAIL the table (%s) with %s: zas refuses %d lines gas takes, takes %d it refuses; %d differ"
                  % (g, c, len(Z - G), len(G - Z), len(diff)))
            for i in sorted(Z - G)[:3]:
                print("     zas refuses: " + src[i - 1].strip())
            for i in sorted(G - Z)[:3]:
                print("     zas takes: " + src[i - 1].strip())
            for l in diff[:4]:
                print("     differs: " + l.strip())
print(("PASS " if not bad else "FAIL ") + "the instruction table: %d lines for %d CPU settings, %d taken, the same "
      "lines refused and the same objects as GNU as" % (total, len(CONFIGS), taken))
sys.exit(bad)
EOF
[ $? = 0 ] || fail=1

# ---- the cases in tools/tests/zas ----
for c in "relax.s -m68040" "relax.s -m68030" "relax.s -m68060" "dirs.s -m68040" "dirs.s -m68020" "prog.s -m68020 -Itools/tests/zas" "review.s -m68040"; do
    set -- $c
    "$AS" "${@:2}" -o "$T/case.g.o" "tools/tests/zas/$1" 2>"$T/case.log" &&
        "$T/zas-asan" "${@:2}" -o "$T/case.z.o" "tools/tests/zas/$1" 2>>"$T/case.log" && cmp -s "$T/case.g.o" "$T/case.z.o"
    check $? "tools/tests/zas/$1 (${*:2}): GNU as's object" "$(head -3 "$T/case.log")"
done

# ---- sources that must fail ----
must_fail() {   # NAME EXPECTED-TEXT SOURCE [OPTIONS]
    printf '%b\n' "$3" >"$T/bad.s"
    : >"$T/bad.o"
    "$T/zas-asan" ${4:-} -o "$T/bad.o" "$T/bad.s" >"$T/out.txt" 2>&1
    local st=$?
    [ $st = 1 ] && grep -qF -- "$2" "$T/out.txt" && [ ! -e "$T/bad.o" ]
    check $? "$1: refused, no object, the message says it" "status $st: $(head -2 "$T/out.txt")"
}
must_fail "an unknown instruction" "bad.s:2: unknown instruction frob" '\tnop\n\tfrob %d0'
must_fail "a bad operand" "bad operand" '\tmove.l (%d0,%a1,%a2),%d0'
must_fail "an operand the instruction has not" "operands not valid" '\tlea %d0,%a1'
must_fail "an instruction the CPU has not" "not available on this CPU" '\tbfextu %d0{1:2},%d1' -m68000
must_fail "a size the instruction has not" "no such size" '\texg.l %d0,%d1'
must_fail "a value out of range" "value out of range" '\tmove.w #70000,%d0'
must_fail "a short branch out of reach" "out of range" '\tbra.s far\n\t.skip 200\nfar:\tnop'
must_fail "a numbered label never defined" "local label 1 is not defined" '\tbra 1f'
must_fail "a symbol defined twice" "already defined" 'a:\tnop\na:\tnop'
must_fail "division by zero" "division by zero" '\t.long 1/0'
must_fail ".if without .endif" ".if without .endif" '\t.if 1\n\tnop'
must_fail ".err" "stop here" '\t.error "stop here"'
must_fail "a missing .include" "cannot open nothere.i" '\t.include "nothere.i"'
must_fail "a macro calling itself for ever" "nested too deep" '\t.macro m\n\tm\n\t.endm\n\tm'
must_fail "data in a bss section" "non-zero data" '\t.bss\n\t.long 5'
must_fail "a common symbol" "common symbols are not supported" '\t.comm c,4'
must_fail "a bad expression" "bad expression" '\t.long 3 +'
# the review's crashes and hangs (2026-11-13), each refused now
must_fail "parentheses nested deep" "nested too deep" "\t.long $(printf '(%.0s' $(seq 2000))1$(printf ')%.0s' $(seq 2000))"
must_fail "minus signs nested deep" "nested too deep" "\t.long $(printf -- '-%.0s' $(seq 3000))1"
must_fail "a chain of aliases" "nested too deep" "$(for k in $(seq 400 -1 1); do printf '\t.set a%d, a%d-1\n' $k $((k - 1)); done)\t.long a400"
must_fail ".incbin past the file" "beyond the file's end" '\t.byte 0\n\t.incbin "tools/tests/zas/blob.bin",1,0x7fffffffffffffff'
must_fail ".fill without operands" "bad .fill" '\t.fill'
must_fail ".irpc without a string" "bad .irpc" '\t.irpc x\n\t.endr'
must_fail "an unclosed .rept in a .rept" ".endr missing" '\t.rept 2\n\t.rept 2'
must_fail "an unclosed .irpc in an .irp" ".endr missing" '\t.irp a,1,2\n\t.irpc b,xy'
must_fail "too many values on a line" "too many values" "\t.float $(seq -s, 1 70)"
must_fail "a macro with too many parameters" "at most 32 parameters" "\t.macro m $(seq -s, -f p%g 1 40)\n\t.endm"
must_fail "a subsection" "subsections are not supported" '\t.text 1'
"$T/zas-asan" -o "$T/none.o" "$T/nothere.s" >"$T/out.txt" 2>&1
[ $? = 1 ] && grep -qF "cannot open $T/nothere.s" "$T/out.txt" && [ ! -e "$T/none.o" ]
check $? "an input that is not there: refused with its name" "$(head -2 "$T/out.txt")"

# ---- damaged sources under AddressSanitizer and UBSan ----
N=${ZAS_FUZZ:-400}
python3 - "$T" "$N" tools/tests/zas/relax.s tools/tests/zas/dirs.s "$T"/table-branch.s "$T"/table-mmu.s \
    $(ls "$T"/corpus/s.*.s | head -40) <<'EOF'
import os, random, subprocess, sys
T, N, files = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
rnd = random.Random(52)
crash = []
os.makedirs(T + "/fuzz", exist_ok=True)
TOK = [".rept 2", ".rept 0", ".endr", ".irp a,1,2", ".irpc b,xy", ".macro m a=1", ".endm", ".exitm", "m", "m 3,4",
       ".if 1", ".if 0", ".else", ".elseif 1", ".endif", ".set s,s+1", ".set t,u+4", "s = s - 1", ".long s,t,u",
       ".section .text.x,\"ax\"", ".pushsection .data.y,\"aw\"", ".popsection", ".previous", ".text", ".bss",
       ".lcomm l,4", ".skip 3", ".fill 2,3,1", ".balignw 4,0x4e71", ".align 4", ".org .+2", ".byte 1", ".ascii \"\\",
       ".include \"tools/tests/zas/inc.i\"", "1:", "2:", "jra 1b", "jra 1f", "jbsr 2f", "bra.s 1b", "dbra %d0,1b",
       "lea 1f(%pc),%a0", "move.l (1b,%pc,%d0.l),%d1", "nop", "moveq #s,%d0", ".float 1.5", ".double 1e-310",
       "move.l x@GOT.w(%a5),%a0", "jsr f@PLTPC", ".weak w", ".globl g", "g:", ".type g,@function", ".size g,.-g",
       ".cpu 68000", ".cpu 68060", "fmove.x %fp0,%fp1", "bfextu %d0{1:2},%d1", "(", ")", "\\", "/*", "*/", "|"]
for i in range(N):
    if i % 2:
        lines = [" ".join(rnd.choice(TOK) for _ in range(rnd.choice((1, 1, 1, 2, 3)))).encode() for _ in range(rnd.randint(1, 60))]
        f = "%s/fuzz/f.s" % T
        open(f, "wb").write(b"\n".join(b"\t" + l for l in lines))
        try:
            r = subprocess.run([T + "/zas-asan", "-m68040", "-o", T + "/fuzz/f.o", f], capture_output=True, timeout=60)
            st = r.returncode
        except subprocess.TimeoutExpired:
            st = "timeout"
        if st not in (0, 1):
            crash.append(i)
            os.rename(f, "%s/fuzz/crash%d.s" % (T, i))
            print("     generated source %d: status %s %s" % (i, st, (r.stderr[-300:] if st != "timeout" else b"").decode("latin1")))
        continue
    src = open(rnd.choice(files), "rb").read()
    lines = src.split(b"\n")
    if len(lines) > 400:
        k = rnd.randrange(len(lines) - 400)
        lines = lines[k:k + 400]
    for _ in range(rnd.randint(1, 8)):
        op = rnd.randrange(5)
        j = rnd.randrange(len(lines))
        if op == 0 and lines[j]:
            b = bytearray(lines[j]); b[rnd.randrange(len(b))] = rnd.randrange(256); lines[j] = bytes(b)
        elif op == 1:
            lines[j] = lines[j][:rnd.randrange(len(lines[j]) + 1)]
        elif op == 2:
            lines.insert(j, lines[rnd.randrange(len(lines))])
        elif op == 3:
            del lines[j]
        else:
            lines[j] += rnd.choice([b"(", b")", b"\\", b"\"", b"'", b",", b"{", b"[", b"@", b"%", b"#", b"/*", b"1:", b"\x00"])
        if not lines:
            lines = [b""]
    f = "%s/fuzz/f.s" % T
    open(f, "wb").write(b"\n".join(lines))
    try:
        r = subprocess.run([T + "/zas-asan", "-m68040", "-o", T + "/fuzz/f.o", f], capture_output=True, timeout=60)
        st = r.returncode
    except subprocess.TimeoutExpired:
        st = "timeout"
    if st not in (0, 1):
        crash.append(i)
        os.rename(f, "%s/fuzz/crash%d.s" % (T, i))
        if os.environ.get("ZAS_KEEP"):
            subprocess.run(["cp", "%s/fuzz/crash%d.s" % (T, i), os.environ["ZAS_KEEP"]])
        print("     damaged source %d: status %s %s" % (i, st, (r.stderr[-300:] if st != "timeout" else b"").decode("latin1")))
print(("PASS " if not crash else "FAIL ") + "%d damaged and generated sources under AddressSanitizer and UBSan: each assembled or "
      "refused, never a crash%s" % (N, "" if not crash else " (%d crashed)" % len(crash)))
sys.exit(1 if crash else 0)
EOF
[ $? = 0 ] || fail=1

[ $fail = 0 ] && { echo "zas-host-test: all passed"; exit 0; }
echo "zas-host-test: FAILED"; exit 1
