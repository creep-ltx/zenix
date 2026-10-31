#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# ZINE in FS-UAE (DESIGN D29 sections 15 and 16): step 2, the kernel's pause and resume with the null blob
# (/tests/zine/null.blob: back at once through the resume routine), once and then twenty times in a row in a script,
# and Zenix as before afterwards: the memory (as after the first run), the console's input (CON_INPUT), a file written
# and read back. zine runs only at the Amiga's own console: the serial line's input is the shown console's (kernel/tty.c).
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
make -s BUILD=$BD all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
rm -f $BD/zenix.hdf; make -s BUILD=$BD $BD/zenix.hdf >/dev/null 2>&1
OUT=${ZENIX_OUT:-/tmp/zenix-zine}; mkdir -p $OUT
ZENIX_BUILD=$BD tools/run-test.sh fsuae/a1200-020-fast.fs-uae 400 tools/tests/zine-null-test.txt > $OUT/serial.log 2>&1
L=$OUT/serial.log
pass=1
ok() { echo "PASS $1"; }
no() { echo "FAIL $1"; pass=0; }
log=$(tr -d '\r' < $L)
n=$(echo "$log" | grep -ac '^zine: back after [01] s (0)')
[ "$n" = 32 ] || [ "$n" = 33 ] && ok "twenty-one pauses and resumes with the null blob, then the programs (the kernel's 0-1 s line 32 times)" || no "the kernel's 'back' line $n times, not 32 (21 null sessions, zexit, the long name, zhello, zcrash, zbadcall, zalloc, three by name, the group member twice)"
echo "$log" | grep -aq '^count=20 ok=20' && ok "the console's shell ran zine twenty times and went on" || no "the console's loop: $(echo "$log" | grep -a '^count=')"
f1=$(echo "$log" | grep -a -m1 '^fast RAM free' | tr -dc 0-9); f2=$(echo "$log" | grep -a '^fast RAM free' | sed -n 2p | tr -dc 0-9)
c1=$(echo "$log" | grep -a -m1 '^chip RAM free' | tr -dc 0-9); c2=$(echo "$log" | grep -a '^chip RAM free' | sed -n 2p | tr -dc 0-9)
[ -n "$f1" ] && [ "$f1" = "$f2" ] && [ "$c1" = "$c2" ] && ok "free memory after twenty more sessions as after the first (chip $c1 KB, fast $f1 KB)" \
    || no "free memory changed: chip $c1 -> $c2 KB, fast $f1 -> $f2 KB"
