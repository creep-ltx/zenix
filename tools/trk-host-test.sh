#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# DESIGN D56 on the PC: the tracker player's replayers and mixer against generated modules (tools/trk-host-test.py).
cd "$(dirname "$0")/.." || exit 1
exec python3 -B tools/trk-host-test.py "$@"
