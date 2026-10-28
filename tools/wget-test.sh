#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# wget and sntp in FS-UAE (DESIGN D21 10 step 7, the programs): the stock A1200 (68EC020, 2 MB chip RAM) with FS-UAE's
# A2065 (slirp) against the PC's test servers in a user, mount and network namespace (tools/wget-ns.py; unshare
# --map-root-user --map-auto -nm, as tools/host/tls-test.sh): a test CA made at this run (its keys only in the output
# folder, never in git), HTTPS on 10.0.2.2:443 choosing the certificate by SNI, plain HTTP on :80, an SNTP server on
# :123 whose answers a control page on :8080 switches. The image gets the test CA's anchors file as /etc/ssl/anchors
# and the test names in /etc/hosts. A script on the Amiga checks: http; https 1 MB byte for byte with -O; keep-alive
# (two files, one connection) and the NAME.1 rule; a relative redirect and three absolute ones over http; https to
# http refused; -c resuming a cut file (206) byte for byte, a complete file (416), a server ignoring the range (200:
# refused, the file untouched); an unknown root, a wrong name and the owner's root before it is in /etc/ssl/local
# refused (exit 5), then accepted from there; a 404 (exit 8); "the clock is not set" once a run; sntp setting the clock
# and raising the floor, a liar 500 days ahead setting the clock but not the floor, an answer before the floor
# refused, a kiss of death, sntp -r, and the clock set back.
#   tools/wget-test.sh
# Needs openssl, Python's cryptography module, debugfs and unprivileged user namespaces with an /etc/subuid range.
cd "$(dirname "$0")/.." || exit 1
grep() { /usr/bin/grep "$@"; }
BD=${ZENIX_BUILD:-build}
make -s BUILD=$BD all >/dev/null 2>&1 || { echo "FAIL the build"; exit 1; }
OUT=${ZENIX_OUT:-/tmp/zenix-wget}; mkdir -p $OUT
OUT=$(cd "$OUT" && pwd); BD=$(cd "$BD" && pwd)
for t in openssl debugfs e2fsck; do command -v $t >/dev/null || { echo "FAIL $t is not installed"; exit 1; }; done
python3 -c 'import cryptography' 2>/dev/null || { echo "FAIL Python's cryptography module is missing"; exit 1; }
unshare --map-root-user --map-auto -nm true 2>/dev/null || { echo "FAIL unshare --map-root-user --map-auto fails (newuidmap, /etc/subuid)"; exit 1; }
T=$OUT/ns; rm -rf "$T"; mkdir -p "$T"
head -c 64 /dev/urandom > $OUT/seed
rm -f $BD/zenix.hdf; make -s BUILD=$BD $BD/zenix.hdf >/dev/null 2>&1 || { echo "FAIL the image"; exit 1; }
printf 'auto eth0\niface eth0 inet dhcp\n    driver a2065\n' > $OUT/interfaces
printf '127.0.0.1\tlocalhost\n' > $OUT/hosts
for n in rsa.test unknown.test wrong.test local.test; do printf '10.0.2.2\t%s\n' $n >> $OUT/hosts; done
cat > $OUT/wg.sh <<'EOS'
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30; do
    ifconfig eth0 2>/dev/null | grep -q 'lease [0-9]' && break
    sleep 1
