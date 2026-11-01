#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Copy the owner's file manager Ahiru and the CFile core it is built on (the owner's CFile repository: ahiru/, core/,
# fs/) into ports/ahiru/upstream/ (DESIGN D27 section 9, rule 1: one source for AmigaOS and Zenix, a pinned copy
# here): the files the Zenix build compiles or includes, Ahiru's README.md (as upstream/README: the user's
# documentation, which docs/man/ahiru.txt follows), taken from one commit, unchanged but for one thing: the owner's
# CFile repository carries no licence notice, and the owner approved (2026-10-28) publishing Ahiru and the core in
# Zenix's tree under MIT, so each copied source file gets a two-line MIT notice naming him at its top, and
# upstream/LICENSE is his MIT licence (the wording of his other MIT projects: ports/sumi/upstream/LICENSE). Zenix's
# own layer (ports/common/, ports/ahiru/zenix/) is not touched. For an update:
#   tools/ahiru-vendor.sh /path/to/CFile [COMMIT]      (COMMIT: default the checkout's HEAD)
# then put the commit it prints into ports/ahiru/README, build, run tools/ahiru-test.sh, and bring docs/man/ahiru.txt
# in step with the README's changes. A file upstream adds to Ahiru's build is added to the lists here and in the
# Makefile (AHIRU_SRCS); one Zenix replaces is named in ports/ahiru/README.
set -e
cd "$(dirname "$0")/.."
S=$1; C=${2:-HEAD}
[ -n "$S" ] || { echo "usage: tools/ahiru-vendor.sh /path/to/CFile [COMMIT]"; exit 1; }
git -C "$S" rev-parse --verify -q "$C^{commit}" >/dev/null || { echo "ahiru-vendor: $S has no commit $C"; exit 1; }
H=$(git -C "$S" rev-parse "$C^{commit}")
D=ports/ahiru/upstream
# Ahiru's own files, the core's (core/: the formats; fs/: folders, archives, copy, move, delete), and the headers
# of the parts Zenix replaces (fs/os.h, fs/cx.h, fs/mount.h, fs/modload.h, src/paula.h: ports/ahiru/README)
AHIRU="main ui list preview ops fe keys more more2 term filecol"
CORE="core util arc zip adf adfw iso dms version rename sums"     # lha, lzx: Zenix's own (DESIGN D33)
FS="find place ops icon kind arcfs arcw keymap sync renames usercmd sums"
HDRS="ahiru/src/ahiru.h core/arc.h core/core.h core/util.h core/zip.h core/adf.h core/adfw.h
      core/iso.h core/dms.h core/version.h core/rename.h core/sums.h core/ilbm.h core/svx.h core/ptplay.h
      fs/os.h fs/fe.h fs/shared.h fs/place.h fs/ops.h fs/find.h fs/icon.h fs/arcfs.h fs/arcw.h fs/keymap.h fs/sync.h
      fs/renames.h fs/usercmd.h fs/sums.h fs/mount.h fs/cx.h fs/modload.h src/paula.h"
FILES="ahiru/README.md $HDRS"
for f in $AHIRU; do FILES="$FILES ahiru/src/$f.c"; done
for f in $CORE; do FILES="$FILES core/$f.c"; done
for f in $FS; do FILES="$FILES fs/$f.c"; done
NOTE1="/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */"
NOTE2="/* SPDX-License-Identifier: MIT */"
rm -rf "$D"
mkdir -p "$D"
cp ports/sumi/upstream/LICENSE "$D/LICENSE"
for f in $FILES; do
    o=$D/$f
    [ "$f" = ahiru/README.md ] && o=$D/ahiru/README     # (no .md outside docs/: tools/docs-check.sh)
    mkdir -p "$(dirname "$o")"
    case "$f" in
    core/dms.[ch])   # derived from xDMS 1.3, public domain: never the owner's MIT notice (below)
        { echo "/* Zenix's copy: derived from xDMS 1.3 (public domain), through the owner's BBX Go readers. */"
          git -C "$S" show "$H:$f"; } > "$o" ;;
    *.c|*.h)
        if git -C "$S" show "$H:$f" | head -5 | grep -q -i -E 'SPDX|licen[cs]e|copyright'; then
            git -C "$S" show "$H:$f" > "$o"
        else
            { echo "$NOTE1"; echo "$NOTE2"; git -C "$S" show "$H:$f"; } > "$o"
        fi ;;
    *)  git -C "$S" show "$H:$f" > "$o" ;;
    esac
done
# The owner's CFile repository still has the old LHA and LZX readers, derived from LHa for UNIX and unlzx: they never come here. Ahiru's
# arcfs includes core/lha.h and core/lzx.h by relative path: two small headers lead it to Zenix's clean readers (DESIGN D33).
for n in lha lzx; do
    u=unlha; [ $n = lzx ] && u=unlzx
    { echo "// SPDX-License-Identifier: GPL-2.0-only"
      echo "/* Zenix replaces this reader (DESIGN D33, ports/ahiru/README): the clean one, shared with $u. Not from the owner's CFile repository. */"
      echo '#include "../../../../user/arcread.h"'; } > "$D/core/$n.h"
done
echo "ahiru-vendor: $(echo $FILES | wc -w) files from commit $H ($(git -C "$S" show -s --format=%cs "$H"))"
