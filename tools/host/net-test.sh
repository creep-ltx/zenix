#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The network's host tests (DESIGN D19 8.1), on the PC only, no emulator, no root:
#   tools/host/net-test.sh [PART...]      parts: patches rand unit netd slirp size (default: all of them)
#                                         fuzz [SECONDS]: lwIP's input path under libFuzzer (not in the default run)
# patches  every local patch of net/lwip/patches is applied to net/lwip/src
# rand     ChaCha20 (RFC 8439) and SipHash-2-4 (the SipHash paper) against their published vectors
# unit     lwIP's own unit tests with Zenix's lwipopts.h and the local patches (tools/host/lwip-unit)
# netd     netd's core on the PC against a fake of the kernel's socket layer, SLIP over a socketpair to zenix-slirp
#          (tools/host/netd-host.c): TCP to an echo server, UDP, DNS from a test resolver, ICMP, netd's end and restart
# slirp    zenix-slirp alone: SLIP framing, the console bytes, a TCP echo, UDP, DNS and ICMP through libslirp
# size     netd's lwIP for m68k at -Os (needs the cross compiler, TOOLPREFIX as the Makefile): the code, unlinked
# Needs: a C compiler with AddressSanitizer; for slirp and netd libslirp and GLib (pkg-config slirp), and
# unprivileged user namespaces (unshare -rn), where the test resolver may listen on port 53.
cd "$(dirname "$0")/../.." || exit 1
OUT=${NET_TEST_OUT:-build/net-host}
mkdir -p "$OUT"
LW=net/lwip/src
LWIP_CORE="$LW/core/def.c $LW/core/dns.c $LW/core/inet_chksum.c $LW/core/init.c $LW/core/ip.c $LW/core/mem.c
 $LW/core/memp.c $LW/core/netif.c $LW/core/pbuf.c $LW/core/raw.c $LW/core/stats.c $LW/core/sys.c $LW/core/tcp.c
 $LW/core/tcp_in.c $LW/core/tcp_out.c $LW/core/timeouts.c $LW/core/udp.c $LW/core/ipv4/dhcp.c $LW/core/ipv4/etharp.c
 $LW/core/ipv4/icmp.c $LW/core/ipv4/ip4.c $LW/core/ipv4/ip4_addr.c $LW/core/ipv4/ip4_frag.c $LW/netif/ethernet.c"
LWIP_SRC="$LWIP_CORE $LW/netif/slipif.c"
PORT_SRC="net/port/port.c net/port/rand.c"
HOSTCC=${HOSTCC:-cc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
HCFLAGS="-DNETD_HOST -g -O1 -Wall -Wno-unused-parameter -Inet/port -I$LW/include"
fails=0
say() { echo "net-test: $*"; }
bad() { echo "FAIL net-test: $*"; fails=$((fails + 1)); }

part_patches() {
    local p ok=1
    for p in net/lwip/patches/*.patch; do
        if (cd net/lwip && patch -p1 -R --dry-run -s -f <"../../$p" >/dev/null 2>&1); then say "patch applied: ${p##*/}"
        else bad "patch not applied to net/lwip/src: ${p##*/}"; ok=0; fi
    done
    [ $ok = 1 ] && say "patches PASS"
}

part_rand() {
    $HOSTCC $HCFLAGS $SAN -o "$OUT/rand-test" tools/host/rand-test.c net/port/rand.c || { bad "rand-test does not build"; return; }
    if "$OUT/rand-test"; then say "rand PASS"; else bad "rand-test"; fi
}

part_unit() {
    local T=net/lwip/test/unit v
    for v in all pbuf; do
        local def="" files="$T/core/test_def.c $T/core/test_dns.c $T/core/test_netif.c
            $T/core/test_timers.c $T/dhcp/test_dhcp.c $T/etharp/test_etharp.c $T/ip4/test_ip4.c $T/tcp/tcp_helper.c
            $T/tcp/test_tcp_oos.c $T/tcp/test_tcp_state.c $T/tcp/test_tcp.c $T/udp/test_udp.c"
        [ $v = pbuf ] && { def=-DUNIT_PBUF; files=$T/core/test_pbuf.c; }
        $HOSTCC $HCFLAGS $SAN $def -DNETD_HOST_OPTS='"unit-opts.h"' -include stdio.h -Itools/host/lwip-unit -I$T \
            -Wno-sign-compare -Wno-unused-variable -Wno-unused-but-set-variable -Wno-format -o "$OUT/lwip-unit-$v" \
            tools/host/lwip-unit/main.c tools/host/lwip-unit/check.c tools/host/lwip-unit/zenix_tests.c $files $LWIP_CORE $PORT_SRC ||
            { bad "lwip-unit-$v does not build"; continue; }
        # no leak check: some tests take lwIP's lists apart themselves (test_timers), and upstream runs each test in
        # a process of its own (libcheck's fork mode), where that does not matter
        if ASAN_OPTIONS=detect_leaks=0 "$OUT/lwip-unit-$v" >"$OUT/lwip-unit-$v.log" 2>&1; then say "unit ($v): $(tail -1 "$OUT/lwip-unit-$v.log")"
        else grep -a '^FAIL\|ERROR\|lwip-unit:' "$OUT/lwip-unit-$v.log" | head -30; bad "lwIP's unit tests ($v, log $OUT/lwip-unit-$v.log)"; fi
    done
}

