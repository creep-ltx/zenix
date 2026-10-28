#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# ssh and ssh-keygen in FS-UAE (DESIGN D21 10 step 3): the stock A1200 (68EC020, 2 MB chip RAM) with FS-UAE's A2065
# (slirp) against two real OpenSSH sshds on the PC in a user, mount and network namespace (tools/ssh-ns.py; unshare
# --map-root-user --map-auto -nm, as tools/host/ssh-test.py). On the Amiga: ssh-keygen makes root's key; the first
# ssh asks about the unknown host (yes), logs in with the password and appends the Amiga's public key to the PC's
# authorized_keys; then a login with that key runs a command (its exit status 7 comes back); 1 MB from the PC through
# the session, byte for byte, with the server's rekey (RekeyLimit 1M); a host whose key differs from known_hosts is
# refused (exit 255); an interactive shell with a terminal: a command, Ctrl-C to a remote sleep, free and ps on
# console 2 while the session is up (the memory), ~. ends it; a known_hosts without its last line end (the new key on a
# line of its own); root's @revoked refuses a key the user trusts, and ssh-keygen -R keeps an @revoked line (audit
# 2026-10-28 S15, S16). Needs sshd, ssh-keygen and openssl on the PC and
# unprivileged user namespaces with an /etc/subuid range (newuidmap).
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
make -s BUILD=$BD all >/dev/null 2>&1 || { echo "FAIL the build"; exit 1; }
OUT=${ZENIX_OUT:-/tmp/zenix-ssh}; mkdir -p $OUT
OUT=$(cd "$OUT" && pwd); BD=$(cd "$BD" && pwd)
for t in /usr/bin/sshd ssh-keygen openssl debugfs; do command -v $t >/dev/null || { echo "FAIL $t is not installed"; exit 1; }; done
unshare --map-root-user --map-auto -nm true 2>/dev/null || { echo "FAIL unshare --map-root-user --map-auto fails (newuidmap, /etc/subuid)"; exit 1; }
pass=1
get() { debugfs -R "cat $1" $BD/zenix.hdf 2>/dev/null | tr -d '\r'; }
put() {
    debugfs -w -R "rm $2" $BD/zenix.hdf >/dev/null 2>&1
    debugfs -w -R "write $1 $2" $BD/zenix.hdf >/dev/null 2>&1
    debugfs -w -R "sif $2 uid 0" $BD/zenix.hdf >/dev/null 2>&1
    debugfs -w -R "sif $2 gid 0" $BD/zenix.hdf >/dev/null 2>&1
    debugfs -w -R "sif $2 mode 0100$3" $BD/zenix.hdf >/dev/null 2>&1
}
mkd() { debugfs -w -R "mkdir $1" $BD/zenix.hdf >/dev/null 2>&1; debugfs -w -R "sif $1 uid 0" $BD/zenix.hdf >/dev/null 2>&1
        debugfs -w -R "sif $1 mode 040$2" $BD/zenix.hdf >/dev/null 2>&1; }