done
cat /tests/seed > /dev/random
mkdir /dl; cd /dl
echo ==w1; wget http://rsa.test/small.txt; echo w1=$?; sha256sum small.txt
echo ==w2; wget -O /dl/big.bin https://rsa.test/big.bin; echo w2=$?; sha256sum /dl/big.bin
echo ==w3; wget https://rsa.test/small.txt https://rsa.test/dir/f.bin; echo w3=$?; sha256sum small.txt.1 f.bin
echo ==w4; wget -O rel.bin https://rsa.test/rel/dir/f.bin; echo w4=$?; sha256sum rel.bin
echo ==w5; wget -O redir.txt http://rsa.test/redir/3/small.txt; echo w5=$?; sha256sum redir.txt
echo ==w6; wget -O tohttp.txt https://rsa.test/tohttp/small.txt; echo w6=$?
mkdir /dl/c; cd /dl/c; cp /tests/part.bin big.bin; cp /tests/part.bin nr.bin
echo ==w7; wget -c https://rsa.test/big.bin; echo w7=$?; sha256sum big.bin
echo ==w8; wget -c https://rsa.test/big.bin; echo w8=$?
echo ==w9; wget -c -O nr.bin https://rsa.test/norange/big.bin; echo w9=$?; sha256sum nr.bin
cd /dl
echo ==w10; wget -O u1 https://unknown.test/small.txt; echo w10=$?
echo ==w11; wget -O u2 https://wrong.test/small.txt; echo w11=$?
echo ==w12; wget -O u3 https://local.test/small.txt; echo w12=$?
mkdir /etc/ssl/local; cp /tests/owner.pem /etc/ssl/local/owner.pem
echo ==w13; wget -O u4 https://local.test/small.txt; echo w13=$?; sha256sum u4
echo ==w14; wget https://rsa.test/missing; echo w14=$?; ls missing
echo ==w15; wget -q -O q.txt http://rsa.test/small.txt; echo w15=$?
echo 'server 10.0.2.2' > /etc/sntp
echo ==s1; wget -q -O /dev/null http://10.0.2.2:8080/mode/good; sntp; echo s1=$?; date +%s; cat /var/lib/clock
echo ==s2; wget -q -O /dev/null http://10.0.2.2:8080/mode/liar-future; sntp; echo s2=$?; date +%s; cat /var/lib/clock
echo ==s3; wget -q -O /dev/null http://10.0.2.2:8080/mode/liar-past; sntp; echo s3=$?; date +%s; cat /var/lib/clock
echo ==s4; wget -q -O /dev/null http://10.0.2.2:8080/mode/kod; sntp; echo s4=$?
echo ==s5; sntp -r; echo s5=$?; cat /var/lib/clock
echo ==s6; wget -q -O /dev/null http://10.0.2.2:8080/mode/good; sntp; echo s6=$?; date +%s; cat /var/lib/clock
echo ==s7; sntp -x; echo s7=$?
echo ==mem; free; ps
echo ==end
dmesg > /tmp/wg-k.txt
sync
EOS
cat > $OUT/k.txt <<'EOS'
wait zenix:/# 120
sleep 1
send sh /tests/wg.sh > /tmp/wg.out 2>&1; echo wg-do""ne\r
wait wg-done 2400
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
mkd /etc/network; mkd /etc/ssl; mkd /var; mkd /var/lib
put $OUT/interfaces /etc/network/interfaces 644
put $OUT/hosts /etc/hosts 644
put $T/tls/test.anchors /etc/ssl/anchors 644
put $T/tls/owner.pem /tests/owner.pem 644
head -c 300000 $T/www/big.bin > $OUT/part.bin
put $OUT/part.bin /tests/part.bin 644
put $OUT/seed /tests/seed 600
put $OUT/wg.sh /tests/wg.sh 755
date +%s > $OUT/start-time
ZENIX_BUILD=$BD tools/run-test.sh "$OUT/a1200-a2065.fs-uae" 2700 "$OUT/k.txt"
EOS
chmod +x $OUT/inner.sh
sed '/^\[fs-uae\]/a network_card = a2065' fsuae/a1200-020.fs-uae > "$OUT/a1200-a2065.fs-uae"
unshare --map-root-user --map-auto -nm python3 -I tools/wget-ns.py "$T" -- "$OUT/inner.sh" "$T" "$OUT" "$BD" > $OUT/run.log 2>&1
rc=$?
pass=1
[ $rc = 0 ] || { echo "FAIL the run (exit $rc)"; grep -a 'FAIL\|wget-ns\|Error' $OUT/run.log | head -5; pass=0; }
get() { debugfs -R "cat $1" $BD/zenix.hdf 2>/dev/null | tr -d '\r'; }
get /tmp/wg.out > $OUT/wg.out
get /tmp/wg-k.txt > $OUT/k.out
get /var/lib/clock > $OUT/clock.final
debugfs -R "dump /dl/big.bin $OUT/big.got" $BD/zenix.hdf >/dev/null 2>&1
debugfs -R "dump /dl/c/big.bin $OUT/resumed.got" $BD/zenix.hdf >/dev/null 2>&1
python3 - "$OUT" "$T" "$BD" <<'PY' || pass=0
import hashlib, os, re, sys
out, T, BD = sys.argv[1:4]
rd = lambda p: open(p, "rb").read() if os.path.exists(p) else b""
log = rd(out + "/wg.out").decode(errors="replace")
sha = lambda b: hashlib.sha256(b).hexdigest()
big, small, f = rd(T + "/www/big.bin"), rd(T + "/www/small.txt"), rd(T + "/www/dir/f.bin")
part = big[:300000]
bad = 0
def part_of(name):
    m = re.search(r"^==%s\n(.*?)(?=^==)" % re.escape(name), log, re.S | re.M)
    return m.group(1) if m else ""
def check(ok, what, detail=""):
    global bad
    print(("PASS " if ok else "FAIL ") + what + ("" if ok else ": " + str(detail)[-700:]))
    bad += not ok
def st(name):
    m = re.search(r"^%s=(\d+)$" % name, log, re.M)
    return int(m.group(1)) if m else None
w = part_of("w1")
check(st("w1") == 0 and sha(small) in w and "saved: 1300 bytes" in w, "http: small.txt saved, byte for byte", w)
w = part_of("w2")
check(st("w2") == 0 and sha(big) in w and sha(rd(out + "/big.got")) == sha(big), "https: 1 MB with -O, byte for byte", w)
check(w.count("the clock is not set: certificate dates checked only against") == 1,
      "\"the clock is not set\" said once in a run (no trusted clock yet)", w)
w = part_of("w3")
check(st("w3") == 0 and sha(small) in w and sha(f) in w and w.count("connecting to") == 1 and w.count("TLS handshake") == 1,
      "keep-alive: two files over one TLS connection; an existing small.txt kept, the new one small.txt.1", w)
