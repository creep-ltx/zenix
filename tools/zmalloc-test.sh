#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The allocator on the PC (docs/d27-c-library.md "The allocator"): user/zmalloc.c built for a 32-bit PC with the
# kernel's regions imitated (tools/zmalloc-host.c). Checked: its region count is the kernel's NREG; a stray free, a
# double free and a huge request are refused and a damaged header stops the program (134); three seeds of a million
# random calls keep every block's bytes (no overlap, realloc keeps the contents, calloc is zero) and give every chunk
# but the kept ones back; the regions hold megabytes; and each recorded program's allocations (tools/testdata/mtrace,
# from a ZMALLOC_TRACE build in FS-UAE) are replayed and held within the budget below (the bytes held at the peak,
# as `ps` counts them; the power-of-two allocator before 2026-10-30 held the second figure). The speeds are printed
# (the PC: they compare builds, they say nothing of a 68020).
cd "$(dirname "$0")/.." || exit 1
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
pass=1
fail() { echo "FAIL $*"; pass=0; }
kn=$(sed -n 's/^#define NREG  *\([0-9]*\) .*/\1/p' kernel/kernel.h)
zn=$(sed -n 's/^#define ZM_NREG  *\([0-9]*\)u .*/\1/p' user/zmalloc.c)
[ -n "$kn" ] && [ "$kn" = "$zn" ] && echo "PASS the allocator's region count is the kernel's NREG ($kn)" ||
    fail "user/zmalloc.c's ZM_NREG ($zn) is not kernel/kernel.h's NREG ($kn)"
cat > "$T/zshare.h" <<'EOF'
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
typedef uint32_t u32;
typedef size_t zs_size;
typedef size_t zs_ulong;
#define ZS_LIB "libz"
#define malloc zm_malloc
#define free zm_free
#define calloc zm_calloc
#define realloc zm_realloc
void *sys_memalloc(u32 n);
int sys_memfree(void *p);
int sys_write(int fd, const void *b, u32 n);
void sys_exit(int c);
void zs_badptr(const char *who, const void *p);
int zm_dprintf(const char *f, ...);
#define dprintf(fd, ...) zm_dprintf(__VA_ARGS__)
EOF
cp user/zmalloc.c "$T/"
if ! gcc -m32 -O2 -Wall -Wextra -Werror -I"$T" -c "$T/zmalloc.c" -o "$T/zm.o" 2>"$T/cc.log" ||
   ! gcc -m32 -O2 -Wall -Wextra -Wno-clobbered -c tools/zmalloc-host.c -o "$T/h.o" 2>>"$T/cc.log" ||
   ! gcc -m32 "$T/h.o" "$T/zm.o" -o "$T/zmh" 2>>"$T/cc.log"; then
    cat "$T/cc.log"; echo "FAIL the allocator does not build for the PC (gcc -m32)"; exit 1
fi
Z="$T/zmh"
o=$($Z bad 2>&1)
case "$o" in "bad: "*) echo "PASS stray and double frees reported and ignored, a damaged header stops (134)" ;; *) fail "the bad pointers" ;; esac
for s in 1 7 1234; do
    o=$($Z stress -r 16 $s); echo "     $o"
    case "$o" in *"every block kept"*"0 calls out of regions"*) ;; *) fail "stress, seed $s: $o" ;; esac
    e=$(echo "$o" | sed -n 's/.*at the end \([0-9]*\) B in \([0-9]*\) regions/\1 \2/p')
    [ -n "$e" ] && [ ${e% *} -lt 131072 ] || fail "stress, seed $s: freed memory not given back ($e)"
done
[ $pass = 1 ] && echo "PASS three million random calls: every block's bytes kept, freed chunks given back"
for sz in 100 20000; do
    o=$($Z room -r 16 $sz); k=$(echo "$o" | sed -n 's/^room: \([0-9]*\) KB.*/\1/p')
    [ -n "$k" ] && [ "$k" -ge 4096 ] && echo "PASS $o" || fail "the regions hold too little: $o"
done
# trace: the most KB held at the peak (measured 2026-10-30, about 5% room), and what the old allocator held
while read -r name budget old; do
    zcat "tools/testdata/mtrace/$name.gz" > "$T/$name"
    o=$($Z replay -r 16 "$T/$name")
    k=$(echo "$o" | sed -n 's/.*at the end, [0-9]* (\([0-9]*\) KB) at the peak.*/\1/p')
    case "$o" in *failed*) fail "$name: a call failed in the replay: $o"; continue ;; esac
    [ -n "$k" ] && [ "$k" -le "$budget" ] && echo "PASS $name: $k KB held at the peak (budget $budget, the old allocator $old)" ||
        fail "$name: $k KB held at the peak, over its budget of $budget: $o"
done <<'EOF'
ahiru 188 393
ahiru-lha 206 329
maki 102 169
markdown 65 169
netd-loopback 20 85
netd-nfs 12 21
nfscd 65 121
sh 5 5
sumi 53 85
wasabid 20 41
EOF
$Z speed "$T/netd-loopback" "$T/ahiru" | sed 's/^/     /'
[ $pass = 1 ] && echo "zmalloc-test: all passed" || { echo "zmalloc-test: FAILED"; exit 1; }