NETD_SRC="net/netd/core.c net/netd/sock.c net/netd/iface.c net/netd/config.c"
part_size() {
    local tp=${TOOLPREFIX:-}
    [ -z "$tp" ] && { if command -v m68k-elf-gcc >/dev/null; then tp=m68k-elf-; else tp=$HOME/opt/m68k-elf/bin/m68k-elf-; fi; }
    command -v "${tp}gcc" >/dev/null || { say "size skipped: no m68k cross compiler (${tp}gcc)"; return; }
    local d="$OUT/m68k" f objs=""
    rm -rf "$d"; mkdir -p "$d"
    for f in $LWIP_SRC $PORT_SRC; do
        "${tp}gcc" -m68020-60 -Os -msoft-float -ffreestanding -fno-builtin -nostdlib -ffunction-sections -fdata-sections \
            -Wall -Inet/port/shim -Inet/port -I$LW/include -c "$f" -o "$d/$(basename "$f" .c).o" || { bad "m68k: $f"; return; }
        objs="$objs $d/$(basename "$f" .c).o"
    done
    "${tp}gcc" -m68020-60 -x assembler-with-cpp -c net/port/m68k.S -o "$d/m68k.o" || { bad "m68k: net/port/m68k.S"; return; }
    objs="$objs $d/m68k.o"
    "${tp}size" -t $objs >"$d/size.txt"
    say "size: lwIP and its port for m68k at -Os, unlinked (text data bss): $(tail -1 "$d/size.txt" | awk '{ print $1, $2, $3 }')"
    # linked with netd's core, --gc-sections keeping what netd's calls reach (tools/host/net-size)
    local nobjs="" lib
    for f in $NETD_SRC tools/host/net-size/entry.c tools/host/net-size/stubs.c; do
        "${tp}gcc" -m68020-60 -Os -msoft-float -ffreestanding -fno-builtin -nostdlib -ffunction-sections -fdata-sections \
            -Wall -Inet/port/shim -Inet/port -I$LW/include -c "$f" -o "$d/n-$(basename "$f" .c).o" || { bad "m68k: $f"; return; }
        nobjs="$nobjs $d/n-$(basename "$f" .c).o"
    done
    lib=$("${tp}gcc" -m68020-60 -msoft-float -print-libgcc-file-name)
    "${tp}gcc" -m68020-60 -msoft-float -nostdlib -nostartfiles -Wl,--gc-sections -Wl,-e,netd_size_entry \
        -Wl,-Map,"$d/netd.map" -o "$d/netd-size.elf" $objs $nobjs "$lib" || { bad "m68k: netd does not link"; return; }
    local total stubs
    total=$("${tp}size" "$d/netd-size.elf" | awk 'NR == 2 { print $1, $2, $3 }')
    stubs=$("${tp}size" "$d/n-stubs.o" "$d/n-entry.o" | awk 'NR > 1 { t += $1 } END { print t }')
    say "size: netd's core + lwIP + the port for m68k at -Os, linked (text data bss): $total; of the text $stubs are the stand-ins for libz and the OS layer"
    say "size PASS"
}

# in_netns CMD...: CMD in a new user and network namespace (no root needed, nothing leaves the PC), with the
# loopback up, /etc/resolv.conf naming 127.0.0.1 (where the tests' resolvers listen, port 53), and ICMP echo
# sockets allowed (libslirp answers a ping to the gateway with one)
in_netns() {
    local rc="$OUT/resolv.conf"
    echo "nameserver 127.0.0.1" >"$rc"
    unshare -rnm bash -c 'ip link set lo up && echo "0 0" >/proc/sys/net/ipv4/ping_group_range && mount --bind "$0" /etc/resolv.conf && exec "$@"' "$rc" "$@"
}
have_slirp() {
    pkg-config --exists slirp || { bad "libslirp (with GLib) is not installed: pkg-config slirp fails"; return 1; }
    unshare -rn true 2>/dev/null || { bad "unprivileged user namespaces are off (unshare -rn)"; return 1; }
    command -v ip >/dev/null || { bad "no ip command (iproute2) for the test namespace"; return 1; }
}
build_slirp() {
    make -s -C tools/host/zenix-slirp OUT="$PWD/$OUT" CFLAGS="-O1 -g -Wall -Wextra -Werror $SAN" ||
        { bad "zenix-slirp does not build"; return 1; }
}

