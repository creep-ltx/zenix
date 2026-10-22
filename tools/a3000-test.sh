#!/usr/bin/env bash
# Host test of kernel/a3000.c against a WD33C93A + DMAC model: tools/a3000-test.sh   (TRACE=1 shows each command)
set -e
cd "$(dirname "$0")/.."
mkdir -p build
cc -Wall -Wextra -O1 -g -fsanitize=address,undefined -o build/a3000-test tools/a3000-test.c
build/a3000-test
