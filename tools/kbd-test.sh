#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The keyboard in FS-UAE: Swedish keymap layers (plain, Shift, Alt, Shift+Alt), Caps Lock, key repeat, typing
# during heavy output, scrollback at the prompt (Shift+PgUp/PgDn) and the console keys (Amiga+F2, Amiga+F1), typed with wtype on the emulated Amiga keyboard (tools/kbd-test.py, headless cage rig),
# written to files by the shell and read back from the disk image. ZENIX_SERIAL=/tmp/name gives the run its own
# serial port.
cd "$(dirname "$0")/.." || exit 1
make -s all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
rm -f build/zenix.hdf; make -s build/zenix.hdf >/dev/null 2>&1
OUT=${ZENIX_OUT:-/tmp/zenix-kbd}; mkdir -p $OUT
SERIAL=${ZENIX_SERIAL:-/tmp/zenix-serial}
ZENIX_SERIAL=$SERIAL python3 tools/fsuae_conf.py fsuae/a1200-020-fast.fs-uae $OUT/kbd.fs-uae || exit 1
python3 tools/kbd-test.py $OUT/kbd.fs-uae "$SERIAL" > $OUT/run.log 2>&1
pass=1
get() { debugfs -R "cat $1" build/zenix.hdf 2>/dev/null; }
want() { if [ "$(get $1 | od -An -tx1 | tr -d ' \n')" = "$2" ]; then echo "PASS $3"; else echo "FAIL $3: got $(get $1 | od -An -tx1 | tr -d '\n')"; pass=0; fi; }
# å ö ä £ [ { ² \ newline, Latin-1
want /tmp/k1.txt "e5f6e4a35b7bb25c0a" "Swedish layers: plain, Shift, Alt, Shift+Alt"
want /tmp/k2.txt "41423163640a" "Caps Lock on letters only, Shift inverts it"
n=$(get /tmp/k3.txt | tr -cd z | wc -c)
if [ "$n" -ge 10 ] && [ "$n" -le 40 ]; then echo "PASS key repeat ($n z's from a 1.2 s hold)"; else echo "FAIL key repeat: $n z's"; pass=0; fi
if [ "$(get /tmp/k4.txt)" = "typed during output" ]; then echo "PASS keys typed during heavy output arrive"; else echo "FAIL typed during output: got '$(get /tmp/k4.txt)'"; pass=0; fi
if [ "$(get /tmp/k6.txt | grep -c .)" = "2" ]; then echo "PASS Amiga+F2: console 2 has its own login (two logins run)"; else echo "FAIL Amiga+F2: logins running: '$(get /tmp/k6.txt)'"; pass=0; fi
if debugfs -R "stat /tmp/k7.txt" build/zenix.hdf 2>/dev/null | grep -q 'Size: 0$'; then echo "PASS Amiga+F1: back on console 1 (its shell has no jobs; console 2's has one)"; else echo "FAIL Amiga+F1: jobs '$(get /tmp/k7.txt)' (or no file)"; pass=0; fi
if get /tmp/k5.txt | grep -q 'lost\|queue full'; then echo "FAIL the kernel log reports lost input: $(get /tmp/k5.txt | grep 'lost\|queue full')"; pass=0
elif get /tmp/k5.txt | grep -q 'Zenix'; then echo "PASS no input lost (kernel log)"; else echo "FAIL no kernel log in k5.txt"; pass=0; fi
# 4b: the screen after Shift+PgUp differs from before (older lines; digits cover only about 1% of the screen, so a
# whole screenful of other numbers changes about 1% of the pixels), after Shift+PgDn it is the same again
d=$(python3 - "$OUT" <<'PY'
import sys
from PIL import Image
o = sys.argv[1]
a, b, c = (Image.open(f"{o}/sb{i}.png").convert("L") for i in range(3))
def diff(x, y):
    px, py = x.tobytes(), y.tobytes()
    return sum(1 for p, q in zip(px, py) if abs(p - q) > 40) / len(px)
print(f"{diff(a, b):.4f} {diff(a, c):.4f}")
PY
)
read -r up back <<< "$d"
if python3 -c "import sys; sys.exit(0 if $up > 0.003 and $back < 0.0005 else 1)" 2>/dev/null; then echo "PASS Shift+PgUp scrolls back at the prompt, Shift+PgDn returns ($up / $back of the pixels changed)"
else echo "FAIL scrollback at the prompt: $up of the pixels changed after Shift+PgUp, $back after Shift+PgDn"; pass=0; fi
exit $((1 - pass))
