#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# telnet against the BBS simulator in FS-UAE (DESIGN D20 8.2, step 2: the Unix mode): the stock A1200 (2 MB chip RAM)
# with FS-UAE's A2065 (slirp; the serial port stays the console's, so the tap types the keys and reads the console's
# copy), tools/bbs-sim.py on the namespace's 127.0.0.1:2323 (the Amiga's 10.0.2.2), as tools/eth-test.sh runs its
# servers. Checked: the negotiation the board recorded (TTYPE XTERM-16COLOR, NAWS 80 x 32, BINARY both ways, no
# negotiation before the board's first IAC on a port that is not 23, no loop); the Unix screen's UTF-8 shown as
# Latin-1 on the console's copy; the console's ESC[6n answer reaching the board; keys (Ctrl-C, a Swedish letter as
# UTF-8); CONR_LFONLY set on console 1 while telnet runs (termtest rules from console 2, with free and ps: the memory
# while a call is up); the escape prompt (here -e ^E: 0x1D is the serial port's own hook), z then fg, q; the board
# hanging up ("Connection closed by"); telnet with a pipe for keys and a file for the screen (the console's rules
# refused: said once, the call works); a refused port (exit 1). Needs unprivileged user namespaces, as eth-test.
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
make -s BUILD=$BD all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
OUT=${ZENIX_OUT:-/tmp/zenix-bbs}; mkdir -p $OUT
OUT=$(cd "$OUT" && pwd)
BD=$(cd "$BD" && pwd)
unshare -rn true 2>/dev/null || { echo "FAIL unprivileged user namespaces are off (unshare -rn)"; exit 1; }
pass=1
get() { debugfs -R "cat $1" $BD/zenix.hdf 2>/dev/null | tr -d '\r'; }
put() {
    debugfs -w -R "rm $2" $BD/zenix.hdf >/dev/null 2>&1
    debugfs -w -R "write $1 $2" $BD/zenix.hdf >/dev/null 2>&1
    debugfs -w -R "sif $2 uid 0" $BD/zenix.hdf >/dev/null 2>&1
    debugfs -w -R "sif $2 gid 0" $BD/zenix.hdf >/dev/null 2>&1
    debugfs -w -R "sif $2 mode 0100$3" $BD/zenix.hdf >/dev/null 2>&1
}
check() { if [ "$2" = 1 ]; then echo "PASS $1"; else echo "FAIL $1: $3"; pass=0; fi; }

