#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Copy the owner's text editor Sumi (MIT) into ports/sumi/upstream/ (DESIGN D27 section 9, rule 1: one source for
# AmigaOS and Zenix, a pinned copy here): its sources, its README.md (as upstream/README: the user's documentation, which docs/man/sumi.txt
# follows) and the licence, taken from one commit of the owner's TextTools repository, unchanged. Zenix's own layer
# (ports/sumi/zenix/) is not touched. For an update:
#   tools/sumi-vendor.sh /path/to/TextTools [COMMIT]      (COMMIT: default the checkout's HEAD)
# then put the commit it prints into ports/sumi/README, build, run tools/sumi-test.sh, and bring docs/man/sumi.txt in
# step with the README's changes.
set -e
cd "$(dirname "$0")/.."
S=$1; C=${2:-HEAD}
git -C "$S" rev-parse --verify -q "$C^{commit}" >/dev/null || { echo "sumi-vendor: $S has no commit $C"; exit 1; }
H=$(git -C "$S" rev-parse "$C^{commit}")
D=ports/sumi/upstream
FILES="LICENSE sumi/README.md sumi/src/sumi.h sumi/src/main.c sumi/src/buf.c sumi/src/file.c sumi/src/syntax.c
       sumi/src/clip.c sumi/src/keys.c"
rm -rf "$D"
for f in $FILES; do
    o=$D/${f#sumi/}
    [ "$o" = $D/README.md ] && o=$D/README       # (no .md outside docs/: tools/docs-check.sh)
    mkdir -p "$(dirname "$o")"
    git -C "$S" show "$H:$f" > "$o"
done
echo "sumi-vendor: $(echo $FILES | wc -w) files from commit $H ($(git -C "$S" show -s --format=%cs "$H"))"
