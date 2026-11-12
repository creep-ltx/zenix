#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# DESIGN D41 (docs/d41-memory-protection.md section 14): memory protection in FS-UAE, on an emulated 68030 and 68060
# with FS-UAE's MMU model. A copy of the build whose kernel defaults to MMU=ON (tools/kopts.py); then over the serial
# port: the boot line, a victim process with a pattern, writes into its region, into the kernel and to address 0,
# reads of the kernel, a write into a program's own shared code, a protected server that edits its client's buffer
# in place, a system server; each stray access must end its process with the protection line naming the right
# owner, the victim must find its pattern intact, and the system must go on. Then `free` with protection on (the
# memory rounding costs, compared by the caller with an MMU=OFF run) and `bench`'s message figures.
# usage: tools/mmu-test.sh [030|040|060 ...]          (default: 030 060)
#   MMU_TEST_OFF=1: the same script with protection off (the MMU=ON copy is not made): the figures to compare with.
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
OUT=${ZENIX_OUT:-$(mktemp -d "${TMPDIR:-/tmp}/zenix-mmu.XXXXXX")}; mkdir -p "$OUT"
CPUS="${*:-030 060}"
make -s BUILD="$BD" all 2>&1 | grep -v 'RWX permissions' | grep -i 'error\|\*\*\*'; [ "${PIPESTATUS[0]}" = 0 ] || { echo "build failed"; exit 1; }
B="$OUT/build"
rm -rf "$B"; cp -a "$BD" "$B" || exit 1
if [ -z "$MMU_TEST_OFF" ]; then
    python3 tools/kopts.py "$B/kernel.bin" MMU=ON || exit 1
fi
make -s BUILD="$B" "$B/zenix.adf" >/dev/null || { echo "FAIL: the floppy with the patched kernel"; exit 1; }

P='zenix:\S*[#$] '
cat >"$OUT/script.txt" <<EOF
wait zenix:/# 120
send dmesg | grep -e '^mmu:' -e '^kernel:'\r
wait $P 30
send free\r
wait $P 30
send rm -f /tmp/vgo; crash victim 3000 /tmp/vgo > /tmp/v &\r
wait $P 30
sleep 3
send cat /tmp/v\r
wait $P 30
send crash write \$(head -1 /tmp/v | cut -d' ' -f6); echo status \$?\r
wait $P 30
send crash read \$(head -1 /tmp/v | cut -d' ' -f6); echo status \$?\r
wait $P 30
send crash write 0x1000; echo status \$?\r
wait $P 30
send crash read 0; echo status \$?\r
wait $P 30
send crash write \$(dmesg | grep '^kernel:' | head -1 | cut -d' ' -f5); echo status \$?\r
wait $P 30
send /tests/crashpure poke; echo status \$?\r
wait $P 30
send echo alive\r
wait $P 30
send msgpingd > /dev/null &\r
wait $P 30
sleep 2
send msgping 200; echo status \$?\r
wait $P 60
send bench\r
wait $P 300
send ls -l /bin | wc -l; echo status \$?\r
wait $P 30
send touch /tmp/vgo; sleep 3; cat /tmp/v\r
wait $P 30
send echo done\r
wait $P 30
stop
EOF

fail=0
for cpu in $CPUS; do
    case $cpu in
    030) conf=fsuae/matrix/a1200-030.fs-uae; set='uae_mmu_model=68030' ;;
    040) conf=fsuae/matrix/a1200-040.fs-uae; set='' ;;
    060) conf=fsuae/matrix/a1200-060.fs-uae; set='' ;;
    *) echo "mmu-test: $cpu: say 030, 040 or 060"; exit 2 ;;
    esac
    L="$OUT/$cpu.log"
    ZENIX_BUILD="$B" ZENIX_FSUAE_SET="$set${ZENIX_FSUAE_SET:+;$ZENIX_FSUAE_SET}" \
        tools/run-test.sh "$conf" 700 "$OUT/script.txt" 2>&1 | sed 's/\x1b\[[0-9;:]*[mK]//g' > "$L"
    ok=1
    chk() { if grep -qE -- "$2" "$L"; then echo "  ok   $1"; else echo "  FAIL $1"; ok=0; fi; }
    if [ -n "$MMU_TEST_OFF" ]; then
        chk "protection off" '^mmu: memory protection off: '
    else
        chk "the boot line" "^mmu: memory protection on: 680${cpu#0}, 4 KB pages, [0-9]+ KB of tables"
        chk "a write into another process: killed, its owner named" \
            'pid [0-9]+ \(crash\): memory protection: a write to 0x[0-9a-f]+ \(the memory of pid [0-9]+ \(crash\)\): killed \(SIGSEGV\)'
        chk "a read of another process: killed" \
            'pid [0-9]+ \(crash\): memory protection: a read of 0x[0-9a-f]+ \(the memory of pid [0-9]+ \(crash\)\)'
        chk "a write to 0x1000: the kernel's memory" 'memory protection: a write to 0x1000 \(the kernel.s memory\): killed'
        chk "a read of address 0" 'memory protection: a read of 0x0 \(the kernel.s memory\): killed'
        chk "a write into the kernel's image" 'memory protection: a write to 0x[0-9a-f]+ \(the kernel.s memory\): killed'
        chk "a write into its own shared code" 'pid [0-9]+ \(crashpure\): memory protection: a write to 0x[0-9a-f]+ \(its own code \(read only\)\)'
        [ "$(grep -c '^status 139' "$L")" -ge 6 ] && echo "  ok   six stray accesses ended with status 139" ||
            { echo "  FAIL six stray accesses ended with status 139 ($(grep -c '^status 139' "$L"))"; ok=0; }
        if grep -q 'crash: still here' "$L"; then echo "  FAIL a stray access went through"; ok=0; fi
    fi
    chk "the victim kept running and its memory is intact" 'crash: victim pid [0-9]+ intact'
    chk "the system goes on" '^alive$'
    chk "a protected server edits its client's buffer in place" 'msgping: ZERO COPY: THE SERVER EDITS MY BUFFER'
    chk "... and answers 200 calls" 'msgping: 200 round trips'
    chk "the commands after it" '^done$'
    if grep -q 'exception in the kernel\|panic' "$L"; then echo "  FAIL the kernel stopped"; ok=0; fi
    echo "  figures: $(grep -m1 -E '^ *Mem|KB' "$L" | head -1)"
    grep -E 'round trip|stat /bin/sh|null system call' "$L" | sed 's/^/  bench: /'
    [ $ok = 1 ] && echo "PASS mmu-test $cpu" || { echo "FAIL mmu-test $cpu (see $L)"; fail=1; }
done
exit $fail
