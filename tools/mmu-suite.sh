#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# DESIGN D41 section 19: the regular suites on a 68030, 68040 or 68060 with memory protection on (the default there):
# each test as it is, its machine changed by ZENIX_FSUAE_SET (tools/fsuae_conf.py) to the CPU with the emulator's MMU
# model, an FPU and, on the 040/060, 64 MB of 32-bit fast RAM (as the 1260). Each test gets its own output folder and
# serial port under OUT; one after another. The boot line is checked in every log that has the kernel's messages.
# usage: tools/mmu-suite.sh CPU [TEST ...]       CPU: 030, 040 or 060
#   TEST: a tools/TEST.sh, or TEST:A,B,... for its parts A B ... (default: the list below; svc-test without its part f
#   and sound-test are tools/check.sh's STOCK tests: the suite runs them that way only at stock speed, ZENIX_FAST=0)
#   ZENIX_EMU=amiberry: in Amiberry (tools/run-test.sh); the cage rig's screenshot tests run in FS-UAE only
#   ZENIX_MMU_TESTS="TEST ...": that list instead of the default (as the arguments; for tools/check.sh mmu-suite)
#   prio-test and pure-test are in the list since D60 stage 5 (priotest's children share its memory by SYS_shm; the
#   shared code's write is stopped by protection)
cd "$(dirname "$0")/.." || exit 1
cpu=$1; shift
case $cpu in
030) set='uae_cpu_model=68030;uae_mmu_model=68030;uae_fpu_model=68882' ;;
040) set='uae_cpu_model=68040;uae_mmu_model=68040;uae_fpu_model=68040;uae_mbresmem_size=64' ;;
060) set='uae_cpu_model=68060;uae_mmu_model=68060;uae_fpu_model=68060;uae_mbresmem_size=64' ;;
*) echo "usage: tools/mmu-suite.sh 030|040|060 [TEST ...]"; exit 2 ;;
esac
DEF="cmds-test memtest mounttest svc-test:a,z,zs,b,n,r,d,o,zo,u,l,s,h,t,dmg,c lua-test tcc-test zdb-test:020 prio-test pure-test"
[ "${ZENIX_FAST:-0}" = 0 ] && DEF="$DEF svc-test:f sound-test"
TESTS="${*:-${ZENIX_MMU_TESTS:-$DEF}}"
BD=${ZENIX_BUILD:-build}
make -s BUILD="$BD" all >/dev/null 2>&1 || { echo "FAIL mmu-suite: the build"; exit 1; }
OUT=${ZENIX_OUT:-$(mktemp -d "${TMPDIR:-/tmp}/zenix-mmusuite.XXXXXX")}; mkdir -p "$OUT"
SER=${ZENIX_SERIAL:-$OUT/ser}
fail=0
for ta in $TESTS; do
    t=${ta%%:*}; args=; [ "$t" != "$ta" ] && args=${ta#*:}
    [ -x "tools/$t.sh" ] || { echo "FAIL $t: no tools/$t.sh"; fail=1; continue; }
    o="$OUT/${ta//[:,]/_}"; mkdir -p "$o"
    s=$(date +%s)
    # each test its own copy of the build, as in make check: the tests write into the images (a shared copy filled
    # tcc-test's disk after the others had written theirs)
    rm -rf "$o/build"; cp -a "$BD" "$o/build" || { echo "FAIL $ta: no copy of $BD"; fail=1; continue; }
    ZENIX_FSUAE_SET="$set${ZENIX_FSUAE_SET:+;$ZENIX_FSUAE_SET}" ZENIX_OUT="$o" ZENIX_SERIAL="$SER-$t" \
        ZENIX_BUILD="$o/build" tools/"$t".sh ${args//,/ } >"$o/result.txt" 2>&1
    rc=$?
    # every serial log of the test that shows the boot must show protection on (a test may keep no logs: then nothing)
    bad=$(grep -rlaE '^mmu: memory protection off' "$o" 2>/dev/null | head -1)
    if [ $rc = 0 ] && ! grep -qa '^FAIL' "$o/result.txt" && [ -z "$bad" ]; then
        echo "PASS $ta on the $cpu with protection on ($(( $(date +%s) - s )) s)"
        rm -rf "$o/build"
    else
        echo "FAIL $ta on the $cpu (exit $rc${bad:+, protection off in $bad}; see $o/result.txt)"
        grep -a '^FAIL' "$o/result.txt" | head -5 | sed 's/^/    /'
        fail=1
    fi
done
exit $fail
