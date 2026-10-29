#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Build the owner's card package from this tree (DESIGN D15 step 6, docs/d15-self-updates.md; the instructions it
# carries are docs/card-package.md): one folder, never written anywhere but OUTDIR (default BUILD/cardpkg):
#   README.md            docs/card-package.md, headed by this package's version, commit, date and the space BOOT: needs
#   amiga/zenixpkg/      what goes onto the card's boot partition from AmigaOS (copied to RAM:zenixpkg first):
#       Zenix/zenix      the kernel, in D15's place BOOT:Zenix/ (zenixboot takes it from there and does updates)
#       zenixboot        this tree's zenixboot (D15's boot half; also the genet DMA stop of the warm start)
#       bootmenu         this tree's boot menu (the whole-area AllocAbs fix of D15 step 0), also for bootzenix
#       BootMenu.conf    the dual card's menu with the "Zenix (previous)" line after the Zenix line (for comparing)
#       try/zenix-update.zup   (unless --no-try) a trial bundle: this tree with the version number + 50 and the
#                        string + "-try", the base group only as make bundle (DESIGN D22 3.1; a bundle holds
#                        at most 16 packages), to try D15 on the real card (install, confirm, "Zenix (previous)"), and
#                        its signature try/zenix-update.zup.sig (DESIGN D21 6.4: zenixboot refuses it without)
#       licenses/        the notices of the code from other projects in zenixboot (BearSSL, Monocypher)
#   pc/root.img          the new Zenix root for the card's partition 3 (ext2, 1 GB, 16384 inodes, as
#                        tools/mkdualcard.sh made it), every program and the package database (so the next update's
#                        zup knows what is installed), /etc/network/interfaces for the CM4's Ethernet (eth0, DHCP,
#                        auto) and the serial line (sl0, by hand); written by tools/write-root.sh, which carries the
#                        card's own settings into it
#   dom/zenix-dom.img    the DoM image (tools/mkdom.sh: Zenix only, for the stock A1200 and the 060)
#   SHA256SUMS           of every file
# usage: tools/mkcardpkg.sh [--no-try] [--no-dom] [OUTDIR]      BUILD=DIR: the build directory (default build)
#        RELEASE=1: the owner's package (DESIGN D21 6.3): zenixboot trusts only tools/keys/zenix-release-*.pub and the
#        trial bundle carries the release channel "testing" and the release key's signature (SIGNKEY=, default
#        tools/zsign.py's, outside every repository). Without it the package is a TEST package: its zenixboot trusts
#        the TEST key of build/sign-test (made on this PC, gone with make clean) and its README says so first.
# Why a root image and not a bundle for the root: the card's root (from 2026-10-24) has no /sbin/zup, so the first D15
# update cannot install its root half (docs/d15-self-updates.md section 8, "the first D15 update"). Every update after
# this one is a bundle copied to BOOT:Zenix/.
set -euo pipefail
TRY=1 DOM=1
while [ $# -gt 0 ]; do case "$1" in --no-try) TRY=""; shift;; --no-dom) DOM=""; shift;; -*) sed -n '3,25p' "$0"; exit 1;; *) break;; esac; done
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
B=${BUILD:-$ROOT/build}; mkdir -p "$B"; B=$(cd "$B" && pwd)
OUT=${1:-$B/cardpkg}
die() { echo "mkcardpkg: $*" >&2; exit 1; }
case "$OUT" in /dev|/dev/*|/sys/*|/proc/*) die "$OUT: the package goes into a folder, never onto a device";; esac
[ -b "$OUT" ] && die "$OUT is a block device"
if [ -e "$OUT" ]; then
    [ -d "$OUT" ] || die "$OUT exists and is not a folder"
    [ -z "$(ls -A "$OUT")" ] || [ -f "$OUT/.zenix-cardpkg" ] || die "$OUT is not empty and not a card package: stopping"
    rm -rf "$OUT"
fi
mkdir -p "$OUT"/amiga/zenixpkg/Zenix "$OUT"/pc
touch "$OUT/.zenix-cardpkg"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mvar() { make -s -C "$ROOT" -pn 2>/dev/null | sed -n "s/^$1 := //p" | head -1; }
REL=()
case "${RELEASE:-}" in ""|1) ;; *) die "RELEASE=1 makes the owner's package; RELEASE=${RELEASE} means neither";; esac
if [ -n "${RELEASE:-}" ]; then
    REL=(RELEASE=1)
    [ -n "${SIGNKEY:-}" ] && REL+=(SIGNKEY="$SIGNKEY")
    SIGNWITH=(--key "${SIGNKEY:-$HOME/.config/zenix-signing/zenix-release.key}")
else
    TK=$(python3 "$HERE/zsign.py" testkey | cut -d' ' -f1); [ -n "$TK" ] || die "the test key"
    SIGNWITH=(--key "$TK")
fi

make -s -C "$ROOT" BUILD="$B" "${REL[@]}" "$B/root-card.img" "$B/bootmenu" "$B/zenixboot" "$B/kernel.bin" "$B/pkg/list" \
    "$B/zenix-update.zup" >"$T/make.log" 2>&1 || { tail -20 "$T/make.log"; die "the build failed"; }
UAPPS=$(mvar UAPPS); SBINAPPS=$(mvar SBINAPPS); NETDRVS=$(mvar NETDRVS)
[ -n "$UAPPS" ] || die "cannot read UAPPS from the Makefile"
PROGS="$UAPPS $SBINAPPS $(for d in $NETDRVS; do printf 'drv-%s ' "$d"; done)"
# the version the kernel carries (its ZVER record: the number and the string, kernel/version.h or ZVNUM/ZVSTR)
read -r VNUM VSTR < <(python3 -I -c '
import struct, sys
d = open(sys.argv[1], "rb").read(); i = d.find(b"ZVER")
if i < 0: sys.exit("no ZVER record in the kernel")
n = struct.unpack(">I", d[i + 4:i + 8])[0]; s = d[i + 8:i + 24].split(b"\0")[0].decode()
print(n, s)' "$B/kernel.bin") || die "cannot read the kernel's version"
COMMIT=$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)
DIRTY=$(git -C "$ROOT" status --porcelain --untracked-files=no 2>/dev/null | head -1)
[ -n "$DIRTY" ] && COMMIT="$COMMIT (with changes not committed)"

# the Amiga side
A=$OUT/amiga/zenixpkg
cp "$B/kernel.bin" "$A/Zenix/zenix"
cp "$B/zenixboot" "$A/zenixboot"
cp "$B/bootmenu" "$A/bootmenu"
mkdir -p "$A/licenses"
cp "$ROOT/crypto/bearssl/LICENSE" "$A/licenses/BearSSL"; cp "$ROOT/crypto/monocypher/LICENCE" "$A/licenses/Monocypher"
cat >"$A/BootMenu.conf" <<'EOF'
# S:BootMenu.conf - the boot menu (docs/man/bootmenu.txt). One entry per line: LABEL = partition NAME | run COMMAND
# | shell. "default N" picks entry N when the countdown runs out; "timeout S" is the countdown in seconds.
timeout 5
default 1
# background black
AmigaOS = partition DH0
Zenix = run SYS:zenixboot
AmigaOS rescue shell = shell
Zenix (previous) = run SYS:zenixboot PREVIOUS
EOF

# the trial bundle: the same tree, another version
TRYSIZE=0
if [ -n "$TRY" ]; then
    NN=$((VNUM % 100))
    [ $NN -lt 50 ] || die "the version $VNUM already counts $NN within its day: no room for a trial version (+50)"
    TV=$((VNUM + 50)) TS="$VSTR-try"
    [ ${#TS} -le 15 ] || TS="${VSTR:0:11}-try"
    TB=$B/cardpkg-try
    rm -rf "$TB"
    make -s -C "$ROOT" BUILD="$TB" "${REL[@]}" ZVNUM=$TV ZVSTR="$TS" "$TB/kernel.bin" "$TB/pkg/list" >"$T/make-try.log" 2>&1 ||
        { tail -20 "$T/make-try.log"; die "the trial build failed"; }
    mkdir -p "$A/try"
    python3 "$HERE/mkzup.py" bundle "$A/try/zenix-update.zup" --kernel "$TB/kernel.bin" ${RELEASE:+--channel testing} \
        --base-only "$TB/pkg" $(mvar BASEPKGS) >/dev/null
    python3 "$HERE/mkzup.py" check "$A/try/zenix-update.zup" >/dev/null || die "the trial bundle does not check"
    python3 "$HERE/zsign.py" sign "${SIGNWITH[@]}" "$A/try/zenix-update.zup" >/dev/null || die "the trial bundle cannot be signed"
    TRYSIZE=$(stat -c %s "$A/try/zenix-update.zup")
fi
KSIZE=$(stat -c %s "$B/kernel.bin")
BSIZE=$(stat -c %s "$B/zenix-update.zup"); [ "$TRYSIZE" -gt "$BSIZE" ] && BSIZE=$TRYSIZE
# BOOT: at the peak of an update: zenix, zenix.old, zenix.new, the bundle, the state file, the copies kept by the
# README (bootmenu.pre-d15, zenixboot.pre-d15, BootMenu.conf.pre-d15) and room for headers: 64 KB more
NEEDKB=$(( (3 * KSIZE + BSIZE + 2 * $(stat -c %s "$B/bootmenu") + $(stat -c %s "$B/zenixboot") + 65536 + 1023) / 1024 ))

# the PC side: the new root for the card's partition 3
cat >"$T/interfaces" <<'EOF'
# The network (docs/a1200-checklist-network.md, docs/network-serial.md).
# eth0: the CM4's Ethernet on the PiStorm, an address from the router, up at every start.
auto eth0
iface eth0 inet dhcp
    driver genet
# sl0: SLIP over the serial cable to zenix-slirp on the PC. No "auto" line: "ifup sl0" brings it up (until then cu
# has the serial port), "ifdown sl0" gives the port back.
iface sl0 inet static
    driver slip /dev/ttyS0 115200
    address 10.0.2.15/24
    gateway 10.0.2.2
EOF
PKGDB=1 "$HERE/mkroot.sh" "$OUT/pc/root.img" "$B" "$PROGS" 1G 16384 >"$T/mkroot.log" 2>&1 || { cat "$T/mkroot.log"; die "the root failed"; }
debugfs -w -f - "$OUT/pc/root.img" >/dev/null 2>&1 <<EOF
mkdir /etc/network
sif /etc/network uid 0
sif /etc/network gid 0
sif /etc/network mode 040755
write $T/interfaces /etc/network/interfaces
sif /etc/network/interfaces uid 0
sif /etc/network/interfaces gid 0
sif /etc/network/interfaces mode 0100644
EOF
debugfs -R "cat /etc/network/interfaces" "$OUT/pc/root.img" 2>/dev/null | cmp -s - "$T/interfaces" || die "/etc/network/interfaces did not arrive"
e2fsck -fn "$OUT/pc/root.img" >/dev/null 2>&1 || die "the new root is not clean (e2fsck -fn)"

# the DoM
[ -n "$DOM" ] && { mkdir -p "$OUT/dom"; BUILD="$B" "$HERE/mkdom.sh" "$OUT/dom/zenix-dom.img" >/dev/null; }

# the README: this package's facts, then the instructions
{
    echo "# Zenix card package: Zenix $VSTR ($VNUM)"
    echo
    [ -z "${RELEASE:-}" ] && echo "**A TEST PACKAGE: its zenixboot trusts this PC's test key, not the release key. Not for the owner's card.**" && echo
    echo "Built $(date +%Y-%m-%d) from commit $COMMIT. The boot partition (BOOT:) needs **$NEEDKB KB free** (\`Info BOOT:\`)."
    [ -n "$TRY" ] && echo "The trial bundle in \`amiga/zenixpkg/try/\` is Zenix $TS ($TV)."
    [ -n "$DOM" ] && echo "The DoM image is $(( $(stat -c %s "$OUT/dom/zenix-dom.img") / 1048576 )) MB: the DoM must be at least that big."
    echo
    sed -n '/^<!-- package -->$/,$p' "$ROOT/docs/card-package.md" | sed 1d
} >"$OUT/README.md"
(cd "$OUT" && find . -type f ! -name SHA256SUMS ! -name .zenix-cardpkg | sort | sed 's#^\./##' | xargs sha256sum >SHA256SUMS)
echo "$OUT: Zenix $VSTR ($VNUM) from $COMMIT"
[ -z "${RELEASE:-}" ] && echo "  a TEST package (the test key; RELEASE=1 makes the owner's): not for the owner's card"
echo "  amiga/zenixpkg: Zenix/zenix ($KSIZE bytes), zenixboot, bootmenu, BootMenu.conf${TRY:+, try/zenix-update.zup ($TS, $TV)}"
echo "  pc/root.img: the card's new root (1 GB ext2), for tools/write-root.sh"
[ -n "$DOM" ] && echo "  dom/zenix-dom.img: $(( $(stat -c %s "$OUT/dom/zenix-dom.img") / 1048576 )) MB"
echo "  BOOT: needs $NEEDKB KB free; README.md has the steps"
