#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# irc in FS-UAE (DESIGN D58 section 8): the A1200 with 4 MB of fast RAM (the minimum Zenix is made for) and FS-UAE's
# A2065 (slirp) against tools/host/irc-fake.py on the PC, in a user, mount and network namespace (tools/irc-ns.py:
# plain on 10.0.2.2:6667, TLS 1.2 on :6697 with a test CA made at this run, whose anchors file becomes the image's
# /etc/ssl/anchors; rsa.test in /etc/hosts). irc runs on the console with a settings file (rsa.test, tls on, an
# autojoin channel); `termtest screen` reads the console's grid at each step and then types the next keys: a line to
# the channel, /msg (the answer opens a query: the status row's activity), /me, Tab completing a nick, !drop (the
# server ends the connection: irc connects again by itself, TLS again, and joins the channel again), F3 (the query
# window), /whois, UTF-8 typed, Ctrl-N (the status window), Shift-Up (scrolled back: -more-), /quit with a message.
# Then a plain run from the command line (irc -n NICK 10.0.2.2 '#plain') and /quit. Checked on the screen: the title
# row (the channel and its topic), the window's lines with their times, the status row (nick, window, members,
# activity, -more-), the input row and its completion; the MOTD's ISO-8859-1 and UTF-8 lines, mIRC codes and an
# escape sequence dropped, a line over 512 bytes cut and said; and in the server's log every line irc sent (the
# PONG, the ACTION, UTF-8 on the wire, the rejoin on the second connection, QUIT with its message). The settings
# folder (the keys file written, mode 600 for the settings) and the memory with irc open are printed.
#   tools/irc-test.sh
# Needs openssl, Python's cryptography module, debugfs and unprivileged user namespaces with an /etc/subuid range.
cd "$(dirname "$0")/.." || exit 1
grep() { /usr/bin/grep "$@"; }
BD=${ZENIX_BUILD:-build}
make -s BUILD=$BD all >/dev/null 2>&1 || { echo "FAIL the build"; exit 1; }
OUT=${ZENIX_OUT:-/tmp/zenix-irc}; mkdir -p $OUT
OUT=$(cd "$OUT" && pwd); BD=$(cd "$BD" && pwd)
for t in debugfs e2fsck; do command -v $t >/dev/null || { echo "FAIL $t is not installed"; exit 1; }; done
python3 -c 'import cryptography' 2>/dev/null || { echo "FAIL Python's cryptography module is missing"; exit 1; }
unshare --map-root-user --map-auto -nm true 2>/dev/null || { echo "FAIL unshare --map-root-user --map-auto fails (newuidmap, /etc/subuid)"; exit 1; }
T=$OUT/ns; rm -rf "$T"; mkdir -p "$T"
head -c 64 /dev/urandom > $OUT/seed
rm -f $BD/zenix.hdf; make -s BUILD=$BD $BD/zenix.hdf >/dev/null 2>&1 || { echo "FAIL the image"; exit 1; }
printf 'auto eth0\niface eth0 inet dhcp\n    driver a2065\n' > $OUT/interfaces
printf '127.0.0.1\tlocalhost\n10.0.2.2\trsa.test\n' > $OUT/hosts
printf 'server rsa.test\ntls on\nnick amiga\nautojoin #zenix\nhighlight zenix\nquit leaving\n' > $OUT/config
cat > $OUT/setup.sh <<'EOS'
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30; do
    ifconfig eth0 2>/dev/null | grep -q 'lease [0-9]' && break
    sleep 1