part_e() {
rm -f $BD/zenix.hdf; make -s BUILD=$BD $BD/zenix.hdf >/dev/null 2>&1
debugfs -w -R "mkdir /etc/network" $BD/zenix.hdf >/dev/null 2>&1
debugfs -w -R "sif /etc/network uid 0" $BD/zenix.hdf >/dev/null 2>&1
printf 'auto eth0\niface eth0 inet dhcp\n    driver a2065\n' > $OUT/interfaces
put $OUT/interfaces /etc/network/interfaces 644
printf 'sleep 4\necho unix\nsleep 3\necho bye\nsleep 2\n' > $OUT/keys.sh     # keys from a pipe, after the negotiation
put $OUT/keys.sh /tests/keys.sh 755
L='for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30; do ifconfig eth0 2>/dev/null | grep -q "lease [0-9]" && break; sleep 1; done'
cat > $OUT/t.txt <<EOS
wait zenix:/# 90
sleep 1
send $L; echo ==u""p\r
wait ==up 60
send telnet -x -e ^E 10.0.2.2 2323\r
wait Connected to 10.0.2.2 30
sleep 3
send unix\r
wait <<END unix>> 20
send dsr\r
wait <<END dsr>> 20
send \x1d2
wait zenix login: 30
send root\r
sleep 4
send termtest rules /dev/tty1; free; ps\r
sleep 5
send \x1d1
sleep 2
send abc\x03\xe4\r
sleep 3
send \x05
wait telnet> 10
send z
wait zenix:/# 20
send fg\r
sleep 3
send lf\r
wait <<END lf>> 20
send \x05
wait telnet> 10
send q
wait Connection closed 20
send echo st1=\$?\r
wait st1= 20
send telnet -x 10.0.2.2 2323; echo st4=\$?\r
wait Connected to 30
sleep 3
send bye\r
wait st4= 30
send sh /tests/keys.sh | telnet -x 10.0.2.2 2323 > /tmp/t-out.txt 2> /tmp/t-err.txt; echo st2=\$?\r
wait st2= 60
send telnet -x 10.0.2.2 2399; echo st3=\$?\r
wait st3= 60
send termtest rules /dev/tty1\r
sleep 2
send dmesg > /tmp/t-k.txt; sync; echo ==do""ne\r
wait ==done 30
stop
EOS
sed '/^\[fs-uae\]/a network_card = a2065' fsuae/a1200-020.fs-uae > "$OUT/a1200-a2065.fs-uae"
rm -f $OUT/ready $OUT/record.jsonl
ZENIX_BUILD=$BD unshare -rnm bash -c 'ip link set lo up || exit 9
    python3 -I tools/bbs-sim.py --port 2323 --record "$1/record.jsonl" --ready "$1/ready" > "$1/bbs.log" 2>&1 &
    srv=$!
    for i in $(seq 100); do [ -e "$1/ready" ] && break; sleep 0.1; done
    tools/run-test.sh "$2" 700 "$3"; r=$?
    kill $srv 2>/dev/null; wait $srv 2>/dev/null
    exit $r' bbs "$OUT" "$OUT/a1200-a2065.fs-uae" "$OUT/t.txt" > $OUT/t.log 2>&1
rc=$?
[ $rc = 0 ] || { echo "FAIL the run (exit $rc)"; grep -a 'FAIL' $OUT/t.log | head -5; pass=0; }
for f in out err k; do get /tmp/t-$f.txt > $OUT/t-$f.txt; done
python3 - "$OUT" <<'PY' || pass=0
import json, re, sys
out = sys.argv[1]
log = open(out + "/t.log", "rb").read()
rec = [json.loads(l) for l in open(out + "/record.jsonl") if l.strip()]
calls, cur = [], None
for r in rec:
    if r["ev"] == "call":
        cur = []; calls.append(cur)
    elif cur is not None:
        cur.append(r)
bad = 0
def check(ok, what, detail=""):
    global bad
    print(("PASS " if ok else "FAIL ") + what + ("" if ok else ": " + str(detail)))
    bad += not ok
first = calls[0] if calls else []
ev = lambda c, n: [r for r in c if r["ev"] == n]
check(len(calls) == 3, "three calls reached the board (the refused port none)", len(calls))
check([r["name"] for r in ev(first, "ttype")][:2] == ["XTERM-16COLOR", "XTERM-16COLOR"], "TERMINAL-TYPE: XTERM-16COLOR, twice when asked twice", ev(first, "ttype"))
check(any(r["cols"] == 80 and r["rows"] == 32 for r in ev(first, "naws")), "NAWS: 80 x 32 (PAL)", ev(first, "naws"))
ag = ev(first, "agreed")[-1] if ev(first, "agreed") else {}
check(ag.get("him", {}).get("0") and ag.get("us", {}).get("0"), "BINARY agreed both ways", ag)
check(not ev(first, "client_iac_first") and not any(ev(c, "loop") for c in calls), "no negotiation before the board's IAC on port 2323, no loop")
check(any(re.fullmatch(r"\x1b\[\d+;\d+R", r["answer"]) for r in ev(first, "dsr")), "ESC[6n: the console's answer reached the board", ev(first, "dsr"))
keys = "".join(r["hex"] for r in ev(first, "keys"))
check("61626303c3a40d" in keys, "keys: abc, Ctrl-C, a-umlaut as UTF-8 (c3 a4), Return", keys[:120])
check(re.search(rb"\xe5\?x(\x1b\[1m)?B", log) is not None, "the Unix screen's UTF-8 shown as Latin-1 (\\xe5 for a-ring, ? for a box line)")
check(b"rules 64" in log, "CONR_LFONLY set on console 1 while telnet runs (termtest rules /dev/tty1)")
check(len(re.findall(rb"telnet> ", log)) >= 2 and b"<<END lf>>" in log, "the escape prompt twice; z suspended, fg went on (the lf screen came)")
check(b"Connection closed." in log and b"st1=0" in log, "q at the prompt: Connection closed., exit 0")
o, e = open(out + "/t-out.txt", "rb").read(), open(out + "/t-err.txt", "rb").read()
check(b"<<END unix>>" in o and b"Connection closed by 10.0.2.2." in o and b"st2=0" in log, "keys from a pipe, the screen into a file: the call works, the board's bye ends it", o[-200:])
check(e.count(b"console's rules are not available") == 1, "... the console's rules refused there: said once", e)
check(b"st3=1" in log and b"10.0.2.2 port 2399" in log, "a refused port: said, exit 1")
check(b"Connection closed by 10.0.2.2." in log and b"st4=0" in log, "the board hangs up (bye): Connection closed by 10.0.2.2., exit 0")
check(re.findall(rb"rules (\d+)", log)[-1:] == [b"0"], "after telnet the console's rules are back to none")
m = re.search(rb"chip RAM free[^\n]*", log)
ps = re.findall(rb"\n *\d+ +\d+ +\d+ [^\n]* telnet\r?(?=\n)", log)
print("memory while a call is up (2 MB chip RAM, A2065):", (m.group(0) if m else b"?").decode(), "|", b"".join(ps).decode(errors="replace").strip())
sys.exit(1 if bad else 0)
PY
k=$(tr -d '\r' < $OUT/t-k.txt)
echo "$k" | grep -q '\[kernel\]' && { echo "FAIL a process faulted: $(echo "$k" | grep '\[kernel\]' | head -3)"; pass=0; }
e2fsck -fn $BD/zenix.hdf > $OUT/e2fsck.log 2>&1 && echo "PASS the host's e2fsck finds the root clean" || { echo "FAIL e2fsck"; pass=0; }
}

