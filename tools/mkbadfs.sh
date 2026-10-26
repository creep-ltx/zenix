#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Build build/scratch.hdf (ZENIX_BUILD/scratch.hdf if that is set): an RDB disk whose one partition holds an ext2 filesystem with deliberate damage,
# to test fsck.ext2 against. usage: tools/mkbadfs.sh [clean]
set -e
cd "$(dirname "$0")/.."
T=$(mktemp -d)
mkdir -p $T/root/bin $T/root/etc $T/root/home
BD=${ZENIX_BUILD:-build}       # the build directory (make check gives every test a copy of its own)
cp $BD/ls.zx $T/root/bin/ls; cp $BD/sh.zx $T/root/bin/sh
echo hello > $T/root/etc/motd; echo readme > $T/root/home/readme.txt
head -c 20000 /dev/urandom > $T/root/home/big.bin
mke2fs -q -t ext2 -E revision=0 -b 1024 -N 128 -d $T/root -F $T/part.img 4M
if [ "$1" != clean ]; then
    debugfs -w $T/part.img -f - >/dev/null 2>&1 <<CMDS
sif /etc/motd links_count 5
unlink /home/readme.txt
freei /bin/ls
sif /home/big.bin blocks 7
freeb 40
CMDS
fi
python3 tools/mkrdb.py $T/part.img $BD/scratch.hdf
rm -rf $T
