#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The FAT server on the PC (tools/fat-host-test.py). usage: tools/fat-host-test.sh [fuzz-rounds] (default 100)
cd "$(dirname "$0")/.." || exit 1
exec python3 tools/fat-host-test.py "$@"
