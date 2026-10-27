#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Host tests of the updates (DESIGN D15, docs/d15-self-updates.md section 11):
#   1. the bundle format and the packages (tools/mkzup.py): the standard packages hold what their manifests say, a
#      bundle checks out, and every damaged one (header, table of contents, a part, a range, cut short) is refused;
#      the kernel in a bundle must carry the bundle's version; the full root image holds the package database;
#   2. zup built for the PC (tools/host/zup-host.c serves its system calls from a directory): a first install, "already
#      installed" does nothing, a wrong checksum, a truncated bundle, an older version, a missing dependency, a
#      kernel older than the base and too little space all refused before any change; an update with zup itself first,
#      the three-way compare of config files, removed files and links, the order of each file's steps; a power cut
#      before every single change, then zup again: finished or rolled back, never anything between; "Zenix
#      (previous)" and the version comparison put the previous version back, also when cut at every step.
# usage: tools/zup-host-test.sh
cd "$(dirname "$0")/.." || exit 1
T=$(mktemp -d /tmp/zup-host.XXXXXX)
[ -n "$ZUP_KEEP" ] || trap 'rm -rf $T' EXIT
pass=0; fail=0
check() { if [ "$2" = ok ]; then echo "PASS $1"; pass=$((pass+1)); else echo "FAIL $1"; fail=$((fail+1)); fi; }
B=${ZENIX_BUILD:-build}
make -s BUILD=$B packages bundle $B/zenix.hdf >$T/make.log 2>&1 || { cat $T/make.log; echo "FAIL build"; exit 1; }

# ---- 1. the bundle format and the packages ----
python3 tools/mkzup.py check $B/zenix-update.zup >$T/out 2>&1 && check "the bundle of the build checks out" ok || { check "the bundle of the build checks out" bad; cat $T/out; }
python3 -I - "$B" >$T/out 2>&1 <<'EOF' && check "every package's tar holds its manifest's files, in order, with their SHA-256, size and mode" ok || { check "every package's tar holds its manifest's files" bad; cat $T/out; }
import sys, os, gzip, tarfile, io, hashlib
sys.path.insert(0, "tools"); import mkzup
b = sys.argv[1]
for n in open(os.path.join(b, "pkg", "list")).read().split():
    m = mkzup.parse_manifest(open(os.path.join(b, "pkg", n + ".manifest"), "rb").read())
    assert m["name"] == n and m["vnum"] > 0
    tar = tarfile.open(fileobj=io.BytesIO(gzip.decompress(open(os.path.join(b, "pkg", n + ".tgz"), "rb").read())))
    members = tar.getmembers()
    files = [e for e in m["entries"] if e[0] in "fc"]
    assert len(members) == len(files), (n, len(members), len(files))
    for mem, e in zip(members, files):
        data = tar.extractfile(mem).read()
        assert "/" + mem.name == e[6], (mem.name, e[6])
        assert hashlib.sha256(data).hexdigest() == e[4] and len(data) == e[5] and mem.mode == e[1], mem.name
