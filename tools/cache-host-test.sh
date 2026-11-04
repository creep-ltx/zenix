#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# DESIGN D37: the 68040/68060 data cache's block chooser (kernel/cache_plan.h) on the PC, for the Apollo 1260, FS-UAE's
# two 32-bit RAM kinds, A4000 layouts, Zorro II fast only, and boards that are not RAM inside and outside the block
# (tools/cache-host-test.c).
cd "$(dirname "$0")/.." || exit 1
T=$(mktemp -d /tmp/zenix-cache-host.XXXXXX)
trap 'rm -rf "$T"' EXIT
cc -Wall -Wextra -O1 -g -fsanitize=address,undefined -o "$T/cache-host-test" tools/cache-host-test.c || { echo "FAIL the host build"; exit 1; }
"$T/cache-host-test"
