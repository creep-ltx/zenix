#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The Pi's HVS driver's words, EDID and mode list (display/server/hvslist.c, DESIGN D63) on the PC, under
# AddressSanitizer (tools/hvs-host-test.c: the list words against hand-worked values, the geometry, the modes, EDID
# blocks good and broken, the answer line).
cd "$(dirname "$0")/.." || exit 1
T=$(mktemp -d "${TMPDIR:-/tmp}/zenix-hvs-host.XXXXXX")
trap 'rm -rf "$T"' EXIT
cc -Wall -Wextra -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -o "$T/hvs-host-test" \
    tools/hvs-host-test.c display/server/hvslist.c || { echo "FAIL the host build"; exit 1; }
"$T/hvs-host-test"