done
cat /tests/seed > /dev/random
mkdir -p /tmp/h/.config/irc; cp /tests/config /tmp/h/.config/irc/config; chmod 600 /tmp/h/.config/irc/config
echo setup-done
EOS
cat > $OUT/k1.sh <<'EOS'
termtest screen /dev/tty1 'welcome (connection 1)' 900 'hi from amiga\r' > /tmp/s0.txt
free > /tmp/free.txt; ps > /tmp/ps.txt
termtest screen /dev/tty1 '<amiga> hi from amiga' 60 '/msg bob hello bob\r' > /tmp/s1.txt
termtest screen /dev/tty1 'act: 3!' 60 '/me waves\r' > /tmp/s2.txt
termtest screen /dev/tty1 '* amiga waves' 60 'b\x09' > /tmp/s3.txt
termtest screen /dev/tty1 '[#zenix] bob: ' 60 '\x15!drop\r' > /tmp/s4.txt
termtest screen /dev/tty1 'welcome (connection 2)' 900 '\x1bOR' > /tmp/s5.txt
termtest screen /dev/tty1 '[3:bob]' 60 '/whois\r' > /tmp/s6.txt
termtest screen /dev/tty1 'Bob Fake' 60 'räksmörgås\r' > /tmp/s7.txt
termtest screen /dev/tty1 '<amiga> räksmörgås' 60 '\x0e' > /tmp/s8.txt
termtest screen /dev/tty1 '[1:status]' 60 '\x1b[1;2A' > /tmp/s9.txt
termtest screen /dev/tty1 '-more-' 60 '/quit bye from zenix\r' > /tmp/s10.txt
EOS
cat > $OUT/k2.sh <<'EOS'
termtest screen /dev/tty1 'welcome (connection 3)' 300 '/quit\r' > /tmp/p0.txt
EOS
cat > $OUT/k.txt <<'EOS'
wait zenix:/# 120
sleep 1
send sh /tests/setup.sh\r
wait setup-done 120
send sh /tests/k1.sh & HOME=/tmp/h irc; echo st1=$?\r
wait st1= 1800
send sh /tests/k2.sh & HOME=/tmp/h irc -n plain2 10.0.2.2 '#plain'; echo st2=$?\r
wait st2= 600
send for i in 0 1 2 3 4 5 6 7 8 9 10; do echo ==s$i; cat /tmp/s$i.txt; done; echo ==p0; cat /tmp/p0.txt; echo ==mem; cat /tmp/free.txt /tmp/ps.txt; echo ==files; ls -l /tmp/h/.config/irc; grep -c window /tmp/h/.config/irc/keys; echo ==i""done\r
wait ==idone 120
stop
EOS
cat > $OUT/inner.sh <<'EOS'
#!/usr/bin/env bash
# inside the namespace: the files onto the image, then the run
T=$1 OUT=$2 BD=$3
img=$BD/zenix.hdf
put() {     # put FILE PATH MODE: a file on the image, root's
    debugfs -w -R "rm $2" $img >/dev/null 2>&1
    debugfs -w -R "write $1 $2" $img >/dev/null 2>&1
    debugfs -w -R "sif $2 uid 0" $img >/dev/null 2>&1
    debugfs -w -R "sif $2 gid 0" $img >/dev/null 2>&1
    debugfs -w -R "sif $2 mode 0100$3" $img >/dev/null 2>&1
}
mkd() { debugfs -w -R "mkdir $1" $img >/dev/null 2>&1; debugfs -w -R "sif $1 uid 0" $img >/dev/null 2>&1
        debugfs -w -R "sif $1 mode 040755" $img >/dev/null 2>&1; }
mkd /etc/network; mkd /etc/ssl
put $OUT/interfaces /etc/network/interfaces 644
put $OUT/hosts /etc/hosts 644
put $T/tls/test.anchors /etc/ssl/anchors 644
put $OUT/seed /tests/seed 600
put $OUT/config /tests/config 600
for f in setup k1 k2; do put $OUT/$f.sh /tests/$f.sh 755; done
ZENIX_BUILD=$BD tools/run-test.sh "$OUT/a1200-a2065.fs-uae" 2400 "$OUT/k.txt"
EOS
chmod +x $OUT/inner.sh
sed '/^\[fs-uae\]/a network_card = a2065' fsuae/a1200-020-fast.fs-uae > "$OUT/a1200-a2065.fs-uae"
unshare --map-root-user --map-auto -nm python3 -I tools/irc-ns.py "$T" -- "$OUT/inner.sh" "$T" "$OUT" "$BD" 2>&1 | tr -d '\r' > $OUT/run.log
pass=1
python3 - "$OUT/run.log" "$T/irc.log" <<'PY' || pass=0
import re, sys
sys.path.insert(0, "tools")
from screenlog import Log, rows, attrs, cursor
L = Log(sys.argv[1])
check = L.check
srv = open(sys.argv[2], encoding="utf-8", errors="replace").read() if len(sys.argv) > 2 else ""
s = [rows(L.part("s%d" % i, "s%d" % (i + 1) if i < 10 else "p0")) for i in range(11)]
p0 = rows(L.part("p0", "mem"))
check(L.status("st1") == 0 and L.status("st2") == 0, "irc ended with status 0 twice (/quit)", (L.status("st1"), L.status("st2")))
r = s[0]
n = max(r) if r else 0
txt = lambda rr: "\n".join(rr.get(i, "") for i in sorted(rr))
check(n >= 24 and r.get(0, "").startswith(" #zenix: the topic of #zenix"), "the title row: the channel and its topic", r.get(0))
check(re.search(r"^\d\d:\d\d -!- amiga \[user@zenix\.test\] has joined #zenix", txt(r), re.M) is not None,
      "the join, with its time", txt(r))
