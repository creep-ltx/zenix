#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The OFS/FFS server on the PC (tools/ffs-host-test.py). usage: tools/ffs-host-test.sh [fuzz-rounds] (default 440)
cd "$(dirname "$0")/.." || exit 1
exec python3 tools/ffs-host-test.py "$@"
