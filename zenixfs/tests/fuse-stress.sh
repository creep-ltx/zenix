#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Stress test of a mounted ZenixFS on its own fresh 256 MB image: make fuse-stress (quick) or ARGS="--heavy --rounds 3" make fuse-stress
set -e
cd "$(dirname "$0")/.."
W=$(mktemp -d)
trap 'fusermount3 -u "$W/mnt" 2>/dev/null || true; rm -rf "$W"' EXIT
mkdir "$W/mnt"
build/zenixfs-tool mkfs "$W/disk.zenixfs" 256 >/dev/null
build/zenixfs-fuse "$W/disk.zenixfs" "$W/mnt"
sleep 0.5
nice -n 10 python3 tests/fuse-stress.py "$W/mnt" "$W/disk.zenixfs" ${ARGS:---quick}
