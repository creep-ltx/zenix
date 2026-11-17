#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# DESIGN D41 (docs/d41-memory-protection.md sections 14 and 19): memory protection on every CPU model, in FS-UAE or in
# Amiberry (ZENIX_EMU=amiberry, tools/run-test.sh), with the emulator's MMU model. The kernel as built (its default:
# protection on where the CPU is known to have an MMU) unless the model's name ends in +on (tools/kopts.py MMU=ON: the
# LC and EC parts tried too). Where protection must come on: over the serial port the boot line; a victim process
# with a pattern, writes into its region, into the kernel and to address 0, reads of the kernel, a write into a
# program's own shared code, a protected server that edits its client's buffer in place, a system server; each stray
# access must end its process with the protection line naming the right owner, the victim must find its pattern
# intact, and the system must go on. Then `free` and `bench`'s figures. Where it must stay off: the boot line's reason,
# and the system goes on.
# usage: tools/mmu-test.sh [MODEL ...]           (default: 020 030 040 lc040 060 lc060)
#   MODEL: 020 030 ec030 040 lc040 ec040 060 lc060 ec060, each also with +on (lc060+on: forced with MMU=ON)
#   MMU_TEST_OFF=1: the kernel made MMU=OFF (the figures to compare with); every model then expects "off"
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
OUT=${ZENIX_OUT:-$(mktemp -d "${TMPDIR:-/tmp}/zenix-mmu.XXXXXX")}; mkdir -p "$OUT"
CPUS="${*:-020 030 040 lc040 060 lc060}"
make -s BUILD="$BD" all 2>&1 | grep -v 'RWX permissions' | grep -i 'error\|\*\*\*'; [ "${PIPESTATUS[0]}" = 0 ] || { echo "build failed"; exit 1; }
# a build copy per kernel default: as built (AUTO), MMU=ON, MMU=OFF
mkb() {     # mkb NAME WORD: $OUT/build-NAME with its kernel set to WORD (none: as built)
    local b="$OUT/build-$1"
    [ -d "$b" ] && return 0
    cp -a "$BD" "$b" || return 1
    if [ -n "$2" ]; then
        # the copy brought up to date first, so the floppy's make cannot link a new kernel.bin over the patched one
        make -s BUILD="$b" all >/dev/null 2>&1 || { echo "FAIL: the build copy for $2"; return 1; }
        python3 tools/kopts.py "$b/kernel.bin" "$2" >/dev/null || return 1
        make -s BUILD="$b" "$b/zenix.adf" >/dev/null || { echo "FAIL: the floppy with the $2 kernel"; return 1; }
        local v; v=$(python3 tools/kopts.py "$b/kernel.bin")
        case "$2:$v" in MMU=ON:*"wherever the MMU works"*|MMU=OFF:*"memory protection off by default"*) ;;
        *) echo "FAIL: the $2 kernel says: $v"; return 1 ;; esac
    fi
}

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

cat >"$OUT/script-off.txt" <<EOF
wait zenix:/# 120
send dmesg | grep -e '^mmu:' -e '^cpu:'\r
wait $P 30
send free\r
wait $P 30
send echo alive\r
wait $P 30
stop
EOF