# s: over SLIP on the stock A1200 (the serial port is the network's: nothing typed; /etc/profile runs the script once):
# 100 grid screens (about 36 KB) drawn on the console through telnet, four calls: telnet's own nice 1, nice 0 (nice -n
# -1) twice, nice 1 again: no byte may be lost on the serial port's receive ring (D20 section 6: the yield points in a
# console write and /dev/ttyS0's 4 KB ring); the time is counted for each (emulator figures only)
part_s() {
    pkg-config --exists slirp || { echo "FAIL libslirp (with GLib) is not installed: pkg-config slirp fails"; pass=0; return; }
    make -s -C tools/host/zenix-slirp OUT="$OUT" >/dev/null || { echo "FAIL zenix-slirp does not build"; pass=0; return; }
    local SERIAL="${ZENIX_SERIAL:-/tmp/zenix-serial}"
    rm -f $BD/zenix.hdf; make -s BUILD=$BD $BD/zenix.hdf >/dev/null 2>&1
    debugfs -w -R "mkdir /etc/network" $BD/zenix.hdf >/dev/null 2>&1
    debugfs -w -R "sif /etc/network uid 0" $BD/zenix.hdf >/dev/null 2>&1
    printf 'auto sl0\niface sl0 inet static\n    driver slip /dev/ttyS0 115200\n    address 10.0.2.15/24\n    gateway 10.0.2.2\n' > $OUT/sinterfaces
    printf 'if [ ! -e /tmp/ts-ran ]; then\n  touch /tmp/ts-ran\n  sh /tests/tslip.sh > /tmp/ts.out 2>&1\n  echo bbs-slip-""done\nfi\n' > $OUT/profile
    { echo 'sleep 4'; for i in $(seq 100); do echo 'echo grid'; done; echo 'sleep 3'; echo 'echo bye'; echo 'sleep 2'; } > $OUT/bigkeys.sh
    printf 'sh /tests/bigkeys.sh | $* telnet -x 10.0.2.2 2323 > /dev/tty\n' > $OUT/call.sh   # drawn on the console
    cat > $OUT/tslip.sh <<'EOS'
sleep 2
dmesg | grep 'bytes lost' > /tmp/ts-l0.txt
time sh /tests/call.sh 2> /tmp/ts-e1.txt; echo st1=$? > /tmp/ts-1.txt
dmesg | grep 'bytes lost' > /tmp/ts-l1.txt
time sh /tests/call.sh nice -n -1 2> /tmp/ts-e2.txt; echo st2=$? > /tmp/ts-2.txt
dmesg | grep 'bytes lost' > /tmp/ts-l2.txt
time sh /tests/call.sh nice -n -1 2> /tmp/ts-e3.txt; echo st3=$? > /tmp/ts-3.txt
dmesg | grep 'bytes lost' > /tmp/ts-l3.txt
time sh /tests/call.sh 2> /tmp/ts-e4.txt; echo st4=$? > /tmp/ts-4.txt
dmesg | grep 'bytes lost' > /tmp/ts-l4.txt
ifconfig sl0 > /tmp/ts-ifc.txt 2>&1
ifdown sl0
sync
EOS
    put $OUT/sinterfaces /etc/network/interfaces 644
    put $OUT/profile /etc/profile 644
    put $OUT/tslip.sh /tests/tslip.sh 755
    put $OUT/bigkeys.sh /tests/bigkeys.sh 755
    put $OUT/call.sh /tests/call.sh 755
    rm -f $OUT/console.log $OUT/ready $OUT/sready $OUT/srecord.jsonl "$SERIAL"
    echo "nameserver 127.0.0.1" > $OUT/resolv.conf
    unshare -rnm bash -c 'ip link set lo up && echo "0 0" >/proc/sys/net/ipv4/ping_group_range &&
        mount --bind "$0" /etc/resolv.conf || exit 9
        python3 -I tools/bbs-sim.py --port 2323 --record "$1/srecord.jsonl" --ready "$1/sready" > "$1/sbbs.log" 2>&1 &
        bbs=$!
        python3 -I tools/slip-host.py "$1/zenix-slirp" "$2" "$1/console.log" "$1/ready" &
        sh=$!
        trap "kill $bbs $sh 2>/dev/null" TERM
        wait $sh
        kill $bbs 2>/dev/null' \
        "$OUT/resolv.conf" "$OUT" "$SERIAL" > $OUT/shost.log 2>&1 &
    local host=$! secs=600
    for _ in $(seq 100); do [ -e $OUT/ready ] && break; sleep 0.1; done
    [ -e $OUT/ready ] || { echo "FAIL the PC's side did not start: $(cat $OUT/shost.log)"; pass=0; kill $host; return; }
    ( for _ in $(seq $secs); do grep -aq 'bbs-slip-done' $OUT/console.log 2>/dev/null && break; sleep 1; done
      sleep 3; touch "$SERIAL.stop" ) &
    local watch=$!
    ZENIX_NOTAP=1 ZENIX_BUILD=$BD tools/run-test.sh fsuae/a1200-020.fs-uae $((secs + 30)) > $OUT/srun.log 2>&1
    kill $watch $host 2>/dev/null; wait $host 2>/dev/null
    rm -f "$SERIAL.stop"
    grep -aq 'bbs-slip-done' $OUT/console.log || { echo "FAIL the script's end never reached the console log"; pass=0; }
    local f
    for f in out l0 l1 l2 l3 l4 1 2 3 4 e1 e2 e3 e4 ifc; do get /tmp/ts-$f.txt > $OUT/ts-$f.txt; done
    python3 - "$OUT" <<'PY' || pass=0
import json, re, sys
out = sys.argv[1]
rd = lambda n: open(f"{out}/ts-{n}.txt", errors="replace").read()
lost = lambda t: sum(int(x) for x in re.findall(r"serial: (\d+) bytes lost", t))
l = [lost(rd(f"l{i}")) for i in range(5)]
d = [l[i + 1] - l[i] for i in range(4)]                  # calls 1 and 4 at telnet's nice 1, 2 and 3 at nice 0
times = [re.findall(r"real\s+(\S+)", rd(f"e{i}")) for i in range(1, 5)]
rec = [json.loads(x) for x in open(out + "/srecord.jsonl") if x.strip()]
calls, cur = [], None
for r in rec:
    if r["ev"] == "call": cur = []; calls.append(cur)
    elif cur is not None: cur.append(r)
grids = [sum(1 for r in c if r["ev"] == "line" and r["text"] == "grid") for c in calls]
ends = [any(r["ev"] == "end" for r in c) for c in calls]
drawn = not any("not available" in rd(f"e{i}") for i in range(1, 5))
ok = drawn and all(f"st{i}=0" in rd(str(i)) for i in range(1, 5)) and grids == [100] * 4 and all(ends)
print(("PASS" if ok else "FAIL") + f" over SLIP: four calls of 100 grid screens each drawn through telnet, the board's bye ended each (grids {grids})")
lossless = all(x == 0 for x in d)
print(("PASS" if lossless else "FAIL") + f" over SLIP: no byte lost on the serial port's receive ring while telnet drew (DESIGN D20 section 6: {d})")
ok = ok and lossless
print(f"over SLIP (FS-UAE, the stock A1200, an emulator figure): bytes lost on the serial ring, calls in the order nice 1,"
      f" 0, 0, 1: {d} (nice 1: {d[0] + d[3]}, nice 0: {d[1] + d[2]}); times {[t[0] if t else '?' for t in times]}")
sys.exit(0 if ok else 1)
PY
    e2fsck -fn $BD/zenix.hdf > $OUT/e2fsck-s.log 2>&1 && echo "PASS the host's e2fsck finds the root clean (s)" || { echo "FAIL e2fsck (s)"; pass=0; }
}