part_slirp() {
    have_slirp && build_slirp || return
    if in_netns python3 -I tools/host/zenix-slirp-test.py "$OUT/zenix-slirp" "$OUT"; then say "slirp PASS"
    else bad "zenix-slirp-test"; fi
}
part_netd() {
    $HOSTCC $HCFLAGS $SAN -pthread -o "$OUT/netd-host" tools/host/netd-host.c $NETD_SRC $LWIP_SRC $PORT_SRC ||
        { bad "netd-host does not build"; return; }
    if "$OUT/netd-host" --config >"$OUT/netd-host-config.log" 2>&1; then say "netd: $(tail -1 "$OUT/netd-host-config.log")"
    else cat "$OUT/netd-host-config.log"; bad "netd's configuration parsers"; fi
    have_slirp && build_slirp || return
    rm -f "$OUT"/netd-host.*.log "$OUT/netd-host.console"
    if in_netns "$OUT/netd-host" "$OUT/zenix-slirp" "$OUT" >"$OUT/netd-host.log" 2>&1; then
        grep -c '^ok' "$OUT/netd-host.log" | sed 's/^/net-test: netd: checks passed: /'; say "netd PASS"
    else grep -a '^FAIL\|ERROR\|netd-host:' "$OUT/netd-host.log" | head -40; bad "netd-host (log $OUT/netd-host.log, netd's own $OUT/netd-host.netd.log)"; fi
}
# libFuzzer, one process at a time with a memory limit (this PC's rule: knowledge base claude-code/, memory)
part_fuzz() {
    local secs=$1 d="$OUT/fuzz" FCC=${FUZZCC:-clang} fl="-fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined"
    command -v "$FCC" >/dev/null || { bad "fuzz needs clang (libFuzzer)"; return; }
    mkdir -p "$d/corpus" "$d/artifacts"
    $FCC $HCFLAGS $fl -DNETD_HOST_OPTS='"fuzz-opts.h"' -Itools/host/lwip-fuzz -Dnetd_rand=netd_chacha_rand \
        -c net/port/rand.c -o "$d/rand.o" &&
    $FCC $HCFLAGS $fl -DNETD_HOST_OPTS='"fuzz-opts.h"' -Itools/host/lwip-fuzz -o "$d/lwip-fuzz" \
        tools/host/lwip-fuzz/lwip-fuzz.c $NETD_SRC $LWIP_SRC net/port/port.c "$d/rand.o" || { bad "lwip-fuzz does not build"; return; }
    python3 -I tools/host/lwip-fuzz/seeds.py net/lwip/test/fuzz/inputs "$d/seeds" >/dev/null || { bad "fuzz seeds"; return; }
    say "fuzz: $secs s, one process, 2 GB memory limit (log $d/fuzz.log)"
    if nice "$d/lwip-fuzz" -max_total_time="$secs" -rss_limit_mb=2048 -malloc_limit_mb=512 -max_len=4096 -timeout=10 \
            -artifact_prefix="$d/artifacts/" -print_final_stats=1 "$d/corpus" "$d/seeds" >"$d/fuzz.log" 2>&1; then
        say "fuzz: $(grep -a 'stat::number_of_executed_units' "$d/fuzz.log" | awk '{ print $2 }') inputs run, $(ls "$d/corpus" | wc -l) in the corpus, $(grep -a 'cov:' "$d/fuzz.log" | tail -1 | sed 's/.*cov: \([0-9]*\).*/\1/') edges covered"
        say "fuzz PASS: nothing found"
    else
        grep -a 'ERROR\|SUMMARY\|lwip-fuzz:\|Test unit written' "$d/fuzz.log" | head -20
        bad "fuzz found something: $d/artifacts"
    fi
}

if [ $# = 0 ]; then set -- patches rand unit netd slirp size; fi
while [ $# -gt 0 ]; do
    case "$1" in
    patches|rand|unit|netd|slirp|size) "part_$1" ;;
    fuzz) if [[ "$2" =~ ^[0-9]+$ ]]; then part_fuzz "$2"; shift; else part_fuzz 60; fi ;;
    *) echo "net-test: unknown part $1"; exit 2 ;;
    esac
    shift
done
if [ $fails = 0 ]; then say "all PASS"; exit 0; fi
say "$fails FAILED"; exit 1