echo "$log" | grep -aq '^typed-after' && ok "the console takes input afterwards" || no "the console's input afterwards"
echo "$log" | grep -aq '^disk-ok' && ok "a file written and read back afterwards" || no "the disk afterwards"
# step 3, the real blob (/lib/zine/zine.blob) with the test programs, AmigaOS hunk executables written for the purpose
echo "$log" | grep -aq '^zexit-rc=7' && echo "$log" | grep -aq 'zexit ended (return code 7)' && ok "zexit: an AmigaOS program ran and returned 7" || no "zexit: $(echo "$log" | grep -a 'zexit' | tail -2)"
echo "$log" | grep -aq '^zlong-rc=7' && ok "a program whose name is longer than 31 bytes (the owner's Desire-BaconOfHope-HDVersion.exe)" || no "a long name: $(echo "$log" | grep -a 'zexit-with' | tail -2)"
echo "$log" | grep -aq '^hello from an AmigaOS program' && echo "$log" | grep -aq '^zhello-rc=0' && ok "zhello: dos.library's Output() and Write(), the text shown after" || no "zhello: $(echo "$log" | grep -a 'zhello' | tail -2)"
echo "$log" | grep -aq 'ztake ended (return code 0) after [2-4] s' && echo "$log" | grep -aq '^ztake-rc=0' && ok "ztake: its own Copper list for 150 frames, the display given back" || no "ztake: $(echo "$log" | grep -a 'ztake' | tail -2)"
echo "$log" | grep -aq 'zcrash crashed: illegal instruction at' && echo "$log" | grep -aq '^zcrash-rc=1' && ok "zcrash: an illegal instruction is reported, Zenix is back" || no "zcrash: $(echo "$log" | grep -a 'zcrash' | tail -2)"
echo "$log" | grep -aq 'zbadcall called exec.library ObtainSemaphore (offset -564, from' && ok "zbadcall: a call ZINE lacks is named, the program stopped" || no "zbadcall: $(echo "$log" | grep -a 'zbadcall' | tail -2)"
echo "$log" | grep -aq '^zalloc-rc=0' && ok "zalloc: 1 MB fast and 256 KB chip from AllocMem, written and read back, freed, AvailMem as before" || no "zalloc: $(echo "$log" | grep -a 'zalloc' | tail -2)"
echo "$log" | grep -aq '^zscreen-rc=0' && ok "zscreen: an 8-plane screen from intuition, LoadRGB32's colours, CloseScreen" || no "zscreen: $(echo "$log" | grep -a 'zscreen' | tail -2)"
echo "$log" | grep -aq '^zbg=400 last=399' && ok "a background job writing a file during a session: its requests held at the sync and done afterwards, none lost" || no "the held writes: $(echo "$log" | grep -a '^zbg=' | tail -1)"
# D29 12.1: an executable AmigaOS program runs by its name (the kernel runs /bin/zine PATH ARGS); without x it does not
echo "$log" | grep -aq '^byname1=7' && ok "an AmigaOS program run by its path (/tmp/zx/zexit a b: through zine, return code 7)" || no "by path: $(echo "$log" | grep -a 'byname1' | tail -1)"
echo "$log" | grep -aq '^byname2=7' && ok "an AmigaOS program run as ./zexit" || no "./zexit: $(echo "$log" | grep -a 'byname2' | tail -1)"
echo "$log" | grep -aq '^byname3=7' && ok "an AmigaOS program found on PATH" || no "on PATH: $(echo "$log" | grep -a 'byname3' | tail -1)"
echo "$log" | grep -aq '^byname4=126' && ok "an AmigaOS program without the x bit is not run by name" || no "without x: $(echo "$log" | grep -a 'byname4' | tail -1)"
f3=$(echo "$log" | grep -a '^fast RAM free' | tail -1 | tr -dc 0-9); c3=$(echo "$log" | grep -a '^chip RAM free' | tail -1 | tr -dc 0-9)
[ "$f3" = "$f1" ] && [ "$c3" = "$c1" ] && ok "free memory after the real programs as before them" || no "free memory after the real programs: chip $c3 KB, fast $f3 KB (before: $c1, $f1)"
echo "$log" | grep -aq '^zgroup1=1' && echo "$log" | grep -aq 'zine: not allowed: root and the members of the group zine' && ok "a user outside the group zine is refused" || no "zine group, outside: $(echo "$log" | grep -a 'zgroup1\|group zine' | tail -2)"
echo "$log" | grep -aq '^zgroup2=7' && ok "after gpasswd -a user zine the user runs zine (set-user-id root, the group checked by zine)" || no "zine group, member: $(echo "$log" | grep -a 'zgroup2' | tail -2)"
echo "$log" | grep -aq '^zgroup3=7' && ok "zine reads a 640 program of the group zine for a user in it as a supplementary group" || no "zine group, a group-readable program: $(echo "$log" | grep -a 'zgroup3' | tail -1)"
echo "$log" | grep -aq '^zgroup4=1' && ok "zine refuses a 640 program of root's group to that user (it reads only what the user may)" || no "zine group, root's group: $(echo "$log" | grep -a 'zgroup4' | tail -1)"
echo "$log" | grep -aq '^zine-null-done' || no "the script did not reach its end"
echo "$log" | grep -aiE 'panic|exception|guru' && no "the kernel log has a panic or exception"
exit $((1 - pass))
