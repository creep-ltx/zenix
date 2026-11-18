#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# ctop in FS-UAE (roadmap block 3; ports/ctop): the owner's btop-like monitor on the stock A1200. termtest screen reads
# the console's grid back (CON_READ) once ctop has drawn it and then types the next key into the console (CON_INPUT):
# the four boxes (the CPU's line from the kernel's log, chip memory in use, the processes, the root's space among the
# disks, "no network" while netd is not running), h (the keys), Space (back), e (only the Shells: none on Zenix), q;
# then with netd running (netstat starts it) and -d 0.5: the net box's rates and totals from the interfaces' counters
# (bsdsocket.library's calls, ports/ctop/zenix), q; a bad delay and an extra word (status 10, said), ? (the usage,
# status 0); the keys file written ($HOME/.config/ctop.keys); the memory with ctop open printed (free and ps). Every
# wait is for an event (the grid, the shell's lines), none for a time.
# Part n: the net box with real traffic, over FS-UAE's A2065 and slirp in a namespace of its own (as tools/eth-test.sh:
# DHCP, the PC's test servers on 10.0.2.2): eth0's address in the box's title, 200 KB fetched (nettest slipget) while
# ctop -d 0.5 runs, a rate of a kilobyte a second or more on the download line, then both lines back at "0B/s" with
# nothing after them but blanks (the owner saw "0B/s/s" on the PiStorm, 2026-10-30: a shorter rate over a longer one).
#   tools/ctop-test.sh [PART...]      c: the boxes, keys and errors (above); n: the net box with traffic (default: c n)
# Part n needs unprivileged user namespaces (unshare -rn), as eth-test.
cd "$(dirname "$0")/.." || exit 1
OUT=${ZENIX_OUT:-/tmp/zenix-ctop}; mkdir -p $OUT
BD=${ZENIX_BUILD:-build}
make -s BUILD=$BD all >$OUT/build.log 2>&1 || { echo "FAIL the build"; tail -5 $OUT/build.log; exit 1; }
pass=1
# no card probe at boot (D19 6.3): a file without an auto line starts no netd at boot, as before the probe
noprobe_file() {    # IMG
    printf '# the card probe is off in this test\nnoprobe\n' > "$OUT/noprobe-interfaces"
    debugfs -w -f - "$1" >/dev/null 2>&1 <<EOD
mkdir /etc/network
sif /etc/network uid 0
write $OUT/noprobe-interfaces /etc/network/interfaces
sif /etc/network/interfaces uid 0
sif /etc/network/interfaces gid 0
sif /etc/network/interfaces mode 0100644
EOD
}

