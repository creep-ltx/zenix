#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Host test of the terminal (kernel/term.c) against a model of the display: tools/term-test.sh
set -e
cd "$(dirname "$0")/.."
mkdir -p build
cc -Wall -Wextra -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -O1 -g -fsanitize=address,undefined -o build/term-test tools/term-test.c
build/term-test