# b, b16: the BBS modes (D20 step 3) over the A2065, with screenshots (tools/shot.py, the cage rig): b on the stock
# A1200 (2 MB chip RAM, 8 colours: 3 planes), b16 on the A1200 with 4 MB fast RAM (16 colours: 4 planes). Three calls:
# -p (the PC mode), the default (the Amiga mode), -p into a file (the fallback). Checked: what telnet handed the console
# (its copy on the serial port) against bbs-sim's EXPECT for each screen and mode (music, iCE, FF, SUB, the Amiga
# codes, the font switch); the board's record (TERMINAL-TYPE ANSI, NAWS 80x32, the keys per mode: Backspace BS,
# a-ring 0x86 in CP437 or 0xE5 in Latin-1; the Amiga window bounds answer); the console's rules while each
# mode runs (termtest rules from console 2: PC 111, Amiga 46; none after); the "colours 16" note once a call at fewer
# than 16 colours, never at 16; the escape prompt's mode switch; the fallback (CP437 as Latin-1 in the file, said
# once). The screenshots: the PC grid in the CP437 font's glyphs (every cell), the colours in the CGA palette (both
# BBS modes, bright by CONR_BRIGHT), and the Amiga mode's Latin-1 screen in the same Topaz glyphs as the plain console.
part_b() {
    local tag=$1 conf=$2 depth=$3 col=$4
    [ -n "$ZENIX_CAGE_RIG" ] || { echo "FAIL ($tag) ZENIX_CAGE_RIG is not set (the screenshots need the cage rig)"; pass=0; return; }
    # a build of its own with the display window colour-test uses (DIWV=0x38: every row inside FS-UAE's picture)
    local BD0=$BD; BD=$OUT/bbuild-$tag
    make -s -j8 BUILD=$BD DIWV=0x38 $BD/zenix.adf $BD/zenix.hdf >$OUT/bbuild-$tag.log 2>&1 ||
        { echo "FAIL ($tag) the DIWV=0x38 build"; BD=$BD0; pass=0; return; }
    debugfs -w -R "mkdir /etc/network" $BD/zenix.hdf >/dev/null 2>&1
    debugfs -w -R "sif /etc/network uid 0" $BD/zenix.hdf >/dev/null 2>&1
    printf 'auto eth0\niface eth0 inet dhcp\n    driver a2065\n' > $OUT/interfaces
    put $OUT/interfaces /etc/network/interfaces 644
    printf 'sleep 4\necho grid\nsleep 3\necho bye\nsleep 2\n' > $OUT/keys3.sh
    put $OUT/keys3.sh /tests/keys3.sh 755
    python3 -c 'import sys; sys.stdout.buffer.write(bytes(range(0x20, 0x7F)) + b"\r\n" + bytes(range(0xA0, 0x100)) + b"\r\n")' > $OUT/latin1.txt
    put $OUT/latin1.txt /tests/latin1.txt 644
    local L='for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30; do ifconfig eth0 2>/dev/null | grep -q "lease [0-9]" && break; sleep 1; done'
    {
        printf '%s\n' 'wait zenix:/# 120' 'sleep 1' "send $L; echo ==u\"\"p\\r" 'wait ==up 60'
        printf '%s\n' 'send termtest columns\r' 'sleep 5' 'shot columns' 'send q' 'sleep 2'
        # call 1: the PC mode
        printf '%s\n' 'send telnet -p -e ^E 10.0.2.2 2323; echo stp=$?\r' 'wait <<END hello>> 40' 'sleep 2'
        for w in ff grid; do printf '%s\n' "send $w\\r" "wait <<END $w>> 30"; done
        printf '%s\n' 'sleep 3' 'shot pcgrid'
        for w in ff colours; do printf '%s\n' "send $w\\r" "wait <<END $w>> 30"; done
        printf '%s\n' 'sleep 3' 'shot pccolours'
        for w in ice music sub; do printf '%s\n' "send $w\\r" "wait <<END $w>> 30"; done
        printf '%s\n' 'send x\x7f\xe5\r' 'wait <<END unknown>> 30'
        printf '%s\n' 'send \x1d2' 'wait zenix login: 30' 'send root\r' 'sleep 4' 'send termtest rules /dev/tty1; free\r' 'sleep 5' \
            'send \x1d1' 'sleep 2' 'send \x05' 'wait telnet> 15' 'send a' 'sleep 3'
        for w in amiga font; do printf '%s\n' "send $w\\r" "wait <<END $w>> 30"; done
        printf '%s\n' 'send \x05' 'wait telnet> 15' 'send q' 'wait stp= 30'
        # call 2: the default, the Amiga mode
        printf '%s\n' 'send telnet -e ^E 10.0.2.2 2323; echo sta=$?\r' 'wait <<END hello>> 40' 'sleep 2'
        for w in ff latin1; do printf '%s\n' "send $w\\r" "wait <<END $w>> 30"; done
        printf '%s\n' 'sleep 3' 'shot amlatin1'
        for w in ff colours; do printf '%s\n' "send $w\\r" "wait <<END $w>> 30"; done
        printf '%s\n' 'sleep 3' 'shot amcolours'
        for w in amiga amigaart music; do printf '%s\n' "send $w\\r" "wait <<END $w>> 30"; done
        printf '%s\n' 'send x\x7f\xe5\r' 'wait <<END unknown>> 30'
        printf '%s\n' 'send \x1d2' 'sleep 3' 'send termtest rules /dev/tty1\r' 'sleep 4' 'send \x1d1' 'sleep 2' \
            'send bye\r' 'wait sta= 30'
        printf '%s\n' 'send clear; cat /tests/latin1.txt\r' 'sleep 4' 'shot catlatin1'
        # call 3: the PC mode into a file
        printf '%s\n' 'send sh /tests/keys3.sh | telnet -p 10.0.2.2 2323 > /tmp/t3-out.txt 2> /tmp/t3-err.txt; echo stf=$?\r' \
            'wait stf= 60' 'send termtest rules /dev/tty1\r' 'sleep 3' \
            'send dmesg > /tmp/t3-k.txt; sync; echo ==do""ne\r' 'wait ==done 30'
    } > $OUT/b-$tag.txt
    sed '/^\[fs-uae\]/a network_card = a2065' $conf > "$OUT/b-$tag.fs-uae"
    rm -f $OUT/bready $OUT/brecord-$tag.jsonl $OUT/b-$tag-*.png
    ZENIX_BUILD=$BD python3 tools/fsuae_conf.py "$OUT/b-$tag.fs-uae" "$OUT/b-$tag.conf.fs-uae" || { pass=0; BD=$BD0; return; }
    ZENIX_BUILD=$BD unshare -rnm bash -c 'ip link set lo up || exit 9
        python3 -I tools/bbs-sim.py --port 2323 --record "$1/brecord-$4.jsonl" --ready "$1/bready" > "$1/bbbs-$4.log" 2>&1 &
        srv=$!
        for i in $(seq 100); do [ -e "$1/bready" ] && break; sleep 0.1; done
        ZOOMOUT=0 SHOT_LIMIT=700 timeout -k 20 900 python3 tools/shot.py "$2" "$3" "$1/b-$4"; r=$?
        kill $srv 2>/dev/null; wait $srv 2>/dev/null
        exit $r' bbs "$OUT" "$OUT/b-$tag.conf.fs-uae" "$OUT/b-$tag.txt" "$tag" > $OUT/b-$tag.log 2>&1
    for f in out err k; do get /tmp/t3-$f.txt > $OUT/b-$tag-$f.txt; done
    python3 - "$OUT" "$tag" "$depth" "$col" <<'PY' || pass=0
import importlib.util, json, re, sys
out, tag, depth, col = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
spec = importlib.util.spec_from_file_location("bbs_sim", "tools/bbs-sim.py")
sim = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sim)
log = open(f"{out}/b-{tag}.serial.log", "rb").read()
rec = [json.loads(l) for l in open(f"{out}/brecord-{tag}.jsonl") if l.strip()]
calls, cur = [], None
for r in rec:
    if r["ev"] == "call":
        cur = []; calls.append(cur)
    elif cur is not None:
        cur.append(r)
