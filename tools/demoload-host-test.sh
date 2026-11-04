#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The demo floppy's loader on the PC (DESIGN D25 revision 3, 10 step 2): tools/demoload-host-test.c, built with the
# address and undefined-behaviour sanitizers from the loader's own unpacking source, on two disks made by
# tools/mkdemo.py: a small one (the kernel and a 64 KB ext2 image: the fuzzer's many rounds) and the RAM floppy's
# kernel and image (640 KB), DD and HD. With DEMO_ADF=... also on a built demo disk (its kernel and image given as
# DEMO_KERNEL and DEMO_IMAGE).
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
T=$(mktemp -d "${TMPDIR:-/tmp}/zenix-demoload.XXXXXX") || exit 1
trap 'rm -rf "$T"' EXIT
make -s BUILD=$BD $BD/kernel.bin $BD/root-ram.img $BD/demoboot.bin $BD/demoload.zx >/dev/null 2>&1 || { echo "FAIL build"; exit 1; }
cc -std=gnu11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -Wall -Wextra -Wno-sign-compare \
   -Iboot -Iuser/miniz -o $T/t tools/demoload-host-test.c boot/demounpack.c user/miniz/miniz_tinfl.c || { echo "FAIL compile"; exit 1; }
pass=1
truncate -s 64K $T/small.img; mke2fs -q -t ext2 -E revision=0 -b 1024 -N 32 -F $T/small.img 64 >/dev/null 2>&1 || { echo "FAIL mke2fs"; exit 1; }
run() {   # name adf kernel image rounds
    echo "== $1"
    $T/t "$2" "$3" "$4" "$5" || pass=0
}
python3 tools/mkdemo.py $BD/demoboot.bin $BD/demoload.zx $BD/kernel.bin $T/small.img $T/small.adf dd >/dev/null || pass=0
run "a small disk, the fuzzer's 3000 rounds" $T/small.adf $BD/kernel.bin $T/small.img 3000
for kind in dd hd; do
    python3 tools/mkdemo.py $BD/demoboot.bin $BD/demoload.zx $BD/kernel.bin $BD/root-ram.img $T/ram-$kind.adf $kind >/dev/null || pass=0
    run "the RAM floppy's kernel and image on a ${kind^^} disk" $T/ram-$kind.adf $BD/kernel.bin $BD/root-ram.img 200
done
# the bootblock's own checks, as boot/demoboot.S makes them, on the disk's bytes: Kickstart's checksum, offset 8 (the
# stage 2's size on the disk) and 1020 (its memory), the ZEXE header and every relocation inside the image and even
python3 - $T/ram-dd.adf $BD/demoload.zx <<'PY' && echo "PASS the bootblock: checksum, the stage 2's size and memory, its relocations as demoboot.S checks them" || { echo "FAIL the bootblock's fields"; pass=0; }
import struct, sys
d = open(sys.argv[1], "rb").read(); s2 = open(sys.argv[2], "rb").read()
bb = d[:1024]; s = 0
for (v,) in struct.iter_unpack(">I", bb):
    s += v
    if s > 0xFFFFFFFF: s = (s & 0xFFFFFFFF) + 1
assert bb[:4] == b"DOS\0" and s == 0xFFFFFFFF, "checksum"
size, mem = struct.unpack(">I", bb[8:12])[0], struct.unpack(">I", bb[1020:1024])[0]
assert size % 512 == 0 and size >= len(s2) and d[1024:1024 + len(s2)] == s2, "stage 2 size"
img = d[1024:1024 + size]
assert img[4:8] == b"ZEXE"
image, m, nrel, roff = struct.unpack(">4I", img[8:24])
assert image <= size and mem >= m and roff >= image and roff + 4 * nrel <= size and nrel < 0x10000
for i in range(nrel):
    o = struct.unpack(">I", img[roff + 4 * i:roff + 4 * i + 4])[0]
    assert o <= image - 4 and o % 2 == 0, "relocation %d" % o
PY
if [ -n "$DEMO_ADF" ]; then run "the demo disk $DEMO_ADF" "$DEMO_ADF" "$DEMO_KERNEL" "$DEMO_IMAGE" 100; fi
# mkdemo refuses an image that does not fit, saying by how much, and writes nothing
head -c 1048576 /dev/urandom > $T/noise; truncate -s 1200K $T/big.img; mke2fs -q -t ext2 -b 1024 -F $T/big.img 1200 >/dev/null 2>&1
python3 - "$T/big.img" "$T/noise" <<'PY'
import sys; img = bytearray(open(sys.argv[1], "rb").read()); n = open(sys.argv[2], "rb").read()
img[200 * 1024:200 * 1024 + len(n)] = n; open(sys.argv[1], "wb").write(img)
PY
if python3 tools/mkdemo.py $BD/demoboot.bin $BD/demoload.zx $BD/kernel.bin $T/big.img $T/big.adf dd > $T/big.log 2>&1; then
    echo "FAIL mkdemo accepted an image too big for a DD disk"; pass=0
elif grep -q "bytes too big for a DD disk" $T/big.log && [ ! -e $T/big.adf ]; then echo "PASS mkdemo refuses an image too big, saying by how much, writing nothing"
else echo "FAIL mkdemo's refusal: $(cat $T/big.log)"; pass=0; fi
[ $pass = 1 ] && echo "PASS demoload host test" || echo "FAIL demoload host test"
exit $((1 - pass))
