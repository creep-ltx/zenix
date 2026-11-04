#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The box (DESIGN D25 revision 3, 10 step 1: the demo floppy's everyday commands in one program, user/box.c).
# On the PC: the box holds every command's renamed main and every name in its table. In FS-UAE, on the full test
# image with the box's 65 links in place of the separate commands (make BOX=1): rootfs/tests/box.sh starts every
# name once (none unknown) and an unknown name (the list, status 2); then the command tests themselves,
# tools/cmds-test.sh and tools/cmds2-test.sh, run against that image (ZENIX_BOX=1).
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
OUT=${ZENIX_OUT:-/tmp/zenix-box}; mkdir -p $OUT
: "${TOOLPREFIX:=$(command -v m68k-elf-gcc >/dev/null && echo m68k-elf- || echo $HOME/opt/m68k-elf/bin/m68k-elf-)}"
make -s BUILD=$BD all $BD/box.zx >/dev/null 2>&1 || { echo "build failed"; exit 1; }
pass=1
ok() { echo "PASS $1"; }
no() { echo "FAIL $1"; pass=0; }
names=$(make -s --no-print-directory -f Makefile -f - BUILD=$BD print-box <<<'print-box: ; @echo $(BOXAPPS)')
aliases=$(make -s --no-print-directory -f Makefile -f - BUILD=$BD print-box <<<'print-box: ; @echo $(BOXALIASES)')
[ "$(echo $names | wc -w)" = 65 ] && ok "the box has 65 commands" || no "the box has $(echo $names | wc -w) commands, not 65"
syms=$(${TOOLPREFIX}nm --defined-only $BD/box.uelf | awk '$2 == "T" { print $3 }')
miss=""; for n in $names; do grep -qx "${n}__main" <<<"$syms" || miss="$miss $n"; done
[ -z "$miss" ] && ok "every command's main is in the box" || no "missing in the box:$miss"
# the dispatcher's table as compiled (the linker merges short names into longer strings, so the binary is not grepped)
miss=""; for n in $names; do grep -qF "BOX($n) " $BD/boxtab.h || miss="$miss $n"; done
for a in $aliases; do grep -qF "ALIAS(\"${a%%:*}\", ${a#*:}) " $BD/boxtab.h || miss="$miss ${a%%:*}"; done
[ $(grep -c '^    BOX(' $BD/boxtab.h) = 65 ] || miss="$miss (a name too many)"
[ -z "$miss" ] && ok "every name and second name is in the box's table" || no "names missing from the table:$miss"
echo "box: $(wc -c < $BD/box.zx) bytes"
rm -f $BD/zenix.hdf; make -s BUILD=$BD BOX=1 $BD/zenix.hdf >/dev/null 2>&1 || { no "the box image does not build"; exit 1; }
tools/run-test.sh fsuae/a1200-020-fast.fs-uae 360 tools/tests/box-test.txt 2>&1 | sed 's/\x1b\[[0-9;:]*[mK]//g' | tr -d '\r' > $OUT/run.log
grep -aq "^box: 73 names run, 0 not dispatched" $OUT/run.log && ok "every name runs its command from the box (73 names)" || { no "names not dispatched:"; grep -a "^box: " $OUT/run.log | head -20; }
grep -aq "^box: unknown name status 2" $OUT/run.log && ok "an unknown name exits 2" || no "an unknown name's status"
[ "$(grep -a -A2 '^box: unknown name' $OUT/run.log | tail -2 | tr '\n' ' ')" = "1 1 " ] && ok "... saying so and listing the names" || no "an unknown name's message and list"
ZENIX_OUT=$OUT/cmds ZENIX_BOX=1 tools/cmds-test.sh || pass=0
ZENIX_OUT=$OUT/cmds2 ZENIX_BOX=1 tools/cmds2-test.sh || pass=0
rm -f $BD/zenix.hdf      # the next test builds the plain image again
exit $((1 - pass))
