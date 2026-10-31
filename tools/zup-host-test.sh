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
    grep -q '^f 4755 0 0 .* /bin/su$' $B/pkg/shadow.manifest && grep -q '^l less /bin/more$' $B/pkg/pager.manifest &&
    check "base: /etc/passwd is a config file, /etc/autologin is never shipped; su is set-user-id; more is a link" ok ||
    check "base: /etc/passwd is a config file, /etc/autologin is never shipped; su is set-user-id; more is a link" bad
# packages are reproducible: the same build gives the same bytes (mtime 0, no names in the gzip header)
mkdir -p $T/r2 $T/r3
cp $B/init.zx $B/login.zx $B/sh.zx $B/mount.zx $B/zfsd.zx $B/netd.zx $B/nfscd.zx $B/cp437-8x8.psf $B/zenix-8x8.zcf $B/anchors $T/r2/ && cp $B/init.zx $B/login.zx $B/sh.zx $B/mount.zx $B/zfsd.zx $B/netd.zx $B/nfscd.zx $B/cp437-8x8.psf $B/zenix-8x8.zcf $B/anchors $T/r3/   # base (and /sbin/zfsd, /sbin/netd: D19; /sbin/nfscd: D23; the PC font: D20; the root certificates: D21; the Unicode font: D28)
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
grep -qx base.manifest $T/db && grep -qx coreutils.manifest $T/db && grep -qx base-man.manifest $T/db && ! grep -qx 'utils.manifest\|man.manifest' $T/db &&
    [ "$(grep -c '\.manifest$' $T/db)" = "$(tr ' ' '\n' <$B/pkg/list | grep -vxc 'zpkg-new\|')" ] &&   # (zpkg-new: never on an image, D22 A2)
    check "the full root image holds the package database (/var/lib/zpkg): every package of the build, no utils or man" ok || { check "the full root image holds the package database" bad; cat $T/db; }
# the split by family (DESIGN D22 8.4.4): every file of the build in exactly one package, each family taking utils
# and man, base-man taking man, every manual page somewhere; the system bundle carries the base group only (3.1)
python3 -I - "$B" <<'EOF' >$T/out 2>&1 && check "the packages by family: every file in exactly one package; families take utils and man, base-man takes man; every page shipped" ok || { check "the packages by family" bad; cat $T/out; }
import sys, os
sys.path.insert(0, "tools"); import mkzup
b = sys.argv[1]
names = open(os.path.join(b, "pkg", "list")).read().split()
owner = {}
for n in names:
    if n in mkzup.BOOTSTRAP:            # (D22 amendment 2: a copy of zpkg for old systems, in no split)
        continue
    m = mkzup.parse_manifest(open(os.path.join(b, "pkg", n + ".manifest"), "rb").read())
    base = m["group"] == "base"
    assert base == (n in ("zup", "base", "zpkg")), n
    if not base:
        assert m["takes"] == (["man"] if n == "base-man" else ["utils", "man"]), (n, m["takes"])
        assert m["desc"], n
    for e in m["entries"]:
        if e[0] != "d":
            assert e[6] not in owner, (e[6], owner.get(e[6]), n)
            owner[e[6]] = n
for f in os.listdir(os.path.join("docs", "man")):
    assert "/usr/man/" + f in owner, f
assert sum(1 for p in owner if p.startswith("/bin/")) >= 120, len(owner)
print("ok")
EOF
python3 tools/mkzup.py list $B/zenix-update.zup >$T/out 2>&1
nbase=$(tr ' ' '\n' <$B/pkg/list | grep -cx 'zup\|base\|zpkg')
grep -q 'pkg/zup.manifest' $T/out && grep -q 'pkg/base.manifest' $T/out && [ "$(grep -c 'pkg/.*\.manifest' $T/out)" = "$nbase" ] &&
    check "the build's system bundle carries the kernel and the base group only (zup, base and zpkg)" ok || { check "the system bundle is base-only" bad; cat $T/out; }

# ---- 2. zup on the PC ----
U=$T/u; mkdir -p $U
gcc -m32 -DZHOST -std=gnu11 -O1 -g -Wall -Wextra -Wno-format-truncation -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Iuser/miniz -o $U/zup user/zup.c user/zpkgfmt.c user/sha256.c user/miniz/miniz_tinfl.c tools/host/zup-host.c 2>$U/cc.log &&
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
        if q in ("/var/lib/zpkg/result", "/var/lib/zpkg/lock") or q.startswith("/var/lib/zpkg/lock.") or q.startswith("/var/cache/zpkg/"): continue
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

# the same cuts, then straight away the start with the bundle (the recovery and the install in one run, review
# 2026-10-28: what the recovery puts back must be read again before the install decides)
bad=0
for ((n = 1; n <= nch + 1; n++)); do
    rm -rf $U/c; cp -a $U/r1 $U/c; cp $U/fsclean1 $U/fsclean
    zup $U/c $V2 $U/b2.zup ZUP_CUT=$n >/dev/null 2>&1
    zup $U/c $V2 $U/b2.zup >/dev/null 2>&1
    snap $U/c | cmp -s - $U/s2 && [ -z "$(leftovers $U/c)" ] || { bad=$((bad + 1)); echo "cut $n, then the bundle at once: not the new version"; }
done
[ $bad = 0 ] && check "a power cut before each change, then at once a start with the bundle: the new version every time" ok ||
    check "cuts then the bundle at once ($bad bad)" bad

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