check(re.search(r"^\d\d:\d\d <bob> hello amiga, welcome \(connection 1\)", txt(r), re.M) is not None,
      "bob's greeting in the channel window (over TLS)", txt(r))
check(re.search(r"amiga  \[2:#zenix\] 5 nicks", r.get(n - 1, "")) is not None, "the status row: nick, window 2, 5 members",
      r.get(n - 1))
check(r.get(n, "") == "[#zenix]", "the input row's prompt", r.get(n))
check("<amiga> hi from amiga" in txt(s[1]), "a line typed: sent and shown", txt(s[1]))
check("act: 3!" in s[2].get(n - 1, ""), "bob's answer opened window 3, marked in the status row", s[2].get(n - 1))
check("* amiga waves" in txt(s[3]), "/me shown", txt(s[3]))
check(s[4].get(n, "") == "[#zenix] bob:", "Tab completed bob's nick (\"bob: \")", s[4].get(n))
t5 = txt(s[5])
check("disconnected: the server closed the connection" in t5 and "welcome (connection 2)" in t5,
      "the dropped connection: said, then connected again (TLS) and the channel joined again", t5)
check("[3:bob]" in s[6].get(n - 1, "") and "<bob> hi back" in txt(s[6]), "F3: the query window", txt(s[6]))
check("Bob Fake" in txt(s[7]), "/whois in the query: its answer there", txt(s[7]))
check("<amiga> räksmörgås" in txt(s[8]), "UTF-8 typed and shown", txt(s[8]))
t9 = txt(s[9])
check("[1:status]" in s[9].get(n - 1, "") and "Latin-1: räksmörgås" in t9 and "UTF-8: räksmörgås" in t9,
      "Ctrl-N to the status window: the MOTD's ISO-8859-1 and UTF-8 lines", t9)
check("- red bold and [2Jclear" in t9 and re.search(r"over 512 bytes: \d+ bytes cut", t9) and "nul here c12J done" in t9
      and "<>" not in t9,
      "mIRC codes and the escape sequence dropped; the long line cut and said; NUL and C1 handled; garbage not shown", t9)
check("-more-" in s[10].get(n - 1, "") and txt(s[10]) != t9, "Shift-Up: scrolled back, -more- in the status row", txt(s[10]))
check("welcome (connection 3)" in txt(p0) and "[2:#plain]" in p0.get(max(p0) - 1 if p0 else 0, ""),
      "the plain run from the command line: a channel given there joined", txt(p0))
check(re.search(r"^1 > PONG :fake-token-1$", srv, re.M) is not None, "the server's PING answered (TLS)")
check("1 > PRIVMSG #zenix :hi from amiga" in srv and "1 > PRIVMSG bob :hello bob" in srv and
      "1 > PRIVMSG #zenix :\x01ACTION waves\x01" in srv, "the lines typed, on the wire", srv[-1500:])
check("2 > JOIN #zenix" in srv and "2 > PRIVMSG bob :räksmörgås" in srv, "the second connection: the channel again; UTF-8 on the wire")
check("2 > QUIT :bye from zenix" in srv and "3 > JOIN #plain" in srv and "3 > QUIT :leaving" in srv,
      "/quit with its message; the plain run's channel; the settings' quit message")
f = L.part("files", "idone")
check(re.search(r"-rw------- .* config", f) is not None and re.search(r"^1\d$", f, re.M) is not None,
      "the settings kept at mode 600; the keys file written with the defaults (F1..F10)", f)
m = L.part("mem", "files")
ps = re.search(r"^ +\d+ +\d+ +\d+ +-?\d+ +\w+ +(\d+) +(\d+) irc$", m, re.M)
tp = re.search(r"^ +\d+ +\d+ +\d+ +-?\d+ +\w+ +(\d+) +(\d+) tlspipe$", m, re.M)
print("memory with irc open over TLS (A1200 + 4 MB fast, FS-UAE): irc %s KB, tlspipe %s KB" %
      (ps and ps.group(1), tp and tp.group(1)))
L.waits()
L.done()
PY
e2fsck -fn $BD/zenix.hdf > $OUT/e2fsck.log 2>&1 && echo "PASS the host's e2fsck finds the root clean" || { echo "FAIL e2fsck"; pass=0; }
[ $pass = 1 ] && echo "irc-test: all checks passed" || { echo "irc-test: some checks failed (log $OUT/run.log)"; exit 1; }
