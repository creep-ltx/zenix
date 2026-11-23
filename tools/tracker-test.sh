#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# DESIGN D56 in FS-UAE, the A1200 with 4 MB of fast RAM: modplay through the sound server with modules of our own
# (tools/mktrk.py): an 8-channel MOD mixed into one stereo stream (the server's state: channels 0 and 1 at the
# mixing rate's period), a MED module on Paula's four channels as frames, an S3M at 11,025 Hz, an XM; a folder as a
# list with a damaged file in it (said, the next one played, exit 1); the full screen with its views and keys; a keys
# file; the mixer's measure (-B); a file that is not there.
# usage: tools/tracker-test.sh
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
make -s BUILD=$BD all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
rm -f $BD/zenix.hdf; make -s BUILD=$BD $BD/zenix.hdf >/dev/null 2>&1 || { echo "FAIL the disk"; exit 1; }
OUT=${ZENIX_OUT:-/tmp/zenix-tracker-test}; mkdir -p "$OUT"
export ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenix-tracker-serial}
pass=1
L=$OUT/run.log
has() { if tr -d '\r' < "$L" | grep -aqE -- "$2"; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
# the state block printed after marker M: its lines up to the next prompt
after() { tr -d '\r' < "$L" | awk -v m="^$1\$" '$0 ~ m { on = 1; next } on && /zenix:\/#/ { exit } on'; }
python3 -B tools/mktrk.py "$OUT/trk" || { echo "FAIL the modules"; exit 1; }
mkdir -p "$OUT/list"
cp "$OUT/trk/tune.s3m" "$OUT/list/a.s3m"; head -c 300 "$OUT/trk/tune.xm" > "$OUT/list/b.xm"; cp "$OUT/trk/tune.med" "$OUT/list/c.med"
{ echo "mkdir /trk"; echo "mkdir /trk/list"
  for f in "$OUT"/trk/*; do echo "write $f /trk/$(basename "$f")"; done
  for f in "$OUT"/list/*; do echo "write $f /trk/list/$(basename "$f")"; done; } > "$OUT/debugfs.cmd"
debugfs -w -f "$OUT/debugfs.cmd" "$BD/zenix.hdf" >/dev/null 2>&1 || { echo "FAIL debugfs"; exit 1; }
ZENIX_BUILD=$BD tools/run-test.sh fsuae/a1200-020-fast.fs-uae 600 tools/tests/tracker-test.txt 2>&1 | sed 's/\x1b\[[0-9;:?]*[a-zA-Z]//g' > "$L"
has "the script ran to the end" '^(.*# )?tracker-test-done$'
has "modplay -K: the default keys" '^q quit'
has "modplay: help on ?, F1 and Help, as every text UI (audit 2026-11-23 OS-27)" '^(.*# )?helpkeys=3$'
# 1: the 8-channel MOD, mixed: one stereo stream, channels 0 and 1 at 16,000 Hz (period round(3546895 / 16000) = 222)
has "8CHN: its line (mixed)" '/trk/tune8.mod: "zenix eight", 8-channel MOD, 8 channels, 2 positions \(mixed, PAL\)'
S=$(after T1)
echo "$S" | grep -qE '1 stream\(s\).*DMACONR [0-9a-f]{3}3$' && echo "$S" | grep -qE '^ch0: on  per 222 ' && echo "$S" | grep -qE '^ch1: on  per 222 ' &&
    echo "PASS 8CHN mixed: channels 0 and 1 on at period 222" || { echo "FAIL 8CHN mixed: $S"; pass=0; }
has "8CHN: the status line reaches the second position" 'pos 001/002 pat 01 row [0-9]{2} spd  6 bpm 125'
has "... exit 0" '^(.*# )?t0=0'
has "... exit 0" '^(.*# )?t1=0'
# 2: MED on Paula: frames on all four channels
S=$(after T2)
echo "$S" | grep -qE '1 stream\(s\).*[1-9][0-9]* frame\(s\).*DMACONR [0-9a-f]{3}f$' && echo "PASS MED: frames on all four channels" || { echo "FAIL MED frames: $S"; pass=0; }
echo "$S" | grep -qE '^ch1: on  per 214 ' && echo "PASS MED: track 1's C-3 at period 214" || { echo "FAIL MED period: $S"; pass=0; }
has "... exit 0" '^(.*# )?t2=0'
# 3: S3M at 11,025 Hz: period round(3546895 / 11025) = 322
S=$(after T3)
echo "$S" | grep -qE '^ch0: on  per 322 ' && echo "PASS S3M at -r 11025: period 322" || { echo "FAIL S3M rate: $S"; pass=0; }
has "... exit 0" '^(.*# )?t3=0'
S=$(after T4)
echo "$S" | grep -qE '^ch0: on  per 222 ' && echo "PASS XM: mixed at period 222" || { echo "FAIL XM: $S"; pass=0; }
has "... exit 0" '^(.*# )?t4=0'
# 5: the folder: a.s3m, the cut b.xm (said), c.med; exit 1 for the one it could not play
has "the folder: the cut XM said" 'modplay: /trk/list/b.xm: a damaged XM'
has "... and the list played on: exit 1" '^(.*# )?t5=1'
# 6: the full screen: the title, the views, quit
has "the screen: the song's title and kind" 'zenix xm.*FastTracker 2, 8 ch mixed'
has "the screen: the instruments view" ' 02 pingsine'
has "the screen: the keys view" 'louder +\+ ='
has "the screen: q quits, exit 0" '^(.*# )?u1=0'
# 7: a keys file: q does nothing, x quits
has "the keys file: q taken away, x quits (exit 0)" '^(.*# )?u2=0'
# 8: the mixer's measure (emulator time: a note of what FS-UAE's 68020 does, not a speed claim)
has "modplay -B: the measure's line" '/trk/busy8.xm: FastTracker 2, 8 channels, 16000 Hz: 5 s of sound in [0-9]+ ms'
# the 68k mixer (its assembly loop) makes the bytes the PC's C mixer makes: the same FNV-1a over 5 s at 16 kHz
cc -O1 -DTRK_HOST -o "$OUT/tht" tools/trk-host-test.c user/trk.c user/modrep.c user/medrep.c user/s3mrep.c user/xmrep.c user/trkmix.c &&
    F=$("$OUT/tht" mix "$OUT/trk/busy8.xm" 16000 80000 "$OUT/busy8.pcm" | sed -n 's/.*fnv //p')
has "the 68k mixer's bytes are the PC's (fnv $F)" "busy8.xm: .* fnv $F\$"
has "... exit 0" '^(.*# )?b1=0'
has "a file that is not there: said, exit 1" '^(.*# )?e1=1'
[ $pass = 1 ] && echo "tracker-test: all checks passed" || { echo "tracker-test: some checks failed (log $L)"; exit 1; }
