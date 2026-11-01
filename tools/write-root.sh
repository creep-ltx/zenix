#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Put a new Zenix root on the owner's dual-boot SD card (tools/mkdualcard.sh's layout: p1 Emu68's FAT, p2 0x76 the
# Amiga RDB with BOOT and AmigaOS, p3 0x83 the Zenix root, p4 0x83 Zenix's /home), once: the card's root is older than
# zup, so the first D15 update cannot install its root half (docs/d15-self-updates.md section 8, "the first D15
# update"; docs/card-package.md). Only p3 is written. p1, p2 (AmigaOS) and p4 (/home) are never touched.
#
# usage: sudo bash tools/write-root.sh /dev/sdX ROOT.img BACKUPDIR
#          1. checks the card (below), 2. saves the root now on it to BACKUPDIR/zenix-root-DATE.img.zst and checks the
#          copy, 3. carries the owner's settings from it into a copy of ROOT.img (tools/cardroot.py: users and
#          passwords, groups, fstab with the /home line, autologin, the random seed, any other file in /etc), 4.
#          writes that copy to p3, reads it back and compares, checks it with e2fsck -fn
#        sudo bash tools/write-root.sh /dev/sdX --restore BACKUPDIR/zenix-root-DATE.img.zst
#          puts a saved root back on p3 (the way back), reads it back and compares
# Safety: DEVICE must be a whole removable disk (or an mmcblk SD slot) of 1-64 GB, none of its partitions this PC's
# system; its partition table must be the dual card's (p2 type 0x76, p3 type 0x83 holding ext2); the new root must
# fit p3; a 5-second pause before the write (Ctrl-C stops it). Every read and write of p3 goes through the byte
# range the card's own partition table gives p3. Needs: python3, debugfs, e2fsck, zstd, cmp.
# ZENIX_WRITE_ROOT_ZENIXFS=1: ROOT.img may be a ZenixFS image (the owner's choice to try a ZenixFS root, DESIGN D34; never
# by itself): the same checks, the same saved copy and way back; the old root must be ext2. --restore takes back either kind.
# ZENIX_WRITE_ROOT_SELFTEST=1 with a regular file as DEVICE: the same steps on a card image (tools/cardpkg-test.sh),
# without the device checks and the pause.
set -euo pipefail
DEV="${1:-}"; NEW="${2:-}"; ARG3="${3:-}"
HERE="$(cd "$(dirname "$0")" && pwd)"
die() { echo "write-root: $*" >&2; exit 1; }
say() { echo "write-root: $*"; }
[ -n "$DEV" ] && [ -n "$NEW" ] && [ -n "$ARG3" ] ||
    die "usage: sudo bash tools/write-root.sh /dev/sdX ROOT.img BACKUPDIR | /dev/sdX --restore BACKUP.img.zst"
for t in python3 debugfs e2fsck zstd cmp; do command -v $t >/dev/null || die "needs $t"; done

TEST=""
if [ "${ZENIX_WRITE_ROOT_SELFTEST:-}" = 1 ] && [ -f "$DEV" ]; then
    TEST=1; SIZE=$(stat -c %s "$DEV")
else
    [ "$(id -u)" = 0 ] || die "run it with sudo"
    [ -b "$DEV" ] || die "$DEV is not a block device"
    NAME="$(basename "$(readlink -f "$DEV")")"
    [ -d "/sys/block/$NAME" ] || die "$DEV is a partition, not a whole disk: give the disk (e.g. /dev/sda, not /dev/sda3)"
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
fi

# the card's partition table: p2 must be the Amiga RDB, p3 the Zenix root (ext2); its byte range
read -r T2 T3 S3 N3 < <(python3 -I -c '
import struct, sys
f = open(sys.argv[1], "rb"); m = f.read(512)
if m[510:512] != b"\x55\xaa": print("x x 0 0"); sys.exit()
e = [struct.unpack("<B3sB3sII", m[446 + 16 * i:462 + 16 * i]) for i in range(4)]
print(e[1][2], e[2][2], e[2][4], e[2][5])' "$DEV")
[ "$T2" = 118 ] || die "$DEV: partition 2 is not an Amiga RDB (type 0x76): not the dual-boot card, stopping"
[ "$T3" = 131 ] || die "$DEV: partition 3 is not a Linux partition (type 0x83): not the dual-boot card, stopping"
OFF=$((S3 * 512)); LEN=$((N3 * 512))
[ $((OFF + LEN)) -le "$SIZE" ] || die "$DEV: partition 3 reaches past the end of the disk"
magic() { dd if="$1" bs=1 skip=$(($2 + 1080)) count=2 status=none | od -An -tx1 | tr -d ' \n'; }
zmagic() { dd if="$1" bs=1 skip=$(($2 + 32)) count=4 status=none; }
if [ "$(magic "$DEV" $OFF)" = 53ef ]; then P3=ext2
elif [ "$(zmagic "$DEV" $OFF)" = ZNFS ]; then P3=zenixfs
else die "$DEV: partition 3 holds neither ext2 nor ZenixFS: not the Zenix root, stopping"; fi
rd() {   # the byte range of p3 to stdout
    dd if="$DEV" bs=4M iflag=skip_bytes,count_bytes skip=$OFF count=$LEN status=none
}
wr() {   # stdin into the byte range of p3 (at most LEN bytes)
    dd of="$DEV" bs=4M iflag=fullblock oflag=seek_bytes seek=$OFF conv=notrunc,fsync status=progress
}
umount_p3() {
    [ -n "$TEST" ] && return 0
    local p="$DEV"; case "$NAME" in *[0-9]) p="${DEV}p";; esac
    if findmnt -rno SOURCE | grep -qx "${p}3"; then say "unmounting ${p}3 (mounted on this PC)"; umount "${p}3"; fi
}
owner() { [ -n "${SUDO_USER:-}" ] && chown "$SUDO_USER:" "$@" 2>/dev/null || true; }

