#!/usr/bin/env bash
# The keyboard in FS-UAE: Swedish keymap layers (plain, Shift, Alt, Shift+Alt), Caps Lock and key repeat, typed with
# wtype on the emulated Amiga keyboard (tools/kbd-test.py, headless cage rig), written to files by the shell and read
# back from the disk image. ZENIX_SERIAL=/tmp/name gives the run its own serial port.
cd "$(dirname "$0")/.." || exit 1
make -s all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
rm -f build/zenix.hdf; make -s build/zenix.hdf >/dev/null 2>&1
OUT=/tmp/zenix-kbd; mkdir -p $OUT
SERIAL=${ZENIX_SERIAL:-/tmp/zenix-serial}
sed -e "s#/tmp/zenix-serial#$SERIAL#" -e "s#\(\.\./\)\{1,\}build/#$PWD/build/#g" fsuae/a1200-020-fast.fs-uae > $OUT/kbd.fs-uae
python3 tools/kbd-test.py $OUT/kbd.fs-uae "$SERIAL" > $OUT/run.log 2>&1
pass=1
get() { debugfs -R "cat $1" build/zenix.hdf 2>/dev/null; }
want() { if [ "$(get $1 | od -An -tx1 | tr -d ' \n')" = "$2" ]; then echo "PASS $3"; else echo "FAIL $3: got $(get $1 | od -An -tx1 | tr -d '\n')"; pass=0; fi; }
# å ö ä £ [ { ² \ newline, Latin-1
want /tmp/k1.txt "e5f6e4a35b7bb25c0a" "Swedish layers: plain, Shift, Alt, Shift+Alt"
want /tmp/k2.txt "41423163640a" "Caps Lock on letters only, Shift inverts it"
n=$(get /tmp/k3.txt | tr -cd z | wc -c)
if [ "$n" -ge 10 ] && [ "$n" -le 40 ]; then echo "PASS key repeat ($n z's from a 1.2 s hold)"; else echo "FAIL key repeat: $n z's"; pass=0; fi
exit $((1 - pass))
