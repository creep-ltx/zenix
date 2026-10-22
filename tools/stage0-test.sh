#!/usr/bin/env bash
# Host test of boot/stage0.c against a real device tree: tools/stage0-test.sh <bcm2711-rpi-cm4.dtb>
set -e
cd "$(dirname "$0")/.."
mkdir -p build
cc -Wall -Wextra -O1 -g -fsanitize=address,undefined -o build/stage0-test tools/stage0-test.c -lfdt
build/stage0-test "${1:?usage: $0 file.dtb}"
