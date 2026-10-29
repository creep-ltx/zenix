#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Copy one of the owner's terminal programs (MIT, the owner's TextTools repository: Sumi the editor, Maki the pager,
# Markdown the viewer) into ports/PROG/upstream/ (DESIGN D27 section 9, rule 1: one source for AmigaOS and Zenix, a
# pinned copy here): its sources, its README.md (as upstream/README: the user's documentation, which docs/man/PROG.txt
# follows) and the licence, taken from one commit, unchanged. Zenix's own layer (ports/common/, ports/PROG/zenix/) is
# not touched. For an update:
#   tools/texttools-vendor.sh sumi|maki|markdown /path/to/TextTools [COMMIT]      (COMMIT: default the checkout's HEAD)
# then put the commit it prints into ports/PROG/README, build, run tools/PROG-test.sh, and bring docs/man/PROG.txt in
# step with the README's changes.
set -e
cd "$(dirname "$0")/.."
P=$1; S=$2; C=${3:-HEAD}
case "$P" in
    sumi)     SRC="sumi.h main.c buf.c file.c syntax.c clip.c keys.c" ;;
    maki)     SRC="maki.h filecol.h main.c src.c keys.c drawer.c filecol.c" ;;
    markdown) SRC="markdown.h md.h main.c buf.c file.c syntax.c clip.c keys.c md.c mdedit.c view.c" ;;
    *) echo "usage: tools/texttools-vendor.sh sumi|maki|markdown /path/to/TextTools [COMMIT]"; exit 1 ;;
esac
git -C "$S" rev-parse --verify -q "$C^{commit}" >/dev/null || { echo "texttools-vendor: $S has no commit $C"; exit 1; }
H=$(git -C "$S" rev-parse "$C^{commit}")
D=ports/$P/upstream
FILES="LICENSE $P/README.md"
for f in $SRC; do FILES="$FILES $P/src/$f"; done
rm -rf "$D"
for f in $FILES; do
    o=$D/${f#$P/}
    [ "$o" = $D/README.md ] && o=$D/README       # (no .md outside docs/: tools/docs-check.sh)
    mkdir -p "$(dirname "$o")"
    git -C "$S" show "$H:$f" > "$o"
done
echo "texttools-vendor: $P, $(echo $FILES | wc -w) files from commit $H ($(git -C "$S" show -s --format=%cs "$H"))"