print("ok")
EOF
grep -q '^c 644 0 0 [0-9a-f]* [0-9]* /etc/passwd$' $B/pkg/base.manifest && ! grep -q 'autologin' $B/pkg/*.manifest &&
    grep -q '^f 4755 0 0 .* /bin/su$' $B/pkg/utils.manifest && grep -q '^l less /bin/more$' $B/pkg/utils.manifest &&
    check "base: /etc/passwd is a config file, /etc/autologin is never shipped; su is set-user-id; more is a link" ok ||
    check "base: /etc/passwd is a config file, /etc/autologin is never shipped; su is set-user-id; more is a link" bad
# packages are reproducible: the same build gives the same bytes (mtime 0, no names in the gzip header)
mkdir -p $T/r2 $T/r3
cp $B/init.zx $B/login.zx $B/sh.zx $B/mount.zx $B/zfsd.zx $T/r2/ && cp $B/init.zx $B/login.zx $B/sh.zx $B/mount.zx $B/zfsd.zx $T/r3/   # base (and /sbin/zfsd, D19)
python3 tools/mkzup.py packages $T/r2 "init login sh mount" >/dev/null && sleep 1 && python3 tools/mkzup.py packages $T/r3 "init login sh mount" >/dev/null &&
    cmp -s $T/r2/pkg/base.tgz $T/r3/pkg/base.tgz && cmp -s $T/r2/pkg/base.manifest $T/r3/pkg/base.manifest &&
    check "packages are reproducible (the same files give the same bytes)" ok || check "packages are reproducible" bad

# damaged bundles: each must be refused, with the reason
python3 -I - "$B/zenix-update.zup" "$T" <<'EOF'
import sys, struct, hashlib
src, t = sys.argv[1], sys.argv[2]
d = bytearray(open(src, "rb").read())
def put(name, b): open("%s/%s.zup" % (t, name), "wb").write(b)
def fix_toc(b):          # make the TOC's own SHA right again, so that only the range check can catch the damage
    n = struct.unpack(">I", b[52:56])[0]
    b[64:96] = hashlib.sha256(bytes(b[:64]) + bytes(b[96:96 + 80 * n])).digest()
    return b
b = bytearray(d); b[0] = ord('X'); put("magic", b)
b = bytearray(d); b[13] ^= 1; put("version", b)                       # covered by the TOC's SHA-256
b = bytearray(d); b[96 + 10] ^= 1; put("tocname", b)
b = bytearray(d); off = struct.unpack(">I", d[96 + 40:96 + 44])[0]; b[off + 100] ^= 0x40; put("part", b)
put("short", d[:-1])
put("long", d + b"\0" * 16)
put("header", d[:90])
b = bytearray(d); b[52:56] = struct.pack(">I", 100000); put("ntoc", fix_toc(b))
b = bytearray(d); b[96 + 40:96 + 44] = struct.pack(">I", len(d) - 4); put("range", fix_toc(b))
b = bytearray(d); b[96 + 44:96 + 48] = struct.pack(">I", 0xFFFFFFF0); put("wrap", fix_toc(b))
b = bytearray(d); b[96:96 + 40] = b"pkg/../../etc".ljust(40, b"\0"); put("dotdot", fix_toc(b))
b = bytearray(d); b[96:96 + 40] = b"x" * 40; put("noterm", fix_toc(b))
EOF
for v in magic:"not a ZUP1" version:"table of contents' SHA-256" tocname:"table of contents' SHA-256" part:"boot/zenix: SHA-256" \
         short:"the size is" long:"the size is" header:"shorter than the header" ntoc:"bad table of contents" \
         range:"outside the bundle" wrap:"outside the bundle" dotdot:"a bad name" noterm:"not terminated"; do
    n=${v%%:*}; want=${v#*:}
    python3 tools/mkzup.py check $T/$n.zup >$T/out 2>&1; rc=$?
    [ $rc = 1 ] && grep -q "refused: .*$want" $T/out && check "a damaged bundle ($n) is refused: '$want'" ok || { check "a damaged bundle ($n) is refused: '$want'" bad; cat $T/out; }
done
python3 tools/mkzup.py bundle $T/wrongv.zup --kernel $B/kernel.bin --version 2026102699 $B/pkg base >$T/out 2>&1
[ $? = 1 ] && grep -q "the kernel carries version" $T/out && check "a bundle whose kernel carries another version is refused" ok || { check "a bundle whose kernel carries another version is refused" bad; cat $T/out; }
debugfs -R "ls /var/lib/zpkg" $B/zenix.hdf 2>/dev/null | tr -s ' ' '\n' >$T/db
grep -qx base.manifest $T/db && grep -qx utils.manifest $T/db && grep -qx man.manifest $T/db &&
    check "the full root image holds the package database (/var/lib/zpkg)" ok || { check "the full root image holds the package database" bad; cat $T/db; }

# ---- 2. zup on the PC ----
U=$T/u; mkdir -p $U
gcc -m32 -DZHOST -std=gnu11 -O1 -g -Wall -Wextra -Wno-format-truncation -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Iuser/miniz -o $U/zup user/zup.c user/sha256.c user/miniz/miniz_tinfl.c tools/host/zup-host.c 2>$U/cc.log &&
    ! grep -q 'warning' $U/cc.log && check "zup builds for the PC without a warning" ok || { check "zup builds for the PC without a warning" bad; cat $U/cc.log; }
export ZUP_SELF=$U/zup ASAN_OPTIONS=detect_leaks=0
V0=2026102500; V1=2026102600; V2=2026102701
S=$U/src; mkdir -p $S
for f in zup1 zup2 init sh1 sh2 ls1 ls2 old1 new2 su; do printf '%s contents\n' $f >$S/$f; done
head -c 300000 /dev/urandom >$S/big2                                   # several tar blocks and inflate windows
printf 'root::0:0:root:/root:/bin/sh\n' >$S/passwd1
printf 'root::0:0:root:/root:/bin/sh\nnews::9:9::/var/news:/bin/sh\n' >$S/passwd2
printf 'motd v1\n' >$S/motd1; printf 'motd v2\n' >$S/motd2; printf 'root::0:\n' >$S/group1; printf 'root::0:\nusers::100:\n' >$S/group2
printf '# fstab\n' >$S/fstab
python3 -I - $U $V0 $V1 $V2 <<'PYEOF'
import sys, struct
t = sys.argv[1]
for v in sys.argv[2:]:
    open("%s/kernel%s" % (t, v), "wb").write(b"\0" * 64 + b"ZVER" + struct.pack(">I", int(v)) + b"0.2".ljust(16, b"\0") + v.encode())
PYEOF
D="d:755:0:0:/var d:755:0:0:/var/lib d:755:0:0:/var/lib/zpkg d:755:0:0:/var/cache d:755:0:0:/var/cache/zpkg d:755:0:0:/bin d:755:0:0:/etc"
mkpkgs() {     # DIR VERSION zup sh ls passwd motd group [utils extra...]
    local o=$1 v=$2 ls=$5
    python3 tools/mkzup.py pkg $o zup $v 0.2 --group base d:755:0:0:/sbin f:755:0:0:$S/$3:/sbin/zup >/dev/null
    python3 tools/mkzup.py pkg $o base $v 0.2 --group base --dep zup:$v $D f:755:0:0:$S/init:/bin/init f:755:0:0:$S/$4:/bin/sh \
        c:644:0:0:$S/$6:/etc/passwd c:644:0:0:$S/$7:/etc/motd c:644:0:0:$S/$8:/etc/group c:644:0:0:$S/fstab:/etc/fstab >/dev/null
    shift 8
    python3 tools/mkzup.py pkg $o utils $v 0.2 --dep base:$v f:755:0:0:$S/$ls:/bin/ls f:4755:0:0:$S/su:/bin/su "$@" >/dev/null
}
mkpkgs $U/p0 $V0 zup1 sh1 ls1 passwd1 motd1 group1 f:755:0:0:$S/old1:/bin/old l:ls:/bin/more l:ls:/bin/gone
mkpkgs $U/p1 $V1 zup1 sh1 ls1 passwd1 motd1 group1 f:755:0:0:$S/old1:/bin/old l:ls:/bin/more l:ls:/bin/gone
mkpkgs $U/p2 $V2 zup2 sh2 ls2 passwd2 motd2 group2 f:755:0:0:$S/new2:/bin/new f:644:0:0:$S/big2:/bin/big l:sh:/bin/more l:ls:/bin/list
cp -r $U/p2 $U/p2dep; python3 tools/mkzup.py pkg $U/p2dep utils $V2 0.2 --dep base:2026102799 f:755:0:0:$S/ls2:/bin/ls >/dev/null
for b in 0 1 2; do eval v=\$V$b; python3 tools/mkzup.py bundle $U/b$b.zup --kernel $U/kernel$v $U/p$b zup base utils >/dev/null; done
python3 tools/mkzup.py bundle $U/b2dep.zup --kernel $U/kernel$V2 $U/p2dep zup base utils >/dev/null
python3 tools/mkzup.py bundle $U/b2min.zup --kernel $U/kernel$V2 --min 2026102650 $U/p2 zup base utils >/dev/null
# audit3 B6: a final . or .. in an entry's path, and package names with a / or capitals (mkzup.py refuses to make
# them, so the manifests are edited by hand)
for v in "dotdot:d 755 0 0 /bin/.." "dot:l ls /etc/." ; do
    n=${v%%:*}; rm -rf $U/p2$n; cp -r $U/p2 $U/p2$n; printf '%s\n' "${v#*:}" >>$U/p2$n/utils.manifest
    python3 tools/mkzup.py bundle $U/b2$n.zup --kernel $U/kernel$V2 $U/p2$n zup base utils >/dev/null
done
rm -rf $U/p2name; cp -r $U/p2 $U/p2name; mkdir -p $U/p2name/x
sed 's/^name utils$/name x\/utils/' $U/p2/utils.manifest >$U/p2name/x/utils.manifest; cp $U/p2/utils.tgz $U/p2name/x/utils.tgz
python3 tools/mkzup.py bundle $U/b2name.zup --kernel $U/kernel$V2 $U/p2name zup base x/utils >/dev/null
rm -rf $U/p2cap; cp -r $U/p2 $U/p2cap
sed 's/^depends base /depends Base /' $U/p2/utils.manifest >$U/p2cap/utils.manifest
python3 tools/mkzup.py bundle $U/b2cap.zup --kernel $U/kernel$V2 $U/p2cap zup base utils >/dev/null
python3 -I - $U <<'PYEOF'
import sys, struct
t = sys.argv[1]
d = bytearray(open(t + "/b2.zup", "rb").read())
n = struct.unpack(">I", d[52:56])[0]
last = 96 + 80 * (n - 1)
off, size = struct.unpack(">II", d[last + 40:last + 48])
b = bytearray(d); b[off + size // 2] ^= 0x20; open(t + "/b2bad.zup", "wb").write(b)
open(t + "/b2short.zup", "wb").write(d[:off + size // 2])
PYEOF
cat >$U/snap.py <<'PYEOF'
import os, sys, hashlib
r = sys.argv[1]
for dp, dn, fn in sorted(os.walk(r)):
    dn.sort()
    rel = dp[len(r):] or "/"
    if rel.startswith("/var/cache/zpkg"): continue
    for n in sorted(fn + [d for d in dn if os.path.islink(os.path.join(dp, d))]):
        p = os.path.join(dp, n); q = rel.rstrip("/") + "/" + n
        if q in ("/var/lib/zpkg/result",) or q.startswith("/var/cache/zpkg/"): continue
        if os.path.islink(p): print("l", q, os.readlink(p))
        else: print("f", q, oct(os.stat(p).st_mode & 0o7777), hashlib.sha256(open(p, "rb").read()).hexdigest())
    for n in dn:
        p = os.path.join(dp, n)
        if not os.path.islink(p): print("d", rel.rstrip("/") + "/" + n, oct(os.stat(p).st_mode & 0o7777))
PYEOF
snap() { python3 -I $U/snap.py $1; }
leftovers() { find $1 -name '*.zup-new' | head -1; [ -e $1/var/lib/zpkg/journal ] && echo journal; }
zup() {        # ROOT KVNUM BUNDLE [env...]
    local r=$1 k=$2 b=$3; shift 3
    env ZUP_ROOT=$r ZUP_KVNUM=$k ZUP_BUNDLE=$b ZUP_TRACE=$U/trace "$@" $U/zup
}
R=$U/r; mkdir -p $R; rm -f $U/trace
zup $R $V1 $U/b1.zup >$U/out 2>&1; rc=$?
[ $rc = 0 ] && [ "$(cat $R/bin/sh)" = "sh1 contents" ] && [ -L $R/bin/more ] && [ "$(stat -c %a $R/bin/su)" = 4755 ] &&
    [ -f $R/var/lib/zpkg/base.manifest ] && [ -f $R/var/lib/zpkg/zup.manifest ] && [ -z "$(leftovers $R)" ] && grep -q "^note 1 $V1$" $U/trace &&
    grep -q "^zup: installed Zenix 0.2 ($V1)" $R/var/lib/zpkg/result && [ "$(cat $U/fsclean)" = 1 ] &&
    check "a first install: every file, mode and link, the database, the result line, the note, the volume clean again" ok || { check "a first install" bad; cat $U/out; }
rm -f $U/trace; snap $R >$U/s1a
zup $R $V1 $U/b1.zup >$U/out 2>&1; rc=$?
[ $rc = 0 ] && grep -q "installed already: nothing to do" $U/out && ! grep -q "^open-write\|^rename\|^link\|^unlink\|^mark" $U/trace && snap $R | cmp -s - $U/s1a &&
    check "the same bundle again: \"installed already\", nothing written" ok || { check "the same bundle again: nothing written" bad; cat $U/out $U/trace; }
# the owner's own changes: a user added to /etc/passwd, /etc/group deleted
printf 'owner:x:1000:100::/home/owner:/bin/sh\n' >>$R/etc/passwd; rm $R/etc/group
snap $R >$U/s1; cp -a $R $U/r1; cp $U/fsclean $U/fsclean1
for v in "b2bad:SHA-256 does not match" "b2short:the bundle is .* bytes, its header says" "b0:older than the installed base" \
         "b2dep:needs base 2026102799 or newer" "b2min:needs base 2026102650 or newer" "b2:K$V1:newer than the running kernel" \
         "b2:F5:not enough space on the root" "b2dotdot:a bad entry (d)" "b2dot:a bad entry (l)" \
         "b2name:pkg/x/utils: a bad package name" "b2cap:a bad line (depends)"; do
    b=${v%%:*}; want=${v#*:}; k=$V2; e=()
    case "$want" in K*) k=${want%%:*}; k=${k#K}; want=${want#*:} ;; F*) e=(ZUP_FREE_KB=${want%%:*}); e=(ZUP_FREE_KB=5); want=${want#*:} ;; esac
    rm -f $U/trace
    zup $R $k $U/$b.zup "${e[@]}" >$U/out 2>&1; rc=$?
    [ $rc = 1 ] && grep -q "$want" $U/out && snap $R | cmp -s - $U/s1 && [ -z "$(leftovers $R)" ] && ! grep -q "^rename\|^link" $U/trace &&
        [ "$(cat $U/fsclean)" = 1 ] &&
        grep -q "^note 2 " $U/trace && grep -q "was not installed: " $R/var/lib/zpkg/result &&
        check "refused before any change ($b${e:+, a small disk}): '$want'" ok || { check "refused before any change ($b): '$want'" bad; cat $U/out; }
done
rm -f $U/trace; ino1=$(stat -c %i $R/bin/sh)
zup $R $V2 $U/b2.zup >$U/out 2>&1; rc=$?
snap $R >$U/s2; cp -a $R $U/r2
ok=ok
[ $rc = 0 ] || ok=bad
[ "$(cat $R/bin/sh)" = "sh2 contents" ] && [ "$(cat $R/sbin/zup)" = "zup2 contents" ] && [ ! -e $R/bin/old ] && [ -f $R/bin/new ] && cmp -s $R/bin/big $S/big2 || ok=bad
[ "$(readlink $R/bin/more)" = sh ] && [ "$(readlink $R/bin/list)" = ls ] && [ ! -L $R/bin/gone ] && [ "$(stat -c %a $R/bin/su)" = 4755 ] || ok=bad
grep -q '^owner:' $R/etc/passwd && cmp -s $R/etc/passwd.new $S/passwd2 && cmp -s $R/etc/motd $S/motd2 && [ ! -e $R/etc/motd.new ] || ok=bad
[ ! -e $R/etc/group ] && [ ! -e $R/etc/group.new ] && [ ! -e $R/etc/fstab.new ] && [ -z "$(leftovers $R)" ] || ok=bad
check "the update: new, changed and removed files and links, set-user-id kept" $ok
[ $ok = ok ] || cat $U/out
grep -q '^owner:' $R/etc/passwd && cmp -s $R/etc/passwd.new $S/passwd2 && cmp -s $R/etc/motd $S/motd2 && [ ! -e $R/etc/group ] && [ ! -e $R/etc/group.new ] &&
    grep -q "kept your /etc/passwd (the new one: /etc/passwd.new)" $R/var/lib/zpkg/result &&
    check "three-way compare: the owner's /etc/passwd kept and passwd.new written, an unchanged motd replaced, a deleted group not brought back; said in the result line" ok ||
    check "three-way compare of config files" bad
awk '/^rename \/sbin\/zup.zup-new/ { z = NR } /^spawn \/sbin\/zup -2/ { s = NR } /^open-write \/bin\/sh.zup-new/ { b = NR } END { exit !(z && s > z && b > s) }' $U/trace &&
    check "zup updates itself first: its own package, then the new /sbin/zup does the rest" ok || check "zup updates itself first" bad
awk '/^open-write \/bin\/sh.zup-new/ { w = NR } /^chown \/bin\/sh.zup-new/ && !c { c = NR } /^chmod \/bin\/sh.zup-new 755/ && !m { m = NR } /^rename \/bin\/sh.zup-new \/bin\/sh$/ { r = NR }
     /^sync/ && !s { s = NR } /^open-write \/var\/lib\/zpkg\/journal/ && !j { j = NR } /^link \/bin\/sh / { l = NR } END { exit !(j && s > j && l > s && w > l && c > w && m > c && r > m) }' $U/trace &&
    check "the order for a file: journal and sync, a hard link to the previous version, PATH.zup-new written, owner, then mode, then rename" ok || { check "the order for a file" bad; grep -n "su\|journal\|^sync" $U/trace | head -20; }
[ "$(cat $U/fsclean)" = 1 ] && grep -q "^mark unclean" $U/trace && ls $R/var/cache/zpkg | grep -qx "prev-$V2" && [ "$(ls $R/var/cache/zpkg | wc -l)" = 1 ] &&
    [ "$(stat -c %i $R/var/cache/zpkg/prev-$V2/f/$(awk '$9 == "/bin/sh" { print $2 }' $R/var/cache/zpkg/prev-$V2/log.* | head -1))" = "$ino1" ] &&
    check "marked \"needs a check\" during the install and clean after; the previous /bin/sh kept by hard link in prev-$V2 (the old prev set gone)" ok ||
    check "the needs-a-check mark and the prev set" bad
cp $U/fsclean $U/fsclean2

# a power cut before every change of the update, then zup again (the next start, no bundle): the root must be the
# old version, the old one with only zup new, or the new one - nothing in between; then with the bundle: the new one
python3 -I - $U/s1 $U/s2 >$U/s1z <<'PYEOF'
import sys
old = open(sys.argv[1]).read().split("\n"); new = {l.split(" ")[1]: l for l in open(sys.argv[2]).read().split("\n") if l}
out = [new.get(l.split(" ")[1], l) if l and l.split(" ")[1] in ("/sbin/zup", "/var/lib/zpkg/zup.manifest") else l for l in old]
print("\n".join(out), end="")
PYEOF
nch=$(grep -cv '^spawn\|^note' $U/trace)
bad=0; seen_mid=0; seen_old=0; seen_new=0
for ((n = 1; n <= nch + 1; n++)); do
    rm -rf $U/c; cp -a $U/r1 $U/c; cp $U/fsclean1 $U/fsclean; rm -f $U/trace
    zup $U/c $V2 $U/b2.zup ZUP_CUT=$n >/dev/null 2>&1
    zup $U/c $V2 "" >$U/out 2>&1
    snap $U/c >$U/sc
    if cmp -s $U/sc $U/s1; then seen_old=$((seen_old + 1)); elif cmp -s $U/sc $U/s1z; then seen_mid=$((seen_mid + 1)); elif cmp -s $U/sc $U/s2; then seen_new=$((seen_new + 1)); else bad=$((bad + 1)); echo "cut $n: neither old nor new"; diff $U/s1 $U/sc | head -5; fi
    [ -z "$(leftovers $U/c)" ] || { bad=$((bad + 1)); echo "cut $n: leftovers $(leftovers $U/c)"; }
    zup $U/c $V2 $U/b2.zup >/dev/null 2>&1
    snap $U/c | cmp -s - $U/s2 || { bad=$((bad + 1)); echo "cut $n: not the new version after the bundle again"; }
done
[ $bad = 0 ] && [ $seen_old -gt 0 ] && [ $seen_new -gt 0 ] &&
    check "a power cut before each of the $nch changes, then zup: old ($seen_old), old with the new zup ($seen_mid) or new ($seen_new), never between; the bundle again: new" ok ||
    check "power cuts during the update ($bad bad)" bad

# "Zenix (previous)": the old kernel runs with the rollback flag; and the same without the flag (the kernel is older
# than the base); then nothing more to roll back
rm -rf $U/c; cp -a $U/r2 $U/c; rm -f $U/trace
zup $U/c $V1 "" ZUP_FLAGS=1 >$U/out 2>&1; rc=$?
[ $rc = 0 ] && snap $U/c | cmp -s - $U/s1 && [ ! -e $U/c/var/cache/zpkg/prev-$V2 ] && [ -z "$(leftovers $U/c)" ] && grep -q "rolled back to the version before $V2" $U/c/var/lib/zpkg/result &&
    check "Zenix (previous): the root back to the old version exactly (the owner's files too), the prev set gone" ok || { check "Zenix (previous): the root back to the old version" bad; cat $U/out; snap $U/c | diff $U/s1 - | head; }
nrb=$(grep -cv '^spawn\|^note' $U/trace)
zup $U/c $V1 "" ZUP_FLAGS=1 >$U/out 2>&1
grep -q "nothing newer than the kernel" $U/out && snap $U/c | cmp -s - $U/s1 && check "... and once more: nothing left to roll back" ok || check "... and once more: nothing left to roll back" bad
rm -rf $U/c; cp -a $U/r2 $U/c
zup $U/c $V1 "" >$U/out 2>&1
grep -q "the installed base is newer than the kernel" $U/out && snap $U/c | cmp -s - $U/s1 && check "the version comparison alone (no flag) rolls back too" ok || check "the version comparison alone rolls back" bad
bad=0
for ((n = 1; n <= nrb + 1; n++)); do
    rm -rf $U/c; cp -a $U/r2 $U/c
    zup $U/c $V1 "" ZUP_FLAGS=1 ZUP_CUT=$n >/dev/null 2>&1
    zup $U/c $V1 "" ZUP_FLAGS=1 >/dev/null 2>&1
    snap $U/c | cmp -s - $U/s1 && [ -z "$(leftovers $U/c)" ] || { bad=$((bad + 1)); echo "rollback cut $n: not the old version"; }
done
[ $bad = 0 ] && check "a power cut before each of the rollback's $nrb changes, then again: the old version" ok || check "power cuts during the rollback ($bad bad)" bad
zup $U/c $V2 $U/b2.zup >/dev/null 2>&1
snap $U/c | cmp -s - $U/s2 && check "after the rollback the bundle installs again (the new version)" ok || check "after the rollback the bundle installs again" bad
env ZUP_ROOT=$U/c $U/zup >$U/out 2>&1; rc=$?
[ $rc = 0 ] && [ ! -s $U/out ] && check "a kernel without SYS_update: zup does nothing and says nothing" ok || check "a kernel without SYS_update: nothing" bad

# ---- 3. zup on the real ext2 code (audit 2026-10-26 F5) ----
# zup's calls served by user/fsd.c and kernel/ext2.c on an image (tools/host/zup-ext2.c, zup-fsd.c); the power goes
# after every block write of the update (a whole block reaches the disk or none of it), then zup again (the next
# start: the journal), then the bundle again. ext2 has no journal of its own, so a cut inside one request may leave
# a half-done change: it must only ever be a leak (tools/e2safe.py: a count too high, an unnamed inode, blocks or
# inodes marked in use that nothing holds), never a block in use marked free, a count below its names or a ".."
# naming a free inode; the volume stays marked "needs a check" whenever e2fsck finds something; the end is the new
# version. On the old code (blocks freed before the inode let go of them, mkdir's parent count raised after the
# entry, rmdir of a directory an unnamed subdirectory still points at) about 25 of the 2200 cuts were not leaks.
X=$T/x; mkdir -p $X
R2="-Dsys_call=hd_sys_call -Dsys_port_find=hd_port_find"
XF="-m32 -DZHOST -std=gnu11 -O1 -g -Wall -Wextra -Wno-format-truncation -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers"
gcc $XF -DFSD $R2 -c -o $X/fsd.o tools/host/zup-fsd.c 2>$X/cc.log && gcc $XF -DFSD $R2 -c -o $X/hd.o tools/host/hostdisk.c 2>>$X/cc.log &&
    gcc $XF -DFSD -c -o $X/ext2.o kernel/ext2.c 2>>$X/cc.log &&
    gcc $XF -Iuser/miniz -o $X/zup user/zup.c user/sha256.c user/miniz/miniz_tinfl.c tools/host/zup-ext2.c $X/fsd.o $X/hd.o $X/ext2.o 2>>$X/cc.log &&
    check "zup builds for the PC on the ext2 server" ok || { check "zup builds for the PC on the ext2 server" bad; cat $X/cc.log; }
gcc -m32 -DZHOST -DFSD -std=gnu11 -O1 -o $X/fsck user/fsck_ext2.c tools/host/hostdisk.c kernel/ext2.c 2>>$X/cc.log ||
    check "fsck.ext2 builds for the PC" bad
xz() { env HOSTDISK=$1 ZUP_KVNUM=$V2 ZUP_SELF=$X/zup "${@:2}" $X/zup; }
xcat() { debugfs -R "cat $2" $1 2>/dev/null; }
mke2fs -q -F -t ext2 -b 1024 -N 256 -d $U/r1 $X/base.img 4M && e2fsck -fn $X/base.img >/dev/null 2>&1 ||
    check "the ext2 image of the old version" bad
xnew() { [ "$(xcat $1 /bin/sh)" = "sh2 contents" ] && [ "$(xcat $1 /sbin/zup)" = "zup2 contents" ] && xcat $1 /etc/motd | cmp -s - $S/motd2 &&
         xcat $1 /etc/passwd.new | cmp -s - $S/passwd2 && [ -z "$(xcat $1 /var/lib/zpkg/journal)" ] && ! debugfs -R 'stat /var/lib/zpkg/journal' $1 2>/dev/null | grep -q '^Inode:'; }
cp $X/base.img $X/t.img
xz $X/t.img ZUP_BUNDLE=$U/b2.zup >$X/out 2>&1 && e2fsck -fn $X/t.img >$X/e2.log 2>&1 && xnew $X/t.img &&
    dumpe2fs -h $X/t.img 2>/dev/null | grep -q '^Filesystem state: *clean$' &&
    check "the update on ext2 without a cut: the new version, e2fsck clean, the volume clean again" ok || { check "the update on ext2 without a cut" bad; cat $X/out $X/e2.log; }
n=0; cuts=0; leaks=0; bad=0; rc=99; fbad=0; fixed=0
while [ $rc = 99 ] && [ $n -le 20000 ]; do
    cp $X/base.img $X/t.img
    xz $X/t.img ZUP_BUNDLE=$U/b2.zup ZUP_DISKCUT=$n >/dev/null 2>&1; rc=$?
    if [ $rc = 99 ]; then
        cuts=$((cuts + 1))
        # the next start's check (DESIGN D17): fsck.ext2 -y on the cut image, one run: e2fsck clean, the state clean
        if ! e2fsck -fn $X/t.img >/dev/null 2>&1; then
            cp $X/t.img $X/f.img; HOSTDISK=$X/f.img $X/fsck -y hd0 >$X/fsck.log 2>&1; frc=$?; fixed=$((fixed + 1))
            { [ $frc -le 1 ] && e2fsck -fn $X/f.img >/dev/null 2>&1 && dumpe2fs -h $X/f.img 2>/dev/null | grep -q '^Filesystem state: *clean$'; } ||
                { fbad=$((fbad + 1)); echo "  cut after $n sectors: fsck.ext2 -y status $frc did not leave it clean"; }
        fi
        for step in "the next start" "the bundle again"; do
            if [ "$step" = "the next start" ]; then xz $X/t.img >$X/out 2>&1; else xz $X/t.img ZUP_BUNDLE=$U/b2.zup >$X/out 2>&1; fi
            if e2fsck -fn $X/t.img >$X/e2.log 2>&1; then :; else
                [ "$step" = "the next start" ] && leaks=$((leaks + 1))
                python3 -I tools/e2safe.py $X/e2.log >$X/why || { bad=$((bad + 1)); echo "  cut after $n sectors, $step: $(head -2 $X/why | tr '\n' ' ')"; }
                dumpe2fs -h $X/t.img 2>/dev/null | grep -q '^Filesystem state: *not clean' || { bad=$((bad + 1)); echo "  cut after $n sectors, $step: e2fsck finds something, but the volume is marked clean"; }
            fi
        done
        xnew $X/t.img || { bad=$((bad + 1)); echo "  cut after $n sectors: not the new version after the bundle again"; }
    fi
    n=$((n + 2))
done
[ $rc = 0 ] && [ $cuts -gt 1000 ] && [ $bad = 0 ] &&
    check "a power cut after each of the update's $cuts block writes on ext2, then zup and the bundle again: at worst leaks (after $leaks cuts), marked \"needs a check\", the new version in the end" ok ||
    check "power cuts on ext2 ($bad bad, $cuts cuts, last zup $rc)" bad
[ $rc = 0 ] && [ $fixed -gt 100 ] && [ $fbad = 0 ] &&
    check "each of the $fixed cuts that left something is repaired by one fsck.ext2 -y: e2fsck -fn clean, marked clean (DESIGN D17)" ok ||
    check "fsck.ext2 -y after the update's cuts: $fbad of $fixed not repaired in one run" bad

echo "zup-host-test: $pass passed, $fail failed"
[ $fail = 0 ]
