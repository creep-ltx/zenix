#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# exFAT in the emulator (DESIGN D47, docs/d47-exfat.md): an MBR card on the A1200's IDE port, partition 1 of type 07
# formatted by the PC's mkfs.exfat and filled by tools/exfat-img.py (contiguous and fragmented FAT-chained files and
# directories, a valid data length below the size, a Swedish name, a long one), partition 2 the ext2 root. mount finds
# the type by itself; files read and their SHA-256 compared with the PC's; names in any case; a directory of 120
# entries; a program run from it; every change refused; -o rw and the options; the server killed and reading on; df;
# afterwards the partition checked on the PC: fsck.exfat clean and not one byte changed.
# usage: tools/exfat-test.sh
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
make -s BUILD=$BD all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
OUT=${ZENIX_OUT:-/tmp/zenix-exfat-test}; mkdir -p "$OUT"
export ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenix-exfat-serial}
export LC_ALL=C.UTF-8
pass=1
check() { if [ "$2" = ok ]; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
has() { if tr -d '\r' < "$L" | grep -aqE -- "$2"; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
hasnot() { if tr -d '\r' < "$L" | grep -aqE -- "$2"; then echo "FAIL $1"; pass=0; else echo "PASS $1"; fi; }
command -v mkfs.exfat >/dev/null && command -v fsck.exfat >/dev/null || { echo "SKIP exfat-test: exfatprogs is not installed"; exit 0; }
L=$OUT/exfat.log
S=$OUT/src
rm -rf "$S"; mkdir -p "$S"
P=$OUT/part.img
rm -f "$P"; truncate -s 32M "$P"
mkfs.exfat -q -P none -L STICK "$P" || { echo "FAIL mkfs.exfat"; exit 1; }
# the files: written here, then into the volume (contiguous and FAT-chained, fragmented on purpose)
python3 - "$S" "$P" "$BD/ls.zx" <<'PY' || { echo "FAIL the volume could not be written"; exit 1; }
import importlib.util, os, random, sys
src, img, prog = sys.argv[1:4]
spec = importlib.util.spec_from_file_location("ei", os.path.join("tools", "exfat-img.py"))
ei = importlib.util.module_from_spec(spec); spec.loader.exec_module(ei)
r = random.Random(47)
files = {"hello.txt": (b"hello-from-exfat\n", {}),
         "räksmörgås.txt": (b"svenska-pa-stickan\n", {}),
         "Mapp/big.bin": (r.randbytes(300001), {}),
         "Mapp/frag.bin": (r.randbytes(200003), {"nofat": False, "fragment": True}),
         "Mapp/small.bin": (r.randbytes(2049), {"nofat": False}),
         "Mapp/vdl.bin": (r.randbytes(20000), {"vdl": 5000}),
         "A long file name with spaces, longer than sixty-three bytes in all.txt": (b"long-name\n", {}),
         "ls.bin": (open(prog, "rb").read(), {}),
         "a/b/c/d/e/f/deep.txt": (b"deep-down\n", {})}
items = [("Mapp", None, {"nofat": False, "fragment": True}), ("Lista", None, {"nofat": False, "fragment": True})]
for p, (d, o) in files.items():
    items.append((p, d, o))
for i in range(120):
    items.append((f"Lista/f{i:03}.txt", b"file %d\n" % i, {}))
ei.build(img, items)
for p, (d, o) in files.items():                    # what a reader must give: zeros past the valid data length
    full = os.path.join(src, p)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    v = o.get("vdl", len(d))
    open(full, "wb").write(d[:v] + bytes(len(d) - v))
PY
fsck.exfat -n "$P" > "$OUT/fsck0.log" 2>&1 && check "the PC's fsck.exfat finds the written volume clean" ok || { check "the PC's fsck.exfat finds the written volume clean" bad; head -20 "$OUT/fsck0.log"; }
# the card: an MBR, partition 1 type 07 (exFAT) at sector 2048, partition 2 the ext2 root (type 83)
CARD=$BD/exfat-card.hdf
python3 - "$CARD" "$P" "$BD/zenix.hdf" <<'PY' || { echo "FAIL the card"; exit 1; }
import struct, sys
out, p1, p2 = sys.argv[1:4]
a, b = open(p1, "rb").read(), open(p2, "rb").read()
s1, n1 = 2048, len(a) // 512
s2, n2 = s1 + n1, (len(b) + 511) // 512
mbr = bytearray(512)
for slot, (ty, st, n) in enumerate(((0x07, s1, n1), (0x83, s2, n2))):
    mbr[446 + 16 * slot:462 + 16 * slot] = struct.pack("<B3sB3sII", 0, b"\xfe\xff\xff", ty, b"\xfe\xff\xff", st, n)
mbr[510:512] = b"\x55\xaa"
with open(out, "wb") as f:
    f.write(mbr); f.seek(s1 * 512); f.write(a); f.seek(s2 * 512); f.write(b)
    f.truncate((s2 + n2 + 63) // 64 * 64 * 512)
PY
cp "$P" "$OUT/before.img"
# hard_drive_0_type = rdb: without it FS-UAE hides a file that has no RDB behind a virtual RDB of its own
sed -e "s#hard_drive_0 = .*#hard_drive_0 = ../$(basename $BD)/exfat-card.hdf\nhard_drive_0_type = rdb#" fsuae/a1200-020-fast.fs-uae > "$OUT/exfat.fs-uae"
case $BD in /*) sed -i "s#\.\./$(basename $BD)/exfat-card.hdf#$BD/exfat-card.hdf#" "$OUT/exfat.fs-uae" ;; esac
tools/run-test.sh "$OUT/exfat.fs-uae" 600 tools/tests/exfat-test.txt > "$L" 2>&1
sum() { sha256sum "$S/$1" | cut -d' ' -f1; }
echo "--- in the emulator"
has "the script ran to the end" '^(.*# )?exfat-test-done$'
has "fdisk lists the partition (32 MB)" 'ide0p1 +65536 +32 MB'
has "mount finds the type by itself" 'ide0p1 +/mnt +exfat '
has "... with its size and free space (statfs from the bitmap)" 'ide0p1 +/mnt +exfat +3[0-9]{4} +[0-9]+'
has "a file reads" '^hello-from-exfat$'
has "... found in any case" '^(.*# )?any-case=0$'
has "a Swedish name, in any case" '^svenska-pa-stickan$'
has "a contiguous 300 KB file: the PC's SHA-256" "^$(sum Mapp/big.bin)  /mnt/Mapp/big.bin"
has "a fragmented FAT chain: the PC's SHA-256" "^$(sum Mapp/frag.bin)  /mnt/Mapp/frag.bin"
has "2049 bytes in a chain: the PC's SHA-256" "^$(sum Mapp/small.bin)  /mnt/Mapp/small.bin"
has "zeros past the valid data length: the PC's SHA-256" "^$(sum Mapp/vdl.bin)  /mnt/Mapp/vdl.bin"
has "a program: the PC's SHA-256" "^$(sum ls.bin)  /mnt/ls.bin"
has "a long name is listed by its start, ~ and its number" 'A long file name with spaces, longer than sixty-thre~[0-9a-f]+'
has "... and read by its whole name" '^long-name$'
has "a deep path" '^deep-down$'
has "a directory of 120 entries (a fragmented chain)" '^(.*# )?count=120$'
has "writing a file is refused" 'refused\.txt.*read-only'
has "a directory too" 'mkdir.*refused.*read-only'
has "removing too" 'rm: /mnt/hello\.txt: .*read-only'
has "the server killed, the restart reads on" '^(.*# )?after-the-kill=0$'
has "-o rw: mount refuses it" 'exFAT volumes are read-only on Zenix: -o rw is refused'
has "the options: fmode=755 lets the program run from the stick" '^(.*# )?run-from-exfat=0$'
has "... uid= is the owner" '^-rwxr-xr-x +1 +5 +root .*/mnt/ls\.bin$'
hasnot "exfatd did not crash" 'exfatd.*(exception|crash|bus error|address error)'
echo "--- back on the PC"
dd if="$CARD" of="$P" bs=512 skip=2048 count=65536 status=none
if cmp -s "$P" "$OUT/before.img"; then echo "PASS the partition: not one byte changed"; else echo "FAIL the partition changed"; pass=0; fi
fsck.exfat -n "$P" > "$OUT/fsck1.log" 2>&1 && check "fsck.exfat still finds it clean" ok || check "fsck.exfat still finds it clean" bad
[ $pass = 1 ] && rm -rf "$S" "$P" "$OUT/before.img" "$OUT/exfat.fs-uae" "$CARD"
exit $((1 - pass))
