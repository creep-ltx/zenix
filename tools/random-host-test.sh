#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The entropy pool on the host (DESIGN D19 9.3): kernel/random.c's BLAKE2s and ChaCha20 against the RFCs' vectors, the
# crediting rules and the seed file's rule (tools/random-host-test.c); then mkroot.sh and mkzup.py refusing a random
# seed in an image or a package.
cd "$(dirname "$0")/.." || exit 1
T=$(mktemp -d /tmp/zenix-random.XXXXXX)
trap 'rm -rf "$T"' EXIT
pass=1
cc -Wall -Wextra -O1 -g -fsanitize=address,undefined -o "$T/random-host-test" tools/random-host-test.c || { echo "FAIL the host build"; exit 1; }
"$T/random-host-test" || pass=0
# mkroot.sh refuses an image with a seed: a copy of the tree's tools and rootfs with one added
mkdir -p "$T/tree" && cp -r tools rootfs "$T/tree/" && mkdir -p "$T/tree/rootfs/var/lib" && echo x > "$T/tree/rootfs/var/lib/random-seed"
mkdir -p "$T/b" && : > "$T/b/hello.zx"
if NOMAN=1 NOTESTS=1 "$T/tree/tools/mkroot.sh" "$T/r.img" "$T/b" "hello" 1M 64 >"$T/mk.log" 2>&1; then
    echo "FAIL mkroot.sh accepted an image with a random seed"; pass=0
elif grep -q 'refusing to put a random seed' "$T/mk.log" && [ ! -e "$T/r.img" ]; then
    echo "PASS mkroot.sh refuses an image with a random seed"
else
    echo "FAIL mkroot.sh failed for another reason: $(cat "$T/mk.log")"; pass=0
fi
if python3 -c 'import sys; sys.path.insert(0, "tools"); import mkzup; mkzup.check_path("/var/lib/random-seed")' >"$T/zup.log" 2>&1; then
    echo "FAIL mkzup.py accepted a random seed in a package"; pass=0
elif grep -q 'refusing to put a random seed' "$T/zup.log"; then
    echo "PASS mkzup.py refuses a random seed in a package"
else
    echo "FAIL mkzup.py: $(cat "$T/zup.log")"; pass=0
fi
# mkcard.py leaves a seed off the card (audit 2026-10-28 S10: FS-UAE boots build/zenix.hdf in place and may write one)
mkdir -p "$T/rt/var/lib" "$T/rt/etc" && echo seed > "$T/rt/var/lib/random-seed" && echo x > "$T/rt/etc/keep"
mke2fs -q -t ext2 -b 1024 -d "$T/rt" "$T/root.img" 4M >/dev/null 2>&1 && python3 tools/mkffs.py "$T/boot.img" 504K ZenixBoot "$T/rt/etc/keep=keep" >/dev/null 2>&1 ||
    { echo "FAIL could not make the test images"; pass=0; }
if python3 tools/mkcard.py ide "$T/card.hdf" "$T/boot.img" "$T/root.img" >"$T/card.log" 2>&1 &&
   grep -q 'left off the card' "$T/card.log" && debugfs -R "stat /var/lib/random-seed" "$T/root.img" 2>/dev/null | grep -q Inode: &&
   python3 tools/mkcard.py sd "$T/card.img" "$T/boot.img" "$T/root.img" >>"$T/card.log" 2>&1 &&
   ! cmp -s "$T/card.img" /dev/null; then
    off=$(python3 -c 'import struct,sys; d=open(sys.argv[1],"rb").read(512); print(struct.unpack("<I", d[446+32+8:446+32+12])[0]*512)' "$T/card.img")
    dd if="$T/card.img" of="$T/p3.img" bs=1024 skip=$((off / 1024)) count=4096 status=none
    if debugfs -R "stat /var/lib/random-seed" "$T/p3.img" 2>&1 | grep -q 'File not found' &&
       debugfs -R "cat /etc/keep" "$T/p3.img" 2>/dev/null | grep -qx x; then
        echo "PASS mkcard.py leaves a root's random seed off the card (the root itself unchanged)"
    else echo "FAIL mkcard.py put the seed on the card: $(debugfs -R 'ls -l /var/lib' "$T/p3.img" 2>&1 | tail -3)"; pass=0; fi
else
    echo "FAIL mkcard.py with a seeded root: $(cat "$T/card.log")"; pass=0
fi
exit $((1 - pass))
