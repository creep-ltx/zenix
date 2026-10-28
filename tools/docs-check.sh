#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Checks that the documentation is in step with the project.
#   tools/docs-check.sh            check the whole tree
#   tools/docs-check.sh --staged   also check what is about to be committed (use as a pre-commit check)
#   tools/docs-check.sh --commit REV   the same check for a commit already made (e.g. after a merge: --commit HEAD)
# Rules (see CLAUDE.md): every program has a manual page in docs/man and a row in docs/commands.md; manual pages
# fit 78 columns; docs/features.md exists; no documentation files outside docs/ (README.md and CLAUDE.md excepted);
# docs/README.md lists every docs/*.md; no merge conflict markers in tracked files; every relative link in README.md and docs/*.md leads to a file; the counts
# the documents state - "N programs", "N commands", "N system calls", "N emulated setups" (the audit, a record of
# its day, excepted) - match the Makefile's UAPPS, the SYS_ numbers in kernel/kernel.h and the matrix configs;
# with --staged, a changed program (zenixboot, the AmigaOS boot program in boot/, too) needs its manual page changed in
# the same commit.
cd "$(dirname "$0")/.." || exit 1
fail=0
bad() { echo "docs-check: $*"; fail=1; }

progs=$(sed -n '/^UAPPS/,/^UCFLAGS/p' Makefile | tr '\\\n' '  ' | sed 's/UAPPS *:=//; s/UCFLAGS.*//')
for p in $progs; do
    n=$(echo "$p" | sed 's/^mkfs_/mkfs./; s/^fsck_/fsck./')
    [ -f "docs/man/$n.txt" ] || bad "no manual page docs/man/$n.txt for program $p"
    grep -q "man/$n.txt" docs/commands.md || bad "docs/commands.md does not list $n"