w = part_of("w4")
check(st("w4") == 0 and sha(f) in w and "302: following to https://rsa.test:443/dir/f.bin" in w,
      "a relative redirect (x/../../dir/f.bin) resolved and followed", w)
w = part_of("w5")
check(st("w5") == 0 and sha(small) in w and w.count("302: following") == 3, "three redirects over http followed", w)
w = part_of("w6")
check(st("w6") == 7 and "never followed" in w, "a redirect from https to http refused (exit 7)", w)
w = part_of("w7")
check(st("w7") == 0 and "206: resuming 'big.bin' at 300000 of 1048576 bytes" in w and sha(big) in w and
      sha(rd(out + "/resumed.got")) == sha(big), "-c: a cut file resumed (206), the whole file byte for byte", w)
w = part_of("w8")
check(st("w8") == 0 and "already complete" in w, "-c: a complete file (416) left alone", w)
w = part_of("w9")
check(st("w9") == 1 and "refusing to truncate" in w and sha(part) in w,
      "-c: a server ignoring the range (200) refused, exit 1, the file untouched", w)
for n, host, why in (("w10", "unknown.test", "not trusted"), ("w11", "wrong.test", "another name"),
                     ("w12", "local.test", "not trusted")):
    w = part_of(n)
    check(st(n) == 5 and why in w, "a bad certificate refused (%s: exit 5, \"%s\")" % (host, why), w)
w = part_of("w13")
check(st("w13") == 0 and sha(small) in w, "the owner's root from /etc/ssl/local/ accepted", w)
w = part_of("w14")
check(st("w14") == 8 and "404" in w and "missing: " in w, "404: exit 8, no file written", w)
w = part_of("w15")
check(st("w15") == 0 and w.strip() == "w15=0", "-q: nothing on standard error", w)
times = [l.split() for l in rd(T + "/sntp.log").decode().splitlines()]
def answered(kind, n=-1):         # the time of the kind's first (0) or last (-1) answer
    t = [int(x[1]) for x in times if x[0] == kind]
    return t[n] if t else None
def date_after(name):
    m = re.search(r"^%s=\d+\n(\d+)\n" % name, log, re.M)
    return int(m.group(1)) if m else None
def clock_after(name):
    m = re.search(r"^zenix-clock 1 (\d+)$", part_of(name), re.M)
    return int(m.group(1)) if m else None
w = part_of("s1"); g = answered("good", 0)
check(st("s1") == 0 and g and date_after("s1") and abs(date_after("s1") - g) <= 20 and clock_after("s1") and
      abs(clock_after("s1") - g) <= 5 and "the clock set from 10.0.2.2" in w,
      "sntp: the clock set from the server, the floor (/var/lib/clock) raised to it", (w, g))
w = part_of("s2"); lf = answered("liar-future")
check(st("s2") == 0 and lf and date_after("s2") and abs(date_after("s2") - lf) <= 20 and clock_after("s2") == clock_after("s1")
      and "the floor stays at" in w, "sntp: a liar 500 days ahead sets the clock, the floor stays", (w, lf))
w = part_of("s3")
check(st("s3") == 1 and "before the floor" in w and date_after("s3") and abs(date_after("s3") - lf) <= 60 and
      clock_after("s3") == clock_after("s1"), "sntp: an answer 20 years back (before the floor) refused, nothing moved", w)
w = part_of("s4")
check(st("s4") == 1 and "kiss of death DENY" in w, "sntp: a kiss of death obeyed (exit 1)", w)
w = part_of("s5")
bdate = int(re.search(r"ZENIX_BUILD_DATE (\d+)", rd(BD + "/builddate.h").decode()).group(1))
check(st("s5") == 0 and clock_after("s5") == bdate and "reset to the build date" in w, "sntp -r: the floor back at the build date", w)
w = part_of("s6"); g = answered("good")
check(st("s6") == 0 and g and date_after("s6") and abs(date_after("s6") - g) <= 20 and clock_after("s6") and
      abs(clock_after("s6") - g) <= 5, "sntp: the clock set back from the true server, the floor raised again", w)
check(st("s7") == 2, "sntp: a wrong option: exit 2", part_of("s7"))
k = rd(out + "/k.out").decode(errors="replace")
check("==end" in log and "[kernel]" not in k, "the script ran to its end, no process faulted", k[-300:])
m = re.search(r"^==mem\n(.*?)(?=^==)", log, re.S | re.M)
print("memory after the run (2 MB chip RAM, A2065, FS-UAE):", (m.group(1).splitlines()[:3] if m else "?"))
t = [l for l in part_of("w2").splitlines() if "saved" in l]
print("the 1 MB https fetch (FS-UAE's emulated 68EC020, an emulator's time):", t)
sys.exit(1 if bad else 0)
PY
e2fsck -fn $BD/zenix.hdf > $OUT/e2fsck.log 2>&1 && echo "PASS the host's e2fsck finds the root clean" || { echo "FAIL e2fsck"; pass=0; }
exit $((1 - pass))
