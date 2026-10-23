#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Host test of the Amiga MFM track coding (kernel/mfm.h): tools/mfm-test.sh
set -e
cd "$(dirname "$0")/.."
mkdir -p build
cc -Wall -Wextra -O1 -g -fsanitize=address,undefined -o build/mfm-test tools/mfm-test.c
build/mfm-test
