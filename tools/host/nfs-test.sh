#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# nfscd's core on the PC (DESIGN D23 11.1, step 0 of section 13), no emulator, no root:
#   tools/host/nfs-test.sh [PART...]     parts: sim libnfs unfs3 size (default: all of them)
#                                        fuzz [SECONDS]: the core's input under libFuzzer (not in the default run)
# sim     tools/host/nfs-sim.py's self-test, then net/nfs/ (built with AddressSanitizer, tools/host/nfs-client.c)
#         against it in a user and network namespace: every request, data compared byte by byte, and the faults
#         (dropped, delayed, reordered, duplicated answers; cut connections; fragments; a restart with a new write
#         verifier; JUKEBOX; BAD_COOKIE; stale handles; a reused fileid; another root; oversize and garbage answers;
#         the duplicate cache from the same source port; hard, soft, lazy, cancel, kill) (tools/host/nfs-sim-test.py)
# libnfs  libnfs's nfs-ls and nfs-cp (a real client) against nfs-sim: the simulator speaks real NFS
# unfs3   the core against a real NFSv3 server: unfs3 (fetched at a pinned version and checked, built in the test's
#         cache, never committed), with nfs-sim's portmapper, in a namespace with mapped user numbers
# size    net/nfs/ for m68k at -Os: the code, and the check for 64-bit multiplies (tools/check-68020-60.sh)
# Needs: a C compiler with AddressSanitizer, python3, unshare (user namespaces); libnfs's nfs-ls/nfs-cp for libnfs;
# for unfs3 curl (or UNFS3_TAR), flex, bison, libtirpc, and newuidmap for --map-auto (else unfsd's single-user
# mode); for fuzz, clang with libFuzzer. Work files go to NFS_TEST_OUT (default build/nfs-host).
cd "$(dirname "$0")/../.." || exit 1
OUT=${NFS_TEST_OUT:-build/nfs-host}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
HOSTCC=${HOSTCC:-cc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
HCFLAGS="-std=gnu99 -g -O1 -Wall -Wextra -Werror"
NFS_SRC="net/nfs/core.c net/nfs/ops.c net/nfs/proto.c net/nfs/rpc.c net/nfs/table.c net/nfs/write.c"
UNFS3_VER=0.10.0
UNFS3_SHA=9bc6568fba2e43f3e2181f1d1802afec41929c1e55a6e53da982aacc7ce79ebb
UNFS3_URL=https://github.com/unfs3/unfs3/releases/download/unfs3-$UNFS3_VER/unfs3-$UNFS3_VER.tar.gz
fails=0
say() { echo "nfs-test: $*"; }
bad() { echo "FAIL nfs-test: $*"; fails=$((fails + 1)); }

build_client() {
    [ -x "$OUT/nfs-client" ] && [ -z "$(find net/nfs tools/host/nfs-client.c -newer "$OUT/nfs-client" 2>/dev/null)" ] && return 0
    $HOSTCC $HCFLAGS $SAN -o "$OUT/nfs-client" tools/host/nfs-client.c $NFS_SRC || { bad "nfs-client does not build"; return 1; }
}
have_ns() { unshare -rn true 2>/dev/null || { bad "unprivileged user namespaces are off (unshare -rn)"; return 1; }; }

part_sim() {
    python3 -I tools/host/nfs-sim.py --self-test || { bad "nfs-sim.py --self-test"; return; }
    build_client && have_ns || return
    rm -rf "$OUT/sim"; mkdir -p "$OUT/sim"
    if unshare -rn python3 -I tools/host/nfs-sim-test.py "$OUT/nfs-client" "$OUT/sim"; then say "sim PASS"
    else bad "sim (the FAIL lines above)"; fi
}

part_libnfs() {
    command -v nfs-ls >/dev/null && command -v nfs-cp >/dev/null || { bad "libnfs's nfs-ls and nfs-cp are not installed"; return; }
    have_ns || return
    rm -rf "$OUT/libnfs"; mkdir -p "$OUT/libnfs"
    head -c 70000 /dev/urandom >"$OUT/libnfs/in.bin"
    if unshare -rn bash -c '
        ip link set lo up
        python3 -I tools/host/nfs-sim.py --serve --pmap 111 --mount 20048 --nfs 2049 >"$1/sim.log" 2>&1 & s=$!
        for i in $(seq 50); do /usr/bin/grep -q serving "$1/sim.log" && break; sleep 0.1; done
        nfs-cp "$1/in.bin" "nfs://127.0.0.1/export/f.bin?version=3" >/dev/null &&
        nfs-cp "nfs://127.0.0.1/export/f.bin?version=3" "$1/out.bin" >/dev/null &&
        cmp "$1/in.bin" "$1/out.bin" &&
        nfs-ls "nfs://127.0.0.1/export?version=3" | /usr/bin/grep -q " f.bin$"; r=$?
        kill $s; exit $r' _ "$OUT/libnfs"; then say "libnfs PASS (nfs-cp both ways, nfs-ls, against nfs-sim)"
    else bad "libnfs against nfs-sim (log $OUT/libnfs/sim.log)"; fi
}

build_unfs3() {
    local d="$OUT/unfs3" tar
    [ -x "$d/build/unfsd" ] && return 0
    mkdir -p "$d/dl"
    tar=${UNFS3_TAR:-$d/dl/unfs3-$UNFS3_VER.tar.gz}
    if [ ! -f "$tar" ]; then
        command -v curl >/dev/null || { bad "unfs3: no curl to fetch $UNFS3_URL (or set UNFS3_TAR)"; return 1; }
        curl -sSL -o "$tar.part" "$UNFS3_URL" && mv "$tar.part" "$tar" || { bad "unfs3: could not fetch $UNFS3_URL"; return 1; }
    fi
    echo "$UNFS3_SHA  $tar" | sha256sum -c --quiet - || { bad "unfs3: $tar is not the pinned $UNFS3_VER"; return 1; }
    rm -rf "$d/src" "$d/build"; mkdir -p "$d/src" "$d/build"
    tar xzf "$tar" -C "$d/src" || { bad "unfs3: unpacking"; return 1; }
    # unfs3 0.10.0 needs gnu99 and tolerates a missing prototype (locate.c); libtirpc's headers
    (cd "$d/build" && CFLAGS="-g -O2 -std=gnu99 -Wno-implicit-function-declaration -I/usr/include/tirpc" \
        "$d/src/unfs3-$UNFS3_VER/configure" >configure.log 2>&1 && make >make.log 2>&1) ||
        { bad "unfs3 does not build (needs flex, bison, libtirpc: $d/build/*.log)"; return 1; }
}

part_unfs3() {
    build_client && have_ns && build_unfs3 || return
    local d="$OUT/unfs3/run" ns="--map-root-user --map-auto" single=""
    rm -rf "$d"; mkdir -p "$d/export/sub"
    unshare $ns -n true 2>/dev/null || { ns="--map-root-user"; single="-s"; say "unfs3: no --map-auto here: unfsd's single-user mode"; }
    head -c 100000 /dev/urandom >"$d/in.bin"
    printf 'hello from the PC\n' >"$d/export/hello.txt"
    echo "$d/export (rw,insecure,no_root_squash)" >"$d/exports"
    cat >"$d/script" <<EOF
mount
stat hello.txt
get hello.txt $d/hello.out
put $d/in.bin sub/copy.bin 8192
get sub/copy.bin $d/back.bin 3000
mkdir newdir
rename sub/copy.bin newdir/moved.bin
symlink moved.bin newdir/ln
readlink newdir/ln
stat newdir/ln
open newdir/x.txt wcx
open newdir/x.txt wcx
chmod newdir/x.txt 600
unlink newdir/x.txt
ls newdir
statfs
sync
quit
EOF
    unshare $ns -n bash -c '
        ip link set lo up
        d=$1
        python3 -I tools/host/nfs-sim.py --pmap-only 2050 2049 --pmap 111 >"$d/pmap.log" 2>&1 & p=$!
        "$2" -d -p -t $3 -e "$d/exports" -n 2049 -m 2050 >"$d/unfsd.log" 2>&1 & u=$!
        for i in $(seq 50); do /usr/bin/grep -q serving "$d/pmap.log" && break; sleep 0.1; done
        sleep 0.5
        "$4" -e "$d/export" -s 700 -t 30 <"$d/script" >"$d/client.out" 2>&1
        kill $u $p' _ "$d" "$OUT/unfs3/build/unfsd" "$single" "$OUT/nfs-client"
    local o="$d/client.out" okk=1
    chk() { /usr/bin/grep -q -- "$1" "$o" || { bad "unfs3: no line matching '$1' (in $o)"; okk=0; }; }
    chk '^mount -> 2 dev=nfs0 type=nfs'
    chk '^stat hello.txt -> 0 .*size=18'
    chk "^put .* -> 0 bytes=100000"
    chk "^readlink newdir/ln -> 9 moved.bin"
    chk "^stat newdir/ln -> 0 .*size=100000"
    chk "^open newdir/x.txt wcx -> -17"
    chk "^chmod newdir/x.txt 600 -> 0"
    chk "^ls newdir -> 0 entries=4"
    chk "^sync -> 0"
    chk "^quit -> 0"
    /usr/bin/grep -q '^FAIL\|AddressSanitizer\|runtime error' "$o" && { bad "unfs3: the client reported a fault"; okk=0; }
    cmp -s "$d/hello.out" "$d/export/hello.txt" || { bad "unfs3: hello.txt read differs"; okk=0; }
    cmp -s "$d/in.bin" "$d/export/newdir/moved.bin" || { bad "unfs3: the written file on the server's disk differs"; okk=0; }
    cmp -s "$d/in.bin" "$d/back.bin" || { bad "unfs3: the file read back differs"; okk=0; }
    [ -L "$d/export/newdir/ln" ] && [ ! -e "$d/export/newdir/x.txt" ] || { bad "unfs3: the link or the unlink is not on the disk"; okk=0; }
    [ $okk = 1 ] && say "unfs3 PASS (a real NFSv3 server: unfs3 $UNFS3_VER)"
}

part_size() {
    local tp=${TOOLPREFIX:-}
    [ -z "$tp" ] && { if command -v m68k-elf-gcc >/dev/null; then tp=m68k-elf-; else tp=$HOME/opt/m68k-elf/bin/m68k-elf-; fi; }
    command -v "${tp}gcc" >/dev/null || { say "size skipped: no m68k cross compiler (${tp}gcc)"; return; }
    local d="$OUT/m68k" f objs=""
    rm -rf "$d"; mkdir -p "$d"
    for f in $NFS_SRC; do
        "${tp}gcc" -m68020-60 -Os -msoft-float -ffreestanding -fno-builtin -nostdlib -ffunction-sections -fdata-sections \
            -Wall -Wextra -Werror -c "$f" -o "$d/$(basename "$f" .c).o" || { bad "m68k: $f"; return; }
        objs="$objs $d/$(basename "$f" .c).o"
    done
    "${tp}size" -t $objs >"$d/size.txt"
    TOOLPREFIX=$tp tools/check-68020-60.sh $objs || { bad "m68k: 64-bit multiply or divide instructions"; return; }
    local helpers
    helpers=$("${tp}nm" $objs | awk '$1 == "U" && $2 ~ /^__/ { print $2 }' | sort -u | tr '\n' ' ')
    case " $helpers " in *" __muldi3 "*|*" __divdi3 "*|*" __udivdi3 "*|*" __moddi3 "*|*" __umoddi3 "*)
        bad "m68k: a 64-bit multiply or divide through libgcc: $helpers"; return ;; esac
    say "size: net/nfs/ for m68k at -Os, unlinked (text data bss): $(tail -1 "$d/size.txt" | awk '{ print $1, $2, $3 }'); libgcc helpers: ${helpers:-none}"
    say "size PASS"
}

