#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# PFS3 in the emulator (DESIGN D32): a card as the Pi's: MBR, the ext2 root, a type 0x76 partition holding an RDB with PFS\3 (a
# volume of tools/mkpfs3.py), PFS\3 with 4 KB blocks in the table and super blocks, PDS\3 with damage, an empty FFS partition.
# mount by itself, list, read, a Swedish name, links, read-only always, a program run from the volume, the server killed,
# damage answered with errors; the volumes compared afterwards on the PC: not one byte changed.
# usage: tools/pfs3-test.sh
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
make -s BUILD=$BD all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
OUT=${ZENIX_OUT:-/tmp/zenix-pfs3-test}; mkdir -p "$OUT"
export ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenix-pfs3-serial}
export LC_ALL=C.UTF-8 SOURCE_DATE_EPOCH=1700000000
pass=1
check() { if [ "$2" = ok ]; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
has() { if tr -d '\r' < "$L" | grep -aqE -- "$2"; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
hasnot() { if tr -d '\r' < "$L" | grep -aqE -- "$2"; then echo "FAIL $1"; pass=0; else echo "PASS $1"; fi; }
L=$OUT/pfs3.log
CARD=$OUT/pfs3-card.hdf
python3 -B tools/pfs3-test-images.py "$BD/zenix.hdf" "$CARD" "$OUT" "$BD/ls.zx" || { echo "FAIL the images"; exit 1; }
sed -e "s#hard_drive_0 = .*#hard_drive_0 = $(realpath "$CARD")\nhard_drive_0_type = rdb#" fsuae/a1200-020-fast.fs-uae > "$OUT/pfs3.fs-uae"
tools/run-test.sh "$OUT/pfs3.fs-uae" 800 tools/tests/pfs3-test.txt > "$L" 2>&1
echo "--- in the emulator"
has "the script ran to the end" '^(.*# )?pfs3-test-done$'
has "mount finds the type from the RDB inside the MBR's 0x76" 'ide0p3 +/mnt +pfs3 '
has "a Swedish name is listed" 'räksmörgås\.txt'
has "... read, in any case" '^svenska-fran-amigaos$'
has "a subdirectory, any case" '^in-a-subdirectory$'
BIGSHA=$(sha256sum "$OUT/big.bin" | cut -d' ' -f1)
has "a file in many pieces reads right" "^$BIGSHA  /mnt/Mapp/big.bin"
has "a soft link: its text" '^\.\./hardname$'
has "a hard link: the count" '[Ll]inks:[^0-9]*2'
has "a listing over many blocks" '(^|[^0-9])400$'
has "the protection bits as a mode" '^-r-xr-xr-x .*skyddad'
has "writing a file is refused" 'refused\.txt.*read-only'
has "a directory too" 'mkdir.*refused.*read-only'
has "removing too" 'rm: /mnt/hardname: .*read-only'
has "a program copied byte for byte" '^(.*# )?same-copy$'
has "the program runs from the volume" 'run-from-pfs3=0$'
has "the server killed, the restart reads on" '^(.*# )?after-the-kill=0$'
has "-o rw: mount refuses it" 'PFS3 volumes are read-only on Zenix: -o rw is refused'
has "a bad option: the server says so" 'pfs3d: bad option at: nonsense'
has "owner and modes from the options" '^-rw-r----- +[0-9]+ 5 +6 .*ls\.bin'
has "... the directory's" '^drwxr-x--- +[0-9]+ 5 +6 .*/mnt'
has "4 KB blocks in the table, an original PFS volume: read" '^with-super-blocks$'
has "... a big file through super blocks" "^$BIGSHA  /mnt/big.bin"
has "PDS\\\\3 found by itself, read" '^still-good$'
has "damage: a chain that loops is an error" '^(.*# )?loop-chain=1$'
has "damage: a broken directory is an error" '^(.*# )?bad-dir=1'
has "... ls says what" 'ls: /mnt/Mapp: .*(I/O|input/output|error)'
has "... and the rest of the volume still reads" '^(.*# )?root-still=0$'
has "an empty partition refused, with the reason" 'pfs3d: ide0p6: not a PFS volume'
has "the ext2 root (or any other type) refused" 'pfs3d: ide0p2: (in use|not a PFS volume)'
hasnot "pfs3d did not crash" 'pfs3d.*(exception|crash|bus error|address error)'
echo "--- back on the PC"
read -r s2 a b c < "$OUT/layout"
cmpvol() {  # the volume in the card as it was made
    if dd if="$CARD" bs=512 skip="$2" count="$3" status=none | cmp -s - "$OUT/$1"; then check "$1: not one byte changed" ok; else check "$1: not one byte changed" bad; fi
}
cmpvol a.img "$s2" $((a / 512))
cmpvol b.img $((s2 + a / 512)) $((b / 512))
cmpvol c.img $((s2 + (a + b) / 512)) $((c / 512))
rm -f "${OUT:?}/pfs3.fs-uae" "${CARD:?}" "${OUT:?}"/[abc].img "${OUT:?}/rdb.img"
exit $((1 - pass))
