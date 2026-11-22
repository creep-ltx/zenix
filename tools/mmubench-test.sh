#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# DESIGN D60 section 9.4 (stage 0, docs/d60-full-protection.md section 14): the figures memory protection's cost is
# judged on, `bench mmu` (docs/man/bench.txt) with protection off (the kernel made MMU=OFF) and as built (on where the
# CPU has an MMU), on the same machine and build, in FS-UAE or in Amiberry (ZENIX_EMU=amiberry, tools/run-test.sh).
# Prints each figure off -> on, the ratio, and where it lies against section 9.4's budget. Emulator figures: neither
# emulator models the ATC's or the caches' real cost, so only the ratios mean anything, and a pass here is no pass on a
# real CPU (the Apollo 1260 judges). Checked: protection off and on as asked (the boot line), every figure given (no
# "FAILED"), the system going on; the budget is reported, never failed on.
# usage: tools/mmubench-test.sh [CPU ...]       CPU: 030 040 060 (default: all three), the machines of tools/mmu-test.sh
#   MMUBENCH_DIAG=-DMMU_PAGE8: both kernels built with that (a build directory of its own: 8 KB pages, D60 9.2 item 7)
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
OUT=${ZENIX_OUT:-$(mktemp -d "${TMPDIR:-/tmp}/zenix-mmubench.XXXXXX")}; mkdir -p "$OUT"
CPUS="${*:-030 040 060}"
EMU=${ZENIX_EMU:-fs-uae}
D=${MMUBENCH_DIAG:-}
make -s BUILD="$BD" all 2>&1 | grep -v 'RWX permissions' | grep -i 'error\|\*\*\*'; [ "${PIPESTATUS[0]}" = 0 ] || { echo "build failed"; exit 1; }
mkb() {     # mkb NAME WORD: $OUT/build-NAME, made with MMUBENCH_DIAG, its kernel set to WORD (none: as built)
    local b="$OUT/build-$1"
    [ -d "$b" ] && return 0
    cp -a "$BD" "$b" || return 1
    if [ -n "$2" ] || [ -n "$D" ]; then
        # DIAG is not tracked as a dependency: the kernel's objects go first so the copy is made with it
        [ -n "$D" ] && rm -f "$b"/*.o "$b"/kernel*.bin "$b"/kernel*.elf
        make -s BUILD="$b" DIAG="$D" all >/dev/null 2>&1 || { echo "FAIL: the build copy $1"; return 1; }
    fi
    if [ -n "$2" ]; then
        python3 tools/kopts.py "$b/kernel.bin" "$2" >/dev/null || return 1
        make -s BUILD="$b" DIAG="$D" "$b/zenix.adf" >/dev/null || { echo "FAIL: the floppy with the $2 kernel"; return 1; }
    fi
}
P='zenix:\S*[#$] '
cat >"$OUT/script.txt" <<EOF
wait zenix:/# 120
send dmesg | grep -e '^mmu:'\r
wait $P 30
send bench mmu; echo status \$?\r
wait mmubench: (done|FAILED) 900
wait $P 60
send echo alive\r
wait $P 30
stop
EOF
# the budget (D60 9.4): NAME KIND LIMIT [TARGET]; KIND t: a time (on/off), r: a rate (off/on)
budget() {
    case $1 in
    nullcall) echo "t 1.05" ;;                  # "+0": within the timer's noise
    stat|read4k|lsr|bigread) echo "t 1.10" ;;
    msgcall|pipetrip) echo "t 2.0 1.5" ;;
    pipe) echo "t 1.15" ;;
    esac
}
fail=0
for cpu in $CPUS; do
    case $cpu in
    030) conf=fsuae/matrix/a1200-030.fs-uae; set='uae_mmu_model=68030' ;;
    040) conf=fsuae/matrix/a1200-040.fs-uae; set='uae_fpu_model=68040' ;;
    060) conf=fsuae/matrix/a1200-060.fs-uae; set='uae_fpu_model=68060' ;;
    *) echo "mmubench-test: $cpu: say 030, 040 or 060"; exit 2 ;;
    esac
    ok=1
    for k in off on; do
        word=; [ $k = off ] && word=MMU=OFF
        mkb "$k" "$word" || exit 1
        L="$OUT/$cpu-$k.log"
        ZENIX_BUILD="$OUT/build-$k" ZENIX_SERIAL="${ZENIX_SERIAL:-$OUT/ser}-$cpu-$k" \
            ZENIX_FSUAE_SET="$set${ZENIX_FSUAE_SET:+;$ZENIX_FSUAE_SET}" \
            tools/run-test.sh "$conf" 1500 "$OUT/script.txt" 2>&1 | sed 's/\x1b\[[0-9;:]*[mK]//g' > "$L"
        grep -a -m1 '^mmu: memory protection' "$L" | sed "s/^/  $cpu $k: /"
        if [ $k = off ]; then w='^mmu: memory protection off: not asked for'; else w='^mmu: memory protection on: '; fi
        grep -qaE "$w" "$L" || { echo "  FAIL $cpu $k: the boot line is not protection $k"; ok=0; }
        grep -qa '^mmubench: done' "$L" || { echo "  FAIL $cpu $k: bench mmu did not finish"; grep -a 'FAILED' "$L" | head -3; ok=0; }
        grep -qa '^alive' "$L" || { echo "  FAIL $cpu $k: the system did not go on"; ok=0; }
        if grep -qa 'exception in the kernel\|panic' "$L"; then echo "  FAIL $cpu $k: the kernel stopped"; ok=0; fi
    done
    echo "mmubench $cpu ($EMU${D:+, $D}; emulator figures: ratios only)"
    for n in bigread nullcall stat read4k msgcall pipetrip pipe lsr; do
        a=$(grep -a "^mmubench: $n " "$OUT/$cpu-off.log" | head -1 | awk '{print $3, $4}')
        b=$(grep -a "^mmubench: $n " "$OUT/$cpu-on.log" | head -1 | awk '{print $3, $4}')
        read -r kind lim tgt <<<"$(budget $n)"
        python3 - "$n" "$a" "$b" "$lim" "${tgt:-}" <<'PY'
import sys
n, a, b, lim, tgt = sys.argv[1:6]
try:
    va, u = float(a.split()[0]), a.split()[1]; vb = float(b.split()[0])
except (ValueError, IndexError):
    print(f"  {n:9} off {a or '-'} on {b or '-'}: no figure"); sys.exit()
r = (va / vb) if u == "KB/s" else (vb / va)     # the cost: how many times slower with protection on
lim = float(lim); t = float(tgt) if tgt else None
v = "within the budget" if r <= (t or lim) else ("over the target, within the limit" if r <= lim else "OVER the limit")
print(f"  {n:9} {va:>9g} -> {vb:>9g} {u:5} x{r:.2f}  (limit x{lim:g}{f', target x{t:g}' if t else ''}: {v})")
PY
    done
    [ $ok = 1 ] && echo "PASS mmubench-test $cpu" || { echo "FAIL mmubench-test $cpu (see $OUT)"; fail=1; }
done
exit $fail
