#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Make the disk image for the owner's IDE disk-on-module (DoM): Zenix only, for the stock A1200 (68EC020, 2 MB chip
# RAM) and the A1200 with the Apollo 1260 (68060). An RDB disk as the A1200's IDE port and Kickstart's scsi.device read
# it, no MBR:
#   ZB0  FFS (DOS\1), bootable: S/Startup-Sequence runs the boot menu (bootmenu: Zenix, Zenix (previous), the
#        Kickstart shell; Zenix after 3 s), zenixboot, the kernel as Zenix/zenix (DESIGN D15's place, so the same
#        updates work as on the card: a bundle copied to Zenix/zenix-update.zup)
#   ZR0  the Zenix root, ext2, every program and the package database (an update knows what is installed)
#   ZH0  Zenix's /home, ext2 (the homes of rootfs/home); the root's /etc/fstab mounts it
# Zenix names them ide0p1, ide0p2, ide0p3. ZR0 and ZH0 are marked "no mount" for AmigaOS. The root carries
# /etc/network/interfaces with the serial line (sl0, SLIP to zenix-slirp on the PC, docs/network-serial.md; brought up
# by hand with ifup sl0, so cu has the port first) and /etc/resolv.conf for zenix-slirp's DNS.
# Why Zenix only: the DoM is for the stock machine and the 060, which have no AmigaOS of the owner's on it; a dual-boot
# layout needs the owner's AmigaOS partitions, which only the card has (docs/card-package.md).
#
# usage: tools/mkdom.sh OUT.img [ROOTSIZE [HOMESIZE]]     defaults 112M and 64M: the image is about 193 MB, for a DoM
#        of 256 MB or more (the rest of a bigger DoM stays unused; Zenix's fdisk can add a partition there later)
# BUILD=DIR: the build directory (default build). Needs: make, python3, mke2fs, e2fsck, debugfs, fakeroot.
set -euo pipefail
[ $# -ge 1 ] || { sed -n '3,19p' "$0"; exit 1; }
OUT=$1 ROOTSIZE=${2:-112M} HOMESIZE=${3:-64M}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
B=${BUILD:-$ROOT/build}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mvar() { make -s -C "$ROOT" -pn 2>/dev/null | sed -n "s/^$1 := //p" | head -1; }

make -s -C "$ROOT" BUILD="$B" "$B/root-card.img" "$B/bootmenu" "$B/zenixboot" "$B/kernel.bin" "$B/pkg/list" >/dev/null
UAPPS=$(mvar UAPPS); SBINAPPS=$(mvar SBINAPPS); NETDRVS=$(mvar NETDRVS)
[ -n "$UAPPS" ] || { echo "mkdom: cannot read UAPPS from the Makefile" >&2; exit 1; }
PROGS="$UAPPS $SBINAPPS $(for d in $NETDRVS; do printf 'drv-%s ' "$d"; done)"

# the boot partition
printf 'SYS:bootmenu\n' >"$T/startup"
cat >"$T/conf" <<'EOF'
# S:BootMenu.conf - the boot menu (docs/man/bootmenu.txt) of the Zenix DoM.
# "Zenix (previous)" goes back to the version before the last update (zenixboot PREVIOUS).
timeout 3
default 1
Zenix = run SYS:zenixboot
Zenix (previous) = run SYS:zenixboot PREVIOUS
Kickstart shell = shell
EOF
python3 "$HERE/mkffs.py" "$T/boot.img" 16632K ZenixDoM "$T/startup=S/Startup-Sequence" "$T/conf=S/BootMenu.conf" \
    "$B/bootmenu=bootmenu" "$B/zenixboot=zenixboot" "$B/kernel.bin=Zenix/zenix" >/dev/null

# the root: every program, the package database, /home from ide0p3, the serial line's network settings
PKGDB=1 "$HERE/mkroot.sh" "$T/root.img" "$B" "$PROGS" "$ROOTSIZE" 8192 >"$T/mkroot.log" 2>&1 || { cat "$T/mkroot.log"; exit 1; }
cp "$ROOT/rootfs/etc/fstab" "$T/fstab"
printf 'ide0p3    /home   ext2     defaults\n' >>"$T/fstab"
cat >"$T/interfaces" <<'EOF'
# The network (docs/network-serial.md). sl0: SLIP over the serial cable to zenix-slirp on the PC. No "auto" line:
# "ifup sl0" brings it up (until then cu has the serial port), "ifdown sl0" gives the port back.
iface sl0 inet static
    driver slip /dev/ttyS0 115200
    address 10.0.2.15/24
    gateway 10.0.2.2
EOF
printf 'nameserver 10.0.2.3\n' >"$T/resolv.conf"
debugfs -w -f - "$T/root.img" >/dev/null 2>&1 <<EOF
rm /etc/fstab
write $T/fstab /etc/fstab
sif /etc/fstab uid 0
sif /etc/fstab gid 0
sif /etc/fstab mode 0100644
mkdir /etc/network
sif /etc/network uid 0
sif /etc/network gid 0
sif /etc/network mode 040755
write $T/interfaces /etc/network/interfaces
sif /etc/network/interfaces uid 0
sif /etc/network/interfaces gid 0
sif /etc/network/interfaces mode 0100644
write $T/resolv.conf /etc/resolv.conf
sif /etc/resolv.conf uid 0
sif /etc/resolv.conf gid 0
sif /etc/resolv.conf mode 0100644
EOF
for f in /etc/fstab /etc/network/interfaces /etc/resolv.conf; do
    debugfs -R "cat $f" "$T/root.img" 2>/dev/null | cmp -s - "$T/$(basename $f)" || { echo "mkdom: $f did not arrive" >&2; exit 1; }
done
e2fsck -fn "$T/root.img" >/dev/null 2>&1 || { echo "mkdom: the root is not clean" >&2; exit 1; }

# /home
mkdir -p "$T/home"
cp -r "$ROOT/rootfs/home/." "$T/home/"
mkdir -p "$T/home/user" "$T/home/guest"
fakeroot sh -c '
    chown -R 0:0 "$1"
    chown 1000:100 "$1/user"
    chown 1001:100 "$1/guest"
    mke2fs -q -t ext2 -E revision=0 -b 1024 -N 8192 -L home -d "$1" -F "$2" "$3"
' sh "$T/home" "$T/home.img" "$HOMESIZE"
e2fsck -fn "$T/home.img" >/dev/null 2>&1 || { echo "mkdom: /home is not clean" >&2; exit 1; }

python3 "$HERE/mkrdb-multi.py" "$OUT.tmp" "$T/boot.img:ffs:ZB0:boot" "$T/root.img:ext2:ZR0:nomount" \
    "$T/home.img:ext2:ZH0:nomount" >/dev/null
mv -f "$OUT.tmp" "$OUT"
echo "$OUT: $(( $(stat -c %s "$OUT") / 1048576 )) MB, RDB: ZB0 FFS boot (bootmenu, zenixboot, Zenix/zenix)," \
     "ZR0 Zenix root $ROOTSIZE, ZH0 /home $HOMESIZE (ide0p3)"
