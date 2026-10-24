#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Make the image of an Emu68 SD card that boots AmigaOS or Zenix from a boot menu (docs/man/bootmenu.txt):
#   MBR p1  type 0x0c  the Emu68 FAT partition, copied from FAT.img (the owner's own: Emu68, config.txt, Kickstart)
#       p2  type 0x76  an Amiga RDB, copied from AMIGA.img: its first partition is the boot partition (BOOT, FFS,
#                      bootable, the highest boot priority), filled here with bootmenu, zenixboot and the kernel; the
#                      other partitions (AmigaOS's) are kept as they are in AMIGA.img
#       p3  type 0x83  the Zenix root (ext2, every program), ROOTSIZE
#       p4  type 0x83  Zenix's /home (ext2, the homes of rootfs/home), HOMESIZE; /etc/fstab mounts it
# Zenix names these partitions ide0p1 (p1), ide0p2 (p2), then the RDB partitions one by one, then p3 and p4: the
# fstab line is written from the number of partitions in the RDB.
#
# usage: tools/mkdualcard.sh OUT.img FAT.img AMIGA.img [ROOTSIZE [HOMESIZE]]
#   AMIGA.img: an RDB disk (tools/rdbtool from amitools makes and fills one) whose partition 0 is BOOT, 4 MB cylinders
#   or any geometry; BOOT's size is read from the RDB. ROOTSIZE defaults to 1G, HOMESIZE to 3G.
# The image is sparse: holes where there is nothing to write. tools/write-card.sh writes it to a card.
# Needs: make (the Zenix build), python3, mke2fs, e2fsck, debugfs, rdbtool (amitools), sfdisk.
set -euo pipefail
[ $# -ge 3 ] || { sed -n '3,18p' "$0"; exit 1; }
OUT=$1 FAT=$2 AMIGA=$3 ROOTSIZE=${4:-1G} HOMESIZE=${5:-3G}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
B=${BUILD:-$ROOT/build}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

make -s -C "$ROOT" BUILD="$B" "$B/bootmenu" "$B/zenixboot" "$B/kernel.bin" >/dev/null
UAPPS=$(make -s -C "$ROOT" -pn 2>/dev/null | sed -n 's/^UAPPS := //p' | head -1)
[ -n "$UAPPS" ] || { echo "mkdualcard: cannot read UAPPS from the Makefile" >&2; exit 1; }
make -s -C "$ROOT" BUILD="$B" $(for p in $UAPPS; do echo "$B/$p.zx"; done) >/dev/null

# the RDB: the boot partition's place and size, and how many partitions there are
read -r NPART BLO BHI CYLBLK < <(rdbtool -r "$AMIGA" json | python3 -c '
import json, sys
d = json.load(sys.stdin)["rdb"]
parts = d["partitions"]
p0 = parts[0]
assert p0["name"] == "BOOT", "partition 0 of the RDB must be BOOT"
env = p0["dos_env"]
print(len(parts), env["low_cyl"], env["high_cyl"], env["surfaces"] * env["blk_per_trk"])')
BOOTSECT=$(( (BHI - BLO + 1) * CYLBLK ))

# the boot partition: the menu first, Zenix by zenixboot, AmigaOS by the next partition (normally DH0)
printf 'SYS:bootmenu\n' > "$T/startup"
cat > "$T/conf" <<'EOF'
# S:BootMenu.conf - the boot menu (docs/man/bootmenu.txt). One entry per line: LABEL = partition NAME | run COMMAND
# | shell. "default N" picks entry N when the countdown runs out; "timeout S" is the countdown in seconds.
timeout 5
default 1
AmigaOS = partition DH0
Zenix = run SYS:zenixboot
AmigaOS rescue shell = shell
EOF
python3 "$HERE/mkffs.py" "$T/boot.img" $((BOOTSECT * 512)) BOOT "$T/startup=S/Startup-Sequence" \
    "$T/conf=S/BootMenu.conf" "$B/bootmenu=bootmenu" "$B/bootmenu=bootzenix" "$B/zenixboot=zenixboot" "$B/kernel.bin=zenix" >/dev/null
cp --sparse=always "$AMIGA" "$T/amiga.img"
rdbtool "$T/amiga.img" import 0 "$T/boot.img" >/dev/null

# Zenix: the root, with /etc/fstab mounting /home, and /home itself
HOMEDEV=ide0p$((2 + NPART + 2))
"$HERE/mkroot.sh" "$T/root.img" "$B" "$UAPPS" "$ROOTSIZE" 16384 >/dev/null
cp "$ROOT/rootfs/etc/fstab" "$T/fstab"
printf '%s    /home   ext2     defaults\n' "$HOMEDEV" >> "$T/fstab"
debugfs -w -R "rm /etc/fstab" "$T/root.img" >/dev/null 2>&1
debugfs -w -f - "$T/root.img" >/dev/null 2>&1 <<EOF
write $T/fstab /etc/fstab
sif /etc/fstab uid 0
sif /etc/fstab gid 0
sif /etc/fstab mode 0100644
EOF
e2fsck -fn "$T/root.img" >/dev/null
mkdir -p "$T/home"
cp -r "$ROOT/rootfs/home/." "$T/home/"
mkdir -p "$T/home/user" "$T/home/guest"
fakeroot sh -c '
    chown -R 0:0 "$1"
    chown 1000:100 "$1/user"
    chown 1001:100 "$1/guest"
    mke2fs -q -t ext2 -E revision=0 -b 1024 -N 65536 -L home -d "$1" -F "$2" "$3"
' sh "$T/home" "$T/home.img" "$HOMESIZE"
e2fsck -fn "$T/home.img" >/dev/null

# the card: 1 MB-aligned partitions after one another
S1=2048
L1=$(( $(stat -c %s "$FAT") / 512 ))
S2=$(( (S1 + L1 + 2047) / 2048 * 2048 )); L2=$(( $(stat -c %s "$T/amiga.img") / 512 ))
S3=$(( (S2 + L2 + 2047) / 2048 * 2048 )); L3=$(( $(stat -c %s "$T/root.img") / 512 ))
S4=$(( (S3 + L3 + 2047) / 2048 * 2048 )); L4=$(( $(stat -c %s "$T/home.img") / 512 ))
rm -f "$OUT"
truncate -s $(( (S4 + L4) * 512 )) "$OUT"
sfdisk -q "$OUT" <<EOF
label: dos
$S1,$L1,c
$S2,$L2,76
$S3,$L3,83,*
$S4,$L4,83
EOF
put() { dd if="$1" of="$OUT" bs=1M seek=$(( $2 * 512 )) oflag=seek_bytes conv=sparse,notrunc status=none; }
put "$FAT" $S1
put "$T/amiga.img" $S2
put "$T/root.img" $S3
put "$T/home.img" $S4
echo "$OUT: $(( (S4 + L4) / 2048 )) MB: p1 Emu68 FAT, p2 Amiga RDB ($NPART partitions, BOOT with the menu)," \
     "p3 Zenix root $ROOTSIZE, p4 Zenix /home $HOMESIZE (Zenix: $HOMEDEV)"
