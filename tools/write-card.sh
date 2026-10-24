#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Write the Zenix SD card for an Emu68 Amiga (PiStorm), so that it boots Zenix with no floppy:
#   p1 FAT32, 512 MB  Emu68's files (from EMU68-FOLDER, or the ones already on the card)
#   p2 type 0x76      an Amiga RDB with the 16 MB FFS boot partition: S:Startup-Sequence runs zenixboot
#   p3 type 0x83      the Zenix root, ext2, 512 MB, every program
# The partitions come from build/zenix-card.img (tools/mkcard.py; built here first if it is missing).
#
# usage: sudo bash tools/write-card.sh /dev/sdX [EMU68-FOLDER]
#   With EMU68-FOLDER (the folder with config.txt, start4.elf, the Emu68 file and kick.rom): the WHOLE card is
#   erased and p1 is made new with those files.
#   Without it: the card must already have Emu68's FAT partition where this layout puts it (sector 8192, 512 MB, as on
#   the Zenix test card made 2026-10-23); it is kept as it is, and everything after it is replaced.
#   sudo bash tools/write-card.sh /dev/sdX --whole IMAGE
#   Write a whole card image, every partition including p1 and the partition table, byte for byte (the card made by
#   tools/mkdualcard.sh, which carries the owner's own Emu68 partition). Everything on the card is replaced.
# Safety: only the device named is touched; it must be a whole removable disk (or an mmcblk SD slot) of 1-64 GB, none
# of its partitions may hold this PC's system, and there is a 5-second pause to stop it (Ctrl-C).
set -euo pipefail
DEV="${1:-}"; EMU="${2:-}"; WHOLE=""
TOP="$(cd "$(dirname "$0")/.." && pwd)"
IMG="$TOP/build/zenix-card.img"
if [ "$EMU" = --whole ]; then WHOLE="${3:-}"; EMU=""; IMG="$WHOLE"; fi
die() { echo "write-card: $*" >&2; exit 1; }

