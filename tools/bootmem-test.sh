#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# DESIGN D45 (docs/d45-boot-memory.md section 10): the boot's memory given back. First the build's check of the spans
# (tools/check-init-test.py: tools/check-init.py refuses each kind of reference it is there to refuse), then boots in
# FS-UAE: the stock A1200 (68020, 2 MB chip RAM only: the span is chip RAM there), and a 68030 and a 68060 with memory
# protection off (the default) and on (030m, 060m: a copy of the build whose kernel defaults to MMU=ON, as
# tools/mmu-test.sh makes it). Each boot must say "mem: N bytes of boot code given back" with N the boot span's size
# read from kernel.elf, the pool's free count grown by exactly N, the CD span given back too (no CD drive in these
# machines), the memory protection span (section 13) given back with protection off and kept with it on, then run
# programs, a victim's memory intact, and no exception or panic.
# usage: tools/bootmem-test.sh [020 030 060 030m 060m ...]          (default: all five)
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
OUT=${ZENIX_OUT:-$(mktemp -d "${TMPDIR:-/tmp}/zenix-bootmem.XXXXXX")}; mkdir -p "$OUT"
CPUS="${*:-020 030 060 030m 060m}"
make -s BUILD="$BD" all 2>&1 | grep -v 'RWX permissions' | grep -i 'error\|\*\*\*'; [ "${PIPESTATUS[0]}" = 0 ] || { echo "build failed"; exit 1; }
fail=0
mkdir -p "$OUT/cit"
if TOOLPREFIX=${TOOLPREFIX:-$HOME/opt/m68k-elf/bin/m68k-elf-} python3 tools/check-init-test.py "$BD/kernel.elf" "$OUT/cit" > "$OUT/cit.log" 2>&1; then
    echo "  ok   the build's check refuses every kind of reference into a span ($(grep -c '^PASS' "$OUT/cit.log") cases)"
else
    cat "$OUT/cit.log"; echo "  FAIL the build's check (tools/check-init-test.py)"; fail=1
fi
# the spans' sizes as the kernel computes them: [start rounded up to 16, end rounded down to 16)
span() {
    local N=${TOOLPREFIX:-$HOME/opt/m68k-elf/bin/m68k-elf-}nm
    local s e
    s=$($N "$BD/kernel.elf" | awk -v n="__$1_start" '$3 == n { print $1 }')
    e=$($N "$BD/kernel.elf" | awk -v n="__$1_end" '$3 == n { print $1 }')
    echo $(( (0x$e & ~15) - ((0x$s + 15) & ~15) ))
}
NB=$(span zinit) NC=$(span zhwcd) NM=$(span zhwmmu)
echo "  the spans in kernel.elf: boot $NB bytes, CD drive $NC bytes, memory protection $NM bytes"

B="$OUT/build-mmu"
rm -rf "$B"; cp -a "$BD" "$B" || exit 1
python3 tools/kopts.py "$B/kernel.bin" MMU=ON >/dev/null || exit 1
make -s BUILD="$B" "$B/zenix.adf" >/dev/null || { echo "FAIL: the floppy with the patched kernel"; exit 1; }

P='zenix:\S*[#$] '
cat >"$OUT/script.txt" <<EOF
wait zenix:/# 120
send dmesg | grep -e '^mem:' -e '^mmu:'\r
wait $P 30
send free\r
wait $P 30
send crash victim 2; echo status \$?\r
wait $P 60
send ls -l /bin | wc -l; echo status \$?\r
wait $P 30
send echo done\r
wait $P 30
stop
EOF

