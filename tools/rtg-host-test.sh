#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The display server's drawing on a graphics card (display/server/fb.c, DESIGN D43) on the PC, under AddressSanitizer
# (tools/rtg-host-test.c: every drawing operation against a plain model, every format's bytes, the conversion into a
# card with the pointer, clipped).
cd "$(dirname "$0")/.." || exit 1
T=$(mktemp -d "${TMPDIR:-/tmp}/zenix-rtg-host.XXXXXX")
trap 'rm -rf "$T"' EXIT
cc -Wall -Wextra -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -o "$T/rtg-host-test" \
    tools/rtg-host-test.c display/server/fb.c || { echo "FAIL the host build"; exit 1; }
"$T/rtg-host-test"