bad = 0
def check(ok, what, detail=""):
    global bad
    print(("PASS " if ok else "FAIL ") + f"({tag}) " + what + ("" if ok else ": " + str(detail)[:300]))
    bad += not ok
ev = lambda c, n: [r for r in c if r["ev"] == n]
check(len(calls) == 3, "three calls reached the board", len(calls))
def screen(name, nth):
    """the bytes telnet handed the console for the nth time the board sent screen `name` (its copy on the serial
    port, from the END line before it, which may also hold the prompt or another console's output: the screen is its
    end; the console copy turns LF into CR LF, so CRs are not compared)"""
    ends = [m.end() for m in re.finditer(rb"<<END [a-z0-9]+>>\r*\n", log)]
    hits = [m.start() for m in re.finditer(rb"\r*\n<<END " + name.encode() + rb">>", log)]
    if len(hits) <= nth:
        return None
    end = hits[nth]
    start = max(e for e in ends if e <= end)
    return log[start:end].replace(b"\r", b"")
for name, mode, nth in (("ice", "pc", 0), ("music", "pc", 0), ("sub", "pc", 0), ("ff", "pc", 0), ("grid", "pc", 0),
                        ("amiga", "amiga", 0), ("font", "amiga", 0), ("amiga", "amiga", 1), ("amigaart", "amiga", 0),
                        ("music", "amiga", 1), ("latin1", "amiga", 0)):
    want = sim.expect(name, mode)[0].replace(b"\r", b"")
    got = screen(name, nth)
    check(got is not None and got.endswith(want), f"{mode} mode: the console got bbs-sim's EXPECT for '{name}'", (got, want))
c1, c2 = (calls + [[], []])[:2]
check([r["name"] for r in ev(c1, "ttype")][:1] == ["ANSI"] and [r["name"] for r in ev(c2, "ttype")][:1] == ["ANSI"],
      "TERMINAL-TYPE ANSI in both BBS modes", (ev(c1, "ttype"), ev(c2, "ttype")))
check(any(r["cols"] == 80 and r["rows"] == 32 for r in ev(c2, "naws")), "NAWS 80 x 32", ev(c2, "naws"))
k1 = "".join(r["hex"] for r in ev(c1, "keys"))
k2 = "".join(r["hex"] for r in ev(c2, "keys"))
check("7808860d" in k1, "PC mode keys: x, Backspace as BS, a-ring as CP437 0x86, Return", k1[-200:])
check("7808e50d" in k2, "Amiga mode keys: x, Backspace as BS, a-ring as Latin-1 0xE5, Return", k2[-200:])
w1, w2 = ev(c1, "wbr"), ev(c2, "wbr")
check(any(r["answer"] == "\x9b1;1;32;80 r" for r in w1) and any(r["answer"] == "\x9b1;1;32;80 r" for r in w2),
      "the Amiga window bounds (CSI 0 SP q) answered 1;1;32;80 (after the prompt's switch to the Amiga mode too)", (w1, w2))
