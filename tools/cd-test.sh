#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The CD drive in the emulator (DESIGN D44, docs/d44-cd.md): an ATAPI CD drive as the slave of the A1200's IDE port
# (FS-UAE: cdrom_drive_0 with cdrom_drive_0_controller = ide1) holding an ISO 9660 image made by the PC's bsdtar (Rock
# Ridge and Joliet). The kernel finds the drive; raw reads of cd0 (whole and at a sector inside a 2048-byte block) give
# the image's bytes; mount by itself, list, read, SHA-256 sums against the PC's, a Swedish name, links, a deep directory
# Rock Ridge moved, a directory over many blocks, a program run from the disc, everything that would change refused, the
# server killed, -o norock (Joliet's names) and plain ISO names with the mount options.
# usage: tools/cd-test.sh
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
make -s BUILD=$BD all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
OUT=${ZENIX_OUT:-/tmp/zenix-cd-test}; mkdir -p "$OUT"
export ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenix-cd-serial}
export LC_ALL=C.UTF-8
pass=1
has() { if tr -d '\r' < "$L" | grep -aqE -- "$2"; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
hasnot() { if tr -d '\r' < "$L" | grep -aqE -- "$2"; then echo "FAIL $1"; pass=0; else echo "PASS $1"; fi; }
command -v bsdtar >/dev/null || { echo "SKIP cd-test: bsdtar (libarchive) is not installed"; exit 0; }
L=$OUT/cd.log
S=$OUT/src
rm -rf "$S"; mkdir -p "$S/Mapp" "$S/Lista" "$S/a/b/c/d/e/f/g/h/i/j"
echo hello-from-the-cd > "$S/hello.txt"
echo svenska-pa-skivan > "$S/räksmörgås.txt"
echo inner > "$S/Mapp/inner.txt"
echo deep-down > "$S/a/b/c/d/e/f/g/h/i/j/deep.txt"
python3 -c "import random; r = random.Random(44); open('$S/Mapp/big.bin','wb').write(r.randbytes(300001)); open('$S/Mapp/small.bin','wb').write(r.randbytes(2049))"
for i in $(seq -w 0 119); do echo "file $i" > "$S/Lista/f$i.txt"; done
cp "$BD/ls.zx" "$S/ls.bin"; chmod 755 "$S/ls.bin"
ln -s ../hello.txt "$S/Mapp/up"; ln -s /hello.txt "$S/abs"
ISO=$OUT/cd.iso
bsdtar --format iso9660 -cf "$ISO" -C "$S" . 2>"$OUT/bsdtar.log" || { echo "FAIL bsdtar"; cat "$OUT/bsdtar.log"; exit 1; }
sed -e "s#^hard_drive_0_controller = .*#&\ncdrom_drive_0 = $(realpath "$ISO")\ncdrom_drive_0_controller = ide1#" fsuae/a1200-020-fast.fs-uae > "$OUT/cd.fs-uae"
case $BD in /*) sed -i "s#\.\./build/#$BD/#" "$OUT/cd.fs-uae" ;; esac
cp "$ISO" "$OUT/before.iso"
tools/run-test.sh "$OUT/cd.fs-uae" 600 tools/tests/cd-test.txt > "$L" 2>&1
sum() { sha256sum "$1" | cut -d' ' -f1; }
echo "--- in the emulator"
has "the script ran to the end" '^(.*# )?cd-test-done$'
has "the kernel finds the CD drive as the slave" 'ide: ATAPI slave CD drive: '
has "a raw read inside a 2048-byte block (cd0 sectors 65-67)" "^$(dd if="$ISO" bs=512 skip=65 count=3 status=none | sha256sum | cut -d' ' -f1)  /tmp/odd.bin"
has "the whole disc read raw is the image" "^$(sum "$ISO")  /tmp/whole.iso"
has "mount finds the type by itself" 'cd0 +/mnt +iso9660 '
has "a file reads" '^hello-from-the-cd$'
has "a Swedish name (Rock Ridge)" '^svenska-pa-skivan$'
has "a 300 KB file: the PC's SHA-256" "^$(sum "$S/Mapp/big.bin")  /mnt/Mapp/big.bin"
has "a program: the PC's SHA-256" "^$(sum "$S/ls.bin")  /mnt/ls.bin"
has "2049 bytes: the PC's SHA-256" "^$(sum "$S/Mapp/small.bin")  /mnt/Mapp/small.bin"
has "a symbolic link's text" '^\.\./hello\.txt$'
has "... an absolute one's" '^/hello\.txt$'
has "a deep directory (moved by Rock Ridge)" '^deep-down$'
has "a directory over many blocks" '(^|[^0-9])120$'
has ".. inside the disc" '^/mnt/Mapp/\.\./Lista/f077\.txt$'
has "a program runs from the disc" 'run-from-cd=0$'
has "writing a file is refused" 'refused\.txt.*read-only'
has "a directory too" 'mkdir.*refused.*read-only'
has "removing too" 'rm: /mnt/hello\.txt: .*read-only'
has "the server killed, the restart reads on" '^(.*# )?after-the-kill=0$'
has "no raw open while mounted" 'raw-while-mounted=1$'
has "-o rw: mount refuses it" 'ISO 9660 volumes are read-only on Zenix: -o rw is refused'
has "another type on the CD is refused" 'cd0 is read-only: only -t iso9660'
has "-o norock: no Rock Ridge link (Joliet's tree has an empty file there)" '^-r-xr-xr-x +[0-9]+ root +root +0 .*/mnt/Mapp/up$'
has "... Joliet's long Swedish name" 'joliet-name=0$'
has "plain ISO names, the options' owner, the default mode 555" '^-r-xr-xr-x +[0-9]+ 5 +root .*HELLO\.TXT$'
has "... a directory's mode from the options" '^drwxr-x--- +[0-9]+ 5 +root .*/mnt/MAPP$'
has "... a plain name found in any case" 'plain-cat=0$'
hasnot "iso9660d did not crash" 'iso9660d.*(exception|crash|bus error|address error)'
echo "--- back on the PC"
if cmp -s "$ISO" "$OUT/before.iso"; then echo "PASS the image: not one byte changed"; else echo "FAIL the image changed"; pass=0; fi
[ $pass = 1 ] && rm -rf "$S" "$ISO" "$OUT/before.iso" "$OUT/cd.fs-uae"
exit $((1 - pass))