# ---- 2b. live transactions: zup -f FILE and zup -r NAME (DESIGN D22 4.2) ----
# From the update above (V2: zup, base, utils; prev-V2's undo logs hold utils), a base-only system update V3 first,
# then packages installed, upgraded, taken over and removed one transaction each, under the lock, never touching the
# prev set; every refusal changes nothing; a power cut before each change of -f, a takes hand-over and -r (with a
# .save), then zup at the next start: the old state or the new one exactly, prev-V3 byte for byte as it was.
V3=2026102800; V4=2026102900
python3 -I - $U $V3 <<'PYEOF'
import sys, struct
t, v = sys.argv[1], sys.argv[2]
open("%s/kernel%s" % (t, v), "wb").write(b"\0" * 64 + b"ZVER" + struct.pack(">I", int(v)) + b"0.3".ljust(16, b"\0") + v.encode())
PYEOF
for f in zup3 sh3 ed1 ed2 edhelp edtutor ls3 edext; do printf '%s contents\n' $f >$S/$f; done
printf 'set prompt\n' >$S/edrc1; printf 'set prompt\nset verbose\n' >$S/edrc2; printf 'keys 1\n' >$S/edk1; printf 'cfg 3\n' >$S/edrc3
python3 tools/mkzup.py pkg $U/p3 zup $V3 0.3 --group base d:755:0:0:/sbin f:755:0:0:$S/zup3:/sbin/zup >/dev/null
python3 tools/mkzup.py pkg $U/p3 base $V3 0.3 --group base --dep zup:$V3 $D f:755:0:0:$S/init:/bin/init f:755:0:0:$S/sh3:/bin/sh \
    c:644:0:0:$S/passwd2:/etc/passwd c:644:0:0:$S/motd2:/etc/motd c:644:0:0:$S/group2:/etc/group c:644:0:0:$S/fstab:/etc/fstab >/dev/null