T=$OUT/ns; rm -rf $T; mkdir -p $T/home/.ssh
for k in hostkey hostkey2; do ssh-keygen -q -t ed25519 -N "" -C "test-$k" -f $T/$k || { echo "FAIL ssh-keygen"; exit 1; }; done
head -c 1048576 /dev/urandom > $T/home/big.bin
head -c 64 /dev/urandom > $OUT/seed
rm -f $BD/zenix.hdf; make -s BUILD=$BD $BD/zenix.hdf >/dev/null 2>&1
mkd /etc/network 755; mkd /dl 755; mkd /.ssh 700
printf 'auto eth0\niface eth0 inet dhcp\n    driver a2065\n' > $OUT/interfaces
put $OUT/interfaces /etc/network/interfaces 644
put $OUT/seed /tests/seed 600
# port 2223 shows hostkey2: changed. No line end after it: the key ssh adds must still go on a line of its own (S16)
printf '%s' "[10.0.2.2]:2223 $(cut -d' ' -f1,2 $T/hostkey.pub)" > $OUT/known_hosts
echo "@revoked [10.0.2.2]:2222 $(cut -d' ' -f1,2 $T/hostkey.pub)" > $OUT/revoked        # audit 2026-10-28 S15
put $OUT/revoked /tests/revoked 644
mkd /etc/ssh 755
put $OUT/known_hosts /.ssh/known_hosts 644
L='for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30; do ifconfig eth0 2>/dev/null | grep -q "lease [0-9]" && break; sleep 1; done'
{ printf '%s\n' 'wait zenix:/# 120' 'sleep 1' "send $L; echo ==u\"\"p\\r" 'wait ==up 60'
cat <<'EOS'
send cat /tests/seed > /dev/random; ssh-keygen -C amiga-test; echo kg=$?\r
wait kg= 300
send ssh-keygen -l; ls -l /.ssh; echo ==l""s\r
wait ==ls 30
send ssh -p 2222 root@10.0.2.2 'cat >> .ssh/authorized_keys' < /.ssh/id_ed25519.pub; echo sa=$?\r
waitecho continue connecting 300
send yes\r
waitecho password: 300
send zenix-test-pw\r
wait sa= 300
send ssh -v -p 2222 root@10.0.2.2 'echo keylogin-$((6*7)); exit 7' 2> /tmp/s-v1.txt; echo sb=$?\r
wait sb= 300
send ssh -v -p 2222 root@10.0.2.2 cat big.bin > /dl/big.bin 2> /tmp/s-v2.txt; echo sc=$?; sha256sum /dl/big.bin; echo ==s""um\r
wait ==sum 1200
send ssh -p 2223 root@10.0.2.2 true; echo sd=$?\r
wait sd= 300
send ssh -p 2222 root@10.0.2.2\r
waitecho sh-[0-9.]+# 300
send echo hi-$((6*7))\r
waitecho hi-42 60
send \x1d2
wait zenix login: 30
send root\r
sleep 4
send free; ps\r
sleep 5
send \x1d1
sleep 2
send sleep 100\r
sleep 4
send \x03
sleep 2
send echo after-$((1+1))\r
waitecho after-2 60
send \r~.
waitecho Connection to 10.0.2.2 closed 60
sleep 2
send echo se=$?\r
wait se= 30
send cp /.ssh/known_hosts /tmp/kh1; cp /tests/revoked /etc/ssh/ssh_known_hosts; ssh -p 2222 root@10.0.2.2 true; echo sf=$?\r
wait sf= 300
send rm /etc/ssh/ssh_known_hosts; cat /tests/revoked >> /.ssh/known_hosts; ssh-keygen -R '[10.0.2.2]:2222'; echo sg=$?\r
wait sg= 60
send ssh -p 2222 root@10.0.2.2 true; echo sh=$?\r
wait sh= 300
send dmesg > /tmp/s-k.txt; sync; echo ==do""ne\r
wait ==done 30
stop
EOS
} > $OUT/s.txt
sed '/^\[fs-uae\]/a network_card = a2065' fsuae/a1200-020.fs-uae > "$OUT/a1200-a2065.fs-uae"
ZENIX_BUILD=$BD unshare --map-root-user --map-auto -nm python3 -I tools/ssh-ns.py $T -- \
    tools/run-test.sh "$OUT/a1200-a2065.fs-uae" 3000 "$OUT/s.txt" > $OUT/s.log 2>&1
rc=$?
[ $rc = 0 ] || { echo "FAIL the run (exit $rc)"; grep -a 'FAIL\|ssh-ns' $OUT/s.log | head -5; pass=0; }
for f in v1 v2 k; do get /tmp/s-$f.txt > $OUT/s-$f.txt; done
get /tmp/kh1 > $OUT/kh-after
get /.ssh/known_hosts > $OUT/kh-final
get /.ssh/id_ed25519.pub > $OUT/id.pub
debugfs -R "stat /.ssh/id_ed25519" $BD/zenix.hdf 2>/dev/null | grep -o 'Mode: *[0-7]*' > $OUT/idmode
debugfs -R "dump /dl/big.bin $OUT/big.got" $BD/zenix.hdf >/dev/null 2>&1
python3 - "$OUT" "$T" <<'PY' || pass=0
import hashlib, os, re, sys
out, T = sys.argv[1], sys.argv[2]
log = open(out + "/s.log", "rb").read().replace(b"\r", b"")
rd = lambda p: open(p, "rb").read() if os.path.exists(p) else b""
main = rd(T + "/main.log"); other = rd(T + "/other.log")
bad = 0
def check(ok, what, detail=""):
    global bad
    print(("PASS " if ok else "FAIL ") + what + ("" if ok else ": " + str(detail)[:300]))
    bad += not ok
