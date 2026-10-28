#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The owner's card package (tools/mkcardpkg.sh, docs/card-package.md) tested before it goes to the owner:
#   writeroot  tools/write-root.sh on an image of the owner's dual-boot card (p1 FAT, p2 0x76 RDB with the boot
#              partition, p3 the Zenix root of 2026-10-24 built from the old commit with an owner's user, a password
#              and the /home line, p4 /home): p1, p2 and p4 byte for byte unchanged, p3 the new root with the owner's
#              settings carried (passwd, fstab, autologin, a file of their own), the saved copy, --restore puts the
#              old root back exactly; a card whose p2 is no RDB and a root bigger than p3 are refused, nothing written
#   card       FS-UAE, A1200 3.1, 2 MB chip + 8 MB fast, an IDE disk laid out as the card's Amiga side after the
#              README's step 1 (the AmigaDOS commands of docs/card-package.md read from the document and done on the
#              boot partition's files: the old BOOT: of 2026-10-24 first), the root writeroot made (shrunk to 96 MB:
#              the emulator's disk), /home: the menu's Zenix line starts the new kernel from Zenix/zenix; uname, the
#              owner's user, /home mounted, zup -l; then the README's step 4: the trial bundle installed (both
#              halves), confirmed and retired at the next start, and the menu's last line "Zenix (previous)" puts the
#              package's version back (kernel and root); the root e2fsck -fn clean after each start
#   dom        FS-UAE, the DoM image (tools/mkdom.sh) on a stock A1200 (68EC020, 2 MB chip RAM only), Kickstart 3.2
#              when ZENIX_KICK32 names one (else 3.1): the menu starts Zenix by itself; uname, / and /home (ide0p3)
#              mounted, sl0's settings, free; dom060: the same image on a 68060 with 8 MB fast
# usage: tools/cardpkg-test.sh [setup...]      (ZENIX_SERIAL, ZENIX_OUT, ZENIX_BUILD as the other tests; ZENIX_OLDREV the
#        commit of the owner's card, default f0cef81)
cd "$(dirname "$0")/.." || exit 1
TOP=$(pwd)
BD=${ZENIX_BUILD:-build}; mkdir -p "$BD"; B=$(cd "$BD" && pwd)
OUT=${ZENIX_OUT:-$(mktemp -d /tmp/zenix-cardpkg.XXXXXX)}; mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
SER=${ZENIX_SERIAL:-/tmp/zenix-cardpkg-serial}
OLDREV=${ZENIX_OLDREV:-f0cef81}
SETUPS="${*:-writeroot card dom dom060}"
KDIR=$(python3 -c 'import sys; sys.path.insert(0, "tools"); import fsuae_conf; print(fsuae_conf.kickstart_dir())')
K32=${ZENIX_KICK32:-$KDIR/kick32-a1200.rom}
PKG=$OUT/cardpkg
fail() { echo "FAIL $*"; exit 1; }
R=$OUT/results; : >"$R"
check() { if [ "$3" = ok ]; then echo "PASS $1: $2"; else echo "FAIL $1: $2"; fi | tee -a "$R"; }

# the package, as the owner gets it
BUILD=$B tools/mkcardpkg.sh "$PKG" >"$OUT/mkcardpkg.log" 2>&1 || { cat "$OUT/mkcardpkg.log"; fail "mkcardpkg"; }
VNUM=$(sed -n 's/.*Zenix [^ ]* (\([0-9]*\)) from.*/\1/p' "$OUT/mkcardpkg.log")
TV=$(sed -n 's/.*try\/zenix-update.zup ([^,]*, \([0-9]*\)).*/\1/p' "$OUT/mkcardpkg.log")
[ -n "$VNUM" ] && [ -n "$TV" ] || fail "cannot read the versions from mkcardpkg's output"
DOC=$TOP/docs/card-package.md

