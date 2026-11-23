#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# DESIGN D61: the pointer's arithmetic (kernel/pointer.h) on the PC (tools/mouse-host-test.c).
cd "$(dirname "$0")/.." || exit 1
T=$(mktemp -d "${TMPDIR:-/tmp}/zenix-mouse-host.XXXXXX")
trap 'rm -rf "$T"' EXIT
cc -Wall -Wextra -O1 -g -fsanitize=address,undefined -o "$T/mouse-host-test" tools/mouse-host-test.c || { echo "FAIL the host build"; exit 1; }
"$T/mouse-host-test"
