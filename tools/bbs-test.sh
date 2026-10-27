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
# -1) twice, nice 1 again: the bytes the serial port lost (its 1 KB ring, D20 section 6) and the time, counted for each;
# emulator figures only
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
print(f"over SLIP (FS-UAE, the stock A1200, an emulator figure): bytes lost on the serial ring, calls in the order nice 1,"
      f" 0, 0, 1: {d} (nice 1: {d[0] + d[3]}, nice 0: {d[1] + d[2]}); times {[t[0] if t else '?' for t in times]}")
sys.exit(0 if ok else 1)
PY
    e2fsck -fn $BD/zenix.hdf > $OUT/e2fsck-s.log 2>&1 && echo "PASS the host's e2fsck finds the root clean (s)" || { echo "FAIL e2fsck (s)"; pass=0; }
}

[ $# = 0 ] && set -- e s
for p in "$@"; do
    case $p in e|s) "part_$p" ;; *) echo "bbs-test: unknown part $p"; exit 2 ;; esac
done
exit $((1 - pass))