# the owner's card of 2026-10-24: the old tree, its build, its root as tools/mkdualcard.sh made it (1 GB, 16384 inodes)
old_card() {
    local o=$OUT/old
    [ -f $o/root.img ] && return 0
    rm -rf $o; mkdir -p $o/src
    git archive "$OLDREV" | tar -x -C $o/src || return 1
    local up; up=$(make -s -C $o/src -pn 2>/dev/null | sed -n 's/^UAPPS := //p' | head -1)
    make -s -C $o/src -j8 build/kernel.bin build/zenixboot build/bootmenu $(for p in $up; do echo build/$p.zx; done) \
        >$o/make.log 2>&1 || return 1
    $o/src/tools/mkroot.sh $o/root.img $o/src/build "$up" 1G 16384 >$o/mkroot.log 2>&1 || return 1
    # the owner's changes: a root password, a user of their own, the /home line (the emulator's disk: ide0p3; the
    # real card's is ide0p9), a file of their own in /etc
    debugfs -R 'cat /etc/passwd' $o/root.img 2>/dev/null >$o/passwd
    sed -i 's/^root::/root:$5$testsalt$testhash:/' $o/passwd
    echo 'owner::1002:100:Owner:/home/owner:/bin/sh' >>$o/passwd
    debugfs -R 'cat /etc/fstab' $o/root.img 2>/dev/null >$o/fstab
    echo 'ide0p3    /home   ext2     defaults' >>$o/fstab
    echo 'the owner wrote this' >$o/note
    debugfs -w -f - $o/root.img >/dev/null 2>&1 <<EOF
rm /etc/passwd
write $o/passwd /etc/passwd
sif /etc/passwd mode 0100644
rm /etc/fstab
write $o/fstab /etc/fstab
sif /etc/fstab mode 0100644
write $o/note /etc/owner-note
sif /etc/owner-note mode 0100644
EOF
    e2fsck -fn $o/root.img >/dev/null 2>&1 || return 1
    # its boot partition (tools/mkdualcard.sh's, with zenix.old as the owner's card has it)
    mkdir -p $o/boot/S
    printf 'SYS:bootmenu\n' >$o/boot/S/Startup-Sequence
    sed -n '/^AmigaOS rescue shell/q;p' "$PKG/amiga/zenixpkg/BootMenu.conf" >$o/boot/S/BootMenu.conf
    echo 'AmigaOS rescue shell = shell' >>$o/boot/S/BootMenu.conf
    cp $o/src/build/bootmenu $o/boot/bootmenu; cp $o/src/build/bootmenu $o/boot/bootzenix
    cp $o/src/build/zenixboot $o/boot/zenixboot
    cp $o/src/build/kernel.bin $o/boot/zenix; cp $o/src/build/kernel.bin $o/boot/zenix.old
    # /home, small
    mkdir -p $o/home/user $o/home/guest $o/home/owner; echo 'owner home' >$o/home/owner/file
    fakeroot sh -c 'chown -R 0:0 "$1"; chown 1000:100 "$1/user"; chown 1001:100 "$1/guest"; chown 1002:100 "$1/owner"
        mke2fs -q -t ext2 -E revision=0 -b 1024 -N 1024 -L home -d "$1" -F "$2" 16M' sh $o/home $o/home.img
}
ffs_from() {   # DIR OUT: an FFS boot volume of the files in DIR (33 cylinders, as the card's test disks)
    local args=() f
    while IFS= read -r f; do args+=("$1/$f=$f"); done < <(cd "$1" && find . -type f | sed 's#^\./##' | sort)
    python3 tools/mkffs.py "$2" 16632K BOOT "${args[@]}" >/dev/null
}
# the fake card: MBR p1 FAT (empty, 8 MB), p2 0x76 an RDB with BOOT, p3 the old root, p4 /home
fake_card() {   # OUT
    local o=$OUT/old c=$1
    ffs_from $o/boot $o/bootffs.img && python3 tools/mkrdb-multi.py $o/rdb.img $o/bootffs.img:ffs:BOOT:boot >/dev/null || return 1
    local s1=2048 l1=16384 s2 l2 s3 l3 s4 l4
    s2=$(( (s1 + l1 + 2047) / 2048 * 2048 )); l2=$(( ($(stat -c %s $o/rdb.img) + 511) / 512 ))
    s3=$(( (s2 + l2 + 2047) / 2048 * 2048 )); l3=$(( $(stat -c %s $o/root.img) / 512 ))
    s4=$(( (s3 + l3 + 2047) / 2048 * 2048 )); l4=$(( $(stat -c %s $o/home.img) / 512 ))
    rm -f $c; truncate -s $(( (s4 + l4) * 512 )) $c
    printf 'label: dos\n%s,%s,c\n%s,%s,%s\n%s,%s,83,*\n%s,%s,83\n' $s1 $l1 $s2 $l2 "${P2TYPE:-76}" $s3 $l3 $s4 $l4 | sfdisk -q $c || return 1
    dd if=/dev/urandom of=$c bs=512 seek=$s1 count=64 conv=notrunc status=none      # something in p1 to compare
    dd if=$o/rdb.img of=$c bs=512 seek=$s2 conv=notrunc status=none
    dd if=$o/root.img of=$c bs=1M oflag=seek_bytes seek=$((s3 * 512)) conv=sparse,notrunc status=none
    dd if=$o/home.img of=$c bs=1M oflag=seek_bytes seek=$((s4 * 512)) conv=sparse,notrunc status=none
    echo "$s1 $l1 $s2 $l2 $s3 $l3 $s4 $l4" >$c.layout
}
range_sum() { dd if="$1" bs=1M iflag=skip_bytes,count_bytes skip=$(($2 * 512)) count=$(($3 * 512)) status=none | sha256sum | cut -c1-64; }
range_get() { dd if="$1" of="$4" bs=1M iflag=skip_bytes,count_bytes skip=$(($2 * 512)) count=$(($3 * 512)) conv=sparse status=none; }

