#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Host test of the kernel's allocator (kernel/mem.c) around the 1 GB line (MEM_LOW1G): tools/mem-test.sh
set -e
cd "$(dirname "$0")/.."
mkdir -p build
cc -m32 -fno-pie -no-pie -Wall -Wextra -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -Wno-builtin-declaration-mismatch -O1 -g -fsanitize=undefined -Wl,--defsym,__zinit_start=0x20010100 -Wl,--defsym,__zinit_end=0x20018100 \
    -Wl,--defsym,__zhwcd_start=0x20018100 -Wl,--defsym,__zhwcd_end=0x20018400 -o build/mem-test tools/mem-test.c
build/mem-test