python3 tools/mkzup.py bundle $U/b3.zup --kernel $U/kernel$V3 --base-only $U/p3 zup base >/dev/null
Q=$U/q; mkdir -p $Q $U/pool
qpkg() {       # NAME VNUM [mkzup pkg options and entries...]: a package bundle in $U/pool/NAME-VNUM.zup
    local n=$1 v=$2; shift 2
    rm -rf $Q/$n; mkdir -p $Q/$n
    python3 tools/mkzup.py pkg $Q/$n $n $v 0.3 "$@" >/dev/null && python3 tools/mkzup.py bundle $U/pool/$n-$v.zup --no-kernel --min $V3 $Q/$n $n >/dev/null ||
        echo "qpkg $n $v: mkzup failed"
}
EDD="d:755:0:0:/usr d:755:0:0:/usr/share d:755:0:0:/usr/share/ed"
qpkg ed $V3 --dep base:$V3 $EDD f:755:0:0:$S/ed1:/bin/ed c:644:0:0:$S/edrc1:/etc/edrc c:644:0:0:$S/edk1:/etc/edkeys f:644:0:0:$S/edhelp:/usr/share/ed/help l:ed:/bin/red
qpkg ed $V4 --dep base:$V3 $EDD f:755:0:0:$S/ed2:/bin/ed c:644:0:0:$S/edrc2:/etc/edrc c:644:0:0:$S/edk1:/etc/edkeys f:644:0:0:$S/edtutor:/usr/share/ed/tutor l:ed:/bin/red
qpkg utils $V3 --dep base:$V3 f:755:0:0:$S/ls3:/bin/ls
qpkg coreutils $V3 --dep base:$V3 --takes utils f:755:0:0:$S/ls3:/bin/ls l:sh:/bin/more
qpkg textutils $V3 --dep base:$V3 --takes utils f:4755:0:0:$S/su:/bin/su f:755:0:0:$S/new2:/bin/new f:644:0:0:$S/big2:/bin/big l:ls:/bin/list
qpkg edcfg $V3 --dep base:$V3 --takes ed c:644:0:0:$S/edrc3:/etc/edrc
qpkg edext $V3 --dep ed:$V3 f:755:0:0:$S/edext:/bin/edext
qpkg kernelish $V3 --group base f:755:0:0:$S/ed1:/bin/kernelish                # refusals
qpkg zpkg $V3 f:755:0:0:$S/ed1:/bin/zpkgx
qpkg clash $V3 f:755:0:0:$S/ed1:/bin/ed
qpkg sneaky $V3 f:644:0:0:$S/ed1:/var/lib/zpkg/base.manifest
qpkg viasym $V3 f:755:0:0:$S/ed1:/opt/evil
qpkg stray $V3 f:755:0:0:$S/ed1:/bin/stray
qpkg dirclash $V3 d:755:0:0:/bin/ed
qpkg takebase $V3 --takes base f:755:0:0:$S/sh3:/bin/sh
qpkg needdep $V3 --dep nothere:1 f:755:0:0:$S/ed1:/bin/needdep
qpkg ancestor $V3 l:/tmp:/var/cache
# the reviews' attacks (2026-10-28), made by hand (mkzup.py refuses to make them): zup's own names, a path twice, a
# link target "-", a tar member bigger than its manifest says
qedit() {      # NAME LINE...: package NAME-V3 with these lines added to its manifest (or, with SED=expr, edited)
    local n=$1; shift
    rm -rf $Q/$n; mkdir -p $Q/$n
    python3 tools/mkzup.py pkg $Q/$n $n $V3 0.3 f:755:0:0:$S/ed1:/bin/$n >/dev/null
    for l in "$@"; do printf '%s\n' "$l" >>$Q/$n/$n.manifest; done
    [ -n "${SED:-}" ] && sed -i "$SED" $Q/$n/$n.manifest
    python3 tools/mkzup.py bundle $U/pool/$n-$V3.zup --no-kernel --min $V3 $Q/$n $n >/dev/null || echo "qedit $n: mkzup failed"
}
qedit zupnew "l ../../sbin/zup /bin/p.zup-new"
qedit dotnew "l ed /etc/edrc.new"
qedit twice "l ed /bin/twice"
qedit dash "l - /bin/dashlink"
SED='s/^\(f 755 0 0 [0-9a-f]*\) [0-9]* /\1 3 /' qedit tarsize
qpkg edext4 $V3 --dep ed:$V4 f:755:0:0:$S/edext:/bin/edext4
rm -rf $Q/newer; mkdir -p $Q/newer; python3 tools/mkzup.py pkg $Q/newer newer $V3 0.3 f:755:0:0:$S/ed1:/bin/newer >/dev/null
python3 tools/mkzup.py bundle $U/pool/newer-$V3.zup --no-kernel --min $V4 $Q/newer newer >/dev/null
cp $U/b3.zup $U/pool/system.zup
python3 -I - $U/pool/ed-$V3.zup $U/pool/damaged.zup <<'PYEOF'
import sys, struct
d = bytearray(open(sys.argv[1], "rb").read())
n = struct.unpack(">I", d[52:56])[0]
for i in range(n):
    name, off, size = struct.unpack(">40sII", d[96 + 80 * i:96 + 80 * i + 48])
    if name.rstrip(b"\0").endswith(b".tgz"): d[off + size // 2] ^= 0x10
open(sys.argv[2], "wb").write(d)
PYEOF
cat >$U/prevsnap.py <<'PYEOF'
import os, sys, hashlib
r = sys.argv[1]
for dp, dn, fn in sorted(os.walk(r)):
    dn.sort()
    for n in sorted(fn):
        p = os.path.join(dp, n); st = os.lstat(p)
        print(p[len(r):], st.st_nlink > 1, oct(st.st_mode), hashlib.sha256(open(p, "rb").read()).hexdigest())
PYEOF
pvsnap() { python3 -I $U/prevsnap.py $1/var/cache/zpkg/prev-$V3; }
# zl ROOT [VAR=VALUE...] ARGS...: zup with a running kernel V3 (the pool is copied into the root's cache)
zl() {
    local r=$1; shift
    local e=(); while [[ "${1:-}" == *=* ]]; do e+=("$1"); shift; done
    mkdir -p $r/var/cache/zpkg/pkg; cp -n $U/pool/*.zup $r/var/cache/zpkg/pkg/
    env ZUP_ROOT=$r ZUP_KVNUM=$V3 ZUP_TRACE=$U/trace "${e[@]}" $U/zup "$@"
}
pf() { echo /var/cache/zpkg/pkg/$1.zup; }
L=$U/l; rm -rf $L; cp -a $U/r2 $L; cp $U/fsclean2 $U/fsclean
zl $L -f $(pf utils-$V3) >$U/out 2>&1; rc=$?
[ $rc = 1 ] && grep -q "utils is part of the previous system the boot menu can go back to" $U/out && [ -f $L/var/lib/zpkg/utils.manifest ] &&
    grep -q "^name utils" $L/var/lib/zpkg/utils.manifest &&
    check "zup -f refuses a package the newest prev set's undo logs hold (utils, from the D15 bundles): the way back stays whole" ok ||
    { check "zup -f refuses a package in the prev set" bad; cat $U/out; }
env ZUP_ROOT=$L ZUP_KVNUM=$V3 ZUP_BUNDLE=$U/b3.zup $U/zup >$U/out 2>&1; rc=$?
[ $rc = 0 ] && [ "$(cat $L/bin/sh)" = "sh3 contents" ] && [ "$(ls $L/var/cache/zpkg | grep prev-)" = "prev-$V3" ] &&
    ! grep -q "utils.manifest" $L/var/cache/zpkg/prev-$V3/log.* && [ "$(cat $L/bin/ls)" = "ls2 contents" ] &&
    check "a base-only system update (zup and base, D22 3.1): its prev set holds no utils, utils stays as it was" ok || { check "a base-only system update" bad; cat $U/out; }
cp -a $L $U/S0; snap $L >$U/sS0; pvsnap $L >$U/pS0; cp $U/fsclean $U/fsclean3
lockfree() { [ ! -e $1/var/lib/zpkg/lock ] && [ ! -e $1/var/lib/zpkg/journal ] && [ -z "$(find $1 -name '*.zup-new')" ]; }
rm -f $U/trace
zl $L -f $(pf ed-$V3) >$U/out 2>&1; rc=$?
ok=ok
[ $rc = 0 ] && [ "$(cat $L/bin/ed)" = "ed1 contents" ] && [ "$(readlink $L/bin/red)" = ed ] && cmp -s $L/etc/edrc $S/edrc1 && [ -f $L/usr/share/ed/help ] || ok=bad
grep -q "^name ed$" $L/var/lib/zpkg/ed.manifest && grep -q "^version $V3 " $L/var/lib/zpkg/ed.manifest && lockfree $L || ok=bad
pvsnap $L | cmp -s - $U/pS0 && [ ! -e $L/var/cache/zpkg/live ] && [ "$(cat $U/fsclean)" = 1 ] && ! grep -q "^note\|^open-write /var/lib/zpkg/result" $U/trace || ok=bad
grep -q "^open-write /var/lib/zpkg/lock" $U/trace && grep -q "^link /bin/ed\|^rename /bin/ed.zup-new /bin/ed$" $U/trace || ok=bad
check "zup -f: a new package installed (files, link, config, its manifest), under the lock; prev-$V3 untouched, live/ gone, no note, no result, the volume clean" $ok
[ $ok = ok ] || cat $U/out
cp -a $L $U/E3; snap $L >$U/sE3
rm -f $U/trace
zl $L -f $(pf ed-$V3) >$U/out 2>&1; rc=$?
[ $rc = 0 ] && grep -q "ed $V3 is installed already: nothing to do" $U/out && snap $L | cmp -s - $U/sE3 && lockfree $L &&
    check "the same package again: \"installed already\", nothing changed" ok || { check "the same package again" bad; cat $U/out; }
printf 'my own line\n' >>$L/etc/edrc; cp -a $L $U/E3e; snap $L >$U/sE3e
zl $L -f $(pf ed-$V4) >$U/out 2>&1; rc=$?
ok=ok
[ $rc = 0 ] && [ "$(cat $L/bin/ed)" = "ed2 contents" ] && grep -q "my own line" $L/etc/edrc && cmp -s $L/etc/edrc.new $S/edrc2 && [ ! -e $L/usr/share/ed/help ] &&
    [ -f $L/usr/share/ed/tutor ] && grep -q "kept your /etc/edrc" $U/out && lockfree $L && pvsnap $L | cmp -s - $U/pS0 || ok=bad
check "zup -f: an upgrade (a file changed, one gone, one new; the owner's config kept, the new one as .new); prev-$V3 untouched" $ok
[ $ok = ok ] || cat $U/out
cp -a $L $U/E4; snap $L >$U/sE4
zl $L -f $(pf ed-$V3) >$U/out 2>&1; rc=$?
[ $rc = 1 ] && grep -q "older than the installed" $U/out && snap $L | cmp -s - $U/sE4 && lockfree $L &&
    check "zup -f: an older version is refused without -d, nothing changed" ok || { check "an older version refused" bad; cat $U/out; }
zl $L -f $(pf ed-$V3) -d >$U/out 2>&1; rc=$?
[ $rc = 0 ] && [ "$(cat $L/bin/ed)" = "ed1 contents" ] && [ -f $L/usr/share/ed/help ] && grep -q "^version $V3 " $L/var/lib/zpkg/ed.manifest &&
    zl $L -f $(pf ed-$V4) >/dev/null 2>&1 && snap $L | cmp -s - $U/sE4 &&
    check "zup -f -d: the downgrade zpkg asked for; then the upgrade again gives the same root" ok || { check "zup -f -d" bad; cat $U/out; }
# refusals: each before any change (the root, prev-V3 and the lock as they were)
mkdir -p $L/sbin; ln -s /sbin $L/opt; printf 'stray\n' >$L/bin/stray
snap $L >$U/sR; pvsnap $L >$U/pR
for v in "kernelish:in the group base" "zpkg:in the group base" "clash:/bin/ed belongs to package ed" \
         "sneaky:a package may not write there" "viasym:/opt is a symbolic link" "stray:/bin/stray exists and belongs to no package" \
         "dirclash:a directory in one package and not in ed" "takebase:takes base, a base-group package" \
         "needdep:needs nothing" "newer:needs base $V4 or newer" "system:a system bundle" "damaged:SHA-256 does not match" \
         "T:a new system is on trial" "ancestor:a package may not write there" "zupnew:a name zup keeps for itself" \
         "dotnew:a name zup keeps for itself" "twice:/bin/twice is listed twice" "dash:a bad entry (l)"; do
    n=${v%%:*}; want=${v#*:}; e=()
    [ $n = needdep ] && want="needs nothere 1 or newer"
    [ $n = T ] && { n=ed-$V3; e=(ZUP_FLAGS=2); } || { [ -f $U/pool/$n.zup ] || n=$n-$V3; }
    rm -f $U/trace
    zl $L "${e[@]}" -f $(pf $n) >$U/out 2>&1; rc=$?
    [ $rc = 1 ] && grep -q "$want" $U/out && snap $L | cmp -s - $U/sR && pvsnap $L | cmp -s - $U/pR && lockfree $L &&
        ! grep -v "zpkg/lock" $U/trace | grep -q "^rename\|^link\|^open-write /var/lib/zpkg/journal" &&
        check "zup -f refused before any change ($n${e:+, during a trial}): '$want'" ok || { check "zup -f refused ($n): '$want'" bad; cat $U/out; }
done
for v in "base:in the group base" "zup:in the group base" "nothere:nothere is not installed" "../x:not a package name"; do
    n=${v%%:*}; want=${v#*:}
    zl $L -r $n >$U/out 2>&1; rc=$?
    [ $rc = 1 ] && grep -q "$want" $U/out && snap $L | cmp -s - $U/sR && lockfree $L &&
        check "zup -r refused ($n): '$want'" ok || { check "zup -r refused ($n): '$want'" bad; cat $U/out; }
done
zl $L -f $(pf edext-$V3) >/dev/null 2>&1; zl $L -r ed >$U/out 2>&1; rc=$?
[ $rc = 1 ] && grep -q "edext needs ed: refused" $U/out && [ -f $L/bin/ed ] && zl $L -r edext >/dev/null 2>&1 && [ ! -e $L/bin/edext ] && [ ! -e $L/var/lib/zpkg/edext.manifest ] &&
    check "zup -r refuses a package another one needs (named); removing that one first works" ok || { check "zup -r and dependents" bad; cat $U/out; }
zl $L -f $(pf edext4-$V3) >/dev/null 2>&1; zl $L -f $(pf ed-$V3) -d >$U/out 2>&1; rc=$?
[ $rc = 1 ] && grep -q "edext4 needs ed $V4 or newer: the downgrade to $V3 is refused" $U/out && [ "$(cat $L/bin/ed)" = "ed2 contents" ] &&
    zl $L -r edext4 >/dev/null 2>&1 && check "zup -f -d refuses a downgrade another package needs the newer version of" ok || { check "a downgrade and its dependents" bad; cat $U/out; }
zl $L -f $(pf tarsize-$V3) >$U/out 2>&1; rc=$?
[ $rc = 1 ] && grep -q "its archive's size is not its manifest's" $U/out && snap $L | cmp -s - $U/sR && lockfree $L &&
    check "a tar member bigger than its manifest says: stopped there, rolled back, nothing changed" ok || { check "a tar member's size" bad; cat $U/out; }
printf 'zupj 1\ntarget %s 1 1\nA - 644 0 0 - - - /bin/sysfile\nend\n' $V3 >$L/var/lib/zpkg/journal
zl $L -f $(pf coreutils-$V3) >$U/out 2>&1; rc=$?
[ $rc = 1 ] && grep -q "a system update is unfinished: restart first" $U/out && [ -f $L/var/lib/zpkg/journal ] && [ ! -e $L/var/lib/zpkg/coreutils.manifest ] &&
    check "zup -f leaves a system update's journal to the next start (its result is boot's)" ok || { check "zup -f and a system journal" bad; cat $U/out; }
rm -f $L/var/lib/zpkg/journal
rm -f $L/opt $L/bin/stray; rmdir $L/sbin 2>/dev/null; snap $L | cmp -s - $U/sE4 || { check "the root after the refusals is E4 again" bad; snap $L | diff $U/sE4 - | head; }
# the lock
mkdir -p $U/lk; cp "$(command -v sleep)" $U/lk/zup; cp "$(command -v sleep)" $U/lk/zpkg
$U/lk/zup 30 & lp=$!
printf '%d\n' $lp >$L/var/lib/zpkg/lock
zl $L -f $(pf coreutils-$V3) >$U/out 2>&1; rc=$?
[ $rc = 1 ] && grep -q "another zup (pid $lp) is changing packages" $U/out && [ "$(cat $L/var/lib/zpkg/lock)" = $lp ] && [ ! -e $L/var/lib/zpkg/coreutils.manifest ] &&
    check "a second zup is refused while the lock's holder lives" ok || { check "a second zup refused while one runs" bad; cat $U/out; }
printf '999999\n' >$L/var/lib/zpkg/lock
zl $L -f $(pf coreutils-$V3) >$U/out 2>&1; rc=$?
[ $rc = 1 ] && grep -q "another zup or zpkg (pid $lp) is still changing packages" $U/out && [ ! -e $L/var/lib/zpkg/coreutils.manifest ] &&
    check "a dead holder's lock is not taken over while another zup still lives (one zpkg left behind)" ok || { check "a stale lock with a zup still running" bad; cat $U/out; }
kill $lp; wait $lp 2>/dev/null
cat >$U/lk/zpkg <<EOF
#!/usr/bin/env bash
printf '%d\n' \$\$ >$L/var/lib/zpkg/lock
env ZUP_ROOT=$L ZUP_KVNUM=$V3 $U/zup -f $(pf coreutils-$V3)
rc=\$?
rm -f $L/var/lib/zpkg/lock
exit \$rc
EOF
chmod +x $U/lk/zpkg
$U/lk/zpkg >$U/out 2>&1; rc=$?
[ $rc = 0 ] && [ "$(cat $L/bin/ls)" = "ls3 contents" ] && [ -f $L/var/lib/zpkg/coreutils.manifest ] &&
    check "zup -f under its parent zpkg's lock (the parent's pid in it)" ok || { check "zup -f under its parent's lock" bad; cat $U/out; }
rm -rf $L; cp -a $U/E4 $L
printf '999999\n' >$L/var/lib/zpkg/lock
zl $L -f $(pf coreutils-$V3) >$U/out 2>&1; rc=$?
ok=ok
[ $rc = 0 ] && lockfree $L && [ "$(cat $L/bin/ls)" = "ls3 contents" ] && [ "$(readlink $L/bin/more)" = sh ] || ok=bad
grep -q "^f .* /bin/ls$" $L/var/lib/zpkg/coreutils.manifest && ! grep -q " /bin/ls$\| /bin/more$" $L/var/lib/zpkg/utils.manifest && grep -q " /bin/su$" $L/var/lib/zpkg/utils.manifest || ok=bad
[ -n "$(find $L/var/cache/zpkg -maxdepth 1 -name live)" ] && ok=bad
pvsnap $L | cmp -s - $U/pS0 || ok=bad
check "a stale lock (a dead pid) is taken over; takes: coreutils takes /bin/ls and /bin/more from utils (utils's manifest rewritten in the same transaction)" $ok
[ $ok = ok ] || cat $U/out
cp -a $L $U/C1; snap $L >$U/sC1
zl $L -f $(pf textutils-$V3) >$U/out 2>&1; rc=$?
[ $rc = 0 ] && [ ! -e $L/var/lib/zpkg/utils.manifest ] && [ "$(stat -c %a $L/bin/su)" = 4755 ] && cmp -s $L/bin/big $S/big2 && [ "$(readlink $L/bin/list)" = ls ] &&
    grep -q " /bin/su$" $L/var/lib/zpkg/textutils.manifest &&
    check "takes: textutils takes the rest of utils; utils's manifest goes with its last entry" ok || { check "takes the rest of utils" bad; cat $U/out; }
zl $L -f $(pf edcfg-$V3) >$U/out 2>&1; rc=$?
[ $rc = 0 ] && grep -q "my own line" $L/etc/edrc && cmp -s $L/etc/edrc.new $S/edrc3 && ! grep -q " /etc/edrc$" $L/var/lib/zpkg/ed.manifest &&
    grep -q "^c .* /etc/edrc$" $L/var/lib/zpkg/edcfg.manifest &&
    check "takes a config file: compared with the taken package's entry (the owner's change kept, the new one as .new)" ok || { check "takes a config file" bad; cat $U/out; }
zl $L -r ed >$U/out 2>&1; rc=$?
[ $rc = 0 ] && [ ! -e $L/bin/ed ] && [ ! -L $L/bin/red ] && [ ! -e $L/etc/edkeys ] && [ -f $L/etc/edrc ] && [ ! -e $L/usr/share/ed ] && [ ! -e $L/usr ] &&
    [ ! -e $L/var/lib/zpkg/ed.manifest ] && lockfree $L && pvsnap $L | cmp -s - $U/pS0 &&
    check "zup -r: files, link and the unchanged config gone, a taken config stays, its empty directories gone; prev-$V3 untouched" ok || { check "zup -r" bad; cat $U/out; ls -R $L/usr 2>&1 | head; }
cp -a $L $U/C3
rm -rf $L; cp -a $U/E4 $L
zl $L -r ed >$U/out 2>&1; rc=$?
[ $rc = 0 ] && [ ! -e $L/etc/edrc ] && grep -q "my own line" $L/etc/edrc.save && [ ! -e $L/etc/edkeys ] && [ -f $L/etc/edrc.new ] &&
    grep -q "kept your changed /etc/edrc as .save" $U/out &&
    check "zup -r: a config file the owner changed is kept as PATH.save" ok || { check "zup -r with .save" bad; cat $U/out; }
cp -a $L $U/Rs; snap $L >$U/sRs
rm -rf $L; cp -a $U/E4 $L; printf 'old save\n' >$L/etc/edrc.save
zl $L -r ed >$U/out 2>&1; rc=$?
[ $rc = 1 ] && grep -q "/etc/edrc.save exists: move it away first" $U/out && [ -f $L/bin/ed ] && grep -q "old save" $L/etc/edrc.save &&
    check "zup -r never replaces an older .save (refused, nothing changed)" ok || { check "zup -r and an older .save" bad; cat $U/out; }
rm -rf $L; cp -a $U/E4 $L
zl $L -r ed -n >$U/out 2>&1; rc=$?
[ $rc = 0 ] && [ ! -e $L/etc/edrc ] && [ ! -e $L/etc/edrc.save ] &&
    check "zup -r -n: the changed config file deleted too" ok || { check "zup -r -n" bad; cat $U/out; }

# a power cut before each change, then zup at the next start: the old root or the new one, never between; prev-V3
# byte for byte (the commit line included: the cut after it too); the lock and journal gone after the start that
# finishes or rolls back; the same operation again: the new root
livecuts() {   # NAME FROMDIR TOSNAP ARGS...
    local name=$1 from=$2 to=$3; shift 3
    local n nch bad=0 o=0 nw=0
    rm -rf $L; cp -a $from $L; rm -f $U/trace
    zl $L "$@" >/dev/null 2>&1
    nch=$(grep -cv '^spawn\|^note' $U/trace)
    snap $from >$U/cfrom
    for ((n = 1; n <= nch + 1; n++)); do
        rm -rf $L; cp -a $from $L; cp $U/fsclean3 $U/fsclean
        zl $L ZUP_CUT=$n "$@" >/dev/null 2>&1
        env ZUP_ROOT=$L ZUP_KVNUM=$V3 $U/zup >$U/out 2>&1
        snap $L >$U/sc
        if cmp -s $U/sc $U/cfrom; then o=$((o + 1)); elif cmp -s $U/sc $to; then nw=$((nw + 1)); else bad=$((bad + 1)); echo "  $name cut $n: neither old nor new"; diff $U/cfrom $U/sc | head -6; fi
        pvsnap $L | cmp -s - $U/pS0 || { bad=$((bad + 1)); echo "  $name cut $n: prev-$V3 changed"; }
        [ ! -e $L/var/lib/zpkg/journal ] && [ -z "$(find $L -name '*.zup-new')" ] || { bad=$((bad + 1)); echo "  $name cut $n: a journal or .zup-new left"; }
        zl $L "$@" >/dev/null 2>&1
        snap $L | cmp -s - $to && lockfree $L && pvsnap $L | cmp -s - $U/pS0 || { bad=$((bad + 1)); echo "  $name cut $n: not the new root after the operation again"; }
    done
    [ $bad = 0 ] && [ $o -gt 0 ] && [ $nw -gt 0 ] &&
        check "a power cut before each of the $nch changes of $name, then the next start: old ($o) or new ($nw), prev-$V3 untouched; then again: new" ok ||
        check "power cuts during $name ($bad bad)" bad
}
livecuts "zup -f (a new package)" $U/S0 $U/sE3 -f $(pf ed-$V3)
livecuts "zup -f (an upgrade with a kept config)" $U/E3e $U/sE4 -f $(pf ed-$V4)
livecuts "zup -f (a takes hand-over)" $U/E4 $U/sC1 -f $(pf coreutils-$V3)
livecuts "zup -r (with a .save)" $U/E4 $U/sRs -r ed

# the system's way back after live transactions: "Zenix (previous)" puts base and zup back to V2 from prev-V3; what
# zup -f and -r did stays (D22 4.4: the two never touch the same package)
rm -rf $L; cp -a $U/C3 $L
env ZUP_ROOT=$L ZUP_KVNUM=$V2 ZUP_FLAGS=1 $U/zup >$U/out 2>&1; rc=$?
[ $rc = 0 ] && [ "$(cat $L/bin/sh)" = "sh2 contents" ] && [ "$(cat $L/sbin/zup)" = "zup2 contents" ] && grep -q "^version $V2 " $L/var/lib/zpkg/base.manifest &&
    [ "$(cat $L/bin/ls)" = "ls3 contents" ] && [ -f $L/var/lib/zpkg/textutils.manifest ] && [ ! -e $L/var/lib/zpkg/utils.manifest ] && [ -f $L/var/lib/zpkg/edcfg.manifest ] &&
    [ ! -e $L/var/cache/zpkg/prev-$V3 ] && lockfree $L &&
    check "Zenix (previous) after live transactions: base and zup back to $V2, the live-installed packages untouched" ok || { check "the D15 way back after live transactions" bad; cat $U/out; }

# ---- 2c. the split on a machine installed before it (D22 8.4.4, step 4) ----
# This build's programs as the D15 bundles shipped them (zup, base, utils, man), installed from an empty root; a family
# refused while the prev set holds utils; then the base-only system update; then every family and base-man by zup -f:
# utils and man are gone, every file is as it was, the prev set untouched.
M=$U/mig; rm -rf $M; mkdir -p $M/pool $M/r/bin $M/r/etc     # an image has /bin and /etc (mkroot.sh)
python3 -I - "$B" $M <<'PYEOF' >$M/prep.log 2>&1 || { echo "FAIL the split's preparation"; cat $M/prep.log; }
import sys, os, io, gzip, tarfile, struct, tempfile
sys.path.insert(0, "tools"); import mkzup
b, m = sys.argv[1], sys.argv[2]
names = open(os.path.join(b, "pkg", "list")).read().split()
man = {n: mkzup.parse_manifest(open(os.path.join(b, "pkg", n + ".manifest"), "rb").read()) for n in names}
v0 = man["base"]["vnum"]; v1 = v0 + 1
data = {}
for n in names:
    tar = tarfile.open(fileobj=io.BytesIO(gzip.decompress(open(os.path.join(b, "pkg", n + ".tgz"), "rb").read())))
    for mem in tar.getmembers():
        data["/" + mem.name] = tar.extractfile(mem).read()
src = tempfile.mkdtemp(dir=m)
def ents(entries, only=None):
    out = []
    for kind, mode, uid, gid, digest, size, path, target in entries:
        if only and not only(kind, path):
            continue
        if kind in "fc":
            f = os.path.join(src, str(len(os.listdir(src))))
            open(f, "wb").write(data[path])
            out.append((kind, mode, uid, gid, f, path))
        elif kind == "l":
            out.append(("l", 0o777, 0, 0, target, path))
        else:
            out.append(("d", mode, uid, gid, None, path))
    return out
def write(dirn, n, v, entries, **kw):
    mkzup.make_package(os.path.join(m, dirn), n, v, "0.2", entries, **kw)
for k, v in ((0, v0), (1, v1)):
    open(os.path.join(m, "kernel%d" % k), "wb").write(b"\0" * 64 + b"ZVER" + struct.pack(">I", v) + b"0.2".ljust(16, b"\0") + b"k")
    write("p%d" % k, "zup", v, ents(man["zup"]["entries"]), group="base")
    write("p%d" % k, "base", v, ents(man["base"]["entries"]), group="base", deps=[("zup", v)])
if "zpkg" in man:                                       # zpkg joins the base group with the system update
    write("p1", "zpkg", v1, ents(man["zpkg"]["entries"]), group="base", deps=[("zup", v1)])
fam = [n for n in names if n not in ("zup", "base", "zpkg") and n not in mkzup.BOOTSTRAP]
utils, pages, seen = [], [("d", 0o755, 0, 0, None, "/usr"), ("d", 0o755, 0, 0, None, "/usr/man")], set()
for n in fam:
    for e in ents(man[n]["entries"]):
        if e[5] in seen: continue
        seen.add(e[5])
        if e[5].startswith("/usr/man/"): pages.append(e)
        elif e[0] != "d" or not e[5].startswith("/usr"): utils.append(e)   # a family's own directories (ZINE's /lib/zine)
write("p0", "utils", v0, utils, deps=[("base", v0)])
write("p0", "man", v0, pages)
for n in fam:
    mm = man[n]
    write("f", n, v1, ents(mm["entries"]), deps=[("base", v1)], desc=mm["desc"], takes=mm["takes"])
open(os.path.join(m, "versions"), "w").write("%d %d\n%s\n" % (v0, v1, " ".join(fam)))
PYEOF
read -r MV0 MV1 <$M/versions; MFAM=$(sed -n 2p $M/versions)
python3 tools/mkzup.py bundle $M/old.zup --kernel $M/kernel0 $M/p0 zup base utils man >/dev/null &&
    python3 tools/mkzup.py bundle $M/new.zup --kernel $M/kernel1 --base-only $M/p1 $(cd $M/p1 && ls *.manifest | sed 's/\.manifest$//') >/dev/null || echo "FAIL the split's bundles"
mkdir -p $M/r/var/cache/zpkg/pkg
for n in $MFAM; do python3 tools/mkzup.py bundle $M/r/var/cache/zpkg/pkg/$n.zup --no-kernel --min $MV1 $M/f $n >/dev/null || echo "FAIL bundle $n"; done
echo 1 >$U/fsclean
env ZUP_ROOT=$M/r ZUP_KVNUM=$MV0 ZUP_BUNDLE=$M/old.zup $U/zup >$M/out 2>&1 && [ -f $M/r/var/lib/zpkg/utils.manifest ] && [ -f $M/r/var/lib/zpkg/man.manifest ] &&
    check "the split: this build as the D15 bundles shipped it (zup, base, utils, man) installed on an empty root" ok || { check "the split: the old install" bad; tail $M/out; }
mfiles() { (cd $1 && find bin sbin etc usr \( -type f -o -type l \) -printf '%p %m %l\n' | sort; find bin sbin etc usr -type f | sort | xargs sha256sum); }
mfiles $M/r >$M/files0
env ZUP_ROOT=$M/r ZUP_KVNUM=$MV0 $U/zup -f /var/cache/zpkg/pkg/coreutils.zup >$M/out 2>&1; rc=$?
[ $rc = 1 ] && grep -q "part of the previous system" $M/out && check "the split: a family is refused while the prev set holds utils" ok || { check "the split: refused before the base-only update" bad; cat $M/out; }
env ZUP_ROOT=$M/r ZUP_KVNUM=$MV1 ZUP_BUNDLE=$M/new.zup $U/zup >$M/out 2>&1 && ls $M/r/var/cache/zpkg | grep -qx "prev-$MV1" &&
    ! grep -q "utils.manifest\|man.manifest" $M/r/var/cache/zpkg/prev-$MV1/log.* &&
    check "the split: the base-only system update; its prev set holds no utils or man" ok || { check "the split: the base-only update" bad; tail $M/out; }
pvm=$(python3 -I $U/prevsnap.py $M/r/var/cache/zpkg/prev-$MV1)
bad=0
for n in $MFAM; do
    env ZUP_ROOT=$M/r ZUP_KVNUM=$MV1 $U/zup -f /var/cache/zpkg/pkg/$n.zup >$M/out 2>&1 || { bad=$((bad + 1)); echo "  zup -f $n:"; tail -3 $M/out; }
done
ls $M/r/var/lib/zpkg | grep '\.manifest$' | sed 's/\.manifest$//' | sort >$M/db
printf '%s\n' $(cd $M/p1 && ls *.manifest | sed 's/\.manifest$//') $MFAM | sort >$M/dbwant
[ $bad = 0 ] && cmp -s $M/db $M/dbwant && mfiles $M/r | grep -v zpkg | cmp -s - <(grep -v zpkg $M/files0) && { ! grep -qw zpkg $B/pkg/list || [ -f $M/r/bin/zpkg ]; } && [ "$(python3 -I $U/prevsnap.py $M/r/var/cache/zpkg/prev-$MV1)" = "$pvm" ] && lockfree $M/r &&
    check "the split: each of the $(echo $MFAM | wc -w) families and base-man by zup -f: utils and man gone, every file as it was (zpkg new with the system), the prev set untouched" ok ||
    { check "the split: the families ($bad failed)" bad; diff $M/db $M/dbwant; mfiles $M/r | diff $M/files0 - | head; }

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
    gcc $XF -Iuser/miniz -o $X/zup user/zup.c user/zpkgfmt.c user/sha256.c user/miniz/miniz_tinfl.c tools/host/zup-ext2.c $X/fsd.o $X/hd.o $X/ext2.o 2>>$X/cc.log &&
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

# live transactions on the real ext2 code (D22 4.2, 8.1): a power cut after each block write of zup -f (a new
# package) and of zup -r (with a .save), then the next start, then the same operation again: at worst leaks, marked
# "needs a check" whenever e2fsck finds something, prev-V3 as it was, the new state in the end
xl() { local img=$1; shift; local e=(); while [[ "${1:-}" == *=* ]]; do e+=("$1"); shift; done
       env HOSTDISK=$img ZUP_KVNUM=$V3 ZUP_SELF=$X/zup "${e[@]}" $X/zup "$@"; }
xprev() { for f in $(debugfs -R "ls /var/cache/zpkg/prev-$V3" $1 2>/dev/null | tr -s ' ' '\n' | grep '^log'); do xcat $1 /var/cache/zpkg/prev-$V3/$f; done
          debugfs -R "ls /var/cache/zpkg/prev-$V3/f" $1 2>/dev/null | tr -s ' ' '\n' | grep -v '^$' | sort; }
xed_new() { [ "$(xcat $1 /bin/ed)" = "ed1 contents" ] && xcat $1 /var/lib/zpkg/ed.manifest | grep -q '^name ed$' && [ -z "$(xcat $1 /var/lib/zpkg/journal)" ]; }
xrm_new() { [ -z "$(xcat $1 /bin/ed)" ] && [ -z "$(xcat $1 /var/lib/zpkg/ed.manifest)" ] && xcat $1 /etc/edrc.save | grep -q 'my own line' && [ -z "$(xcat $1 /var/lib/zpkg/journal)" ]; }
for c in "f:S0:xed_new:-f /var/cache/zpkg/pkg/ed-$V3.zup" "r:E4:xrm_new:-r ed"; do
    IFS=: read -r cn from fn args <<<"$c"
    rm -rf $X/stage; cp -a $U/$from $X/stage; mkdir -p $X/stage/var/cache/zpkg/pkg; cp $U/pool/ed-$V3.zup $X/stage/var/cache/zpkg/pkg/
    mke2fs -q -F -t ext2 -b 1024 -N 512 -d $X/stage $X/lbase.img 8M && e2fsck -fn $X/lbase.img >/dev/null 2>&1 || check "the ext2 image for zup -$cn" bad
    xprev $X/lbase.img >$X/prev0
    cp $X/lbase.img $X/t.img
    # shellcheck disable=SC2086
    xl $X/t.img $args >$X/out 2>&1 && e2fsck -fn $X/t.img >$X/e2.log 2>&1 && $fn $X/t.img && xprev $X/t.img | cmp -s - $X/prev0 &&
        dumpe2fs -h $X/t.img 2>/dev/null | grep -q '^Filesystem state: *clean$' &&
        check "zup -$cn on ext2 without a cut: done, e2fsck clean, prev-$V3 as it was, the volume clean again" ok || { check "zup -$cn on ext2 without a cut" bad; cat $X/out $X/e2.log; }
    n=0; cuts=0; leaks=0; bad=0; rc=99
    while [ $rc = 99 ] && [ $n -le 5000 ]; do
        cp $X/lbase.img $X/t.img
        # shellcheck disable=SC2086
        xl $X/t.img ZUP_DISKCUT=$n $args >/dev/null 2>&1; rc=$?
        if [ $rc = 99 ]; then
            cuts=$((cuts + 1))
            for step in "the next start" "the operation again"; do
                # shellcheck disable=SC2086
                if [ "$step" = "the next start" ]; then xl $X/t.img >$X/out 2>&1; else xl $X/t.img $args >$X/out 2>&1; fi
                if ! e2fsck -fn $X/t.img >$X/e2.log 2>&1; then
                    [ "$step" = "the next start" ] && leaks=$((leaks + 1))
                    python3 -I tools/e2safe.py $X/e2.log >$X/why || { bad=$((bad + 1)); echo "  zup -$cn, cut after $n sectors, $step: $(head -2 $X/why | tr '\n' ' ')"; }
                    dumpe2fs -h $X/t.img 2>/dev/null | grep -q '^Filesystem state: *not clean' || { bad=$((bad + 1)); echo "  zup -$cn, cut after $n sectors, $step: e2fsck finds something, but the volume is marked clean"; }
                fi
            done
            $fn $X/t.img && xprev $X/t.img | cmp -s - $X/prev0 || { bad=$((bad + 1)); echo "  zup -$cn, cut after $n sectors: not the new state, or prev-$V3 changed"; }
        fi
        n=$((n + 1))
    done
    [ $rc = 0 ] && [ $cuts -gt 50 ] && [ $bad = 0 ] &&
        check "zup -$cn on ext2: a power cut after each of its $cuts block writes, then the next start and again: at worst leaks (after $leaks cuts), marked \"needs a check\", prev-$V3 untouched, the new state in the end" ok ||
        check "zup -$cn: power cuts on ext2 ($bad bad, $cuts cuts, last zup $rc)" bad
done

echo "zup-host-test: $pass passed, $fail failed"
[ $fail = 0 ]