[ -n "$DEV" ] || die "usage: sudo bash tools/write-card.sh /dev/sdX [EMU68-FOLDER]"
[ "$(id -u)" = 0 ] || die "run it with sudo"
[ -b "$DEV" ] || die "$DEV is not a block device"
NAME="$(basename "$(readlink -f "$DEV")")"
[ -d "/sys/block/$NAME" ] || die "$DEV is a partition, not a whole disk: give the disk (e.g. /dev/sda, not /dev/sda1)"
case "$NAME" in
mmcblk*) ;;
*) [ "$(cat "/sys/block/$NAME/removable")" = 1 ] || die "$DEV is not a removable disk: stopping" ;;
esac
SIZE=$(blockdev --getsize64 "$DEV")
if [ "$SIZE" -lt 1000000000 ] || [ "$SIZE" -gt 64000000000 ]; then die "$DEV is $SIZE bytes, not an SD card of 1-64 GB: stopping"; fi
while read -r src tgt; do
    case "$src" in "$DEV"*|/dev/"$NAME"*)
        case "$tgt" in /|/boot|/boot/*|/home|/usr|/var|/efi) die "$src is mounted on $tgt: that is this PC's system disk";; esac ;;
    esac
done < <(findmnt -rno SOURCE,TARGET)
if [ "${2:-}" = --whole ]; then
    [ -n "$WHOLE" ] && [ -f "$WHOLE" ] || die "--whole needs the image file"
    LEN=$(stat -c %s "$WHOLE")
    P="$DEV"; case "$NAME" in *[0-9]) P="${DEV}p";; esac
    [ "$LEN" -le "$SIZE" ] || die "$DEV ($SIZE bytes) is too small for $WHOLE ($LEN bytes)"
    echo "Card: $DEV, $SIZE bytes. Now on it:"
    lsblk -o NAME,SIZE,TYPE,FSTYPE,LABEL,MOUNTPOINTS "$DEV" || true
    echo "ERASING THE WHOLE CARD in 5 seconds (Ctrl-C to stop): writing $WHOLE ($((LEN / 1048576)) MB) onto it."
    sleep 5
    for p in "$P"?*; do [ -b "$p" ] && umount "$p" 2>/dev/null || true; done
    dd if="$WHOLE" of="$DEV" bs=4M conv=fsync status=progress
    blockdev --rereadpt "$DEV" || true; sleep 2; udevadm settle || true
    for p in "$P"?*; do [ -b "$p" ] && umount "$p" 2>/dev/null || true; done
    echo "write-card: checking what was written"
    cmp -n "$LEN" "$WHOLE" "$DEV" || die "the card differs from the image"
    for p in "${P}3" "${P}4"; do [ -b "$p" ] && e2fsck -fn "$p" | tail -1; done
    sync
    echo "Done. Partitions:"; sfdisk -l "$DEV" | tail -5
    exit 0
fi
if [ -n "$EMU" ]; then
    [ -d "$EMU" ] || die "$EMU is not a folder"
    [ -f "$EMU/config.txt" ] && [ -f "$EMU/start4.elf" ] || die "$EMU does not look like Emu68's files (no config.txt or start4.elf)"
fi
P="$DEV"; case "$NAME" in *[0-9]) P="${DEV}p";; esac         # /dev/sda1, but /dev/mmcblk0p1

# the image: built by the user who ran sudo, so that build/ stays theirs
if [ ! -f "$IMG" ]; then
    U="${SUDO_USER:-}"; [ -n "$U" ] || die "$IMG is missing: run make build/zenix-card.img first"
    echo "write-card: building $IMG first (as $U)"
    sudo -u "$U" env HOME="$(getent passwd "$U" | cut -d: -f6)" make -C "$TOP" -s build/zenix-card.img
fi

# the layout, read from the image's partition table: start and size of p1-p3, in sectors
read -r S1 N1 S2 N2 S3 N3 < <(python3 -I -c '
import struct, sys
m = open(sys.argv[1], "rb").read(512)
print(*[v for i in range(3) for v in struct.unpack("<II", m[454 + 16 * i:462 + 16 * i])])' "$IMG")
[ $(( (S3 + N3) * 512 )) -le "$SIZE" ] || die "$DEV is too small for the image"
if [ -z "$EMU" ]; then
    read -r CS1 CN1 CT1 < <(python3 -I -c '
import struct, sys
m = open(sys.argv[1], "rb").read(512)
t = m[450] if m[510:512] == b"\x55\xaa" else 0
print(*struct.unpack("<II", m[454:462]), t)' "$DEV")
    if [ "$CS1" != "$S1" ] || [ "$CN1" != "$N1" ] || { [ "$CT1" != 11 ] && [ "$CT1" != 12 ]; }; then
        die "the card's first partition is not Emu68's FAT partition at sector $S1 ($((N1 / 2048)) MB): give the Emu68 folder as the second argument to make the whole card new"
    fi
fi

echo "Card: $DEV, $SIZE bytes. Now on it:"
lsblk -o NAME,SIZE,TYPE,FSTYPE,LABEL,MOUNTPOINTS "$DEV" || true
if [ -n "$EMU" ]; then
    echo "ERASING THE WHOLE CARD in 5 seconds (Ctrl-C to stop): p1 new with the files from $EMU, then the Zenix partitions."
else
    echo "Keeping p1 (Emu68's files); replacing everything after it with the Zenix partitions in 5 seconds (Ctrl-C to stop)."
fi
sleep 5
for p in "$P"?*; do [ -b "$p" ] && umount "$p" 2>/dev/null || true; done

# the partition table: the image's, with the card's own disk signature kept when p1 is kept
python3 -I -c '
import sys
img, dev, keep = sys.argv[1], sys.argv[2], sys.argv[3] == "1"
m = bytearray(open(img, "rb").read(512))
with open(dev, "r+b") as f:
    if keep:
        old = f.read(512)
        m[440:446] = old[440:446]
        f.seek(0)
    f.write(m)
' "$IMG" "$DEV" "$([ -z "$EMU" ] && echo 1 || echo 0)"
[ -z "$EMU" ] || dd if=/dev/zero of="$DEV" bs=1M seek=$((S1 / 2048)) count=1 conv=fsync status=none   # no old FAT left in p1
copy() {   # start sectors: the same place in the image and on the card
    dd if="$IMG" of="$DEV" bs=4M iflag=skip_bytes,count_bytes oflag=seek_bytes skip=$(($1 * 512)) seek=$(($1 * 512)) \
        count=$(($2 * 512)) conv=fsync status=progress
}
echo "write-card: p2, the boot partition (RDB + FFS, $((N2 / 2048)) MB)"; copy "$S2" "$N2"
echo "write-card: p3, the Zenix root (ext2, $((N3 / 2048)) MB)"; copy "$S3" "$N3"
blockdev --rereadpt "$DEV" || true; sleep 2; udevadm settle || true
for p in "$P"?*; do [ -b "$p" ] && umount "$p" 2>/dev/null || true; done

if [ -n "$EMU" ]; then
    echo "write-card: p1, Emu68's files"
    mkfs.vfat -F 32 -n EMU68 "${P}1" >/dev/null
    M="$(mktemp -d)"; mount "${P}1" "$M"; cp -r "$EMU"/. "$M"/; sync; ls "$M"; umount "$M"; rmdir "$M"
fi

echo "write-card: checking what was written"
same() { cmp -s <(dd if="$IMG" bs=4M iflag=skip_bytes,count_bytes skip=$(($1 * 512)) count=$(($2 * 512)) status=none) \
                <(dd if="$DEV" bs=4M iflag=skip_bytes,count_bytes skip=$(($1 * 512)) count=$(($2 * 512)) status=none); }
same "$S2" "$N2" || die "p2 on the card differs from the image"
same "$S3" "$N3" || die "p3 on the card differs from the image"
e2fsck -fn "${P}3" | tail -1
sync
echo "Done. Partitions:"; sfdisk -l "$DEV" | tail -4
echo "Put the card in the Amiga and switch it on: Kickstart boots the Zenix boot partition, Zenix starts by itself."
