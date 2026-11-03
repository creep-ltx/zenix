#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# zpkg in FS-UAE (DESIGN D22 8.2, step 5, without the system path): a test repository (the test channel, signed with
# the TEST key, tools/mkrepo.py) made from this build (version V1) and a second build (V2: zenix-tests with a /bin/hello
# that says HELLO, and two new packages, extra needing extralib), served by tools/host/http-test-server.py in a user,
# mount and network namespace (the Amiga reaches it as 10.0.2.2), its prefixes giving a damaged mirror (/damaged/)
# and one that cuts the first answer to each file (/cut/), beside the first state of the repository (/r1: behind).
#   tools/zpkg-emu-test.sh [PART...]
#     e   the stock A1200 (68EC020, 2 MB chip RAM) with FS-UAE's A2065: -Q, -Sy, -Qu, -Ss, -Si, -S with a dependency,
#         -Qo, -Ql, the deps file, -Syu replacing a program another process runs (named), free during it, --back,
#         -Rns, a mirror behind (replay) skipped, a damaged mirror refused and the next one used, a download cut and
#         resumed, an expired list refused (the clock floor ahead) and --ignore-expiry, -U with and without a valid
#         .sig, the refusals (-R base, -S of a base package, a non-root user); e2fsck after
#     s   the same over SLIP (zenix-slirp on the serial cable): -Sy, -S extra, -Q, -Rns, free
#     m   the same on 1 MB of chip RAM (SLIP: the A2065's buffers leave the 1 MB machine too little), free during -S
# Needs debugfs, e2fsck, openssl, unprivileged user namespaces (wget-test's), libslirp (part s, as slip-test).
cd "$(dirname "$0")/.." || exit 1
grep() { /usr/bin/grep "$@"; }
BD=${ZENIX_BUILD:-build}
OUT=${ZENIX_OUT:-/tmp/zenix-zpkg}; mkdir -p "$OUT"
make -s BUILD=$BD all >"$OUT/make.log" 2>&1 || { tail "$OUT/make.log"; echo "FAIL the build"; exit 1; }
OUT=$(cd "$OUT" && pwd); BD=$(cd "$BD" && pwd)
for t in openssl debugfs e2fsck; do command -v $t >/dev/null || { echo "FAIL $t is not installed"; exit 1; }; done
unshare --map-root-user --map-auto -nm true 2>/dev/null || { echo "FAIL unshare --map-root-user --map-auto fails (newuidmap, /etc/subuid)"; exit 1; }
pass=1
V1=$(sed -n 's/^#define ZENIX_VNUM \([0-9]*\)u$/\1/p' kernel/version.h); V2=$((V1 + 1))
KEY=build/sign-test/zenix-test.key
python3 tools/zsign.py testkey >/dev/null || { echo "FAIL the test key"; exit 1; }

# ---- the repository: r1 (this build), then r (and the second build) ----
# the second build: this one's files with version V2 (the kernel's ZVER number and every manifest's version line),
# so that only what really changed is new to mkrepo (3.4): the system stays, zenix-tests changes
rm -rf $OUT/b2; mkdir -p $OUT/b2; cp -a $BD/pkg $OUT/b2/
python3 -I - $BD/kernel.bin $OUT/b2/kernel.bin $V1 $V2 $OUT/b2/pkg <<'PYEOF' || { echo "FAIL the second build"; exit 1; }
import sys, struct, os, re
src, dst, v1, v2, pk = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
d = bytearray(open(src, "rb").read())
i = d.find(b"ZVER" + struct.pack(">I", v1))
assert i >= 0
d[i + 4:i + 8] = struct.pack(">I", v2)
open(dst, "wb").write(d)
for f in os.listdir(pk):
    if f.endswith(".manifest"):
        t = open(os.path.join(pk, f)).read()
        t = re.sub(r"^version %d " % v1, "version %d " % v2, t, flags=re.M)
        t = re.sub(r"^depends (\S+) %d$" % v1, r"depends \1 %d" % v2, t, flags=re.M)
        open(os.path.join(pk, f), "w").write(t)