echo "Card: $DEV, $SIZE bytes. Zenix root: partition 3, $((LEN / 1048576)) MB at byte $OFF."
[ -n "$TEST" ] || lsblk -o NAME,SIZE,TYPE,FSTYPE,LABEL,MOUNTPOINTS "$DEV" || true

if [ "$NEW" = --restore ]; then
    BK="$ARG3"; [ -f "$BK" ] || die "no backup $BK"
    BLEN=$(zstd -dc "$BK" | wc -c)
    [ "$BLEN" = "$LEN" ] || die "$BK holds $BLEN bytes, partition 3 is $LEN: not this card's root"
    umount_p3
    if [ -z "$TEST" ]; then echo "PUTTING THE SAVED ROOT BACK on partition 3 in 5 seconds (Ctrl-C to stop)."; sleep 5; fi
    zstd -dc "$BK" | wr
    say "checking what was written"
    cmp <(zstd -dc "$BK") <(rd) || die "partition 3 differs from $BK"
    sync
    say "done: the saved root is back on partition 3"
    exit 0
fi

[ -f "$NEW" ] || die "no new root $NEW"
BKD="$ARG3"; mkdir -p "$BKD"; owner "$BKD"
NLEN=$(stat -c %s "$NEW")
[ "$NLEN" -le "$LEN" ] || die "the new root ($NLEN bytes) does not fit partition 3 ($LEN bytes)"
[ "$P3" = ext2 ] || die "partition 3 holds ZenixFS: put the saved ext2 root back first (--restore)"
if [ "$(zmagic "$NEW" 0)" = ZNFS ]; then
    [ "${ZENIX_WRITE_ROOT_ZENIXFS:-}" = 1 ] || die "$NEW is a ZenixFS image: only with ZENIX_WRITE_ROOT_ZENIXFS=1, the owner's choice (docs/d34-zenixfs-root.md)"
    Z="$HERE/../zenixfs/build/zenixfs-tool"; [ -x "$Z" ] || die "needs $Z (make -C zenixfs build/zenixfs-tool)"
    "$Z" fsck "$NEW" >/dev/null 2>&1 || die "$NEW is not clean (zenixfs-tool fsck)"
    NEWFS=zenixfs
else
    [ "$(magic "$NEW" 0)" = 53ef ] || die "$NEW is not an ext2 image"
    e2fsck -fn "$NEW" >/dev/null 2>&1 || die "$NEW is not clean (e2fsck -fn)"
    NEWFS=ext2
fi
umount_p3

# 2. the root now on the card, saved and checked
STAMP=$(date +%Y%m%d-%H%M%S)
BK="$BKD/zenix-root-$STAMP.img.zst"
WORK=$(mktemp -d "$BKD/.write-root.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
say "saving the root now on the card to $BK"
rd | zstd -q -T0 -3 -o "$BK"
owner "$BK"
cmp <(zstd -dc "$BK") <(rd) || die "the saved copy differs from the card: stopping, nothing was written"
zstd -dc "$BK" | dd of="$WORK/old.img" bs=4M conv=sparse status=none
e2fsck -fn "$WORK/old.img" >"$WORK/old-e2fsck.log" 2>&1 || say "note: the old root is not clean (e2fsck -fn: $WORK/old-e2fsck.log); its settings are read all the same"
dumpe2fs -h "$WORK/old.img" 2>/dev/null | grep -E '^(Inode count|Free inodes|Last mount time|Last write time):' | sed 's/^/  old root: /' || true

# 3. the owner's settings carried into a copy of the new root
cp --sparse=always "$NEW" "$WORK/new.img"
python3 "$HERE/cardroot.py" carry "$WORK/old.img" "$WORK/new.img" || die "carrying the settings failed: nothing was written"

# 4. the write, read back, checked
if [ -z "$TEST" ]; then
    echo "WRITING THE NEW ZENIX ROOT to partition 3 in 5 seconds (Ctrl-C to stop). Partitions 1, 2 and 4 are not touched."
    sleep 5
fi
wr <"$WORK/new.img"
say "checking what was written"
cmp -n "$NLEN" "$WORK/new.img" <(rd) || die "partition 3 differs from the new root: put the old one back with --restore $BK"
if [ "$NEWFS" = zenixfs ]; then "$Z" fsck "$WORK/new.img" | tail -1; else e2fsck -fn "$WORK/new.img" | tail -1; fi
sync
[ "$NLEN" -lt "$LEN" ] && say "note: the new root is $((NLEN / 1048576)) MB, partition 3 $((LEN / 1048576)) MB: the rest stays unused"
say "done. The old root is saved in $BK (the way back: sudo bash tools/write-root.sh $DEV --restore $BK)"
