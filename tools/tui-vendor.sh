#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Copy the owner's text-UI library (tui.library, MIT) into ports/tui/upstream/ (DESIGN D27 section 9, rule 1: one
# source for AmigaOS and Zenix, a pinned copy here): the files the Zenix build compiles or includes and the licence,
# taken from one commit of the owner's AmigaLibs repository, unchanged. Zenix's own layer (ports/tui/zenix/) is not
# touched. For an update:
#   tools/tui-vendor.sh /path/to/AmigaLibs [COMMIT]      (COMMIT: default the checkout's HEAD)
# then put the commit it prints into ports/tui/README, build, and run tools/tui-test.sh.
set -e
cd "$(dirname "$0")/.."
S=$1; C=${2:-HEAD}
git -C "$S" rev-parse --verify -q "$C^{commit}" >/dev/null || { echo "tui-vendor: $S has no commit $C"; exit 1; }
H=$(git -C "$S" rev-parse "$C^{commit}")
D=ports/tui/upstream
FILES="LICENSE tui/src/core.c tui/src/glue.c tui/src/version.h tui/include/tui.h
       tui/include/libraries/tui.h tui/include/clib/tui_protos.h"
rm -rf "$D"
for f in $FILES; do
    o=$D/${f#tui/}
    mkdir -p "$(dirname "$o")"
    git -C "$S" show "$H:$f" > "$o"
done
echo "tui-vendor: $(echo $FILES | wc -w) files from commit $H ($(git -C "$S" show -s --format=%cs "$H"))"
