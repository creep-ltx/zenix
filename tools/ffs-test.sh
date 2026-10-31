#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# OFS and FFS in the emulator (DESIGN D31): a card as the Pi's: MBR, the ext2 root, a type 0x76 partition holding an RDB with
# DOS\3, DOS\0, DOS\3 with 1 KB blocks, and a volume whose bitmap is marked invalid; and a DOS\1 floppy image without
# a table. mount by itself, list, read, a Swedish name, read-only by default, -o rw: write, rename, delete, soft links,
# a program run from the volume, the server killed; the images judged afterwards on the PC by tools/ffscheck.py and xdfscan.
# usage: tools/ffs-test.sh
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
make -s BUILD=$BD all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
OUT=${ZENIX_OUT:-/tmp/zenix-ffs-test}; mkdir -p "$OUT"
export ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenix-ffs-serial}
export LC_ALL=C.UTF-8 SOURCE_DATE_EPOCH=1700000000
pass=1
check() { if [ "$2" = ok ]; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
has() { if tr -d '\r' < "$L" | grep -aqE -- "$2"; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
hasnot() { if tr -d '\r' < "$L" | grep -aqE -- "$2"; then echo "FAIL $1"; pass=0; else echo "PASS $1"; fi; }
L=$OUT/ffs.log
CARD=$OUT/ffs-card.hdf
F=$OUT/df1.adf
python3 -B tools/ffs-test-images.py "$BD/zenix.hdf" "$CARD" "$F" "$OUT" || { echo "FAIL the images"; exit 1; }
# hard_drive_0_type = rdb: without it FS-UAE hides a file that has no RDB behind a virtual RDB of its own
sed -e "s#hard_drive_0 = .*#hard_drive_0 = $(realpath "$CARD")\nhard_drive_0_type = rdb#" \
    -e "s#^floppy_drive_0 = .*#&\nfloppy_drive_1 = $(realpath "$F")\nfloppy_drive_count = 2\nwritable_floppy_images = 1#" fsuae/a1200-020-fast.fs-uae > "$OUT/ffs.fs-uae"
tools/run-test.sh "$OUT/ffs.fs-uae" 600 tools/tests/ffs-test.txt > "$L" 2>&1
echo "--- in the emulator"
has "the script ran to the end" '^(.*# )?ffs-test-done$'
has "mount finds the type from the RDB inside the MBR's 0x76" 'ide0p3 +/mnt +ffs '
has "a Swedish name is listed" 'räksmörgås\.txt'
has "... read" '^svenska-fran-linux$'
has "a subdirectory, any case" '^in-a-subdirectory$'
BIGSHA=$(sha256sum "$OUT/big.bin" | cut -d' ' -f1)
has "a file with extension blocks reads right" "^$BIGSHA  /mnt/Mapp/big.bin"
has "a soft link: its text" '^\.\./gone\.txt$'
has "... followed" '^to-be-removed$'
has "a hard link: the count" '[Ll]inks:[^0-9]*3'
has "read-only by default" 'refused\.txt.*read-only'
has "writes and reads back" '^hello-from-zenix$'
has "a Swedish name written, found in other case" '^svenska$'
has "a program copied byte for byte" '^(.*# )?same-copy$'
has "ln -s writes a link" '^\.\./ls\.bin$'
has "ln makes a hard link" '^(.*# )?hl-same$'
has "chmod 777 is refused" 'chmod: /mnt/ls\.bin: cannot change'
has "chmod 444 protects from delete, rm answers" 'rm: /mnt/ls\.bin: cannot remove'
has "the program runs from the volume" '^(.*# )?run-from-ffs=0$'
has "a name AmigaDOS cannot hold is refused" 'a:b.*(invalid argument)'
has "a bad option: mount says so" 'cannot mount ide0p3: its filesystem server did not start'
has "... and the log why" 'ffsd: bad option at: nonsense'
has "saved by the timer, the server killed, the restart reads it" '^by-the-timer$'
has "OFS: reads" '^old-file-system$'
has "OFS: a big file" "^$BIGSHA  /mnt/big.bin"
has "OFS: a program copied" '^(.*# )?same-ofs$'
has "1 KB blocks: read from the RDB's geometry" '^one-kb-blocks$'
has "1 KB blocks: read-only even asked for rw" 'blocks over 512 bytes are never written'
has "a bitmap marked invalid: read" '^bitmap-marked-invalid$'
has "... mounted read-only with a line" 'bitmap is marked invalid'
has "not an AmigaDOS volume: refused" 'not an AmigaDOS volume'
has "the floppy (no table) reads" '^on-the-floppy$'
has "... read-only by default" 'nope\.txt.*read-only'
has "... lists what was written" 'skriven\.txt'
hasnot "ffsd did not crash" 'ffsd.*(exception|crash|bus error|address error)'
echo "--- back on the PC"
read -r s2 cyl < "$OUT/layout"
dd if="$CARD" of="$OUT/p3.img" bs=512 skip="$s2" count=$((cyl * 2 / 512)) status=none
dd if="$CARD" of="$OUT/p4.img" bs=512 skip=$((s2 + cyl * 2 / 512)) count=$((cyl / 512)) status=none
dd if="$CARD" of="$OUT/p5.img" bs=512 skip=$((s2 + cyl * 3 / 512)) count=$((cyl / 512)) status=none
python3 -B tools/ffs-test-images.py --judge "$OUT" "$BD/ls.zx" && check "the volumes are whole, every file as written" ok || check "the volumes are whole, every file as written" bad
if command -v xdfscan >/dev/null; then
    for n in p3 p4; do
        cp "$OUT/$n.img" "$OUT/$n.hdf"
        xdfscan "$OUT/$n.hdf" > "$OUT/$n.scan" 2>&1
        if grep -aiE 'traceback|error|mismatch' "$OUT/$n.scan" | grep -qav 'Invalid bitmap'; then check "xdfscan: $n" bad; head -5 "$OUT/$n.scan"; else check "xdfscan: $n" ok; fi
    done
fi
rm -f "${OUT:?}/ffs.fs-uae" "${CARD:?}" "${OUT:?}"/p[345].img "${OUT:?}"/p[34].hdf
exit $((1 - pass))
