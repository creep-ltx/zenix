#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Host tests of the updates (DESIGN D15, docs/d15-self-updates.md section 11):
#   1. the bundle format and the packages (tools/mkzup.py): the standard packages hold what their manifests say, a
#      bundle checks out, and every damaged one (header, table of contents, a part, a range, cut short) is refused;
#      the kernel in a bundle must carry the bundle's version; the full root image holds the package database.
# usage: tools/zup-host-test.sh
cd "$(dirname "$0")/.." || exit 1
T=$(mktemp -d /tmp/zup-host.XXXXXX)
trap 'rm -rf $T' EXIT
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
cp $B/init.zx $B/login.zx $B/sh.zx $B/mount.zx $T/r2/ && cp $B/init.zx $B/login.zx $B/sh.zx $B/mount.zx $T/r3/
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

echo "zup-host-test: $pass passed, $fail failed"
[ $fail = 0 ]
