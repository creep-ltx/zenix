#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Build the ext2 root disk: rootfs/ plus the programs from build/*.zx as /bin/<name> (zup, zfsd, netd and wasabid
# as /sbin/<name>), and the licence notices of the code from other projects in /usr/share/licenses.
# usage: mkroot.sh out.hdf builddir "prog prog ..."
set -e
OUT="$1"; B="$2"; PROGS="$3"; SIZE="${4:-8M}"; INODES="${5:-512}"
T=$(mktemp -d)
# the image is built as a temporary file next to the output and moved into place only when mke2fs and e2fsck
# succeeded: a failure (the programs no longer fit, say) must not leave a half-written image that the next make
# takes for up to date. Any failure also removes the staging directory.
TMPOUT="$OUT.tmp.$$"
trap 'rm -rf "$T" "$TMPOUT"' EXIT
cp -r "$(dirname "$0")/../rootfs/." "$T/"
# root's home (owner 2026-10-31): /root in a release build, / in a test build (tools/mkzup.py release_passwd)
[ -z "$ZENIX_RELEASE" ] || python3 "$(dirname "$0")/mkzup.py" release-passwd > "$T/etc/passwd"
[ -n "$NOTESTS" ] || mkdir -p "$T/root"
[ -n "$NOTESTS" ] && rm -rf "$T/tests"      # NOTESTS=1: leave the test scripts out (the small RAM-disk image)
# the eleven corrupt programs mem.sh feeds to the loader (tools/mkbadzexe.py), with the scripts, so mem.sh also runs on
# a real machine's card (2026-10-24: on the A1200 they were missing and the check passed for the wrong reason)
if [ -z "$NOTESTS" ] && [ -f "$B/hello.zx" ]; then
    python3 "$(dirname "$0")/mkbadzexe.py" "$B/hello.zx" "$T/tests" >/dev/null && chmod 755 "$T"/tests/badzexe*
fi
# programs with shared code for the D13 tests (tools/pure-test.sh), only in the test directory (not commands): hello
# and crash built as ZEX2 (and sigtest, for D16's handlers in shared code: tools/signal-test.sh), a second hello whose text says HELLO (an updated program), and the corrupt ZEX2 files
# (NOPURE=1: not these, the mount tests' small floppy has no room)
if [ -z "$NOTESTS" ] && [ -z "$NOPURE" ] && [ -f "$B/hello.pzx" ] && [ -f "$B/crash.pzx" ]; then
    cp "$B/hello.pzx" "$T/tests/hellopure"
    cp "$B/crash.pzx" "$T/tests/crashpure"
    [ -f "$B/sigtest.pzx" ] && cp "$B/sigtest.pzx" "$T/tests/sigtestpure" && chmod 755 "$T/tests/sigtestpure"   # D16: handlers in shared code
    python3 -c 'import sys; d = open(sys.argv[1], "rb").read(); assert d.count(b"hello from pid") == 1
open(sys.argv[2], "wb").write(d.replace(b"hello from pid", b"HELLO from pid"))' "$B/hello.pzx" "$T/tests/hellopure2"
    python3 "$(dirname "$0")/mkbadzexe.py" "$B/hello.pzx" "$T/tests" >/dev/null
    chmod 755 "$T"/tests/hellopure* "$T"/tests/crashpure "$T"/tests/badzex2*