pub = rd(out + "/id.pub")
check(b"kg=0" in log and pub.startswith(b"ssh-ed25519 AAAA") and pub.rstrip().endswith(b"amiga-test"),
      "ssh-keygen made root's Ed25519 key (OpenSSH's one-line public key)", pub)
check(b"0600" in rd(out + "/idmode") or b"600" in rd(out + "/idmode"), "... the private key file has mode 600", rd(out + "/idmode"))
check(pub.split()[1] in rd(T + "/home/.ssh/authorized_keys") if pub.split() else False,
      "the first login (password, after yes to the unknown host) appended the Amiga's key to authorized_keys")
check(b"continue connecting" in log and b"Accepted password for root" in main and b"sa=0" in log,
      "trust on first use: asked, yes, then the password login worked")
kh = rd(out + "/kh-after")
check(re.search(rb"^\[10\.0\.2\.2\]:2222 ssh-ed25519 ", kh, re.M) is not None, "known_hosts got [10.0.2.2]:2222's key", kh)
check(b"Accepted publickey for root" in main and b"keylogin-42" in log and b"sb=7" in log,
      "a login with the key made on Zenix: the command ran, its exit status 7 came back")
hb = hashlib.sha256(rd(T + "/home/big.bin")).hexdigest()
check(b"sc=0" in log and hashlib.sha256(rd(out + "/big.got")).hexdigest() == hb and hb.encode() in log,
      "1 MB from the PC through the session, byte for byte (sha256sum on Zenix too)")
v2 = rd(out + "/s-v2.txt")
m = re.search(rb"ssh: (\d+) key exchange", v2)
check(m is not None and int(m.group(1)) >= 2, "... with the server's rekey after 1 MB (RekeyLimit 1M)", m.group(0) if m else v2[-200:])
check(b"HAS CHANGED: refused" in log and b"sd=255" in log and b"Accepted" not in other,
      "a host whose key differs from known_hosts: refused, exit 255, no login")
kh = rd(out + "/kh-after")
check(re.search(rb"^\[10\.0\.2\.2\]:2223 ssh-ed25519 \S+\n\[10\.0\.2\.2\]:2222 ssh-ed25519 ", kh, re.M) is not None,
      "a known_hosts without its last line end: the new key went on a line of its own (audit 2026-10-28 S16)", kh)
f = log[log.find(b"/etc/ssh/ssh_known_hosts; ssh -p 2222"):]
check(b"IS REVOKED: refused" in f and b"\nsf=255" in f and b"ssh-keygen -R" not in f[:f.find(b"\nsf=")],
      "a key the user trusts but root's ssh_known_hosts revokes: refused, and no advice to remove the line (S15)", f[:600])
khf = rd(out + "/kh-final")
check(b"@revoked [10.0.2.2]:2222" in khf and b"sg=0" in log and b"marked line" in log and b"sh=255" in log and
      log.count(b"IS REVOKED: refused") >= 2,
      "ssh-keygen -R keeps the @revoked line (as OpenSSH): the host stays refused (S15)", khf)
check(b"hi-42" in log and b"after-2" in log, "an interactive shell with a terminal: a command, Ctrl-C to a remote sleep")
check(b"Connection to 10.0.2.2 closed." in log and b"se=255" in log, "~. ends the session (exit 255, as OpenSSH)")
k = rd(out + "/s-k.txt").decode(errors="replace")
check("[kernel]" not in k, "no process faulted", k[-300:])
mm = re.search(rb"chip RAM free[^\n]*", log)
ps = re.findall(rb"\n( *\d+ +\d+ +\d+ +-?\d+ +\w+ +\d+ +\d+ ssh)(?=\n)", log)
print("memory while an ssh session is up (2 MB chip RAM, A2065, FS-UAE):", (mm.group(0) if mm else b"?").decode(), "|",
      b" ".join(p.strip() for p in ps).decode(errors="replace"))
for f, n in (("s-v1.txt", "login"), ("s-v2.txt", "1 MB")):
    t = [l for l in rd(out + "/" + f).decode(errors="replace").splitlines() if l.startswith("ssh: [")]
    print("the %s's steps (FS-UAE, an emulator's times): %s" % (n, " | ".join(t[:12])))
sys.exit(1 if bad else 0)
PY
e2fsck -fn $BD/zenix.hdf > $OUT/e2fsck.log 2>&1 && echo "PASS the host's e2fsck finds the root clean" || { echo "FAIL e2fsck"; pass=0; }
exit $((1 - pass))
