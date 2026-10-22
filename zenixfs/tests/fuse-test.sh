#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# FUSE tests (need fuse3): a plain-tools round trip, then kill -9 of the filesystem in the middle of a big copy, 8 times.
# Every killed image must pass fsck. Run: make fuse-test
set -e
cd "$(dirname "$0")/.."
M=$(mktemp -d); I=$M/disk.zenixfs; mkdir $M/mnt
cleanup() { fusermount3 -uz $M/mnt 2>/dev/null || true; rm -rf $M; }; trap cleanup EXIT
build/zenixfs-tool mkfs $I 128 >/dev/null
build/zenixfs-fuse $I $M/mnt; sleep 0.5
cp -a ../docs $M/mnt/docs; cp -a ../kernel $M/mnt/kernel
diff -r ../docs $M/mnt/docs; diff -r ../kernel $M/mnt/kernel
head -c 3000000 /dev/urandom > $M/rand; cp $M/rand $M/mnt/rand; cmp $M/rand $M/mnt/rand
mkdir $M/mnt/t; echo hi > $M/mnt/t/a; ln $M/mnt/t/a $M/mnt/t/b; ln -s a $M/mnt/t/c; mv $M/mnt/t/a $M/mnt/t/d
[ "$(cat $M/mnt/t/b)" = hi ] && [ "$(stat -c %h $M/mnt/t/d)" = 2 ]
rm -rf $M/mnt/kernel $M/mnt/t
fusermount3 -u $M/mnt 2>/dev/null || { sleep 1; fusermount3 -u $M/mnt; }
build/zenixfs-tool fsck $I >/dev/null
echo "fuse round trip: ok"
for round in 1 2 3 4 5 6 7 8; do
  rm -f $I; build/zenixfs-tool mkfs $I 128 >/dev/null
  build/zenixfs-fuse $I $M/mnt; sleep 0.4
  ( for i in 1 2 3 4 5 6 7 8 9; do cp -a ../kernel $M/mnt/k$i; cp -a ../docs $M/mnt/d$i; head -c 2000000 /dev/urandom > $M/mnt/big$i; done ) 2>/dev/null &
  copier=$!; sleep 0.0$round
  pkill -9 -f "zenixfs-fuse $I" || true
  wait $copier 2>/dev/null || true
  fusermount3 -uz $M/mnt 2>/dev/null || true; sleep 0.3
  build/zenixfs-tool fsck $I >/dev/null
done
echo "kill -9 test: 8 killed images, all clean"
