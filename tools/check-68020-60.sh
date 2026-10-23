#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Fail if a built ELF contains a 64-bit multiply or divide: mulu.l/muls.l <ea>,Dh:Dl or divu.l/divs.l <ea>,Dr:Dq with a
# 64-bit dividend. The 68060 does not implement them (it traps to vector 61, "unimplemented integer instruction"), and
# under Zenix there is no 68060.library to emulate them, so the kernel would panic or the program be killed. The
# compiler emits them for division by a constant unless it is told -m68020-60 (the Makefile does that); this check is
# what proves it.
#   usage: tools/check-68020-60.sh file.elf...
# Found on the opcode bytes, not the mnemonic: first word 0x4C00-0x4C3F (MUL.L) or 0x4C40-0x4C7F (DIV.L), second word
# bit 10 set (the 64-bit size bit): 0 Dl(3) S 1 0 0 0 | 0 0 0 0 0 Dh(3), that is hex [0-7][4c]0[0-7]. The 32-bit forms
# (bit 10 clear: mulu.l #k,Dn and divul.l <ea>,Dr:Dq) are allowed. Verified against objdump's mnemonics: it prints the
# 64-bit forms as "mulul #k,%dh,%dl" / "divul %ea,%dr,%dq" (three operands with two different registers) and the 32-bit
# ones as "mulul #k,%dn" / "divull ...".
TOOLPREFIX="${TOOLPREFIX:-m68k-elf-}"     # the Makefile passes its own; default: from PATH
command -v "${TOOLPREFIX}objdump" >/dev/null || { echo "check-68020-60: ${TOOLPREFIX}objdump not found (set TOOLPREFIX)"; exit 1; }
fail=0
for f in "$@"; do
    bad=$("${TOOLPREFIX}objdump" -d "$f" | grep -E '^ *[0-9a-f]+:[[:space:]]+4c[0-7][0-9a-f] [0-7][4c]0[0-7]( |$)')
    if [ -n "$bad" ]; then
        echo "check-68020-60: $f has 64-bit multiply/divide instructions (not on a 68060):"
        echo "$bad" | head -5
        fail=1
    fi
done
exit $fail
