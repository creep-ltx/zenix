#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The PFS3 server on the PC (tools/pfs3-host-test.py). usage: tools/pfs3-host-test.sh [fuzz-rounds] (default 1500)
cd "$(dirname "$0")/.." || exit 1
exec python3 tools/pfs3-host-test.py "$@"
