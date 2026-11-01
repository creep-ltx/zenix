#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The installer on the PC (tools/zinstall-host-test.py). usage: tools/zinstall-host-test.sh [fuzz-rounds]
cd "$(dirname "$0")/.." || exit 1
exec python3 -B tools/zinstall-host-test.py "$@"