rules = [int(x) for x in re.findall(rb"rules (\d+)", log)]
check(rules[:3] == [111, 46, 0], "the console's rules: PC mode 111, Amiga mode 46, none after the calls", rules)
note = rb"telnet: %d colours: ANSI art loses its shading; `colours 16` in /etc/console shows it fully" % col
n = log.count(note) if col < 16 else log.count(b"`colours 16`")
check(n == (2 if col < 16 else 0), f"the colours note: once a call on the console at {col} colours" if col < 16 else
      "no colours note at 16 colours", n)
check(b"telnet> " in log and b"stp=0" in log and b"sta=0" in log, "the escape prompt; both calls ended with status 0")
o = open(f"{out}/b-{tag}-out.txt", "rb").read()
e = open(f"{out}/b-{tag}-err.txt", "rb").read()
m = re.search(rb"8: (.{16})", o)
check(bool(m) and m.group(1) == bytes([0xC7, 0xFC, 0xE9, 0xE2, 0xE4, 0xE0, 0xE5, 0xE7, 0xEA, 0xEB, 0xE8, 0xEF, 0xEE, 0xEC, 0xC4, 0xC5]),
      "the PC mode into a file: CP437 shown as Latin-1 (row 8 of the grid)", m.group(1) if m else o[-200:])
m = re.search(rb"B: (.{16})", o)
check(bool(m) and m.group(1)[:5] == b"###|+", "... box characters and shades as + | #", m.group(1) if m else "")
check(e.count(b"the PC font is not available here") == 1 and e.count(b"rules are not available here") == 1 and b"stf=0" in log,
      "... the font and the rules refused there: each said once, the call works", e)
k = open(f"{out}/b-{tag}-k.txt", errors="replace").read()
check("[kernel]" not in k, "no process faulted", k[-300:])
sys.exit(1 if bad else 0)
PY
    local G=$OUT/b-$tag
    for s in columns pcgrid pccolours amlatin1 amcolours catlatin1; do
        [ -f "$G-$s.png" ] || { echo "FAIL ($tag) no screenshot $s (see $G.serial.log)"; pass=0; BD=$BD0; return; }
    done
    python3 tools/colour-test.py columns $G-columns.png > $G.geom || { echo "FAIL ($tag) termtest columns: $(cat $G.geom)"; pass=0; BD=$BD0; return; }
    ctb() { local r; python3 tools/colour-test.py "$@" > $G.r; r=$?; tail -1 $G.r; return $r; }   # the status, then its line
    m=$(ctb bbsgrid $G-pcgrid.png $G.geom $BD/cp437-8x8.psf 3); check "($tag) PC mode: the grid in the CP437 font: $m" $(( $? == 0 ))
    m=$(ctb bbscolours $G-pccolours.png $G.geom $depth 3); check "($tag) PC mode: $m" $(( $? == 0 ))
    m=$(ctb bbscolours $G-amcolours.png $G.geom $depth 3); check "($tag) Amiga mode: $m" $(( $? == 0 ))
    m=$(ctb bbssame $G-amlatin1.png $G.geom 3 $G-catlatin1.png $G.geom 1 4)
    check "($tag) Amiga mode: Latin-1 in Topaz, as the plain console draws it: $m" $(( $? == 0 ))
    e2fsck -fn $BD/zenix.hdf > $OUT/e2fsck-b.log 2>&1 && echo "PASS ($tag) the host's e2fsck finds the root clean" || { echo "FAIL ($tag) e2fsck"; pass=0; }
    rm -rf $BD; BD=$BD0
}