part_c() {
rm -f $BD/zenix.hdf                     # a fresh root: no keys file from an earlier run
make -s BUILD=$BD $BD/zenix.hdf >>$OUT/build.log 2>&1 || { echo "FAIL the image"; tail -5 $OUT/build.log; exit 1; }
noprobe_file $BD/zenix.hdf              # the net box first without netd: none at boot
# the root's size as ctop shows it, from the image's superblock (the image grows with the system: FA-7 made it 20 MB)
rootkb=$(dumpe2fs -h $BD/zenix.hdf 2>/dev/null | awk -F: '/^Block count/ { n = $2 } /^Block size/ { b = $2 }
    END { if (n && b) print n * b / 1024 }')
ZENIX_BUILD=$BD tools/run-test.sh fsuae/a1200-020.fs-uae 400 tools/tests/ctop-test.txt 2>&1 | tr -d '\r' > $OUT/run.log
python3 - "$OUT/run.log" "$rootkb" <<'PY'
import re, sys
sys.path.insert(0, "tools")
from screenlog import Log, rows
L = Log(sys.argv[1])
def sbytes(v):      # ctop's own size format (ports/ctop/upstream/src/ctop.c sbytes)
    k, whole, frac = 0, v, 0
    while whole >= 1000 and k < 4:
        frac = (whole % 1024) * 10 // 1024; whole //= 1024; k += 1
    return str(whole) + ("." + str(frac) if k and whole < 10 else "") + "BKMGT"[k]
rootsz = sbytes(int(sys.argv[2]) * 1024) if sys.argv[2].isdigit() else None
check, part = L.check, L.part
s = [part("c%d" % i, "c%d" % (i + 1)) for i in range(4)] + [part("c4", "file")]
f = part("file", "cdone")
r = [rows(x) for x in s]
col = lambda rr, a, b: "\n".join(rr.get(i, "")[a:b] for i in range(32))
check([L.status(n) for n in ("st", "st2", "st5")] == [0, 0, 0], "ctop ran and ended with status 0 (q, q, ?)",
      [L.status(n) for n in ("st", "st2", "st5")])
r0 = r[0]
check(len(r0) == 32 and "cpu" in r0.get(0, "") and "1000ms" in r0.get(0, "") and "68020" in r0.get(1, ""),
      "the cpu box on the console's 32 rows: the refresh interval, the processor from the kernel's log",
      (r0.get(0), r0.get(1)))
c0 = col(r0, 44, 80)
check(re.search(r"chip \d+%", c0) is not None and re.search(r"Tasks \d+, \d+ running/ready", c0) is not None,
      "the cpu panel: chip memory in use, the processes and how many run or are ready", c0)
m0 = col(r0, 0, 36)
check(re.search(r"Chip: .* 1\.7M", m0) is not None and re.search(r"Used: .* \d+% \d", m0) is not None,
      "the mem box: chip memory's size (the kernel's boot log: 1,812 KB) and what is in use", m0)
check(rootsz is not None and re.search(r"disks", m0) is not None and
      re.search(r"^│/ +\S+ +\d+% +" + re.escape(rootsz) + "│$", m0, re.M) is not None,
      "the disks: the root (/) with its meter, its use and its size (%s, the image's)" % rootsz, m0)
check("no bsdsocket.library:" in m0 and "no network" in m0, "the net box while netd is not running: no network", m0)
p0 = col(r0, 36, 80)
check(re.search(r"Program:", p0) is not None and re.search(r"- ctop +0 run ", p0) is not None and
      re.search(r"- diskd +10 wai ", p0) is not None and re.search(r"- init +0 wai ", p0) is not None and
      re.search(r"\[idle\] +0 rdy ", p0) is not None and re.search(r"\d+/\d+─╯$", r0.get(31, "")) is not None,
      "the proc box: every process (ctop running, diskd at nice -10 as priority 10, the kernel's idle loop), "
      "the count", p0)
check(r[1].get(0, "").startswith(" ctop 0.1b1") and any(x.strip().startswith("q <Esc> <C-c>") for x in r[1].values())
      and "CPU% is sampled" in r[1].get(31, ""), "h: the keys as bound (the help screen)", (r[1].get(0), r[1].get(31)))
check(re.search(r"Tasks \d+", col(r[2], 44, 80)) is not None, "Space: back to the boxes", r[2].get(4))
check(r[3].get(31, "").endswith("0/0─╯") and "ctop" not in col(r[3], 36, 80),
      "e: only the Shells (none on Zenix: an empty list)", r[3].get(31))
r4 = r[4]
m4 = col(r4, 0, 36)
check("500ms" in r4.get(0, "") and re.search(r"v 0B/s", m4) is not None and re.search(r"\^ 0B/s", m4) is not None and
      m4.count("Total 0B") == 2 and re.search(r"- netd +0 wai ", col(r4, 36, 80)) is not None,
      "with netd running and -d 0.5: the net box's rates and totals (the interfaces' counters, none but the "
      "loopback's here), netd among the processes", m4)
check(L.status("st3") == 10 and re.search(r"^ctop: x: not a delay in seconds$", L.text, re.M) is not None and
      L.status("st4") == 10 and re.search(r"^ctop: extra: unexpected argument$", L.text, re.M) is not None and
      re.search(r"^usage: ctop \[-d SECONDS\]$", L.text, re.M) is not None,
      "a bad delay and an extra word: said, status 10; ?: the usage", (L.status("st3"), L.status("st4")))
check(re.search(r"^-rw-r--r-- .*\.config/ctop\.keys$", f, re.M) is not None and
      re.search(r"^; ctop's keys: KEY ACTION", f, re.M) is not None,
      "the keys file written at the first start ($HOME/.config/ctop.keys, the built-in keys)", f[:400])
L.memory(f, "ctop", "ctop")
L.waits()
L.done()
PY
[ $? = 0 ] || pass=0
}

part_n() {
    unshare -rn true 2>/dev/null || { echo "FAIL (n) unprivileged user namespaces are off (unshare -rn)"; pass=0; return; }
    local o b
    o=$(cd "$OUT" && pwd); b=$(cd "$BD" && pwd)
    rm -f $b/zenix.hdf
    make -s BUILD=$BD $BD/zenix.hdf >>$OUT/build.log 2>&1 || { echo "FAIL (n) the image"; pass=0; return; }
    get() { debugfs -R "cat $1" $b/zenix.hdf 2>/dev/null | tr -d '\r'; }
    put() {     # put FILE PATH MODE: a file on the image, root's
        debugfs -w -R "rm $2" $b/zenix.hdf >/dev/null 2>&1
        debugfs -w -R "write $1 $2" $b/zenix.hdf >/dev/null 2>&1
        debugfs -w -R "sif $2 uid 0" $b/zenix.hdf >/dev/null 2>&1
        debugfs -w -R "sif $2 gid 0" $b/zenix.hdf >/dev/null 2>&1
        debugfs -w -R "sif $2 mode 0100$3" $b/zenix.hdf >/dev/null 2>&1
    }
    debugfs -w -R "mkdir /etc/network" $b/zenix.hdf >/dev/null 2>&1
    debugfs -w -R "sif /etc/network uid 0" $b/zenix.hdf >/dev/null 2>&1
    printf 'auto eth0\niface eth0 inet dhcp\n    driver a2065\n' > $o/interfaces
    put $o/interfaces /etc/network/interfaces 644
    # the screens: the address (the lease), a rate of 1K/s or more during the download, then each line at 0B/s
    cat > $o/ctopnet.sh <<'EOS'
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30; do
    ifconfig eth0 2>/dev/null | grep -q 'lease [0-9]' && break
    sleep 1
done
termtest screen /dev/tty1 '10.0.2''.15' 60 > /tmp/n0.txt
nettest -n 204800 slipget > /tmp/n-nt.txt 2>&1 &
termtest screen /dev/tty1 'K/''s' 60 > /tmp/n1.txt
wait
termtest screen /dev/tty1 'v 0B/''s' 60 > /tmp/n2.txt
termtest screen /dev/tty1 '^ 0B/''s' 60 'q' > /tmp/n3.txt
EOS
    put $o/ctopnet.sh /tests/ctopnet.sh 755
    cat > $o/n.txt <<'EOS'
wait zenix:/# 90
sleep 1
send (sh /tests/ctopnet.sh > /tmp/n.out 2>&1) & ctop -d 0.5; echo stn=$?; sync; echo ctop-net-do""ne\r
wait ctop-net-done 300
stop
EOS
    sed '/^\[fs-uae\]/a network_card = a2065' fsuae/a1200-020.fs-uae > "$o/a1200-a2065.fs-uae"
    local rc=$o/resolv.conf
    echo "nameserver 127.0.0.1" > $rc
    rm -f $o/ready
    ZENIX_BUILD=$b unshare -rnm bash -c 'ip link set lo up && mount --bind "$0" /etc/resolv.conf || exit 9
        python3 -I tools/slip-host.py - none none "$1/ready" > "$1/host.log" 2>&1 &
        srv=$!
        for i in $(seq 100); do [ -e "$1/ready" ] && break; sleep 0.1; done
        tools/run-test.sh "$2" 400 "$3"; r=$?
        kill $srv 2>/dev/null; wait $srv 2>/dev/null
        exit $r' "$rc" "$o" "$o/a1200-a2065.fs-uae" "$o/n.txt" 2>&1 | tr -d '\r' > $o/n.log
    local f
    for f in n0 n1 n2 n3 n-nt; do get /tmp/$f.txt > $o/$f.txt; done
    python3 - "$o" <<'PY' || pass=0
import os, re, sys
sys.path.insert(0, "tools")
from screenlog import rows
o = sys.argv[1]
rd = lambda n: open(os.path.join(o, n), errors="replace").read()
bad = 0
def check(ok, what, detail=""):
    global bad
    print(("PASS (n) " if ok else "FAIL (n) ") + what + ("" if ok else ": " + repr(detail)[:900]))
    bad += not ok
r = [rows(rd("n%d.txt" % i)) for i in range(4)]
box = lambda rr: [x[:36] for x in (rr.get(i, "") for i in range(32))]
log = rd("n.log")
check("serial-tap: wait timed out" not in log and "the run ended during a wait" not in log, "every wait was met",
      re.findall(r"FAIL: serial-tap.*", log))
check(re.search(r"(?:^|\x1b\[\?1049l)stn=0$", log, re.M) is not None, "ctop ran and ended with status 0 (q)", log[-300:])
check(any(re.search(r"net.*10\.0\.2\.15", x) for x in box(r[0])), "the net box's title: eth0's address from DHCP",
      box(r[0])[24:32])
check(re.search(r"^nettest slipget: PASS ", rd("n-nt.txt"), re.M) is not None, "200 KB fetched over the A2065",
      rd("n-nt.txt"))
check(any(re.search(r"[v^] \d+(\.\d)?[KMG]/s +\u2502", x) for x in box(r[1])),
      "during the download: a rate of 1K/s or more, blanks after it", box(r[1])[24:32])
for i, (k, what) in enumerate((("v", "download"), ("^", "upload")), 2):
    lines = [x for x in box(r[i]) if re.search(re.escape(k) + r" \S", x)]
    check(len(lines) == 1 and re.search(re.escape(k) + r" 0B/s {10,}\u2502$", lines[0]) is not None,
          "after it: the %s line at 0B/s with nothing after it but blanks (no leftover of the longer rate)" % what,
          lines)
sys.exit(1 if bad else 0)
PY
    e2fsck -fn $b/zenix.hdf > $o/e2fsck-n.log 2>&1 && echo "PASS (n) the host's e2fsck finds the root clean" || { echo "FAIL (n) e2fsck"; pass=0; }
}

[ $# = 0 ] && set -- c n
for p in "$@"; do
    case $p in c|n) "part_$p" ;; *) echo "ctop-test: unknown part $p"; exit 2 ;; esac
done
exit $((1 - pass))
