#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The AmigaOS bitmap font reader on the host (display/server/amifont.c, docs/d38-desktop.md "Fonts"): the fonts and
# damaged files tools/mkamifont.py makes, read and compared, and mutations of a good one under AddressSanitizer
# (tools/amifont-host-test.c).
cd "$(dirname "$0")/.." || exit 1
T=$(mktemp -d "${TMPDIR:-/tmp}/zenix-amifont.XXXXXX")
trap 'rm -rf "$T"' EXIT
python3 tools/mkamifont.py cases "$T/cases" || { echo "FAIL the generator"; exit 1; }
cc -Wall -Wextra -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -o "$T/amifont-host-test" \
    tools/amifont-host-test.c display/server/amifont.c || { echo "FAIL the host build"; exit 1; }
"$T/amifont-host-test" "$T/cases"