# z: Zmodem (D20 step 4) and the phone book (step 5) over the A2065 on the stock A1200 (2 MB chip RAM), against the
# simulator's real lsz and lrz (LRZSZ_DIR or PATH, as tools/host/telnet-test.sh). Two boards: 2323 with BINARY, 2324
# refusing it (rz -e / sz -e then), both called by name from root's phone book (/.phonebook). Downloads into /dl:
# two files whole (every byte value, CR NUL), the same file already there skipped, another file of the same name
# saved as NAME.1 (the old one untouched), a leading-dot name refused, a .part of the same file resumed only after
# ZCRC, another file's .part not spliced (kept, the file as NAME.1), a 1 MB download cancelled with Ctrl-X five times
# (its .part kept) and then resumed after ZCRC, whole; uploads asked for when the board's lrz starts, the files
# compared on the board. Checked byte for byte against the PC's files.
part_z() {
    local LSZ LRZ
    if [ -n "${LRZSZ_DIR:-}" ]; then LSZ="$LRZSZ_DIR/lsz"; LRZ="$LRZSZ_DIR/lrz"
    elif command -v lsz >/dev/null; then LSZ=$(command -v lsz); LRZ=$(command -v lrz)
    elif command -v lrzsz-sz >/dev/null; then LSZ=$(command -v lrzsz-sz); LRZ=$(command -v lrzsz-rz); fi
    [ -x "$LSZ" ] && [ -x "$LRZ" ] || { echo "FAIL (z) lrzsz not found: lsz/lrz on PATH or LRZSZ_DIR"; pass=0; return; }
    rm -f $BD/zenix.hdf; make -s BUILD=$BD $BD/zenix.hdf >/dev/null 2>&1
    debugfs -w -R "mkdir /etc/network" $BD/zenix.hdf >/dev/null 2>&1
    debugfs -w -R "sif /etc/network uid 0" $BD/zenix.hdf >/dev/null 2>&1
    debugfs -w -R "mkdir /dl" $BD/zenix.hdf >/dev/null 2>&1
    debugfs -w -R "sif /dl uid 0" $BD/zenix.hdf >/dev/null 2>&1
    printf 'auto eth0\niface eth0 inet dhcp\n    driver a2065\n' > $OUT/interfaces
    put $OUT/interfaces /etc/network/interfaces 644
    local F=$OUT/zfiles; rm -rf $F $OUT/zup $OUT/zup2 $OUT/zdl; mkdir -p $F $OUT/zup $OUT/zup2 $OUT/zdl
    python3 - "$F" "$OUT" <<'PY'
import os, random, sys
f, out = sys.argv[1], sys.argv[2]
r = random.Random(20261027)
w = lambda n, b: open(os.path.join(f, n), "wb").write(b)
w("random.bin", bytes(r.getrandbits(8) for _ in range(150000)))
w("bytes.bin", bytes(range(256)) * 64 + b"\r\x00\r\n\x18\x18B00\xff\xff\x11\x13" * 500)
w("same.txt", b"the same file on both sides\n" * 40)
w("other.txt", b"the board's other.txt\n" * 30)
w(".profile", b"echo a board must never write this\n")
w("resume.bin", bytes(r.getrandbits(8) for _ in range(120000)))
w("splice.bin", bytes(r.getrandbits(8) for _ in range(90000)))
w("big.bin", bytes(r.getrandbits(8) for _ in range(1 << 20)))
w("nob.bin", bytes(range(256)) * 200 + b"\r\x00" * 300)
d = open(os.path.join(f, "resume.bin"), "rb").read()
open(os.path.join(out, "z-resume.part"), "wb").write(d[:50000])               # the same file: resumed
open(os.path.join(out, "z-splice.part"), "wb").write(b"another file" * 3000)   # not this file: never spliced
open(os.path.join(out, "z-same.txt"), "wb").write(open(os.path.join(f, "same.txt"), "rb").read())
open(os.path.join(out, "z-other.txt"), "wb").write(b"Zenix's own other.txt\n")
open(os.path.join(out, "z-up.bin"), "wb").write(bytes(r.getrandbits(8) for _ in range(100000)))
open(os.path.join(out, "z-up2.txt"), "wb").write(b"\xff\xff\r\x00 two IACs, CR NUL\r\n" * 200)
open(os.path.join(out, "z-book"), "w").write("# the test boards\nsim   10.0.2.2 2323 amiga\nnob 10.0.2.2  2324 pc gizmo\n")
PY
    put $OUT/z-resume.part /dl/resume.bin.part 644
    put $OUT/z-splice.part /dl/splice.bin.part 644
    put $OUT/z-same.txt /dl/same.txt 644
    put $OUT/z-other.txt /dl/other.txt 644
    put $OUT/z-up.bin /up.bin 644
    put $OUT/z-up2.txt /up2.txt 644
    put $OUT/z-book /.phonebook 644
    local L='for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30; do ifconfig eth0 2>/dev/null | grep -q "lease [0-9]" && break; sleep 1; done'
    cat > $OUT/z.txt <<EOS
wait zenix:/# 120
sleep 1
send $L; echo ==u""p\r
wait ==up 60
send telnet; echo stl=\$?\r
wait stl= 20
send cd /dl; telnet -e ^E sim; echo stz=\$?\r
wait <<END hello>> 40
sleep 1
send sz random.bin bytes.bin same.txt other.txt .profile\r
wait <<END sz>> 240
send sz resume.bin splice.bin\r
wait <<END sz>> 240
send sz big.bin\r
waitecho big\.bin: [0-9]+ of 1048576 120
sleep 2
send \x18\x18\x18\x18\x18
wait <<END sz>> 60
send sz big.bin\r
wait <<END sz>> 400
send rz\r
waitecho send which files 60
send /up.bin /u\x7fup2.txt\r
wait <<END rz>> 240
send bye\r
wait stz= 30
send telnet -e ^E nob; echo stn=\$?\r
wait <<END hello>> 40
sleep 1
send sz nob.bin\r
wait <<END sz>> 240
send rz\r
waitecho send which files 60
send /up2.txt\r
wait <<END rz>> 240
send bye\r
wait stn= 30
send cd /; ls -a /dl > /tmp/z-ls.txt; free > /tmp/z-free.txt; dmesg > /tmp/z-k.txt; sync; echo ==do""ne\r
wait ==done 30
stop
EOS
    sed '/^\[fs-uae\]/a network_card = a2065' fsuae/a1200-020.fs-uae > "$OUT/a1200-a2065.fs-uae"
    rm -f $OUT/zready $OUT/zready2 $OUT/zrecord.jsonl $OUT/zrecord2.jsonl
    ZENIX_BUILD=$BD unshare -rnm bash -c 'ip link set lo up || exit 9
        python3 -I tools/bbs-sim.py --port 2323 --window 16384 --record "$1/zrecord.jsonl" --ready "$1/zready" --files "$1/zfiles" \
            --upload "$1/zup" --lsz "$4" --lrz "$5" > "$1/zbbs.log" 2>&1 &
        a=$!
        python3 -I tools/bbs-sim.py --port 2324 --window 16384 --refuse-binary --record "$1/zrecord2.jsonl" --ready "$1/zready2" \
            --files "$1/zfiles" --upload "$1/zup2" --lsz "$4" --lrz "$5" > "$1/zbbs2.log" 2>&1 &
        b=$!
        for i in $(seq 100); do [ -e "$1/zready" ] && [ -e "$1/zready2" ] && break; sleep 0.1; done
        tools/run-test.sh "$2" 1500 "$3"; r=$?
        kill $a $b 2>/dev/null; wait $a $b 2>/dev/null
        exit $r' bbs "$OUT" "$OUT/a1200-a2065.fs-uae" "$OUT/z.txt" "$LSZ" "$LRZ" > $OUT/z.log 2>&1
    local rc=$?
    [ $rc = 0 ] || { echo "FAIL (z) the run (exit $rc)"; grep -a 'FAIL' $OUT/z.log | head -5; pass=0; }
    for f in ls free k; do get /tmp/z-$f.txt > $OUT/z-$f.txt; done
    local n
    for n in random.bin bytes.bin same.txt other.txt other.txt.1 resume.bin splice.bin.part splice.bin.1 big.bin \
             nob.bin profile .profile big.bin.part resume.bin.part; do
        rm -f "$OUT/zdl/$n"; debugfs -R "dump /dl/$n $OUT/zdl/$n" $BD/zenix.hdf >/dev/null 2>&1
        debugfs -R "stat /dl/$n" $BD/zenix.hdf 2>/dev/null | grep -q Inode || rm -f "$OUT/zdl/$n"
    done
    python3 - "$OUT" <<'PY' || pass=0
import hashlib, json, os, re, sys
out = sys.argv[1]
log = open(out + "/z.log", "rb").read()
F, D = out + "/zfiles", out + "/zdl"
sha = lambda p: hashlib.sha256(open(p, "rb").read()).hexdigest() if os.path.exists(p) else None
rd = lambda p: open(p, "rb").read() if os.path.exists(p) else None
bad = 0
def check(ok, what, detail=""):
    global bad
    print(("PASS " if ok else "FAIL ") + "(z) " + what + ("" if ok else ": " + str(detail)[:300]))
    bad += not ok
def rec(n):
    return [json.loads(l) for l in open(out + n) if l.strip()]
r1, r2 = rec("/zrecord.jsonl"), rec("/zrecord2.jsonl")
x1 = [r for r in r1 if r["ev"] == "xfer"]
x2 = [r for r in r2 if r["ev"] == "xfer"]
check(b"the phone book (/.phonebook):" in log and re.search(rb"sim +10\.0\.2\.2 2323 amiga", log) is not None
      and b"stl=2" in log, "telnet alone: the usage and the phone book listed, exit 2")
check(b"unknown word gizmo" in log, "the phone book's unknown word said and ignored")
check(sum(1 for r in r1 if r["ev"] == "call") == 1 and sum(1 for r in r2 if r["ev"] == "call") == 1,
      "telnet sim and telnet nob called the boards by name (ports 2323 and 2324)")
check(any(r["ev"] == "ttype" and r["name"] == "ANSI" for r in r2), "... nob's mode from the book (pc: TERMINAL-TYPE ANSI)")
for n in ("random.bin", "bytes.bin"):
    check(sha(f"{D}/{n}") == sha(f"{F}/{n}"), f"download: {n} whole, byte for byte")
check(b"same.txt: the same file is here already" in log and sha(f"{D}/same.txt") == sha(f"{F}/same.txt"),
      "the same file already there: skipped")
check(rd(f"{D}/other.txt") == b"Zenix's own other.txt\n" and sha(f"{D}/other.txt.1") == sha(f"{F}/other.txt")
      and b"saved as other.txt.1" in log, "another file of that name: never overwritten, the download is other.txt.1")
check(not os.path.exists(f"{D}/.profile") and not os.path.exists(f"{D}/profile") and b".profile: refused" in log,
      "a leading-dot name (.profile): refused, nothing written")
check(sha(f"{D}/resume.bin") == sha(f"{F}/resume.bin") and not os.path.exists(f"{D}/resume.bin.part")
      and b"resume.bin: going on from resume.bin.part (the same file: CRC checked)" in log,
      "a .part of the same file: resumed after ZCRC, whole")
check(rd(f"{D}/splice.bin.part") == b"another file" * 3000 and sha(f"{D}/splice.bin.1") == sha(f"{F}/splice.bin")
      and b"splice.bin.part is not the same file" in log,
      "another file's .part: not spliced (kept), the file came whole as splice.bin.1")
check(b"Zmodem: cancelled" in log, "Ctrl-X five times: the download cancelled")
check(sha(f"{D}/big.bin") == sha(f"{F}/big.bin") and not os.path.exists(f"{D}/big.bin.part")
      and b"big.bin: going on from big.bin.part (the same file: CRC checked)" in log,
      "the cancelled 1 MB download resumed after ZCRC, whole")
u = [r for r in x1 if r["cmd"] == "rz"]
check(len(u) == 1 and u[0]["rc"] == 0 and u[0]["files"].get("up.bin") == sha(out + "/z-up.bin")
      and u[0]["files"].get("up2.txt") == sha(out + "/z-up2.txt"), "upload: up.bin and up2.txt whole on the board", u)
check(len(x1) == 5 and all(r["rc"] == 0 for i, r in enumerate(x1) if i != 2),
      "lsz/lrz on the board ended well (but the cancelled one)", x1)
check(sha(f"{D}/nob.bin") == sha(f"{F}/nob.bin"), "without BINARY (rz -e): nob.bin whole (CR NUL intact)")
u2 = [r for r in x2 if r["cmd"] == "rz"]
check(len(u2) == 1 and u2[0]["rc"] == 0 and u2[0]["files"].get("up2.txt") == sha(out + "/z-up2.txt"),
      "without BINARY (sz -e): up2.txt whole on the board", u2)
check(b"stz=0" in log and b"stn=0" in log, "both calls ended with status 0")
k = open(out + "/z-k.txt", errors="replace").read()
check("[kernel]" not in k, "no process faulted", k[-300:])
m = re.search(r"chip RAM free[^\n]*", open(out + "/z-free.txt", errors="replace").read())
print("(z) memory after the calls (2 MB chip RAM, A2065):", m.group(0) if m else "?")
sys.exit(1 if bad else 0)
PY
    e2fsck -fn $BD/zenix.hdf > $OUT/e2fsck-z.log 2>&1 && echo "PASS (z) the host's e2fsck finds the root clean" || { echo "FAIL (z) e2fsck"; pass=0; }
}

[ $# = 0 ] && set -- e s b b16 z
for p in "$@"; do
    case $p in
    e|s|z) "part_$p" ;;
    b) part_b b fsuae/a1200-020.fs-uae 3 8 ;;
    b16) part_b b16 fsuae/a1200-020-fast.fs-uae 4 16 ;;
    *) echo "bbs-test: unknown part $p"; exit 2 ;;
    esac
done
exit $((1 - pass))
