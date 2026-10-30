#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Compile a C file for a program with shared code (DESIGN D13, docs/d13-shared-code.md section 3):
#   tools/purecc.sh CC FLAGS... -c SRC -o OUT
# gcc -msep-data reaches every global through the address table in a5 with a 32-bit offset (move.l (d32,a5): 8 bytes);
# rewriting sym@GOT(%a5) to sym@GOT.w(%a5) before assembling gives the 4-byte (d16,a5) form (R_68K_GOT16O). A table
# over 32 KB is then a link error, never a silent fault. Any other table form left in the assembly is refused.
set -e
CC=$1; shift
args=(); asargs=(); out=
while [ $# -gt 0 ]; do
    case "$1" in
    -o) out=$2; shift 2 ;;
    -c) shift ;;
    -Wa,*) asargs+=("$1"); shift ;;     # the assembler's (ctop's inline asm names registers without %)
    *) args+=("$1"); shift ;;
    esac
done
[ -n "$out" ] || { echo "purecc: no -o" >&2; exit 2; }
"$CC" "${args[@]}" -msep-data -S -o "$out.s"
sed -i 's/@GOT(%a5)/@GOT.w(%a5)/g' "$out.s"
if grep -n '@GOT[^.]\|@GOT$\|@GOT32\|@GOTOFF' "$out.s" >&2; then
    echo "purecc: $out.s: a table reference in a form this build does not rewrite" >&2; rm -f "$out.s"; exit 1
fi
"$CC" -m68020-60 "${asargs[@]}" -c "$out.s" -o "$out"
rm -f "$out.s"
