#!/usr/bin/env bash
# Checks that the documentation is in step with the project.
#   tools/docs-check.sh            check the whole tree
#   tools/docs-check.sh --staged   also check what is about to be committed (use as a pre-commit check)
# Rules (see CLAUDE.md): every program has a manual page in docs/man and a row in docs/commands.md; manual pages
# fit 78 columns; docs/features.md exists; no documentation files outside docs/ (README.md and CLAUDE.md excepted);
# with --staged, a changed program needs its manual page changed in the same commit.
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
    head -1 "$f" | grep -q "([18])" || bad "$f: first line must look like NAME(1)<tab>Zenix manual"
done
[ -f docs/features.md ] || bad "docs/features.md is missing"

# documentation files must live in docs/ (or be file system content / test inputs / archived)
stray=$(git ls-files '*.md' '*.txt' | grep -v '^docs/\|^rootfs/\|^tools/tests/\|^archive/\|^README.md$\|^CLAUDE.md$')
[ -z "$stray" ] || bad "documentation outside docs/: $stray"

if [ "$1" = "--staged" ]; then
    changed=$(git diff --cached --name-only)
    for c in $changed; do
        case "$c" in
        user/*.c)
            b=$(basename "$c" .c); n=$(echo "$b" | sed 's/^mkfs_/mkfs./; s/^fsck_/fsck./')
            case "$b" in libz|regex|miniz|fsd|pw|sha|zenixfsd|zenixfs_disk) continue;; esac     # libraries and servers: no page of their own
            echo "$changed" | grep -qx "docs/man/$n.txt" || bad "$c changed but docs/man/$n.txt was not updated in this commit"
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
