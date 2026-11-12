#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# DESIGN D40 in FS-UAE, the A1200 with 4 MB of fast RAM (the machine Zenix is made for): sndd started by the first
# call and claiming Paula; play's WAV (8-bit mono, 16-bit stereo 44.1 kHz), 8SVX and AIFF with the periods and
# channels each must give (the server's state block, play -S, taken while it plays: DMACONR's audio bits, each
# channel's period and volume); two plays at once on the two pairs, a third refused; a file that is not sound; Ctrl-C
# stopping a play (the channels off after); modplay with its status line, then on all four channels with frames
# applied; sndd killed under a play (play goes on with a new stream); the bell silent while sndd holds Paula and back
# after sndd's idle end. The files are generated (tools/mksound.py, tools/mkmod.py) and put on the disk with debugfs.
# usage: tools/sound-test.sh
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
make -s BUILD=$BD all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
rm -f $BD/zenix.hdf; make -s BUILD=$BD $BD/zenix.hdf >/dev/null 2>&1 || { echo "FAIL the disk"; exit 1; }
OUT=${ZENIX_OUT:-/tmp/zenix-sound-test}; mkdir -p "$OUT"
export ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenix-sound-serial}
pass=1
L=$OUT/run.log
has() { if tr -d '\r' < "$L" | grep -aqE -- "$2"; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
hasnot() { if tr -d '\r' < "$L" | grep -aqE -- "$2"; then echo "FAIL $1"; pass=0; else echo "PASS $1"; fi; }
# the state block printed after marker M: its lines up to the next prompt
after() { tr -d '\r' < "$L" | awk -v m="^$1\$" '$0 ~ m { on = 1; next } on && /zenix:\/#/ { exit } on'; }
python3 -B tools/mksound.py "$OUT/snd" || { echo "FAIL the sound files"; exit 1; }
python3 -B tools/mkmod.py "$OUT/snd/test.mod" || { echo "FAIL the module"; exit 1; }
{ echo "mkdir /snd"; for f in "$OUT"/snd/*; do echo "write $f /snd/$(basename "$f")"; done; } > "$OUT/debugfs.cmd"
debugfs -w -f "$OUT/debugfs.cmd" "$BD/zenix.hdf" >/dev/null 2>&1 || { echo "FAIL debugfs"; exit 1; }
ZENIX_BUILD=$BD tools/run-test.sh fsuae/a1200-020-fast.fs-uae 420 tools/tests/sound-test.txt 2>&1 | sed 's/\x1b\[[0-9;:]*[mK]//g' > "$L"
has "the script ran to the end" '^(.*# )?sound-test-done$'
has "the first call starts sndd, which claims Paula" 'drv: pid [0-9]+ \(sndd\) claims the device at (0x)?0*dff0a0'
has "play -S: the server's state, PAL" '^sndd: PAL, clock 3546895 Hz, 0 stream'
has "... exit 0" '^(.*# )?s0=0'
S=$(tr -d '\r' < "$L" | sed -n '/^d0-done$/q;p' | grep -c 'audio: first beep')
[ "$S" = 0 ] && echo "PASS the bell is silent while sndd holds Paula (no beep logged)" || { echo "FAIL the bell while sndd holds Paula"; pass=0; }
has "... and again after a restarted sndd" '^.?0$'
S=$(after U1)
echo "$S" | grep -qE '^sndd: .* 0 underrun' && echo "PASS 10 s of 11 kHz sound without an underrun (the reader keeps up on the 68020)" || { echo "FAIL underruns: $S"; pass=0; }
# 1: 8-bit mono 11,025 Hz WAV: period round(3546895 / 11025) = 322, on the pair 0 + 1, volume 64
has "WAV 8-bit mono: its line" '/snd/tone8.wav: WAV 8-bit mono 11025 Hz -> 11015 Hz \(period 322\)'
S=$(after M1)
echo "$S" | grep -qE '^sndd: .* 1 stream\(s\).*DMACONR [0-9a-f]{2}[0-9a-f]3$' && echo "PASS WAV: channels 0 and 1 playing (DMACONR)" || { echo "FAIL WAV: channels 0 and 1 playing: $S"; pass=0; }
echo "$S" | grep -qE '^ch0: on  per 322 vol 64 len 660' && echo "$S" | grep -qE '^ch1: on  per 322 vol 64' &&
    echo "PASS WAV: period 322, volume 64, 60 ms halves (660 bytes)" || { echo "FAIL WAV: period, volume, halves: $S"; pass=0; }
echo "$S" | grep -qE '^ch2: off' && echo "PASS WAV: the other pair free" || { echo "FAIL the other pair free"; pass=0; }
has "... exit 0" '^(.*# )?p1=0'
# 2: 16-bit stereo 44.1 kHz: two to one, 22,050 Hz, period 161
has "WAV 16-bit stereo 44.1 kHz: its line" '/snd/st16.wav: WAV 16-bit stereo 44100 Hz -> 22030 Hz \(period 161\)'
S=$(after M2)
echo "$S" | grep -qE '^ch0: on  per 161' && echo "$S" | grep -qE '^ch1: on  per 161' && echo "PASS stereo: left on 0, right on 1, period 161" || { echo "FAIL stereo: $S"; pass=0; }
has "... exit 0" '^(.*# )?p2=0'
# 3: two plays at once, a third refused
S=$(after M3)
echo "$S" | grep -qE '2 stream\(s\).*DMACONR [0-9a-f]{3}f$' && echo "PASS two plays at once: all four channels" || { echo "FAIL two plays: $S"; pass=0; }
echo "$S" | grep -qE '^ch3: on  per 322 vol 64 .* stream 2' && echo "$S" | grep -qE '^ch2: on  per 322 .* stream 2' && echo "PASS the second on the pair 3 (left) + 2 (right)" || { echo "FAIL the second pair: $S"; pass=0; }
has "a third: sound is in use" '^play: sound is in use by another program'
has "... exit 2" '^(.*# )?busy=2'
# 4-6: 8SVX, AIFF, not sound
has "8SVX: its line (8,363 Hz, period 424)" '/snd/tone.8svx: 8SVX 8-bit mono 8363 Hz -> 8365 Hz \(period 424\)'
has "... exit 0" '^(.*# )?p4=0'
has "AIFF: its line (16,000 Hz, period 222)" '/snd/tone.aiff: AIFF 16-bit mono 16000 Hz -> 15977 Hz \(period 222\)'
has "... exit 0" '^(.*# )?p5=0'
has "not a sound file: said" 'play: /tmp/x: not a WAV, 8SVX or AIFF file'
has "... exit 1" '^(.*# )?p6=1'
# 7: Ctrl-C
has "Ctrl-C ends play (status 130)" '^(.*# )?p7=130'
S=$(after p7=130)
echo "$S" | grep -qE '0 stream\(s\).*DMACONR [0-9a-f]{3}0$' && echo "PASS after Ctrl-C: no stream, the channels' DMA off" || { echo "FAIL after Ctrl-C: $S"; pass=0; }
# 8: modplay
has "modplay: its title line" '/snd/test.mod: "zenix sound test", 31 samples, 2 positions, 2 patterns \(PAL\)'
has "modplay: the status line reaches the second position" 'pos 001/002 pat 01 row [0-9]{2} spd  6 bpm 150'
has "... exit 0" '^(.*# )?m1=0'
S=$(after M8)
echo "$S" | grep -qE '1 stream\(s\).*[1-9][0-9]* frame\(s\).*DMACONR [0-9a-f]{3}f$' && echo "PASS modplay: frames applied, all four channels on" || { echo "FAIL modplay frames: $S"; pass=0; }
echo "$S" | grep -qE ' 0 wait limit' && echo "PASS modplay: no DMA wait ran into its limit" || { echo "FAIL wait limits: $S"; pass=0; }
has "... exit 0" '^(.*# )?m2=0'
# 9: sndd killed under a play: the claim released, a new sndd claims, play goes on
has "sndd killed: its claim released" 'drv: claim of (0x)?0*dff0a0 \(pid [0-9]+\) released \(the driver ended\)'
has "play went on with a new sndd (exit 0)" '^(.*# )?k1=0'
# 10: the bell after sndd's idle end
has "sndd ended by itself after 30 s without a stream" '^(.*# )?n=1'
has "the bell sounds again after (its DMA seen)" 'audio: first beep done, channel 0 end-of-sample flag seen [1-9]'
[ $pass = 1 ] && echo "sound-test: all checks passed" || { echo "sound-test: some checks failed (log $L)"; exit 1; }