setup_writeroot() {
    local n=writeroot d=$OUT/writeroot
    rm -rf $d; mkdir -p $d
    old_card || { check $n "the old card (commit $OLDREV) built" bad; return; }
    fake_card $d/card.img || { check $n "the card image" bad; return; }
    read -r s1 l1 s2 l2 s3 l3 s4 l4 <$d/card.img.layout
    local p1 p2 p4 p3
    p1=$(range_sum $d/card.img $s1 $l1); p2=$(range_sum $d/card.img $s2 $l2); p4=$(range_sum $d/card.img $s4 $l4)
    p3=$(range_sum $d/card.img $s3 $l3)
    ZENIX_WRITE_ROOT_SELFTEST=1 bash tools/write-root.sh $d/card.img $PKG/pc/root.img $d/backups >$d/write.log 2>&1
    check $n "write-root ends with done" "$(grep -q '^write-root: done' $d/write.log && echo ok || echo bad)"
    check $n "partitions 1, 2 and 4 byte for byte unchanged" "$([ "$(range_sum $d/card.img $s1 $l1)" = $p1 ] && [ "$(range_sum $d/card.img $s2 $l2)" = $p2 ] && [ "$(range_sum $d/card.img $s4 $l4)" = $p4 ] && echo ok || echo bad)"
    range_get $d/card.img $s3 $l3 $d/p3.img
    check $n "partition 3: the new root, e2fsck -fn clean" "$(e2fsck -fn $d/p3.img >$d/e2fsck.log 2>&1 && debugfs -R 'stat /sbin/zup' $d/p3.img 2>/dev/null | grep -q '^Inode:' && echo ok || echo bad)"
    local pw; pw=$(debugfs -R 'cat /etc/passwd' $d/p3.img 2>/dev/null)
    check $n "the owner's password and user kept" "$(echo "$pw" | grep -q '^root:\$5\$testsalt\$testhash:0:0:' && echo "$pw" | grep -q '^owner::1002:100:' && echo "$pw" | grep -q '^user::1000:' && echo ok || echo bad)"
    check $n "the card's /home line, its autologin, the owner's own file, the new groups (wheel)" \
        "$(debugfs -R 'cat /etc/fstab' $d/p3.img 2>/dev/null | grep -q '^ide0p3 *[/]home' && [ "$(debugfs -R 'cat /etc/autologin' $d/p3.img 2>/dev/null)" = root ] && [ "$(debugfs -R 'cat /etc/owner-note' $d/p3.img 2>/dev/null)" = 'the owner wrote this' ] && debugfs -R 'cat /etc/group' $d/p3.img 2>/dev/null | grep -q '^wheel::10:' && echo ok || echo bad)"
    check $n "the new root's own network settings and package database" "$(debugfs -R 'cat /etc/network/interfaces' $d/p3.img 2>/dev/null | grep -q '^iface eth0 inet dhcp' && debugfs -R 'stat /var/lib/zpkg/base.manifest' $d/p3.img 2>/dev/null | grep -q '^Inode:' && echo ok || echo bad)"
    local bk; bk=$(ls $d/backups/zenix-root-*.img.zst 2>/dev/null | head -1)
    check $n "the old root saved, the same bytes" "$([ -n "$bk" ] && [ "$(zstd -dc $bk | sha256sum | cut -c1-64)" = $p3 ] && echo ok || echo bad)"
    ZENIX_WRITE_ROOT_SELFTEST=1 bash tools/write-root.sh $d/card.img --restore $bk >$d/restore.log 2>&1
    check $n "--restore: partition 3 the old root again, the rest unchanged" "$([ "$(range_sum $d/card.img $s3 $l3)" = $p3 ] && [ "$(range_sum $d/card.img $s2 $l2)" = $p2 ] && [ "$(range_sum $d/card.img $s4 $l4)" = $p4 ] && echo ok || echo bad)"
    # the merged root for the emulator: write again, keep p3, shrunk to the emulator disk's size
    ZENIX_WRITE_ROOT_SELFTEST=1 bash tools/write-root.sh $d/card.img $PKG/pc/root.img $d/backups >$d/write2.log 2>&1
    range_get $d/card.img $s3 $l3 $OUT/card-root.img
    e2fsck -fp $OUT/card-root.img >$d/fsck.log 2>&1; resize2fs $OUT/card-root.img 96M >$d/resize.log 2>&1 && truncate -s 96M $OUT/card-root.img
    # refusals: a card whose partition 2 is no RDB; a root bigger than partition 3
    P2TYPE=83 fake_card $d/bad.img; local before; before=$(sha256sum <$d/bad.img)
    ZENIX_WRITE_ROOT_SELFTEST=1 bash tools/write-root.sh $d/bad.img $PKG/pc/root.img $d/backups >$d/bad.log 2>&1
    check $n "a card whose partition 2 is no RDB: refused, nothing written" "$(grep -q 'not an Amiga RDB' $d/bad.log && [ "$(sha256sum <$d/bad.img)" = "$before" ] && echo ok || echo bad)"
    truncate -s 1100M $d/big.img; mke2fs -q -t ext2 -E revision=0 -F $d/big.img
    before=$(range_sum $d/card.img $s3 $l3)
    ZENIX_WRITE_ROOT_SELFTEST=1 bash tools/write-root.sh $d/card.img $d/big.img $d/backups >$d/big.log 2>&1
    check $n "a root bigger than partition 3: refused, nothing written" "$(grep -q 'does not fit partition 3' $d/big.log && [ "$(range_sum $d/card.img $s3 $l3)" = $before ] && echo ok || echo bad)"
    rm -f $d/card.img $d/bad.img $d/big.img $d/p3.img
}

