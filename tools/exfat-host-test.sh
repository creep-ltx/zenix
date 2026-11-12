#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The exFAT server on the PC (tools/exfat-host-test.py, DESIGN D47). usage: tools/exfat-host-test.sh [fuzz-rounds] (default 600)
cd "$(dirname "$0")/.." || exit 1
exec python3 tools/exfat-host-test.py "$@"
