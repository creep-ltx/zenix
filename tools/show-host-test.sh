#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The picture viewer's decoders and renderer on the host (display/show/pic.c, DESIGN D50, docs/d50-viewers.md): the
# pictures and damaged files tools/mkpics.py makes, decoded and compared pixel by pixel or refused, the renderer's
# pens, and 20,000 mutations under AddressSanitizer and UBSan (tools/show-host-test.c).
cd "$(dirname "$0")/.." || exit 1
T=$(mktemp -d "${TMPDIR:-/tmp}/zenix-show.XXXXXX")
trap 'rm -rf "$T"' EXIT
python3 tools/mkpics.py cases "$T/cases" || { echo "FAIL the generator"; exit 1; }
cc -Wall -Wextra -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -o "$T/show-host-test" \
    tools/show-host-test.c display/show/pic.c || { echo "FAIL the host build"; exit 1; }
"$T/show-host-test" "$T/cases" "${SHOW_MUTATIONS:-20000}"
