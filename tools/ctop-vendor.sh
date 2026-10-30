#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Copy the owner's ctop (his CUtils repository, public, MIT) into ports/ctop/upstream/ (DESIGN D27 section 9, rule 1:
# one source for AmigaOS and Zenix, a pinned copy here): src/ctop.c, docs/ctop.md (as upstream/README: the user's
# documentation, which docs/man/ctop.txt follows) and the licence, taken from one commit, unchanged. The part of
# CUtils' library ctop calls (lib/cu.h, lib/cu_*.c: exec's task lists and hooks, the Pi's mailbox) is not copied:
# Zenix's layer gives it (ports/ctop/zenix/cu.h and cu.c, ports/ctop/README). For an update:
#   tools/ctop-vendor.sh /path/to/CUtils [COMMIT]      (COMMIT: default the checkout's HEAD)
# then put the commit it prints into ports/ctop/README, build, run tools/ctop-test.sh, and bring docs/man/ctop.txt in
# step with the README's changes; if upstream's ctop.c starts to call a cu_ function or a CuTask field Zenix's cu.h
# does not have, the build says so.
set -e
cd "$(dirname "$0")/.."
S=$1; C=${2:-HEAD}
[ -n "$S" ] || { echo "usage: tools/ctop-vendor.sh /path/to/CUtils [COMMIT]"; exit 1; }
git -C "$S" rev-parse --verify -q "$C^{commit}" >/dev/null || { echo "ctop-vendor: $S has no commit $C"; exit 1; }
H=$(git -C "$S" rev-parse "$C^{commit}")
D=ports/ctop/upstream
rm -rf "$D"
mkdir -p "$D/src"
git -C "$S" show "$H:LICENSE" > "$D/LICENSE"
git -C "$S" show "$H:src/ctop.c" > "$D/src/ctop.c"
git -C "$S" show "$H:docs/ctop.md" > "$D/README"      # (no .md outside docs/: tools/docs-check.sh)
echo "ctop-vendor: ports/ctop/upstream from $H"
