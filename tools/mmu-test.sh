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
# DESIGN D60 stage 2 (docs/d60-full-protection.md 5, 13): the kernel's records and its copies in a server's inbox: a
# server rewriting its client's message in flight changes nothing, forged and second replies are refused, a server
# that may not debug its client is refused (-EACCES), the kernel's path arrives as a copy in the server's own message;
# a filesystem server (msgpingd -fswin on /tmp/w) read into guarded buffers at every alignment, also through itself
# into the root's server (fwd: a window passed on, the copy pages synced); a read past the end of one's memory is
# -EFAULT. The filesystem server's windows are exact: a write one byte past the buffer, one before it, or into it after
# the reply ends it with the protection line naming the client.
# DESIGN D60 stage 3 (docs/d60-full-protection.md 3.1, 5.5, 7.4, 13): no process is a system one. A server whose port
# is PEER_EXACT (msgpingd as built) sees only what its client declared: a write one byte past a declared page ends it;
# a user's server asking for its client's stack gets -EFAULT, and touching it without asking ends it; a service (the
# service manager started it: trusted to ask) touching memory it was not given ends too, and is started again by the
# next call, which its asking then serves. An old server (msgpingd -old: its port not marked) gets D41's lend by
# SYS_debug's rule. SYS_shm: an offer taken by a child, the bytes shared both ways, the owner's early free (the child
# keeps the memory, then owns it), a drop.
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
send msgpingd -old > /dev/null &\r
wait $P 30
sleep 2
send msgping rewrite\r
wait $P 30
send msgpingd -old -port pu -uid 1000 > /dev/null & sleep 1; msgping call pu; dmesg | grep 'may not see'\r
wait $P 30
@EXACT@
send rm -f /tmp/wsrc; mkdir -p /tmp/w; msgpingd -port w -fswin & sleep 1; svctest mount w /tmp/w\r
wait $P 30
send cat /tmp/w/f > /tmp/wsrc; ls /tmp/w | wc -l; msgping winread /tmp/w/f\r
wait $P 60
send stat /tmp/w/where > /dev/null; stat /tmp/w/forge; msgping readpast /tmp/w/f; echo win-common-done\r
wait $P 30
@CONFINE@
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