part_fuzz() {
    local secs=$1 d="$OUT/fuzz" FCC=${FUZZCC:-clang}
    command -v "$FCC" >/dev/null || { bad "fuzz needs clang (libFuzzer)"; return; }
    mkdir -p "$d/corpus" "$d/seeds" "$d/artifacts"
    $FCC -std=gnu99 -g -O1 -DSEEDS -o "$d/nfs-fuzz-seeds" tools/host/nfs-fuzz.c $NFS_SRC && "$d/nfs-fuzz-seeds" "$d/seeds" ||
        { bad "the fuzzer's seeds"; return; }
    $FCC -std=gnu99 -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -o "$d/nfs-fuzz" \
        tools/host/nfs-fuzz.c $NFS_SRC || { bad "nfs-fuzz does not build"; return; }
    say "fuzz: $secs s, one process, 2 GB memory limit (log $d/fuzz.log)"
    if nice "$d/nfs-fuzz" -max_total_time="$secs" -rss_limit_mb=2048 -malloc_limit_mb=512 -max_len=8192 -timeout=10 \
            -artifact_prefix="$d/artifacts/" -print_final_stats=1 "$d/corpus" "$d/seeds" >"$d/fuzz.log" 2>&1; then
        say "fuzz: $(/usr/bin/grep -a 'stat::number_of_executed_units' "$d/fuzz.log" | awk '{ print $2 }') inputs run, $(ls "$d/corpus" | wc -l) in the corpus, $(/usr/bin/grep -a 'cov:' "$d/fuzz.log" | tail -1 | sed 's/.*cov: \([0-9]*\).*/\1/') edges covered: nothing found"
        say "fuzz PASS"
    else
        /usr/bin/grep -a 'ERROR\|SUMMARY\|Test unit written\|abort\|answered\|never' "$d/fuzz.log" | head -20
        bad "fuzz found something: $d/artifacts"
    fi
}

parts=("$@")
[ ${#parts[@]} = 0 ] && parts=(sim libnfs unfs3 size)
for ((i = 0; i < ${#parts[@]}; i++)); do
    p=${parts[i]}
    case "$p" in
    sim|libnfs|unfs3|size) "part_$p" ;;
    fuzz) if [[ "${parts[i + 1]:-}" =~ ^[0-9]+$ ]]; then part_fuzz "${parts[i + 1]}"; i=$((i + 1)); else part_fuzz 60; fi ;;
    *) echo "nfs-test: no part called $p"; exit 2 ;;
    esac
done
[ $fails = 0 ] && { say "all passed"; exit 0; }
say "$fails FAILED"; exit 1
