#!/usr/bin/env bash
# The terminal bell starts Paula's audio DMA: after `beep`, the kernel log says the channel's end-of-sample flag was seen.
# usage: tools/audio-test.sh   (ZENIX_SERIAL defaults to /tmp/zenix-audio-serial)
cd "$(dirname "$0")/.." || exit 1
make -s all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
export ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenix-audio-serial}
while pgrep -x fs-uae >/dev/null; do sleep 1; done
OUT=/tmp/zenix-audio.log
tools/run-test.sh fsuae/a1200-020-fast.fs-uae 45 tools/tests/audio-test.txt > $OUT 2>&1
if grep -qE '^audio: first beep done, channel 0 end-of-sample flag seen [1-9][0-9]* time' $OUT; then echo "PASS beep runs the audio DMA"; else echo "FAIL beep runs the audio DMA"; exit 1; fi
