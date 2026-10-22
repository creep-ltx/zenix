#!/usr/bin/env bash
# Host test of kernel/emmc.c against an SDHCI model: tools/emmc-test.sh   (TRACE=1 shows every command)
set -e
cd "$(dirname "$0")/.."
mkdir -p build
cc -Wall -Wextra -O1 -g -fsanitize=address,undefined -o build/emmc-test tools/emmc-test.c
build/emmc-test
