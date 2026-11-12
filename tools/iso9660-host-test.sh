#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The ISO 9660 server on the PC (tools/iso9660-host-test.py, DESIGN D44). usage: tools/iso9660-host-test.sh [fuzz-rounds] (default 600)
cd "$(dirname "$0")/.." || exit 1
exec python3 tools/iso9660-host-test.py "$@"
