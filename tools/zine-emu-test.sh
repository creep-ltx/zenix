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
n=$(echo "$log" | grep -ac '^zine: back after [0-9]* s (0)')
[ "$n" = 41 ] && ok "every session came back (the kernel's 'back' line 41 times)" || no "the kernel's 'back' line $n times, not 41 (21 null sessions; zexit, the long name, zhello, ztake, zcrash, zbadcall, zalloc, zscreen, zovl twice, zpkt twice, zmmu, zfpu; ztake with the background writer; three by name; the group member twice)"
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
# 16.6: an overlaid program: its root loaded with the overlay handle and tables, two nodes through its own overlay
# supervisor (Seek, LoadSeg(NULL, table, handle)), one replacing the other, the data after the nodes read through the
# handle; then an overlay entry with no node behind it: LoadSeg's refusal named and the supervisor's alert
echo "$log" | grep -aq '^ZINE: zovl is overlaid: 2 later parts' && ok "zovl: zine sees an overlaid program (2 later parts)" || no "zovl: zine's overlay line: $(echo "$log" | grep -a 'zovl is' | tail -1)"
echo "$log" | grep -aq '^zovl: LoadSeg wrote the overlay handle, the overlay table and the hunk table into the root' && ok "zovl: LoadSeg wrote the overlay handle and both tables into the root's overlay header" || no "zovl: the overlay header: $(echo "$log" | grep -a '^zovl' | head -2)"
na=$(echo "$log" | grep -ac "^zovl: node A ran; the root's data says: hello from the root's data hunk$")
[ "$na" = 2 ] && ok "zovl: node A ran twice (loaded, replaced by B, loaded again), relocated to itself and to the root's data hunk" || no "zovl: node A's line $na times, not 2"
echo "$log" | grep -aq '^zovl: node B ran; its BSS hunk was clear$' && echo "$log" | grep -aq "^zovl: node B's BSS slot cleared when A came back" && ok "zovl: node B ran in A's place (its code and chip BSS), its slot cleared after" || no "zovl: node B: $(echo "$log" | grep -a 'node B' | tail -2)"
echo "$log" | grep -aq '^zovl: the data after the last node read through the overlay handle' && echo "$log" | grep -aq '^zovl: memory as before' && echo "$log" | grep -aq '^zovl-rc=0' && ok "zovl: the data after the nodes read through the overlay handle, memory as before, return code 0" || no "zovl: the end: $(echo "$log" | grep -a 'zovl' | tail -3)"
echo "$log" | grep -aq '^ZINE: zovl is damaged (no overlay part where its overlay table points) (the overlay part at byte [0-9]*)' && echo "$log" | grep -aq 'zovl crashed (alert 0700000c: its overlay supervisor could not load a part)' && echo "$log" | grep -aq '^zovlbad-rc=1' && ok "zovl bad: LoadSeg's refusal named, the supervisor's AN_BadOverlay ends it, Zenix back" || no "zovl bad: $(echo "$log" | grep -a 'zovl' | tail -3)"
# 16.7: a cruncher's data after its overlay table (Spaceballs' Peek's form), read with packets the program sends to its
# overlay handle's fh_Type itself; a packet to Output()'s; dos Execute answered done, not run; a dos call ZINE lacks
# shows its first argument's text
echo "$log" | grep -aq '^ZINE: zpkt keeps 1 KB of data after its overlay table and reads it itself through its overlay file' && ok "zpkt: zine's line for an overlay table with data and no parts" || no "zpkt: zine's line: $(echo "$log" | grep -a 'ZINE: zpkt' | head -1)"
echo "$log" | grep -aq "^zpkt: the data after the overlay table read with packets to the overlay handle's handler port" && echo "$log" | grep -aq '^ZINE: a packet of type 9999 sent to a file.s handler, which ZINE does not know: answered with an error' && ok "zpkt: ACTION_SEEK from the end, ACTION_READ, ACTION_FLUSH answered through fh_Type, an unknown action refused and named" || no "zpkt: the packets: $(echo "$log" | grep -a 'zpkt' | tail -3)"
echo "$log" | grep -aq "^zpkt: written with a packet to Output()'s handler port" && ok "zpkt: ACTION_WRITE to Output()'s handler port" || no "zpkt: the written packet"
echo "$log" | grep -aq '^ZINE: zpkt asked dos Execute to run "run >nil: setpatch": not run (ZINE has no shell), answered as done' && echo "$log" | grep -aq '^zpkt: Execute answered done' && echo "$log" | grep -aq '^zpkt-rc=0' && ok "zpkt: Execute named, not run, answered DOSTRUE; return code 0" || no "zpkt: Execute: $(echo "$log" | grep -a 'Execute\|zpkt-rc' | tail -2)"
echo "$log" | grep -aq '^ZINE: the call.s first argument (d1) reads "RAM:zpkt-gone"' && echo "$log" | grep -aq 'zpkt called dos.library DeleteFile (offset -72, from' && echo "$log" | grep -aq '^zpktu-rc=1' && ok "zpkt u: a dos call ZINE lacks named with its first argument's text" || no "zpkt u: $(echo "$log" | grep -a 'DeleteFile\|first argument\|zpktu' | tail -3)"
# (a kernel log line may land inside the zbg line: the count and the last line are checked apart)
echo "$log" | grep -aq '^zbg=400' && echo "$log" | grep -aq 'last=399$' && ok "a background job writing a file during a session: its requests held at the sync and done afterwards, none lost" || no "the held writes: $(echo "$log" | grep -a '^zbg=' | tail -1)"
# 16.5: a 68030 MMU instruction on a CPU without that MMU (the 68020 here) is a no-op; an FPU instruction this machine
# lacks (no FPU here) is named in the crash line
echo "$log" | grep -aq '^zmmu-rc=0' && echo "$log" | grep -aq 'ZINE: an MMU instruction (PMOVE, PFLUSH, PTEST) taken as a no-op' && ok "zmmu: PMOVE to and from TC, TT0, CRP, SRP (memory, (An)+, -(sp), d16(An), #imm), PFLUSHA, and in supervisor mode the 68040's PFLUSHA and PMOVE TC, as no-ops on a 68020" || no "zmmu: $(echo "$log" | grep -a 'zmmu\|MMU instruction' | tail -2)"
echo "$log" | grep -aq 'zfpu crashed: line F instruction at .* (instruction f200 000e: FSIN, ' && echo "$log" | grep -aq '^zfpu-rc=1' && ok "zfpu: FSIN without an FPU: the crash line names it" || no "zfpu: $(echo "$log" | grep -a 'zfpu' | tail -2)"
# D29 12.1: an executable AmigaOS program runs by its name (the kernel runs /bin/zine PATH ARGS); without x it does not
echo "$log" | grep -aq '^byname1=7' && ok "an AmigaOS program run by its path (/tmp/zx/zexit a b: through zine, return code 7)" || no "by path: $(echo "$log" | grep -a 'byname1' | tail -1)"
echo "$log" | grep -aq '^byname2=7' && ok "an AmigaOS program run as ./zexit" || no "./zexit: $(echo "$log" | grep -a 'byname2' | tail -1)"
echo "$log" | grep -aq '^byname3=7' && ok "an AmigaOS program found on PATH" || no "on PATH: $(echo "$log" | grep -a 'byname3' | tail -1)"
echo "$log" | grep -aq '^byname4=126' && ok "an AmigaOS program without the x bit is not run by name" || no "without x: $(echo "$log" | grep -a 'byname4' | tail -1)"
f3=$(echo "$log" | grep -a '^fast RAM free' | tail -1 | tr -dc 0-9); c3=$(echo "$log" | grep -a '^chip RAM free' | tail -1 | tr -dc 0-9)
# the mark: taken after the files above are written, just before the real programs (they stay in the RAM disk)
f1=$(echo "$log" | grep -a '^fast RAM free' | sed -n 3p | tr -dc 0-9); c1=$(echo "$log" | grep -a '^chip RAM free' | sed -n 3p | tr -dc 0-9)
[ "$f3" = "$f1" ] && [ "$c3" = "$c1" ] && ok "free memory after the real programs as before them" || no "free memory after the real programs: chip $c3 KB, fast $f3 KB (before: $c1, $f1)"
echo "$log" | grep -aq '^zgroup1=1' && echo "$log" | grep -aq 'zine: not allowed: root and the members of the group zine' && ok "a user outside the group zine is refused" || no "zine group, outside: $(echo "$log" | grep -a 'zgroup1\|group zine' | tail -2)"
echo "$log" | grep -aq '^zgroup2=7' && ok "after gpasswd -a user zine the user runs zine (set-user-id root, the group checked by zine)" || no "zine group, member: $(echo "$log" | grep -a 'zgroup2' | tail -2)"
echo "$log" | grep -aq '^zgroup3=7' && ok "zine reads a 640 program of the group zine for a user in it as a supplementary group" || no "zine group, a group-readable program: $(echo "$log" | grep -a 'zgroup3' | tail -1)"
echo "$log" | grep -aq '^zgroup4=1' && ok "zine refuses a 640 program of root's group to that user (it reads only what the user may)" || no "zine group, root's group: $(echo "$log" | grep -a 'zgroup4' | tail -1)"
echo "$log" | grep -aq '^zine-null-done' || no "the script did not reach its end"
# 16.4/16.5 on a 68040 with its FPU (the matrix's A1200 040, 8 MB fast): the MMU's registers saved and put back around
# each session, the 68030 MMU instructions as no-ops, FSIN (which the emulated 040 lacks, as a real one) named
L4=$OUT/serial-040.log
ZENIX_BUILD=$BD tools/run-test.sh fsuae/matrix/a1200-040.fs-uae 300 tools/tests/zine-040-test.txt > $L4 2>&1
log4=$(tr -d '\r' < $L4)
echo "$log4" | grep -aq '^zmmu-rc=0' && echo "$log4" | grep -aq 'ZINE: an MMU instruction (PMOVE, PFLUSH, PTEST) taken as a no-op' && ok "68040: zmmu's 68030 MMU instructions as no-ops" || no "68040 zmmu: $(echo "$log4" | grep -a 'zmmu\|MMU instruction' | tail -2)"
echo "$log4" | grep -aq 'zfpu crashed: line F instruction at .* (instruction f200 000e: FSIN, ' && echo "$log4" | grep -aq '^zfpu-rc=1' && ok "68040: FSIN (the 040 lacks it) named, from its frame (format 2: the next instruction's PC)" || no "68040 zfpu: $(echo "$log4" | grep -a 'zfpu' | tail -2)"
echo "$log4" | grep -aq '^ztake-rc=0' && echo "$log4" | grep -aq '^zcrash-rc=1' && ok "68040: ztake and zcrash, Zenix back each time (the MMU registers and caches restored)" || no "68040 ztake/zcrash: $(echo "$log4" | grep -a 'ztake-rc\|zcrash-rc' | tail -2)"
g1=$(echo "$log4" | grep -a -m1 '^fast RAM free' | tr -dc 0-9); g2=$(echo "$log4" | grep -a '^fast RAM free' | sed -n 2p | tr -dc 0-9)
[ -n "$g1" ] && [ "$g1" = "$g2" ] && echo "$log4" | grep -aq '^zine-040-done' && echo "$log4" | grep -aq '^disk-ok' && ok "68040: free memory as before, the disk written afterwards" || no "68040: memory $g1 -> $g2 KB, or the end not reached"
echo "$log4" | grep -aiE 'panic|exception|guru' && no "68040: the kernel log has a panic or exception"
echo "$log" | grep -aiE 'panic|exception|guru' && no "the kernel log has a panic or exception"
exit $((1 - pass))