# D60 stage 3: servers that see only what is declared or asked for; a service's stray write; SYS_shm
sed -i -e '/^@EXACT@$/r /dev/stdin' -e '/^@EXACT@$/d' "$OUT/script.txt" <<EOF
send msgpingd -port px > /dev/null & sleep 1; msgping past px\r
wait $P 30
send msgpingd -port py -uid 1000 > /dev/null & sleep 1; msgping peer py; msgping wild py\r
wait $P 30
send svctest reg pz 0 /bin/msgpingd -port pz; msgping wild pz; msgping peer pz; msgping peer pz\r
wait $P 30
send msgping shm; echo shm-status \$?\r
wait $P 30
EOF
# the confined filesystem server's stray writes: each ends msgpingd -fswin, which is started again for the next
sed -i -e '/^@CONFINE@$/r /dev/stdin' -e '/^@CONFINE@$/d' "$OUT/script.txt" <<EOF
send msgping winread /tmp/w/fwd\r
wait $P 60
send msgping winread /tmp/w/over 10; msgping winread /tmp/w/under 10\r
wait $P 30
send msgping winread /tmp/w/over 11; sleep 1; msgpingd -port w -fswin & sleep 1\r
wait $P 30
send msgping winread /tmp/w/under 12; sleep 1; msgpingd -port w -fswin & sleep 1\r
wait $P 30
send cat /tmp/w/after > /dev/null; ls /tmp/w > /dev/null; sleep 1; echo win-confine-done\r
wait $P 30
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
    if [ "$want" = on ] && [ -z "$MMU_TEST_OFF" ]; then     # D60 stage 2
        chk "a client's message rewritten while it is served changes nothing" \
            'msgping: message rewritten in flight: result 7, a\[2\] 4242'
        chk "a server that may not debug its client: refused" 'msgping: call to pu: -13'
        chk "... and said once" 'mmu: msgpingd \(pid [0-9]+\) may not see the memory of pid [0-9]+ \(msgping\)'
        chk "the filesystem server mounted" 'svctest mount /tmp/w: 0'
        chk "reads at every alignment: the bytes and the bytes around them" 'msgping: winread /tmp/w/f ok 9'
        chk "the kernel's path arrives as a copy in the server's message" 'msgpingd: the path is a copy in my own message'
        chk "forged and second replies refused" 'msgpingd: forged replies -22 -22, reply 0, second reply -22'
        chk "a read past the end of one's memory" 'msgping: readpast -14'
        chk "a declared buffer: a write one byte past it ends the server, the buffer untouched" \
            'msgping: past px: -104, a\[2\] 0, untouched'
        chk "a user's server asking for its client's stack: -EFAULT" 'msgping: peer py: 0, a\[2\] -14, untouched'
        chk "... and touching it without asking ends it" 'msgping: wild py: -104, a\[2\] 0, untouched'
        chk "a service touching memory it was not given ends" 'msgping: wild pz: -104, a\[2\] 0, untouched'
        chk "... is started again by the next call, which its asking serves" 'msgping: peer pz: 0, a\[2\] 0, untouched'
        [ "$(grep -caE '\(msgpingd\): memory protection: a (write to|read of)' "$L")" -ge 3 ] &&
            echo "  ok   the three stray servers ended with the protection line" ||
            { echo "  FAIL the three stray servers ended with the protection line"; ok=0; }
        chk "SYS_shm: offered, taken, shared both ways, freed early, kept, dropped" 'msgping: shm ok'
        chk "... exit status" '^shm-status 0'
        chk "confined: reads through the server into the root's server (a window passed on)" \
            'msgping: winread /tmp/w/fwd ok 9'
        chk "confined: a write past a buffer in a part page lands in the copy page: the client's bytes as they were" \
            'msgping: winread /tmp/w/over case 10: read 100, the bytes right'
        chk "... (before it)" 'msgping: winread /tmp/w/under case 10: read 100, the bytes right'
        chk "confined: past a page's end: the read fails, the client's bytes untouched" \
            'msgping: winread /tmp/w/over case 11: read -5, untouched'
        chk "... (before a page's start)" 'msgping: winread /tmp/w/under case 12: read -5, untouched'
        chk "confined: a write one byte past the buffer ends the server, naming the client" \
            'pid [0-9]+ \(msgpingd\): memory protection: a write to 0x[0-9a-f]+ \(the memory of pid [0-9]+ \(msgping\)\)'
        chk "confined: a write into the buffer after the reply ends the server" \
            'pid [0-9]+ \(msgpingd\): memory protection: a write to 0x[0-9a-f]+ \(the memory of pid [0-9]+ \(cat\)\)'
        [ "$(grep -caE '\(msgpingd\): memory protection: a write to' "$L")" -ge 6 ] &&
            echo "  ok   confined: past, before and after the reply: three ends" ||
            { echo "  FAIL confined: past, before and after the reply: three ends"; ok=0; }
        chk "... and the system goes on" '^win-confine-done'
        if grep -qa 'winread.*BAD' "$L"; then echo "  FAIL a winread case:"; grep -a 'winread.*BAD' "$L" | head -3; ok=0; fi
    fi
    chk "... and answers 200 calls" 'msgping: 200 round trips'
    chk "the commands after it" '^done$'
    if grep -q 'exception in the kernel\|panic' "$L"; then echo "  FAIL the kernel stopped"; ok=0; fi
    echo "  figures: $(grep -a -E '^(chip|fast) RAM free' "$L" | head -2 | tr '\n' ' ')"
    grep -E 'round trip|stat /bin/sh|null system call' "$L" | sed 's/^/  bench: /'
    [ $ok = 1 ] && echo "PASS mmu-test $cpu" || { echo "FAIL mmu-test $cpu (see $L)"; fail=1; }
done
exit $fail