fail=0
for cpu in $CPUS; do
    m=${cpu%+on}; word=
    [ "$m" != "$cpu" ] && word=MMU=ON
    [ -n "$MMU_TEST_OFF" ] && word=MMU=OFF
    # the machine: the A1200 with the CPU, its MMU model and its FPU (none: an LC or EC part); what must happen
    case $m in
    020)   conf=fsuae/a1200-020-fast.fs-uae; set=''; want='off: no MMU on a 68020' ;;
    030)   conf=fsuae/matrix/a1200-030.fs-uae; set='uae_mmu_model=68030'; want=on ;;
    ec030) conf=fsuae/matrix/a1200-030.fs-uae; set='uae_mmu_model=68ec030'; want='off: no MMU' ;;
    040)   conf=fsuae/matrix/a1200-040.fs-uae; set='uae_fpu_model=68040'; want=on ;;
    lc040) conf=fsuae/matrix/a1200-040.fs-uae; set='uae_fpu_model=0'; want='off: an LC or EC CPU'
           [ -n "$word" ] && want=on ;;
    ec040) conf=fsuae/matrix/a1200-040.fs-uae; set='uae_fpu_model=0;uae_mmu_model=68ec040'; want='off: an LC or EC CPU'
           [ -n "$word" ] && want='off: no MMU' ;;
    060)   conf=fsuae/matrix/a1200-060.fs-uae; set='uae_fpu_model=68060'; want=on ;;
    lc060) conf=fsuae/matrix/a1200-060.fs-uae; set='uae_fpu_model=0'; want='off: an LC or EC CPU'
           [ -n "$word" ] && want=on ;;
    ec060) conf=fsuae/matrix/a1200-060.fs-uae; set='uae_fpu_model=0;uae_mmu_model=68ec060'; want='off: an LC or EC CPU'
           [ -n "$word" ] && want='off: no MMU' ;;
    *) echo "mmu-test: $cpu: say 020, 030, ec030, 040, lc040, ec040, 060, lc060 or ec060 (+on: forced)"; exit 2 ;;
    esac
    [ "$word" = MMU=OFF ] && want='off: not asked for'          # the kernel file's default made 0
    if [ "${m#ec}" != "$m" ] && [ "${ZENIX_EMU:-fs-uae}" != amiberry ]; then
        echo "SKIP mmu-test $cpu: FS-UAE 3.2.35 has no EC MMU model (68ec030/040/060 crash it); ZENIX_EMU=amiberry has them"
        continue
    fi
    mkb "${word:-auto}" "$word" || exit 1
    B="$OUT/build-${word:-auto}"
    L="$OUT/$cpu.log"
    script="$OUT/script.txt"; [ "$want" = on ] || [ -n "$MMU_TEST_OFF" ] || script="$OUT/script-off.txt"
    echo "mmu-test $cpu (${ZENIX_EMU:-fs-uae}, kernel ${word:-as built}): expects protection $want"
    ZENIX_BUILD="$B" ZENIX_FSUAE_SET="$set${ZENIX_FSUAE_SET:+;$ZENIX_FSUAE_SET}" \
        tools/run-test.sh "$conf" 700 "$script" 2>&1 | sed 's/\x1b\[[0-9;:]*[mK]//g' > "$L"
    ok=1
    chk() { if grep -qaE -- "$2" "$L"; then echo "  ok   $1"; else echo "  FAIL $1"; ok=0; fi; }
    grep -a -m1 '^mmu: memory protection' "$L" | sed 's/^/  boot: /'
    if [ "$want" != on ]; then
        chk "protection off: ${want#off: }" "^mmu: memory protection off: ${want#off: }"
        chk "the system goes on" '^alive$'
        if grep -qa 'exception in the kernel\|panic' "$L"; then echo "  FAIL the kernel stopped"; ok=0; fi
        echo "  figures: $(grep -a -E '^(chip|fast) RAM free' "$L" | head -2 | tr '\n' ' ')"
        grep -aE 'round trip|stat /bin/sh|null system call' "$L" | sed 's/^/  bench: /'
        [ $ok = 1 ] && echo "PASS mmu-test $cpu" || { echo "FAIL mmu-test $cpu (see $L)"; fail=1; }
        continue
    else
        chk "the boot line" "^mmu: memory protection on: 680${m: -2}, 4 KB pages, [0-9]+ KB of tables"
        chk "a write into another process: killed, its owner named" \
            'pid [0-9]+ \(crash\): memory protection: a write to 0x[0-9a-f]+ \(the memory of pid [0-9]+ \(crash\)\): killed \(SIGSEGV\)'
        chk "a read of another process: killed" \
            'pid [0-9]+ \(crash\): memory protection: a read of 0x[0-9a-f]+ \(the memory of pid [0-9]+ \(crash\)\)'
        chk "a write to 0x1000: the kernel's memory" 'memory protection: a write to 0x1000 \(the kernel.s memory\): killed'
        chk "a read of address 0" 'memory protection: a read of 0x0 \(the kernel.s memory\): killed'
        # the kernel's own address from its boot line, matched exactly (audit 2026-11-12 A6-15: the 0x1000 line above
        # satisfied a loose pattern, so a wrong or empty address passed)
        ka=$(grep -aoE '^kernel: [0-9]+ bytes at 0x[0-9a-f]+' "$L" | head -1 | awk '{print $5}')
        if [ -n "$ka" ] && [ "$ka" != 0x1000 ]; then
            chk "a write into the kernel's image ($ka)" "memory protection: a write to $ka \\(the kernel.s memory\\): killed"
        else
            echo "  FAIL the kernel's address in its boot line ('$ka')"; ok=0
        fi
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
    echo "  figures: $(grep -a -E '^(chip|fast) RAM free' "$L" | head -2 | tr '\n' ' ')"
    grep -E 'round trip|stat /bin/sh|null system call' "$L" | sed 's/^/  bench: /'
    [ $ok = 1 ] && echo "PASS mmu-test $cpu" || { echo "FAIL mmu-test $cpu (see $L)"; fail=1; }
done
exit $fail