fi
mkdir -p "$T/bin" "$T/tmp"
for p in $PROGS; do
    name=$(echo "$p" | sed 's/^mkfs_/mkfs./; s/^fsck_/fsck./')    # mkfs_ext2 -> mkfs.ext2
    dir=bin; case $p in zup|zfsd|netd|wasabid|nfscd|fatd|ffsd|pfs3d) dir=sbin; mkdir -p "$T/sbin";; esac  # the updater (D15), the servers (D19)
    case $p in drv-*) dir=sbin/net; name=${p#drv-}; mkdir -p "$T/sbin/net";; esac   # the network's drivers (D19 step 7)
    cp "$B/$p.zx" "$T/$dir/$name"
    chmod 755 "$T/$dir/$name"
done
# the notices the licences of lwIP (BSD-3, in netd) and miniz (MIT, in gzip and zup) ask to travel with the programs
# (owner decision, DESIGN D19 2026-10-27): on every image that carries one of those programs; the base package
# carries them too (tools/mkzup.py)
lic() { mkdir -p "$T/usr/share/licenses"; cp "$(dirname "$0")/../$1" "$T/usr/share/licenses/$2"; }
case " $PROGS " in *" netd "*) lic net/lwip/COPYING lwIP;; esac
case " $PROGS " in *" gzip "*|*" zup "*) lic user/miniz/LICENSE miniz;; esac
case " $PROGS " in *" rz "*) lic user/xfer/LICENSE zmodem;; esac
# Sumi, Maki, Markdown, Ahiru (ports/PROG): the notices of the owner's MIT code in them, their own and the text-UI library's
# (named as their packages name them, tools/mkzup.py FAMILY_LICENSES: one file one package)
case " $PROGS " in *" sumi "*) lic ports/sumi/upstream/LICENSE sumi; lic ports/tui/upstream/LICENSE tui;; esac
case " $PROGS " in *" maki "*) lic ports/maki/upstream/LICENSE maki; lic ports/tui/upstream/LICENSE maki-tui;; esac
case " $PROGS " in *" markdown "*) lic ports/markdown/upstream/LICENSE markdown; lic ports/tui/upstream/LICENSE markdown-tui;; esac
case " $PROGS " in *" ahiru "*) lic ports/ahiru/upstream/LICENSE ahiru; lic ports/tui/upstream/LICENSE ahiru-tui;; esac
case " $PROGS " in *" ctop "*) lic ports/ctop/upstream/LICENSE ctop; lic ports/tui/upstream/LICENSE ctop-tui;; esac
# the C library's notice (picolibc, BSD-style licences file by file, DESIGN D27 3.2) on the full images, as the base
# package; and its test program in /tests
[ -z "$NOMAN" ] && lic libc/picolibc/COPYING.picolibc picolibc
if [ -z "$NOTESTS" ] && [ -f "$B/libctest.zx" ]; then cp "$B/libctest.zx" "$T/tests/libctest" && chmod 755 "$T/tests/libctest"; fi
if [ -z "$NOTESTS" ] && [ -f "$B/libctest.pzx" ]; then cp "$B/libctest.pzx" "$T/tests/libctestpure" && chmod 755 "$T/tests/libctestpure"; fi
# ZINE (DESIGN D29): its blob, the small AmigaOS /bin/zine loads (only on images with /bin/zine), and on test images
# the null blob and the test programs (AmigaOS hunk executables written for the purpose) in /tests/zine
if [ -f "$B/zine.blob" ] && [ -f "$T/bin/zine" ]; then mkdir -p "$T/lib/zine" && cp "$B/zine.blob" "$T/lib/zine/"; fi
if [ -z "$NOTESTS" ] && [ -f "$B/zine-null.blob" ] && [ -d "$B/zinetests" ]; then
    mkdir -p "$T/tests/zine" && cp "$B/zine-null.blob" "$T/tests/zine/null.blob" && { [ ! -f "$B/zine-trace.blob" ] || cp "$B/zine-trace.blob" "$T/tests/zine/trace.blob"; } && cp "$B"/zinetests/z[a-z]* "$T/tests/zine/"
    rm -f "$T"/tests/zine/*.o
    cp "$(dirname "$0")/tests/zine/null20" "$T/tests/zine/"
    cp "$B/zinetests/zexit" "$T/tests/zine/zexit-with-a-name-longer-than-31-bytes"   # the name in full (2026-10-31)
fi
# the text-UI library's test program (ports/tui) and the notice of the owner's MIT code in it, which travels with it
if [ -z "$NOTESTS" ] && [ -f "$B/tuitest.zx" ]; then
    cp "$B/tuitest.zx" "$T/tests/tuitest" && chmod 755 "$T/tests/tuitest" && lic ports/tui/upstream/LICENSE tui
fi
# the PC font (DESIGN D20 4.2) on the full images (those with manual pages), with its notice, as the base package;
# the Unicode font (DESIGN D28 6, made from the same font_8x8.c and Zenix's own glyphs) beside it
if [ -z "$NOMAN" ] && [ -f "$B/cp437-8x8.psf" ]; then
    mkdir -p "$T/usr/share/consolefonts"
    cp "$B/cp437-8x8.psf" "$T/usr/share/consolefonts/"
    lic tools/font_8x8.COPYING font_8x8
fi
if [ -z "$NOMAN" ] && [ -f "$B/zenix-8x8.zcf" ]; then
    mkdir -p "$T/usr/share/consolefonts"
    cp "$B/zenix-8x8.zcf" "$T/usr/share/consolefonts/"
fi
# the root certificates (DESIGN D21 8.1) and the owner's own folder for more, on the full images, as the base package
if [ -z "$NOMAN" ] && [ -f "$B/anchors" ]; then
    mkdir -p "$T/etc/ssl/local"
    cp "$B/anchors" "$T/etc/ssl/anchors"
    lic tools/certdata.COPYING certdata
fi
case " $PROGS " in *" ssh "*|*" wget "*|*" cryptotest "*) lic crypto/bearssl/LICENSE BearSSL; lic crypto/monocypher/LICENCE Monocypher;; esac
case " $PROGS " in *" ssh "*|*" ssh-keygen "*) lic crypto/openssh/LICENCE OpenSSH-bcrypt;; esac
if [ -z "$NOMAN" ]; then          # NOMAN=1: leave the manual pages out (small install / test images)
    mkdir -p "$T/usr/man"
    cp "$(dirname "$0")/../docs/man/"*.txt "$T/usr/man/"      # the manual pages live in docs/man
    python3 "$(dirname "$0")/mkman.py" "$T/usr/man.gen" $PROGS
    for f in "$T"/usr/man.gen/*.txt; do b=$(basename "$f"); [ -e "$T/usr/man/$b" ] || cp "$f" "$T/usr/man/$b"; done
    rm -rf "$T/usr/man.gen"
fi
# a second name for a program, only where the program is on this image (a small image leaves some out: no dead links)
alias_of() { [ -e "$T/bin/$1" ] && ln -s "$1" "$T/bin/$2"; true; }
alias_of less more                                                 # more is less
alias_of gzip gunzip; alias_of gzip zcat; alias_of chown chgrp; alias_of pgrep pkill; alias_of reboot bootamigaos
alias_of ifconfig ifup; alias_of ifconfig ifdown                   # ifup and ifdown are ifconfig (DESIGN D19 6.4)
alias_of rz sz                                                     # sz is rz (DESIGN D20 5)
alias_of unlha unlzx                                               # one program, by its name (DESIGN D33)
# the package database (DESIGN D15): the manifest of every package of this build (make packages) whose files this
# image holds exactly as the manifest says, so the first update knows what is installed (tools/mkzup.py installdb).
# PKGDB=1: the full images only (the Makefile sets it for zenix.hdf and root-card.img)
if [ -n "$PKGDB" ] && [ -f "$B/pkg/list" ]; then
    mkdir -p "$T/var/lib/zpkg" "$T/var/cache/zpkg"
    python3 "$(dirname "$0")/mkzup.py" installdb "$T" "$B/pkg" "$T/var/lib/zpkg"
fi
# no image ever ships a random seed (DESIGN D19 9.3): every copy of it (a card, the demo floppy) would start with the
# same one. Boot writes the machine's own at its first start.
if [ -e "$T/var/lib/random-seed" ]; then echo "mkroot.sh: refusing to put a random seed (var/lib/random-seed) on an image" >&2; exit 1; fi
rm -f "$TMPOUT"
# revision 0: the original ext2, 128-byte inodes, no optional features
# ownership and modes under fakeroot, so the image holds uid 0 (not whoever runs the build): root owns everything,
# /tmp is sticky and open to all, a normal user has a home, su and passwd are set-user-id root
mkdir -p "$T/home/user" "$T/home/guest"
fakeroot sh -c '
    chown -R 0:0 "$1"
    chown 1000:100 "$1/home/user"
    chown 1001:100 "$1/home/guest"
    chmod 1777 "$1/tmp"
    [ ! -d "$1/root" ] || chmod 700 "$1/root"
    chmod 4755 "$1/bin/su" "$1/bin/sudo" "$1/bin/passwd"
    [ ! -f "$1/bin/zine" ] || chmod 4755 "$1/bin/zine"     # root and the group zine (zine checks it)
    chmod 644 "$1/etc/passwd" "$1/etc/group" "$1/etc/svctab" "$1/etc/daemons"
    mke2fs -q -t ext2 -E revision=0 -b 1024 -N "$2" -d "$1" -F "$3" "$4"
' sh "$T" "$INODES" "$TMPOUT" "$SIZE"
e2fsck -fn "$TMPOUT" >/dev/null
mv -f "$TMPOUT" "$OUT"
echo "$OUT: ext2 root, $(echo $PROGS | wc -w) programs"
