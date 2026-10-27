#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The wall clock on the host (audit 2026-10-26 F8): kernel/clock.h across the tick count's wrap and past 2038
# (tools/clock-host-test.c).
cd "$(dirname "$0")/.." || exit 1
T=$(mktemp -d /tmp/zenix-clock.XXXXXX)
trap 'rm -rf "$T"' EXIT
cc -Wall -Wextra -O1 -g -fsanitize=address,undefined -o "$T/clock-host-test" tools/clock-host-test.c || { echo "FAIL the host build"; exit 1; }
"$T/clock-host-test"
