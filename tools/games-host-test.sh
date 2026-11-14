#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The games on the PC (roadmap block 7 item 7, DESIGN D52 section 8; ports/frotz, ports/bsdgames, games/), the pinned
# sources built for the PC with Zenix's settings, under AddressSanitizer and UBSan where they are C:
#  - Frotz (its core and dumb front end, ports/frotz/zenix's headers) plays All Things Devours with
#    rootfs/tests/games/devours.in: status 0, the rooms, the undo and the quit in its transcript, the reference
#    tools/games-test.sh compares Zenix's with, line for line;
#  - adventure's text, "encrypted" by the PC's mkdata with the C library's random() (zenix/zrandom.c), read back by
#    the game built with the same generator: its first room; banner and number, whose output Zenix's must equal;
#  - strfile indexes games/fortunes: as many strings as the file's %-lines say, the header big-endian;
#  - the curses (ports/bsdgames/zenix/curses.c): 300 frames of random drawing through every call the games use, the
#    screen decoded from its output equal to each frame, and a refresh with nothing changed writing nothing
#    (tools/tests/games/cursestest.c, screen.py);
#  - gridrun (games/gridrun.lua) with a stand-in for the zenix module (tools/tests/games/zstub.lua): scripted keys,
#    three rounds, every call's arguments checked, nothing drawn outside the window; its module read by modplay's
#    replayer (user/modrep.c), every channel playing.
# GAMES_EXPECTED=DIR keeps the references (tools/games-test.sh).
cd "$(dirname "$0")/.." || exit 1
T=$(mktemp -d "${TMPDIR:-/tmp}/zenix-games.XXXXXX")
trap 'rm -rf "$T"' EXIT
fail=0
check() { if [ "$1" = 0 ]; then echo "PASS $2"; else echo "FAIL $2${3:+: $3}"; fail=1; fi; }
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
export ASAN_OPTIONS=detect_leaks=0