for cpu in $CPUS; do
    case $cpu in
    020) conf=fsuae/a1200-020.fs-uae; set=''; bd="$BD"; ram=chip ;;
    030) conf=fsuae/matrix/a1200-030.fs-uae; set='uae_mmu_model=68030'; bd="$BD"; ram=fast ;;
    060) conf=fsuae/matrix/a1200-060.fs-uae; set=''; bd="$BD"; ram=fast ;;
    030m) conf=fsuae/matrix/a1200-030.fs-uae; set='uae_mmu_model=68030'; bd="$B"; ram=fast ;;
    060m) conf=fsuae/matrix/a1200-060.fs-uae; set=''; bd="$B"; ram=fast ;;
    *) echo "bootmem-test: $cpu: say 020, 030, 060, 030m or 060m"; exit 2 ;;
    esac
    L="$OUT/$cpu.log"
    ZENIX_BUILD="$bd" ZENIX_FSUAE_SET="$set${ZENIX_FSUAE_SET:+;$ZENIX_FSUAE_SET}" \
        tools/run-test.sh "$conf" 400 "$OUT/script.txt" 2>&1 | sed 's/\x1b\[[0-9;:]*[mK]//g' > "$L"
    ok=1
    chk() { if grep -qE -- "$2" "$L"; then echo "  ok   $1"; else echo "  FAIL $1"; ok=0; fi; }
    line=$(grep -aoE "^mem: [0-9]+ bytes of boot code given back \(\w+ RAM free [0-9]+ -> [0-9]+\)" "$L" | head -1)
    if [ -n "$line" ]; then
        read -r n kind a b < <(echo "$line" | sed -E 's/^mem: ([0-9]+) .*\((\w+) RAM free ([0-9]+) -> ([0-9]+)\)/\1 \2 \3 \4/')
        [ "$n" = "$NB" ] && echo "  ok   the boot span given back: $n bytes, as kernel.elf says" ||
            { echo "  FAIL the boot span given back: $n bytes, kernel.elf says $NB"; ok=0; }
        [ $((b - a)) = "$n" ] && echo "  ok   the $kind RAM free count grew by exactly that: $a -> $b" ||
            { echo "  FAIL the free count grew by $((b - a)), not $n"; ok=0; }
        [ "$kind" = "$ram" ] && echo "  ok   it is $kind RAM, as the kernel's home" ||
            { echo "  FAIL it is $kind RAM, the kernel's home is $ram RAM"; ok=0; }
    else
        echo "  FAIL no line 'mem: N bytes of boot code given back'"; ok=0
    fi
    chk "the CD span given back ($NC bytes, no CD drive)" "^mem: $NC bytes of CD drive code given back"
    case $cpu in
    *m) c=${cpu%m}
        chk "with memory protection on" "^mmu: memory protection on: 680${c#0}"
        if grep -q "^mem: [0-9]* bytes of memory protection code given back" "$L"; then
            echo "  FAIL the memory protection span given back with protection on"; ok=0
        else echo "  ok   the memory protection span kept ($NM bytes): protection is on"; fi ;;
    *)  chk "memory protection off" "^mmu: memory protection off"
        line=$(grep -aoE "^mem: [0-9]+ bytes of memory protection code given back \(\w+ RAM free [0-9]+ -> [0-9]+\)" "$L" | head -1)
        if [ -n "$line" ]; then
            read -r n a b < <(echo "$line" | sed -E 's/^mem: ([0-9]+) .*free ([0-9]+) -> ([0-9]+)\)/\1 \2 \3/')
            [ "$n" = "$NM" ] && [ $((b - a)) = "$n" ] &&
                echo "  ok   the memory protection span given back: $n bytes, as kernel.elf says, free $a -> $b" ||
                { echo "  FAIL the memory protection span: $n bytes (kernel.elf $NM), free $a -> $b"; ok=0; }
        else echo "  FAIL no line 'mem: N bytes of memory protection code given back'"; ok=0; fi ;;
    esac
    chk "a program's memory intact" 'crash: victim pid [0-9]+ intact'
    chk "programs run after it" '^status 0$'
    chk "the commands after it" '^done$'
    if grep -q 'exception in the kernel\|panic\|mem: span [012] at' "$L"; then echo "  FAIL the kernel stopped, or a span stayed"; ok=0; fi
    [ $ok = 1 ] && echo "PASS bootmem-test $cpu" || { echo "FAIL bootmem-test $cpu (see $L)"; fail=1; }
done
exit $fail
