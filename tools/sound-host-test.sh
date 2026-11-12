#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# DESIGN D40 on the PC: play's file reader and modplay's replayer against generated files (tools/sound-host-test.py).
cd "$(dirname "$0")/.." || exit 1
exec python3 -B tools/sound-host-test.py "$@"
