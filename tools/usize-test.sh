#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Host test of the frozen first sizes of the structs the kernel writes into a program's memory (audit3 B3):
# tools/usize-test.c checks the arithmetic and the constants; here every system call that writes one is checked to
# pass its frozen *_FIRST constant (never sizeof, which grows with the struct) as the size a size-0 caller gets.
cd "$(dirname "$0")/.." || exit 1
mkdir -p build
cc -m32 -Wall -Wextra -Wno-builtin-declaration-mismatch -O1 -g -fsanitize=undefined -o build/usize-test tools/usize-test.c || { echo "FAIL build"; exit 1; }
build/usize-test; rc=$?
# CALL FIRST: the UOKS line after "case CALL:" must name FIRST as its first size
while read -r call first; do
    got=$(awk -v c="case $call:" 'index($0, c) { f = 1 } f && /UOKS\(/ { sub(/.*UOKS\(/, ""); split($0, a, ","); gsub(/ /, "", a[4]); print a[4]; exit }' kernel/syscall.c)
    if [ "$got" = "$first" ]; then echo "ok:   $call: a size-0 caller gets $first"
    else echo "FAIL: $call passes '${got:-nothing}' as its first size, not $first"; rc=1; fi
done <<'EOF'
SYS_stat USTAT_FIRST
SYS_lstat USTAT_FIRST
SYS_fstat USTAT_FIRST
SYS_readdir UDIRENT_FIRST
SYS_mountinfo UMOUNTINFO_FIRST
SYS_procinfo UPROC_FIRST
SYS_statfs USTATFS_FIRST
SYS_meminfo UMEMINFO_FIRST
SYS_update UUPDATE_FIRST
EOF
n=$(grep -c "UOKS(n, " kernel/syscall.c)
[ "$n" = 9 ] && echo "ok:   9 calls write a struct (all checked)" || { echo "FAIL: $n UOKS calls in syscall.c, the test knows 9"; rc=1; }
exit $rc