# the README's AmigaDOS commands done on the files of a boot partition: BLOCK is the code block after the line
# matching HEADING (its Nth block); BOOT: is DIR, RAM:zenixpkg the package's amiga/zenixpkg
amigados() {   # HEADING N DIR
    local cmds; cmds=$(awk -v h="$1" -v want="$2" '
        index($0, h) == 1 { inh = 1; next }
        inh && /^## / { exit }
        inh && /^ *```/ { if (inb) { inb = 0; if (k == want) exit } else { inb = 1; k++ } ; next }
        inh && inb && k == want { sub(/^ +/, ""); print }' "$DOC")
    [ -n "$cmds" ] || { echo "amigados: no commands under $1" >&2; return 1; }
    map() { local p=$1; p=${p#BOOT:}; [ "$p" != "$1" ] && { echo "$3/$p"; return; }
        p=${1#RAM:zenixpkg/}; [ "$p" != "$1" ] && { echo "$PKG/amiga/zenixpkg/$p"; return; }; echo "BAD:$1"; }
    while read -r verb a b; do
        case "$verb" in
        Info|Type|bootzenix|"") ;;
        Copy) cp "$(map $a x $3)" "$(map $b x $3)" || return 1 ;;
        MakeDir) mkdir "$(map $a x $3)" || return 1 ;;
        Rename) mv "$(map $a x $3)" "$(map $b x $3)" || return 1 ;;
        Echo) local f=${a#>>}; local t=${b#\"}; t=${t%\"}; printf '%s\n' "$t" >>"$(map $f x $3)" || return 1 ;;
        *) echo "amigados: unknown command $verb" >&2; return 1 ;;
        esac
    done <<<"$cmds"
}
run_emu() {   # NAME RUN SECONDS SCRIPT CHIP FAST CPU KICK
    local d=$OUT/$1
    cat >$d/conf.fs-uae <<EOF
[fs-uae]
accuracy = 1
amiga_model = A1200
kickstart_file = $8
chip_memory = $5
fast_memory = $6
${7:+uae_cpu_model = $7}
floppy_drive_speed = 0
serial_port = /tmp/zenix-serial
uae_serial_direct = true
hard_drive_0 = $d/disk.hdf
hard_drive_0_controller = ide0
joystick_port_0 = none
joystick_port_1 = none
EOF
    ZENIX_SERIAL=$SER-$1 tools/run-test.sh $d/conf.fs-uae $3 $4 >$d/$2.log 2>&1
    tr -d '\r' <$d/$2.log | sed 's/\x1b\[[0-9;:]*[mK]//g' >$d/$2.txt
}
has() { grep -aqE -- "$3" $OUT/$1/$2.txt && echo ok || echo bad; }
inorder() {
    local f=$OUT/$1/$2.txt n=0 l; shift 2
    for re in "$@"; do l=$(grep -anE -- "$re" $f | awk -F: -v n=$n '$1 > n { print $1; exit }'); [ -n "$l" ] || { echo bad; return; }; n=$l; done
    echo ok
}
boot_get() { rm -rf $OUT/$1/boot; mkdir -p $OUT/$1/boot; python3 tools/ffsread.py $OUT/$1/disk.hdf --part ZB0 extract $OUT/$1/boot; }
boot_put() { ffs_from $OUT/$1/boot $OUT/$1/ffs.img && python3 tools/ffsread.py $OUT/$1/disk.hdf --part ZB0 load $OUT/$1/ffs.img; }
root_ok() { python3 tools/ffsread.py $OUT/$1/disk.hdf --part ZR0 dump $OUT/$1/root-$2.img && e2fsck -fn $OUT/$1/root-$2.img >$OUT/$1/e2fsck-$2.log 2>&1 && echo ok || echo bad; }
S_PROMPT='wait zenix:/# 600'

setup_card() {
    local n=card d=$OUT/card
    [ -f $OUT/card-root.img ] || setup_writeroot >/dev/null
    [ -f $OUT/card-root.img ] || { check $n "the card's root from writeroot" bad; return; }
    rm -rf $d; mkdir -p $d; cp -r $OUT/old/boot $d/boot
    amigados '## Step 1' 2 $d/boot || { check $n "the README's step 1 on the old boot partition" bad; return; }
    check $n "the README's step 1: Zenix/zenix, the new zenixboot and menu, the old ones kept, the menu's last line" \
        "$(cmp -s $d/boot/Zenix/zenix $B/kernel.bin && cmp -s $d/boot/zenixboot $B/zenixboot && cmp -s $d/boot/bootzenix $B/bootmenu && cmp -s $d/boot/zenixboot.pre-d15 $OUT/old/src/build/zenixboot && [ "$(tail -1 $d/boot/S/BootMenu.conf)" = 'Zenix (previous) = run SYS:zenixboot PREVIOUS' ] && cmp -s $d/boot/S/BootMenu.conf $PKG/amiga/zenixpkg/BootMenu.conf && echo ok || echo bad)"
    # the emulator has no DH0: the menu's default is the Zenix line (2) here; the owner chooses it by hand
    cp $d/boot/S/BootMenu.conf $d/owner.conf
    sed -i 's/^default 1$/default 2/; s/^timeout 5$/timeout 1/' $d/boot/S/BootMenu.conf
    ffs_from $d/boot $d/ffs.img || { check $n "the boot partition" bad; return; }
    python3 tools/mkrdb-multi.py $d/disk.hdf $d/ffs.img:ffs:ZB0:boot $OUT/card-root.img:ext2:ZR0:nomount $OUT/old/home.img:ext2:ZH0:nomount >/dev/null
    cat >$d/s1.txt <<EOF
$S_PROMPT
sleep 2
send uname; mount; grep -c owner /etc/passwd; cat /home/owner/file; /sbin/zup -l; free; echo c\x61rd1\r
wait (?m)^card1 120
stop
EOF
    run_emu $n 1 900 $d/s1.txt 2048 8192 "" '$ZENIX_KICKSTART_DIR/kicka1200.rom'
    check $n "the menu's Zenix line: zenixboot takes Zenix/zenix" "$(has $n 1 'Zenix/zenix')"
    check $n "the new kernel runs (uname: $VNUM)" "$(has $n 1 "^Zenix [^ ]+ \($VNUM\) m68k")"
    check $n "the owner's user, /home mounted with the owner's files" "$(grep -aqx 1 $d/1.txt && grep -aq '^owner home$' $d/1.txt && grep -aqE '/home' $d/1.txt && echo ok || echo bad)"
    check $n "zup -l: the packages of $VNUM" "$(has $n 1 "base.*$VNUM")"
    check $n "the root: e2fsck -fn clean" "$(root_ok $n 1)"
    # step 4: the trial bundle, as the README copies it
    boot_get $n
    amigados '## Step 4' 1 $d/boot || { check $n "the README's step 4 commands" bad; return; }
    boot_put $n
    cat >$d/s2.txt <<EOF
$S_PROMPT
sleep 2
send uname; /sbin/zup -l; echo c\x61rd2\r
wait (?m)^card2 120
send reboot zenix\r
wait (?m)^klog: boot [0-9]+, 300
$S_PROMPT
sleep 2
send uname; echo c\x61rd3\r
wait (?m)^card3 120
stop
EOF
    run_emu $n 2 1500 $d/s2.txt 2048 8192 "" '$ZENIX_KICKSTART_DIR/kicka1200.rom'
    check $n "the trial bundle: zenixboot installs its kernel, zup the root half" "$(inorder $n 2 "update: installing version $TV" 'renamed zenix.new to zenix' "^zup: installed Zenix .*\($TV\)" "^Zenix [^ ]+-try \($TV\) m68k")"
    check $n "the next start: confirmed, the bundle retired (.done)" "$(inorder $n 2 "version $TV came up" 'renamed zenix-update.zup to zenix-update.done' "^Zenix [^ ]+-try \($TV\) m68k")"
    check $n "the root: e2fsck -fn clean" "$(root_ok $n 2)"
    # the way back: the menu's last line (the owner picks it; here it is the default)
    boot_get $n
    check $n "the boot partition after the trial: zenix.old the package's kernel, the bundle .done" "$(cmp -s $d/boot/Zenix/zenix.old $B/kernel.bin && [ -f $d/boot/Zenix/zenix-update.done ] && echo ok || echo bad)"
    sed -i 's/^default 2$/default 4/' $d/boot/S/BootMenu.conf; boot_put $n
    cat >$d/s3.txt <<EOF
$S_PROMPT
sleep 2
send uname; /sbin/zup -l; grep -c owner /etc/passwd; echo c\x61rd4\r
wait (?m)^card4 120
stop
EOF
    run_emu $n 3 900 $d/s3.txt 2048 8192 "" '$ZENIX_KICKSTART_DIR/kicka1200.rom'
    check $n "Zenix (previous): both halves back to $VNUM" "$(inorder $n 3 'renamed zenix to zenix.rejected' 'renamed zenix.old to zenix' "rolled back to the version before $TV" "^Zenix [^ ]+ \($VNUM\) m68k")"
    check $n "... the owner's user still there" "$(grep -aqx 1 $d/3.txt && echo ok || echo bad)"
    check $n "the root: e2fsck -fn clean" "$(root_ok $n 3)"
    rm -f $d/root-*.img
}

setup_dom() {   # NAME CHIP FAST CPU
    local n=$1 d=$OUT/$1 k=$K32
    rm -rf $d; mkdir -p $d
    [ -f "$k" ] || k='$ZENIX_KICKSTART_DIR/kicka1200.rom'
    cp $PKG/dom/zenix-dom.img $d/disk.hdf
    cat >$d/s1.txt <<EOF
$S_PROMPT
sleep 2
send uname; mount; grep -c "^iface sl0" /etc/network/interfaces; free; echo d\x6fm1\r
wait (?m)^dom1 120
stop
EOF
    run_emu $n 1 900 $d/s1.txt $2 $3 "$4" "$k"
    check $n "the DoM's menu starts Zenix by itself ($(basename "$k"))" "$(inorder $n 1 'Zenix/zenix' "^Zenix [^ ]+ \($VNUM\) m68k")"
    check $n "/ and /home (ide0p3) mounted, sl0's settings there" "$(grep -aqE 'ide0p3.*/home|/home.*ide0p3' $d/1.txt && grep -aqx 1 $d/1.txt && echo ok || echo bad)"
    check $n "free answers" "$(has $n 1 'RAM free')"
    check $n "the root: e2fsck -fn clean" "$(root_ok $n 1)"
    rm -f $d/root-*.img $d/disk.hdf
}

for s in $SETUPS; do
    case $s in
    writeroot) setup_writeroot ;;
    card) setup_card ;;
    dom) setup_dom dom 2048 0 "" ;;
    dom060) setup_dom dom060 2048 8192 68060 ;;
    *) check $s "a known setup" bad ;;
    esac
done
echo "logs in $OUT"
grep -q '^FAIL' "$R" && exit 1
exit 0
