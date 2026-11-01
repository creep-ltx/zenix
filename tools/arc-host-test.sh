#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# unlha and unlzx on the PC (tools/arc-host-test.py). usage: tools/arc-host-test.sh [fuzz-rounds] (default 4000)
cd "$(dirname "$0")/.." || exit 1
exec python3 -B tools/arc-host-test.py "$@"