done
for f in docs/man/*.txt; do
    n=$(basename "$f" .txt)
    grep -q "man/$n.txt" docs/commands.md || bad "docs/commands.md does not link $f"
    awk -v f="$f" 'length($0) > 78 { print "docs-check: " f ":" FNR " is longer than 78 columns"; exit 1 }' "$f" || fail=1
    head -1 "$f" | grep -q "([148])" || bad "$f: first line must look like NAME(1)<tab>Zenix manual (sections 1, 4 devices, 8)"
done
[ -f docs/features.md ] || bad "docs/features.md is missing"
# no merge conflict markers left in any tracked text file (a conflicted merge was once committed and pushed, 2026-10-27)
for f in $(git ls-files -- '*.md' '*.txt' '*.c' '*.h' '*.S' '*.sh' '*.py' Makefile); do
    /usr/bin/grep -qE '^(<<<<<<< |>>>>>>> )' "$f" && bad "$f: merge conflict markers"
done

# docs/README.md is the index of every document
for f in docs/*.md; do
    b=$(basename "$f"); [ "$b" = README.md ] && continue
    grep -q "](${b})" docs/README.md || bad "docs/README.md does not list $b"
done
# every relative link leads to a file (in the documents and the command index)
for f in README.md docs/*.md; do
    d=$(dirname "$f")
    for l in $(grep -o '\]([^)#:]*)' "$f" | sed 's/^](//; s/)$//'); do
        [ -e "$d/$l" ] || bad "$f links to $l, which does not exist"
    done
done
# the counts written in the documents
nprog=$(echo $progs | wc -w)
nsys=$(grep -cE '^#define SYS_[a-z_0-9]+ +[0-9]+' kernel/kernel.h)
nmat=$(git ls-files 'fsuae/*.fs-uae' 'fsuae/matrix/*.fs-uae' | grep -v '^fsuae/tools/' | sed 's#.*/##; s#\.fs-uae$##' | grep -vxc 'mounts\|zroot')
count() {    # count <what> <regex with the number as group 1> <real number>
    for f in README.md docs/*.md; do
        case "$f" in docs/audit-*) continue;; esac
        grep -noE "$2" "$f" | while IFS=: read -r ln m; do
            n=$(echo "$m" | grep -oE '[0-9]+' | head -1)
            [ "$n" = "$3" ] || echo "docs-check: $f:$ln says \"$m\", but there are $3 $1"
        done
    done
}
cnt=$( { count programs '(^|[ (])[0-9]+ programs\b' "$nprog"; count programs '(^|[ (])[0-9]+ commands\b' "$nprog"
         count "system calls" '(^|[ (])[0-9]+ system calls\b' "$nsys"; count setups '(^|[ (])[0-9]+ (emulated )?(machine )?setups\b' "$nmat"; } )
[ -z "$cnt" ] || { echo "$cnt"; fail=1; }

# documentation files must live in docs/ (or be file system content / test inputs / archived)
stray=$(git ls-files '*.md' '*.txt' | grep -v '^docs/\|^rootfs/\|^tools/tests/\|^archive/\|^README.md$\|^CLAUDE.md$')
[ -z "$stray" ] || bad "documentation outside docs/: $stray"

if [ "$1" = "--staged" ] || [ "$1" = "--commit" ]; then
    if [ "$1" = "--staged" ]; then range=(--cached); else range=("${2:-HEAD}~1" "${2:-HEAD}"); fi
    changed=$(git diff "${range[@]}" --name-only)
    # DOCS_NO_BEHAVIOUR="reason": the program changes alter no behaviour (a compiler warning, a comment), so no page or
    # feature line needs to change; the reason is printed, so it shows in the log of whoever commits
    if [ -n "${DOCS_NO_BEHAVIOUR:-}" ]; then echo "docs-check: no behaviour change claimed: $DOCS_NO_BEHAVIOUR"; changed=$(echo "$changed" | grep -v '^user/.*\.c$\|^boot/zenixboot'); fi
    for c in $changed; do
        case "$c" in
        user/xfer/*) ;;          # the transfer engine and its file rules (DESIGN D20 5): a library, no page of its own
        user/zpkg/*) ;;          # zpkg's core (DESIGN D22): a library of the coming zpkg, no page of its own yet
        user/wget/*) ;;          # wget's core (DESIGN D21 step 7): a library of the coming wget, no page of its own yet
        user/*.c)
            b=$(basename "$c" .c); n=$(echo "$b" | sed 's/^mkfs_/mkfs./; s/^fsck_/fsck./')
            case "$b" in libz|regex|miniz|fsd|pw|clockfile|sha|sha256|zenixfsd|zenixfs_disk|zpkgfmt) continue;; esac     # libraries and servers: no page of their own
            # a change of the licence line only changes no behaviour
            [ -z "$(git diff "${range[@]}" -U0 -- "$c" | grep '^[-+]' | grep -v '^+++\|^---\|SPDX-License-Identifier')" ] && continue
            echo "$changed" | grep -qx "docs/man/$n.txt" || bad "$c changed but docs/man/$n.txt was not updated in this commit"
            ;;
        boot/zenixboot*)        # an AmigaOS program, not in UAPPS, but a program with a page all the same
            [ -z "$(git diff "${range[@]}" -U0 -- "$c" | grep '^[-+]' | grep -v '^+++\|^---\|SPDX-License-Identifier')" ] && continue
            echo "$changed" | grep -qx "docs/man/zenixboot.txt" || bad "$c changed but docs/man/zenixboot.txt was not updated in this commit"
            ;;
        esac
    done
    # a new program or a changed kernel interface also needs the feature list
    if echo "$changed" | grep -q '^user/.*\.c$\|^kernel/\(kernel\|fsproto\|diskproto\)\.h$'; then
        echo "$changed" | grep -q '^docs/features.md$\|^docs/architecture.md$' ||
            echo "docs-check: warning: programs or kernel interfaces changed; did docs/features.md (or architecture.md) need a line?"
    fi
fi
[ $fail = 0 ] && echo "docs-check: ok"
exit $fail
