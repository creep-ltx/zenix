#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# unlha and unlzx on Zenix in FS-UAE, the stock A1200 (68EC020, 2 MB chip, DESIGN D33): list, test and unpack each kind, the files compared with the
# PC's own by checksum, a Swedish name, modes, refusals and damage, memory. The archives go onto the disk with debugfs; see tools/mkarc.py.
# usage: tools/arc-test.sh
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
make -s BUILD=$BD all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
rm -f $BD/zenix.hdf; make -s BUILD=$BD $BD/zenix.hdf >/dev/null 2>&1
OUT=${ZENIX_OUT:-/tmp/zenix-arc-test}; mkdir -p "$OUT"
export ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenix-arc-serial}
export LC_ALL=C.UTF-8
pass=1
L=$OUT/run.log
has() { if tr -d '\r' < "$L" | grep -aqE -- "$2"; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
hasnot() { if tr -d '\r' < "$L" | grep -aqE -- "$2"; then echo "FAIL $1"; pass=0; else echo "PASS $1"; fi; }
python3 -B tools/mkarc.py "$OUT/set" >/dev/null || { echo "FAIL the test set"; exit 1; }
S=$OUT/set; A=tools/tests/archives
{
    echo "mkdir /arc"
    for f in $A/lh1-1.lzh $A/lh5-1.lzh $A/lh6-2.lzh $A/lh7-0.lzh $A/amiga-1.lha $S/all.lzx $S/lh4-1.lzh $S/dotdot.lzx $S/dotdot.lzh $S/link.lzh $S/badcrc.lzh \
             $S/badcrc.lzx $S/bomb.lzx $S/nolha.lzh; do
        echo "write $f /arc/$(basename "$f")"
    done
} > "$OUT/debugfs.cmd"
debugfs -w -f "$OUT/debugfs.cmd" "$BD/zenix.hdf" >/dev/null 2>&1 || { echo "FAIL debugfs"; exit 1; }
tools/run-test.sh fsuae/a1200-020.fs-uae 600 tools/tests/arc-test.txt 2>&1 | sed 's/\x1b\[[0-9;:]*[mK]//g' > "$L"
sha() { python3 -B -c "
import hashlib,importlib.util,sys
spec=importlib.util.spec_from_file_location('mkarc','tools/mkarc.py'); m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
d=eval(sys.argv[1],{'p':m.payload}); print(hashlib.sha256(d).hexdigest())" "$1"; }
has "the script ran to the end" '^(.*# )?arc-test-done$'
for k in lh1 lh5 lh6 lh7 lh4; do has "-$k-: every checksum right" "^(.*# )?t-$k=0([^0-9]|\$)"; done
has "the Amiga's LhA: the listing's protection bits" 'hsparwed -lh0- f_all'
has "... a filenote" ': a note here'
has "... exit 0" '^(.*# )?l-amiga=0([^0-9]|$)'
has "unpacked on Zenix" '^(.*# )?x-lh5=0([^0-9]|$)'
has "-lh5-: text byte for byte" "^$(sha 'p("text")[:6000]')  /tmp/u/text.txt"
has "-lh5-: a folder, noise byte for byte" "^$(sha 'p("noise")[:800]')  /tmp/u/sub/noise.bin"
has "a Swedish name" '^svenska$'
has "... found" '^(.*# )?swedish=0([^0-9]|$)'
has "LZX: the list" 'r.ksm.rg.s\.txt'
has "LZX: a filenote listed" ': a filenote'
has "LZX: unpacked" '^(.*# )?x-lzx=0([^0-9]|$)'
has "LZX normal: byte for byte" "^$(sha 'p("text")')  /tmp/z/normal.txt"
has "LZX aligned: byte for byte" "^$(sha 'p("mixed")')  /tmp/z/aligned.bin"
has "LZX in a merged group: byte for byte" "^$(sha 'p("mixed")[:16000]')  /tmp/z/merged/b.bin"
has "LZX a second group: byte for byte" "^$(sha 'p("zeros")')  /tmp/z/second/z.bin"
has "LZX: the Swedish name" '^(.*# )?zswedish=0([^0-9]|$)'
has "the mode from the protection bits" '^-rwxr-xr-x .*note\.txt'
has "nothing overwritten without -f" 'note\.txt: already there \(-f overwrites\)'
has "... exit 1" '^(.*# )?again=1([^0-9]|$)'
has "-f overwrites" '^(.*# )?force=0([^0-9]|$)'
has "a name leading out, LZX: refused" 'unlzx: \.\./escaped\.txt: refused'
has "... LhA" 'unlha: \.\./escaped\.txt: refused'
has "... a link" 'refused'
has "... exit 1" '^(.*# )?dd-lzx=1([^0-9]|$)'
has "... exit 1" '^(.*# )?dd-lha=1([^0-9]|$)'
has "... exit 1" '^(.*# )?link=1([^0-9]|$)'
has "the good member still unpacked" '^good\.txt$'
hasnot "... the refused one is nowhere" '^(/tmp/)?escaped\.txt$'
has "a bad checksum, LhA" 'unlha: bad\.txt: checksum error'
has "... exit 1" '^(.*# )?crc-lha=1([^0-9]|$)'
has "a bad checksum, LZX" 'unlzx: bad\.txt: checksum error'
has "... exit 1" '^(.*# )?crc-lzx=1([^0-9]|$)'
has "a bomb: the limit says so" 'over the limit of 512 MB'
has "... exit 2" '^(.*# )?bomb=2([^0-9]|$)'
has "not an archive" 'unlha: /arc/nolha\.lzh: the archive is damaged'
has "... exit 2" '^(.*# )?not=2([^0-9]|$)'
has "usage" 'usage: unlzx l\|t\|x'
has "... exit 2" '^(.*# )?usage=2([^0-9]|$)'
hasnot "no crash" '(exception|bus error|address error|illegal instruction)'
[ "$pass" = 1 ] && echo "arc-test: ok" || { echo "arc-test: FAILED"; exit 1; }