PYEOF
python3 -c 'import sys; d = open(sys.argv[1], "rb").read(); assert d.count(b"hello from pid") == 1
open(sys.argv[2], "wb").write(d.replace(b"hello from pid", b"HELLO from pid"))' $BD/hello.zx $OUT/hello2
python3 tools/mkzup.py repack $OUT/b2/pkg zenix-tests /bin/hello=$OUT/hello2 >/dev/null || { echo "FAIL repack"; exit 1; }
head -c 300000 /dev/urandom >$OUT/extradata; printf 'extra: a test package\n' >$OUT/extrareadme
python3 tools/mkzup.py pkg $OUT/b2/pkg extralib $V2 0.2 --dep base:$V2 --desc "a test library for zpkg-test" \
    d:755:0:0:/usr d:755:0:0:/usr/share d:755:0:0:/usr/share/extralib f:644:0:0:$OUT/extradata:/usr/share/extralib/data >/dev/null &&
python3 tools/mkzup.py pkg $OUT/b2/pkg extra $V2 0.2 --dep base:$V2 --dep extralib:$V2 --desc "a test program for zpkg-test" \
    d:755:0:0:/usr d:755:0:0:/usr/share d:755:0:0:/usr/share/extra f:644:0:0:$OUT/extrareadme:/usr/share/extra/readme \
    c:644:0:0:$OUT/extrareadme:/etc/extra.conf >/dev/null || { echo "FAIL the extra packages"; exit 1; }
printf '%s extralib extra\n' "$(cat $OUT/b2/pkg/list)" >$OUT/b2/pkg/list
W=$OUT/www; rm -rf $W; mkdir -p $W
today=$(date +%Y%m%d)
python3 tools/mkrepo.py add $W/r1 test $BD --key $KEY --date $today >$OUT/repo.log 2>&1 &&
    cp -a $W/r1 $W/r && python3 tools/mkrepo.py add $W/r test $OUT/b2 --key $KEY --date $today >>$OUT/repo.log 2>&1 &&
    python3 tools/mkrepo.py check $W/r --test-pub build/sign-test/zenix-test.pub >>$OUT/repo.log 2>&1 || { cat $OUT/repo.log; echo "FAIL the repository"; exit 1; }