# ---- Frotz
cc -std=gnu99 -O1 -g -fwrapv $SAN -w -Iports/frotz/zenix -o "$T/frotz" ports/frotz/upstream/src/common/*.c \
    ports/frotz/upstream/src/dumb/*.c 2>"$T/cc.log"
check $? "the PC build of ports/frotz (dumb front end)" "$(head -3 "$T/cc.log")"
"$T/frotz" -m -q -s 1 -w 78 ports/frotz/stories/devours.z5 <rootfs/tests/games/devours.in >"$T/devours.out" 2>&1
st=$?
check $st "frotz played devours.z5 to the quit" "$(tail -3 "$T/devours.out")"
for l in "^Foyer\$" "^Basement Landing\$" "^\\[Previous turn undone.\\]\$" "^Are you sure you want to quit\\?"; do
    grep -q "$l" "$T/devours.out"; check $? "frotz's transcript has /$l/"
done
sha256sum ports/frotz/stories/devours.z5 | grep -q '^a0b0569c2f57a975f868242b9a1dfe400a75e6aef92d5480eb51c81a3150eb37 '
check $? "devours.z5 is the IF Archive's release 3 (ports/frotz/stories/README)"

# ---- the BSD games' PC builds (host.h: NetBSD's macros over glibc)
H="-include ports/bsdgames/zenix/host.h"
cc -O2 $H -Drandom=zx_random -Dsrandom=zx_srandom -o "$T/mkdata" ports/bsdgames/upstream/adventure/setup.c \
    ports/bsdgames/zenix/zrandom.c && "$T/mkdata" ports/bsdgames/upstream/adventure/glorkz >"$T/data.c"
check $? "adventure's mkdata wrote its text"
cc -std=gnu99 -O1 -g $SAN -w $H -Drandom=zx_random -Dsrandom=zx_srandom -o "$T/adventure" \
    ports/bsdgames/upstream/adventure/{main,init,done,save,subr,vocab,wizard,io,crc}.c "$T/data.c" \
    ports/bsdgames/zenix/zrandom.c 2>"$T/cc.log"
check $? "the PC build of adventure" "$(head -3 "$T/cc.log")"
printf 'no\nquit\nyes\n' | "$T/adventure" >"$T/adv.out" 2>&1
grep -q "^You are standing at the end of a road before a small brick building" "$T/adv.out"
check $? "adventure's text decrypted with the C library's random(): the first room" "$(head -5 "$T/adv.out")"
for g in banner number; do
    cc -std=gnu99 -O1 -g $SAN -w $H -o "$T/$g" "ports/bsdgames/upstream/$g/$g.c" 2>"$T/cc.log"
    check $? "the PC build of $g" "$(head -3 "$T/cc.log")"
done
"$T/banner" -w 40 ZENIX >"$T/banner.out" && [ "$(grep -c '#' "$T/banner.out")" -gt 40 ]
check $? "banner -w 40 ZENIX ($(wc -l <"$T/banner.out") lines)"
"$T/number" 1999 >"$T/number.out" && grep -q "nine hundred ninety-nine" "$T/number.out"
check $? "number 1999: $(tr '\n' ' ' <"$T/number.out")"
cc -O2 $H -o "$T/strfile" ports/bsdgames/upstream/fortune/strfile/strfile.c && cp games/fortunes "$T/fortunes" &&
    "$T/strfile" -s "$T/fortunes" "$T/fortunes.dat"
check $? "strfile indexed games/fortunes"
n=$(awk 'BEGIN { RS = "\n%\n" } NF { n++ } END { print n }' games/fortunes)
got=$(od -An -tu4 --endian=big -j4 -N4 "$T/fortunes.dat" | tr -d ' ')
[ "$got" = "$n" ] && [ "$(od -An -tu4 --endian=big -N4 "$T/fortunes.dat" | tr -d ' ')" = 1 ]
check $? "the index: version 1, $got strings big-endian (the file has $n)"

# ---- the curses
cc -std=gnu99 -O1 -g $SAN -Wall -Wno-unused $H -Iports/bsdgames/zenix/include -o "$T/cursestest" \
    tools/tests/games/cursestest.c ports/bsdgames/zenix/curses.c 2>"$T/cc.log"
check $? "the PC build of the curses and its test" "$(head -3 "$T/cc.log")"
for size in 24x80 32x80 25x40; do
    LINES=${size%x*} COLUMNS=${size#*x} "$T/cursestest" "$T/frames" "$T/counts" >"$T/cout" </dev/null
    r=$(python3 tools/tests/games/screen.py "$T/cout" "$T/frames" "${size%x*}" "${size#*x}"); rc=$?
    check $rc "curses at $size: $r"
done

# ---- gridrun with the stand-in module, and its music through the replayer
cc -std=gnu99 -O1 -g $SAN -w -DLUA_USE_POSIX -o "$T/lua" ports/lua/upstream/src/*.c -lm 2>"$T/cc.log"
check $? "the PC build of Lua" "$(head -3 "$T/cc.log")"
GRIDRUN_LOG=1 GRIDRUN_SEED=7 ZSTUB_MOD="$T/gridrun.mod" \
    ZSTUB_KEYS="1000:space,1500:up,2200:left,2900:down,6000:space,6200:space,9000:p,9500:p,20000:space,20300:space,30000:q" \
    "$T/lua" -e 'dofile("tools/tests/games/zstub.lua")' games/gridrun.lua >"$T/gr.out" 2>"$T/gr.err"
check $? "gridrun ran to its end with the stand-in" "$(tail -3 "$T/gr.err")"
[ "$(grep -c '^gridrun:	round' "$T/gr.out")" = 3 ] && grep -q '^gridrun:	music	on$' "$T/gr.out" &&
    grep -q "^gridrun:	bye" "$T/gr.out"
check $? "gridrun: the music on, three rounds, the end" "$(tr '\n' ' ' <"$T/gr.out")"
grep -q "zstub: .* samples 3 plays [1-9][0-9]* music [1-9][0-9]* .* keys 11 outside 0 " "$T/gr.err"
check $? "gridrun's calls: $(grep zstub: "$T/gr.err")"
cat >"$T/modchk.c" <<'EOF'
#include <stdio.h>
#include "modrep.h"
int main(int c, char **v)
{
    static unsigned char b[1 << 20];
    static struct mod m;
    char why[64];
    FILE *f = fopen(v[1], "rb");
    size_t n = f ? fread(b, 1, sizeof b, f) : 0;
    if (f) fclose(f);
    if (c < 2 || mod_load(&m, b, (unsigned)n, why) < 0) { printf("refused: %s\n", c < 2 ? "" : why); return 1; }
    int trig[4] = {0}, smp[32] = {0}, ns = 0;
    for (int t = 0; t < 4 * 64 * 6; t++) {
        mod_tick(&m);
        for (int k = 0; k < 4; k++)
            if (m.out[k].flags & MOD_TRIG) { trig[k]++; smp[m.c[k].smp & 31] = 1; }
    }
    for (int i = 0; i < 32; i++) ns += smp[i];
    printf("%s %d %d %d %d %d %d\n", m.title, m.songlen, trig[0], trig[1], trig[2], trig[3], ns);
    return 0;
}
EOF
cc -O1 -g $SAN -Iuser -o "$T/modchk" "$T/modchk.c" user/modrep.c && "$T/modchk" "$T/gridrun.mod" >"$T/mod.out"
check $? "gridrun's module read by modplay's replayer" "$(cat "$T/mod.out")"
read -r title songlen t0 t1 t2 t3 ns <"$T/mod.out"
[ "$title" = gridrun ] && [ "$songlen" = 4 ] && [ "$t0" -gt 0 ] && [ "$t1" -gt 0 ] && [ "$t2" -gt 0 ] && [ "$t3" -gt 0 ] &&
    [ "$ns" = 4 ]
check $? "its four channels and four samples play ($(cat "$T/mod.out"))"

if [ -n "$GAMES_EXPECTED" ]; then mkdir -p "$GAMES_EXPECTED" && cp "$T/devours.out" "$T/banner.out" "$T/number.out" \
    "$T/adv.out" "$GAMES_EXPECTED/"; fi
exit $fail