ZT=$(ls $W/r/pool | grep '^zenix-tests-.*\.zup$' | sort | head -1); ZTV=${ZT#zenix-tests-}; ZTV=${ZTV%.zup}
EXTRA=$(ls $W/r/pool | grep '^extra-.*\.zup$');
# a key this build does not know (made for the test, never kept): its signature over the extra package
openssl genpkey -algorithm ed25519 -out $OUT/foreign.key 2>/dev/null && cp $W/r/pool/$EXTRA $OUT/foreign.zup &&
    python3 tools/zsign.py sign --key $OUT/foreign.key $OUT/foreign.zup >/dev/null || { echo "FAIL a foreign signature"; exit 1; }
echo "zpkg-test: the repository: $(grep -c '^pkg ' $W/r/test/index-* | tail -1) packages; zenix-tests $ZTV (installed) and $(ls $W/r/pool | grep '^zenix-tests-.*\.zup$' | sort | tail -1) (new)"

get() { debugfs -R "cat $1" $IMG 2>/dev/null | tr -d '\r'; }
put() {     # put FILE PATH MODE
    debugfs -w -R "rm $2" $IMG >/dev/null 2>&1
    debugfs -w -R "write $1 $2" $IMG >/dev/null 2>&1
    debugfs -w -R "sif $2 uid 0" $IMG >/dev/null 2>&1
    debugfs -w -R "sif $2 gid 0" $IMG >/dev/null 2>&1
    debugfs -w -R "sif $2 mode 0100$3" $IMG >/dev/null 2>&1
}
mkd() { debugfs -w -R "mkdir $1" $IMG >/dev/null 2>&1; debugfs -w -R "sif $1 uid 0" $IMG >/dev/null 2>&1; debugfs -w -R "sif $1 mode 040755" $IMG >/dev/null 2>&1; }
image() {   # IMG: this build's root, the network (A2065 or SLIP), the script, the cache with zenix-tests's V1 file
    IMG=$1
    cp $BD/zenix.hdf $IMG
    mkd /etc/network; mkd /var/cache/zpkg/pkg; mkd /tests
    put $OUT/interfaces /etc/network/interfaces 644
    put $W/r1/pool/$ZT /var/cache/zpkg/pkg/$ZT 644
    put $W/r1/pool/$ZT.sig /var/cache/zpkg/pkg/$ZT.sig 644
    put $W/r/pool/$EXTRA /tests/$EXTRA 644
    put $W/r/pool/$EXTRA.sig /tests/$EXTRA.sig 644
    head -c 80 /dev/zero >$OUT/badsig; put $W/r/pool/$EXTRA /tests/bad.zup 644; put $OUT/badsig /tests/bad.zup.sig 644
    # the cable's path (DESIGN D21 6.5, D22 7.6: rz receives the files, zpkg -U checks them): one without a .sig, and
    # one signed by a key this build does not know
    put $W/r/pool/$EXTRA /tests/nosig.zup 644
    put $W/r/pool/$EXTRA /tests/foreign.zup 644; put $OUT/foreign.zup.sig /tests/foreign.zup.sig 644
    put $OUT/zp.sh /tests/zp.sh 755
}
conf() {    # the conf files the script switches between: SERVER... (the port is the test server's)
    printf 'channel test\n'; for s in "$@"; do printf 'server http://10.0.2.2:%s/%s\n' "$PORT" "$s"; done
}

# ---- the script on the Amiga ----
cat >$OUT/zp-e.sh <<'EOS'
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30; do
    ifconfig 2>/dev/null | grep -q 'lease [0-9]\|inet 10.0.2.15' && break
    sleep 1
done
cp /tests/conf.good /etc/zpkg.conf
echo ==q1; zpkg -Q; echo q1=$?
echo ==sy; zpkg -Sy; echo sy=$?
echo ==sy2; zpkg -Sy; echo sy2=$?
echo ==qu; zpkg -Qu; echo qu=$?
echo ==ss; zpkg -Ss extra; echo ss=$?
echo ==si; zpkg -Si extra; echo si=$?
echo ==sl; zpkg -Sl; echo sl=$?
echo ==sln; zpkg -Sln; echo sln=$?
echo ==s1; zpkg -S extra; echo s1=$?; zpkg -Q extra extralib; cat /var/lib/zpkg/deps; cat /usr/share/extra/readme
echo ==qo; zpkg -Qo /usr/share/extra/readme /bin/ls /nothing; echo qo=$?
echo ==ql; zpkg -Ql extra; echo ql=$?
echo ==syu; spin 1000000 > /dev/null &
sh -c 'sleep 6; free' > /tmp/free-during.txt &
zpkg -Syu; echo syu=$?; hello | head -1; zpkg -Q zenix-tests; ps | grep -c spin; kill %1
echo ==back; zpkg --back zenix-tests --yes; echo back=$?; hello | head -1; zpkg -Q zenix-tests
echo ==rns; zpkg -Rns extra; echo rns=$?; zpkg -Q extralib; echo q=$?; ls /usr/share/extra; cat /var/lib/zpkg/deps
echo ==sc; zpkg -Sc; echo sc=$?; ls /var/cache/zpkg/pkg
echo ==replay; cp /tests/conf.behind /etc/zpkg.conf; zpkg -Sy; echo replay=$?
echo ==tamper; cp /tests/conf.damaged /etc/zpkg.conf; zpkg -S extra; echo tamper=$?; zpkg -Q extra; zpkg -Rns extra; zpkg -Sc
echo ==cut; cp /tests/conf.cut /etc/zpkg.conf
for n in 1 2 3 4; do zpkg -S extra; r=$?; echo cut$n=$r; [ $r = 0 ] && break; done
zpkg -Q extra; zpkg -Rns extra
echo ==exp; cp /tests/conf.good /etc/zpkg.conf; cp /var/lib/clock /tmp/clock.save 2>/dev/null; echo 'zenix-clock 1 1830297600' > /var/lib/clock
zpkg -Syu; echo exp=$?; zpkg -Syu --ignore-expiry; echo expi=$?; rm /var/lib/clock; cp /tmp/clock.save /var/lib/clock 2>/dev/null
echo ==u; zpkg -S extralib; zpkg -U /tests/bad.zup; echo ubad=$?; zpkg -U /tests/nosig.zup; echo unosig=$?; zpkg -U /tests/foreign.zup; echo uforeign=$?; zpkg -U tests/EXTRAFILE; echo urel=$?; zpkg -U /tests/EXTRAFILE; echo u=$?; zpkg -Q extra
echo ==ref; zpkg -R base; echo rbase=$?; zpkg -S zup; echo szup=$?; zpkg -R extralib; echo rlib=$?; echo 'zpkg -Sy; echo ur=$?' | su user 2>&1; zpkg -X; echo bad=$?
echo ==mem; free; cat /tmp/free-during.txt
echo ==end
dmesg > /tmp/zp-k.txt
sync
EOS
sed -i "s/EXTRAFILE/$EXTRA/g" $OUT/zp-e.sh

cat >$OUT/check.py <<'PY'
import os, re, sys
out, part = sys.argv[1], sys.argv[2]
log = open(os.path.join(out, part + ".out"), "rb").read().decode(errors="replace")
k = open(os.path.join(out, part + "-k.out"), "rb").read().decode(errors="replace") if os.path.exists(os.path.join(out, part + "-k.out")) else ""
bad = 0
def sec(name):
    m = re.search(r"^==%s\n(.*?)(?=^==)" % re.escape(name), log, re.S | re.M)
    return m.group(1) if m else ""
def st(name):
    m = re.search(r"^%s=(\d+)$" % name, log, re.M)
    return int(m.group(1)) if m else None
def check(ok, what, detail=""):
    global bad
    print(("PASS " if ok else "FAIL ") + part + ": " + what + ("" if ok else ": " + str(detail)[-900:]))
    bad += not ok
full = part == "e"
w = sec("sy")
if part == "m" and ("KB of memory are free" in w or "zpkg: out of memory" in w):
    # the 1 MB machine with SLIP up has about 100 KB left (netd and the login); zpkg must say so and change nothing
    # (since D22 amendment 2's boot partition writer, 2026-10-31, zpkg itself (128 KB to load) may not fit: then the
    # shell says "zpkg: out of memory", also nothing changed)
    check(st("sy") in (1, 126, 127) and (re.search(r"only \d+ KB of memory are free", w) or "zpkg: out of memory" in w) and
          "extra: a test package" not in sec("s1"),
          "1 MB of chip RAM: too little memory left with the network up: zpkg (or the shell) says so and nothing changes", w)
    m = re.search(r"only (\d+) KB", w)
    print("zpkg-test m: free memory with SLIP up and one login on 1 MB of chip RAM: %s KB (zpkg asks for 160)" % (m.group(1) if m else "?"))
    print("zpkg-test m: " + " ".join(l.strip() for l in sec("mem").splitlines()[:1]))
    sys.exit(1 if bad else 0)
check(st("sy") == 0 and "a new package list" in w, "-Sy: the signed list fetched, checked and stored", w)
if full:
    w = sec("q1"); check(st("q1") == 0 and "zenix-tests" in w and "coreutils" in w and "base" in w, "-Q: the image's packages", w)
    w = sec("sy2"); check(st("sy2") == 0 and "up to date" in w, "-Sy again: latest only, the stored list used", w)
    w = sec("qu"); check(st("qu") == 0 and "zenix-tests" in w, "-Qu: zenix-tests has a newer version", w)
    w = sec("ss"); check(st("ss") == 0 and "extralib" in w and "a test program for zpkg-test" in w, "-Ss: names and descriptions", w)
    w = sec("sl"); check(st("sl") == 0 and re.search(r"^extra +\S+$", w, re.M) and re.search(r"^coreutils +\S+ \[installed", w, re.M), "-Sl: every package, the installed marked", w)
    w = sec("sln"); check(st("sln") == 0 and re.search(r"^extra ", w, re.M) and not re.search(r"^coreutils ", w, re.M), "-Sln: only those not installed", w)
    w = sec("si"); check(st("si") == 0 and "Depends on  : base>=" in w and "extralib>=" in w, "-Si: the block in words", w)
w = sec("s1")
check(st("s1") == 0 and re.search(r"^extra ", w, re.M) and re.search(r"^extralib ", w, re.M) and "extra: a test package" in w and
      re.search(r"^extralib$", w, re.M) and not re.search(r"^extra$", w, re.M), "-S extra: extralib too (dependencies first), the deps file names extralib only", w)
if full:
    w = sec("qo"); check(st("qo") == 1 and "/usr/share/extra/readme is owned by extra" in w and "/bin/ls is owned by coreutils" in w and
                         "no package owns /nothing" in w, "-Qo: owners (and one path nobody owns: exit 1)", w)
    w = sec("ql"); check(st("ql") == 0 and "extra /usr/share/extra/readme" in w and "extra /etc/extra.conf" in w, "-Ql: the files", w)
    w = sec("syu")
    check(st("syu") == 0 and "HELLO from pid" in w and "spin (pid" in w and "still runs the old version" in w,
          "-Syu: zenix-tests updated (hello says HELLO), spin named as still running the old version", w)
    w = sec("back")
    check(st("back") == 0 and re.search(r"^hello from pid", w, re.M) and "go back to" in w, "--back: the cached older zenix-tests put back", w)
    w = sec("rns")
    check(st("rns") == 0 and st("q") == 1 and "extralib is not installed" in w and "/usr/share/extra: not found" in w and "2 packages removed" in w,
          "-Rns: extra and the dependency only it needed removed; the deps file emptied", w)
    w = sec("sc"); check(st("sc") == 0 and "extra-" not in w, "-Sc: the cache down to the installed versions and the ones before", w)
    w = sec("replay"); check(st("replay") == 0 and "this mirror is behind" in w and "the stored one is used" in w,
                             "a mirror behind (an older signed list, a replay) skipped; the stored list stays", w)
    w = sec("tamper")
    check(st("tamper") == 0 and "its size or SHA-256 is not the list's" in w and re.search(r"^extra ", w, re.M),
          "a damaged mirror: its file refused, the next mirror's used", w)
    w = sec("cut")
    check("resuming at" in w and re.search(r"^extra ", w, re.M) and re.search(r"cut\d=0", w), "a download cut and resumed (Range), then installed", w)
    w = sec("exp")
    check(st("exp") == 1 and "the package list expired on" in w and st("expi") == 0, "an expired list refused (the clock floor 2028), --ignore-expiry goes on once", w)
    w = sec("u")
    check(st("ubad") == 1 and "the signature does not verify" in w and st("urel") == 1 and st("u") == 0 and re.search(r"^extra ", w, re.M),
          "-U: a bad .sig refused, a relative name refused, a signed file installed", w)
    check(st("unosig") == 1 and "no signature (" in w and st("uforeign") == 1 and w.count("the signature does not verify") >= 2,
          "-U (the cable's path): a file without a .sig and one signed by a key this build does not know refused", w)
    w = sec("ref")
    check(st("rbase") == 1 and st("szup") == 1 and "part of the system" in w and st("rlib") == 1 and "is needed by an installed package (extra)" in w and
          "only root changes packages" in w and re.search(r"^ur=1$", w, re.M) and st("bad") == 2, "refused: -R base, -S zup (the system), -R a dependency, a user, a wrong option", w)
w = sec("mem")
check("[kernel]" not in k and "==end" in log, "the script ran to its end, no process faulted", k[-400:])
print("zpkg-test %s: free after: %s; during -Syu / -S: %s" % (part, [l for l in w.splitlines() if "free" in l.lower()][:1],
      [l for l in w.splitlines()[3:] if "free" in l.lower()][:1]))
sys.exit(1 if bad else 0)
PY

# ---- e: the A2065 ----
run_a2065() {   # PART CONF SCRIPT
    local part=$1 fsconf=$2
    printf 'auto eth0\niface eth0 inet dhcp\n    driver a2065\n' >$OUT/interfaces
    cp $3 $OUT/zp.sh
    image $OUT/$part.img
    cat >$OUT/k-$part.txt <<'EOS'
wait zenix:/# 120
sleep 1
send sh /tests/zp.sh > /tmp/zp.out 2>&1; echo zp-do""ne\r
wait zp-done 3000
send reboot\r
wait klog: boot 2 120
wait zenix:/# 120
stop
EOS
    cat >$OUT/inner-$part.sh <<EOS
#!/usr/bin/env bash
ip link set lo up
python3 -I tools/host/http-test-server.py $W $OUT/port-$part & srv=\$!
for i in \$(seq 50); do [ -s $OUT/port-$part ] && break; sleep 0.1; done
PORT=\$(head -1 $OUT/port-$part)
IMG=$OUT/$part.img
$(declare -f put conf)
PORT=\$PORT; W=$W; OUT=$OUT
conf r >$OUT/conf.good; conf r1 >$OUT/conf.behind; conf damaged/r r >$OUT/conf.damaged; conf cut/r >$OUT/conf.cut
for c in good behind damaged cut; do put $OUT/conf.\$c /tests/conf.\$c 644; done
sed "s|^hard_drive_0 = .*|hard_drive_0 = \$IMG|" $fsconf >$OUT/$part.fs-uae
ZENIX_BUILD=$BD tools/run-test.sh "$OUT/$part.fs-uae" 3300 "$OUT/k-$part.txt"
rc=\$?
kill \$srv
exit \$rc
EOS
    chmod +x $OUT/inner-$part.sh
    rm -f $OUT/port-$part
    unshare --map-root-user --map-auto -nm $OUT/inner-$part.sh >$OUT/run-$part.log 2>&1 || { echo "FAIL $part: the run ($(tail -2 $OUT/run-$part.log | tr '\n' ' '))"; pass=0; }
    IMG=$OUT/$part.img
    get /tmp/zp.out >$OUT/$part.out; get /tmp/zp-k.txt >$OUT/$part-k.out
    python3 -I $OUT/check.py $OUT $part || pass=0
    e2fsck -fn $IMG >$OUT/e2fsck-$part.log 2>&1 && echo "PASS $part: the host's e2fsck finds the root clean" || { echo "FAIL $part: e2fsck"; pass=0; }
}
part_e() { sed '/^\[fs-uae\]/a network_card = a2065' fsuae/a1200-020.fs-uae >$OUT/a2065.fs-uae; run_a2065 e $OUT/a2065.fs-uae $OUT/zp-e.sh; }

# ---- s: SLIP (zenix-slirp on the serial cable, as tools/slip-test.sh) ----
part_s() { run_slip s fsuae/a1200-020.fs-uae; }
part_m() { run_slip m fsuae/matrix/a1200-chip1m.fs-uae; }
run_slip() {    # PART CONFIG: over SLIP (on 1 MB of chip RAM the A2065's buffers leave too little: SLIP it is, D19 7)
    local part=$1 fsconf=$2
    pkg-config --exists slirp || { echo "FAIL $part: libslirp (with GLib) is not installed"; pass=0; return; }
    make -s -C tools/host/zenix-slirp OUT="$OUT" >/dev/null || { echo "FAIL $part: zenix-slirp does not build"; pass=0; return; }
    local SERIAL=${ZENIX_SERIAL:-/tmp/zenix-zpkg-serial-$part}
    printf 'auto sl0\niface sl0 inet static\n    driver slip /dev/ttyS0 115200\n    address 10.0.2.15/24\n    gateway 10.0.2.2\n' >$OUT/interfaces
    sed -n '/^for i in/,/^done/p' $OUT/zp-e.sh >$OUT/zp.sh
    cat >>$OUT/zp.sh <<'EOS'
cp /tests/conf.good /etc/zpkg.conf
echo ==sy; zpkg -Sy; echo sy=$?
echo ==s1; sh -c 'sleep 10; free' > /tmp/free-during.txt &
zpkg -S extra; echo s1=$?; zpkg -Q extra extralib; cat /var/lib/zpkg/deps; cat /usr/share/extra/readme
echo ==rns; zpkg -Rns extra; echo rns=$?
echo ==mem; free; cat /tmp/free-during.txt
echo ==end
dmesg > /tmp/zp-k.txt
ifdown sl0
sync
EOS
    image $OUT/$part.img
    printf 'if [ ! -e /tmp/zp-ran ]; then\n  touch /tmp/zp-ran\n  sh /tests/zp.sh > /tmp/zp.out 2>&1\n  echo zpkg-test-""done\nfi\n' >$OUT/profile
    put $OUT/profile /etc/profile 644
    rm -f $OUT/console.log $OUT/ready "$SERIAL" $OUT/port-$part $OUT/port-$part.pid
    echo "nameserver 127.0.0.1" >$OUT/resolv.conf
    unshare -rnm bash -c 'ip link set lo up && mount --bind "$0" /etc/resolv.conf && { python3 -I tools/host/http-test-server.py "$1" "$2" & echo $! >"$2.pid"; } &&
        shift 2 && exec python3 -I "$@"' "$OUT/resolv.conf" "$W" "$OUT/port-$part" "$PWD/tools/slip-host.py" "$OUT/zenix-slirp" \
        "$SERIAL" "$OUT/console.log" "$OUT/ready" >$OUT/host-$part.log 2>&1 &
    local host=$!
    for _ in $(seq 100); do [ -e $OUT/ready ] && [ -s $OUT/port-$part ] && break; sleep 0.1; done
    PORT=$(head -1 $OUT/port-$part 2>/dev/null)
    [ -n "$PORT" ] || { echo "FAIL $part: the PC's side did not start: $(cat $OUT/host-$part.log)"; pass=0; kill $host; return; }
    conf r >$OUT/conf.good; put $OUT/conf.good /tests/conf.good 644
    ( for _ in $(seq 2400); do grep -aq 'zpkg-test-done' $OUT/console.log 2>/dev/null && break; sleep 1; done; sleep 3; touch "$SERIAL.stop" ) &
    local watch=$!
    sed "s|^hard_drive_0 = .*|hard_drive_0 = $IMG|" $fsconf >$OUT/$part.fs-uae
    ZENIX_NOTAP=1 ZENIX_SERIAL=$SERIAL ZENIX_BUILD=$BD tools/run-test.sh $OUT/$part.fs-uae 2500 >$OUT/run-$part.log 2>&1
    kill $watch $host 2>/dev/null; wait $host 2>/dev/null
    [ -s $OUT/port-$part.pid ] && kill "$(cat $OUT/port-$part.pid)" 2>/dev/null
    rm -f "$SERIAL.stop"
    get /tmp/zp.out >$OUT/$part.out; get /tmp/zp-k.txt >$OUT/$part-k.out
    python3 -I $OUT/check.py $OUT $part || pass=0
    e2fsck -fn $IMG >$OUT/e2fsck-$part.log 2>&1 && echo "PASS $part: the host's e2fsck finds the root clean" || { echo "FAIL $part: e2fsck"; pass=0; }
}

[ $# = 0 ] && set -- e s m
for p in "$@"; do
    case $p in e|s|m) "part_$p" ;; *) echo "zpkg-emu-test: unknown part $p"; exit 2 ;; esac
done
exit $((1 - pass))
