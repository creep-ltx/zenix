#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The installer in FS-UAE (DESIGN D35), on image files only. A1200 with Kickstart 3.1; the dialogue typed through the serial line.
#   f  from the floppies onto an empty disk: questions answered wrong first (nothing written), the erase word; then a boot
#      with no floppy: the menu starts Zenix, login, the system's programs, zpkg knows the packages, /home mounted, a file
#      survives a restart; the host's e2fsck and amitools on what was written
#   a  beside an AmigaOS-style layout (an RDB with bootable DH0 and DH1, free cylinders): DH0 and DH1 byte-identical; the first boot
#      with no floppy: the menu's default starts DH0; with the menu's default changed on the image: Zenix, which reads DH0 by ffsd
#   m  a stock A1200, 2 MB of chip RAM alone: ext2, the optional disk skipped, the lowest free memory held to a floor; it boots
#   z  the same with a ZenixFS root: it boots with its kernel; the host's fsck
#   c  the Emu68 card's layout (an MBR, FAT, 0x76 with an RDB, a free slot): FAT and the RDB's partitions unchanged; started from
#      the kernel floppy as the note says, the new root is found in the new 0x76
#   r  the refusals as a person meets them: a disk with no room, a table with a bad checksum, wrong words: nothing written
#   n  from an existing Zenix: its root stays mounted on the same disk; the files through zpkg --fetch from a test server over
#      FS-UAE's A2065 into the free space; the new system boots, the old root unchanged
# usage: tools/zinstall-test.sh [PART...]   (ZENIX_SERIAL, ZENIX_OUT, ZENIX_BUILD as the other tests)
# Big images are emptied by truncate when a part ends; the output folder itself is check.sh's to clear.
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
OUT=${ZENIX_OUT:-/tmp/zenix-zinstall}; mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
SER=${ZENIX_SERIAL:-/tmp/zenix-zinstall-serial}
PARTS="${*:-f a m z c r n}"
make -s BUILD=$BD all install-floppies $BD/zenix.adf $BD/zenix-zfs.adf >"$OUT/make.log" 2>&1 || { tail "$OUT/make.log"; echo "FAIL the build"; exit 1; }
make -s -C zenixfs build/zenixfs-tool >/dev/null 2>&1 || { echo "FAIL the host tool"; exit 1; }
BD=$(cd "$BD" && pwd)
F=$BD/install
ND=$(ls $F/zenix-install-*.adf | wc -l)
Z=zenixfs/build/zenixfs-tool
CK="python3 -B tools/zinstall-check.py"
pass=1
check() { if [ "$2" = ok ]; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
yes_if() { if "$@" >/dev/null 2>&1; then echo ok; else echo bad; fi; }
clean() { tr -d '\r' | sed 's/\x1b\[[0-9;:]*[mK]//g'; }
has() { grep -aqE -- "$2" "$1" && echo ok || echo bad; }
hasnt() { grep -aqE -- "$2" "$1" && echo bad || echo ok; }
empty() { for x in "$@"; do truncate -s 0 "$x"; done; }
fsconf() {     # FILE DISK CHIP FAST NFLOPPY|kernel:FILE|none [EXTRA]
    {
        cat <<EOC
[fs-uae]
accuracy = 1
amiga_model = A1200
kickstart_file = \$ZENIX_KICKSTART_DIR/kicka1200.rom
chip_memory = $3
fast_memory = $4
floppy_drive_speed = 0
serial_port = /tmp/zenix-serial
uae_serial_direct = true
hard_drive_0 = $2
hard_drive_0_controller = ide0
hard_drive_0_type = rdb
joystick_port_0 = none
joystick_port_1 = none
EOC
        case $5 in
        none) ;;
        kernel:*) echo "floppy_drive_0 = ${5#kernel:}" ;;
        *) local nf=$5; [ "$nf" -gt "$ND" ] && nf=$ND; echo "floppy_drive_count = $nf"
           for ((i = 0; i < nf; i++)); do echo "floppy_drive_$i = $F/zenix-install-$((i + 1)).adf"; done ;;
        esac
        [ -z "$6" ] || echo "$6"
    } >"$1"
}
run() { ZENIX_SERIAL=$SER ZENIX_BUILD=$BD tools/run-test.sh "$1" "$2" "$3" 2>&1 | clean >"$4"; }
# the dialogue's end, shared: the install to its Done
INSTALLED='wait Done\. Zenix is on this disk|Stopped\. 1500'
boot_script() {    # FILE HOST USER USERPW ROOTPW: boot from the disk; the new user with a wrong password, then its own; root with its password
    cat >"$1" <<EOS
wait $2 login: 240
sleep 1
send $3\r
wait Password: 30
send wrong-one\r
wait Login incorrect 30
wait $2 login: 30
send $3\r
wait Password: 30
send $4\r
wait $2: 60
sleep 1
send id; hostname; echo zi-us""er-in\r
wait zi-user-in 30
send echo was-here > /home/$3/note; exit\r
wait $2 login: 60
send root\r
wait Password: 30
send $5\r
wait $2:/ 60
sleep 1
send echo zi-root-ok; uname -a\r
sleep 2
send mount\r
sleep 3
send df\r
sleep 3
send free\r
sleep 2
send ls /home /bin | wc -l\r
sleep 3
send /sbin/zup -l | wc -l\r
sleep 5
send zpkg -Q | grep -c .\r
sleep 8
send zpkg -Q base zinstall\r
sleep 6
send man zinstall | head -3\r
sleep 5
send ls -ld /tmp /home/$3; grep -c '^[a-z]*:\*:' /etc/passwd\r
sleep 3
send ls -l /bin/su\r
sleep 2
send echo on-root > /note; sync; echo zi-w""rote\r
wait zi-wrote 30
send reboot\r
wait $2 login: 240
sleep 1
send root\r
wait Password: 30
send $5\r
wait $2:/ 60
sleep 1
send cat /home/$3/note /note\r
sleep 3
send echo zi-se""cond\r
wait zi-second 30
send sync; umount /home; sync\r
sleep 3
stop
EOS
}
# the root's filesystem and /home, from the PC
host_fs() {    # NAME IMG ROOTNAME ext2|zenixfs
    local n=$1 i=$2 root=$3
    $CK cut $i $root $OUT/$n-root.bin
    if [ "$4" = zenixfs ]; then check "$n: the host's fsck finds the root clean" "$(yes_if $Z fsck $OUT/$n-root.bin)"
    else check "$n: the host's e2fsck finds the root clean" "$(yes_if e2fsck -fn $OUT/$n-root.bin)"; fi
    empty $OUT/$n-root.bin
    if [ -n "$($CK parts $i | grep '^ZH')" ]; then
        $CK cut $i "$($CK parts $i | grep '^ZH' | head -1 | cut -d' ' -f1)" $OUT/$n-home.bin
        check "$n: the host's e2fsck finds /home clean" "$(yes_if e2fsck -fn $OUT/$n-home.bin)"
        empty $OUT/$n-home.bin
    fi
}
boot_vol() {   # NAME IMG KERNEL: amitools on the boot partition
    local n=$1 i=$2 k=$3 zb
    zb=$($CK parts $i | grep '^ZB' | tail -1 | cut -d' ' -f1)
    $CK cut $i $zb $OUT/$n-boot.hdf
    if command -v xdftool >/dev/null; then
        xdftool $OUT/$n-boot.hdf read Zenix/zenix $OUT/$n-zenix >/dev/null 2>&1
        check "$n: amitools reads the kernel from the boot partition, the build's" "$(yes_if cmp $OUT/$n-zenix $k)"
        xdftool $OUT/$n-boot.hdf read zenixboot $OUT/$n-zb >/dev/null 2>&1
        check "$n: and zenixboot" "$(yes_if cmp $OUT/$n-zb $BD/zenixboot)"
        xdftool $OUT/$n-boot.hdf read bootmenu $OUT/$n-bm >/dev/null 2>&1
        check "$n: and the menu" "$(yes_if cmp $OUT/$n-bm $BD/bootmenu)"
        xdftool $OUT/$n-boot.hdf read S/BootMenu.conf $OUT/$n-conf >/dev/null 2>&1
    fi
    empty $OUT/$n-boot.hdf
}
booted() {    # NAME LOG ROOTTYPE HOST USER
    local n=$1 l=$2
    check "$n: the disk boots with no floppy to its login, with its name ($4)" "$(has $l "^$4 login:")"
    check "$n: a wrong password is refused" "$(has $l 'Login incorrect')"
    check "$n: the new user logs in with its password, is in users, wheel and zine" "$(grep -aqE "^uid=[0-9]+\\($5\\) gid=100\\(users\\) groups=.*wheel.*zine|^uid=[0-9]+\\($5\\) gid=100\\(users\\) groups=.*zine.*wheel" $l && echo ok || echo bad)"
    check "$n: hostname says $4" "$(has $l "^$4\$")"
    check "$n: root logs in with root's password" "$(has $l '^zi-root-ok')"
    check "$n: the shipped accounts are locked" "$(has $l '^2$')"
    check "$n: / is the new root ($3)" "$(has $l "^ide0p[0-9]+ +/ +$3")"
    check "$n: uname" "$(has $l '^Zenix ')"
    check "$n: man zinstall" "$(has $l 'ZINSTALL\(8\)')"
    check "$n: zpkg knows the packages" "$(has $l '^(base|zinstall) +[0-9]')"
    check "$n: set-user-id survived" "$(has $l '^-rwsr-xr-x')"
    check "$n: the user's home is its own, mode 700" "$(has $l "^drwx------ .* $5 +users .* /home/$5")"
    check "$n: a file on / and one in the user's home survive a restart" "$(grep -aq '^was-here$' $l && grep -aq '^on-root$' $l && echo ok || echo bad)"
    check "$n: no panic" "$(hasnt $l 'panic')"
}
# the accounts as options (unattended): the password files are made on the RAM disk first
ACC_PREP='echo rootpw2 > /tmp/rp; echo beapw2 > /tmp/up; echo zi-pr""ep'
ACC_OPTS='--hostname amiga2 --root-password-file /tmp/rp --user bea --user-password-file /tmp/up'
# ---- f: an empty disk ----
part_f() {
    local d=$OUT/f.hdf
    truncate -s 0 $d; truncate -s 300M $d
    fsconf $OUT/f.fs-uae $d 2048 8192 4
    cat >$OUT/f.txt <<EOS
wait zenix:/# 120
sleep 1
send cat /etc/motd\r
sleep 1
send zinstall --list\r
wait What could be done 30
sleep 2
send zinstall\r
wait Choose a number 30
send q\r
wait Nothing was changed 10
sleep 1
send zinstall\r
wait Choose a number 30
send 5\r
sleep 1
send \r
sleep 1
send 1\r
wait Choose .Return = 1, ext2 30
send \r
wait To erase the whole disk 30
send yes\r
wait Nothing was changed 10
sleep 1
send zinstall\r
wait Choose a number 30
send 1\r
wait Choose .Return = 1, ext2 30
send \r
wait To erase the whole disk 30
send ide0\r
wait Nothing was changed 10
sleep 1
send sync; zinstall\r
wait Choose a number 30
send 1\r
wait Choose .Return = 1, ext2 30
send \r
wait To erase the whole disk 30
send ERASE ide0\r
wait The machine's name 1500
send Bad_Name\r
wait 1-63 of 30
send amiga1\r
wait root's password 30
send rootpw1\r
wait Again: 30
send rootpw1\r
wait Your own login name 30
send Anna\r
wait 1-31 of 30
send anna\r
wait Your full name 30
send Anna Andersson\r
wait anna .nothing shows 30
send annapw1\r
wait Again: 30
send annapwX\r
wait were not the same 30
wait anna .nothing shows 30
send annapw1\r
wait Again: 30
send annapw1\r
$INSTALLED
sleep 2
send free\r
sleep 2
send echo zi-e""nd\r
wait zi-end 30
stop
EOS
    run $OUT/f.fs-uae 1800 $OUT/f.txt $OUT/f1.log
    check "f: the passwords were not shown as they were typed" "$(hasnt $OUT/f1.log 'annapw|rootpw|password .nothing shows as you type.: .')"
    check "f: a bad name and two passwords not the same are asked again" "$(has $OUT/f1.log 'were not the same')"
    check "f: the install says it is done" "$(has $OUT/f1.log '^Done\. Zenix is on this disk')"
    check "f: two wrong answers and a q wrote nothing" "$([ "$(grep -ac 'Nothing was changed' $OUT/f1.log)" = 3 ] && echo ok || echo bad)"
    check "f: it says what to do next" "$(has $OUT/f1.log 'What to do next')"
    grep -a 'Lowest free' $OUT/f1.log
    boot_vol f $d $BD/kernel.bin
    host_fs f $d ZR0 ext2
    fsconf $OUT/f2.fs-uae $d 2048 8192 none
    boot_script $OUT/f2.txt amiga1 anna annapw1 rootpw1
    run $OUT/f2.fs-uae 900 $OUT/f2.txt $OUT/f2.log
    booted f $OUT/f2.log ext2 amiga1 anna
    check "f: the full name is in /etc/passwd" "$($CK cut $d ZR0 $OUT/f-root.bin; debugfs -R 'cat /etc/passwd' $OUT/f-root.bin 2>/dev/null | grep -q '^anna:[^:][^:]*:[0-9]*:100:Anna Andersson:/home/anna:/bin/sh$' && echo ok || echo bad; truncate -s 0 $OUT/f-root.bin)"
    check "f: /home is the new partition" "$(has $OUT/f2.log '^ide0p3 +/home +ext2')"
    host_fs f $d ZR0 ext2
    empty $d
}

# ---- a: beside AmigaOS ----
grow_rdb() {   # IMG FREEMB: the same RDB, with free cylinders behind its partitions
    python3 -I - "$1" "$2" <<'PY'
import struct, sys
p, free = sys.argv[1], int(sys.argv[2])
f = open(p, "r+b")
b = list(struct.unpack(">128I", f.read(512)))
cyl = free * 2048 // 1008
tot = b[16] + cyl
b[16] = tot; b[20] = tot; b[24] = b[25] = b[26] = tot; b[35] = tot - 1
b[2] = 0; b[2] = (-sum(b[:b[1]])) & 0xFFFFFFFF
f.seek(0); f.write(struct.pack(">128I", *b))
f.truncate(tot * 1008 * 512)
PY
}
mk_aos() {   # IMG [FREEMB]: an RDB with DH0, DH1 as HDToolBox could leave them, and free cylinders behind
    local i=$1 free=${2:-300}
    printf 'SYS:zenixboot nosuchkernel\n' >$OUT/ss
    head -c 500000 /dev/urandom >$OUT/wbfile
    python3 tools/mkffs.py $OUT/dh0.img 32256K Workbench --intl $OUT/ss=S/Startup-Sequence $BD/zenixboot=zenixboot $OUT/wbfile=Prefs/wbfile >/dev/null
    python3 tools/mkffs.py $OUT/dh1.img 8064K Work --intl $OUT/wbfile=Data/wbfile >/dev/null
    python3 tools/mkrdb-multi.py $i $OUT/dh0.img:ffs3:DH0:boot $OUT/dh1.img:ffs3:DH1:- >/dev/null
    [ "$free" = 0 ] || grow_rdb $i $free
}
part_a() {
    local d=$OUT/a.hdf
    mk_aos $d
    $CK sums $d DH0 DH1 >$OUT/a.before
    $CK bytes $d 0 64 >$OUT/a.table
    fsconf $OUT/a.fs-uae $d 2048 8192 4
    cat >$OUT/a.txt <<EOS
wait zenix:/# 120
sleep 1
send $ACC_PREP\r
wait zi-prep 10
send zinstall $ACC_OPTS\r
wait Choose a number 30
send \r
wait MB .Return 30
send 100\r
wait Choose .Return = 1, ext2 30
send \r
wait Type yes to go on 30
send no\r
wait Nothing was changed 10
sleep 1
send sync; zinstall $ACC_OPTS\r
wait Choose a number 30
send \r
wait MB .Return 30
send \r
wait Choose .Return = 1, ext2 30
send \r
wait Type yes to go on 30
send yes\r
$INSTALLED
sleep 2
send echo zi-e""nd\r
wait zi-end 30
stop
EOS
    run $OUT/a.fs-uae 1800 $OUT/a.txt $OUT/a1.log
    check "a: the install says it is done" "$(has $OUT/a1.log '^Done\. Zenix is on this disk')"
    check "a: the plan names what it does not touch" "$(has $OUT/a1.log 'Not touched: DH0, DH1')"
    check "a: and the menu" "$(has $OUT/a1.log 'DH0 starts by itself after 5 seconds')"
    check "a: and the way back" "$(has $OUT/a1.log 'The way back')"
    $CK sums $d DH0 DH1 >$OUT/a.after
    check "a: DH0 and DH1 are byte-identical (their checksums)" "$(yes_if cmp $OUT/a.before $OUT/a.after)"
    boot_vol a $d $BD/kernel.bin
    host_fs a $d ZR0 ext2
    # the first boot: nothing pressed: the menu's default, AmigaOS (DH0's Startup-Sequence asks zenixboot for a kernel that is not there, and it says so on the serial line)
    fsconf $OUT/a2.fs-uae $d 2048 8192 none
    cat >$OUT/a2.txt <<'EOS'
wait nosuchkernel 120
sleep 2
stop
EOS
    run $OUT/a2.fs-uae 200 $OUT/a2.txt $OUT/a2.log
    check "a: at power-on the menu's default starts AmigaOS's DH0" "$(has $OUT/a2.log 'nosuchkernel')"
    # the menu's other choice, Zenix: the menu's keys cannot be typed through the serial line, so the default is changed on the image
    cp --sparse=always $d $OUT/a3.hdf
    python3 -I - $OUT/a3.hdf <<'PY'
import sys
f = open(sys.argv[1], "r+b")
d = f.read(1 << 30)
i = d.find(b"timeout 5\ndefault 1\n")
assert i > 0
f.seek(i + 18); f.write(b"2")
PY
    fsconf $OUT/a3.fs-uae $OUT/a3.hdf 2048 8192 none
    cat >$OUT/a3.txt <<'EOS'
wait amiga2 login: 240
sleep 1
send root\r
wait Password: 30
send rootpw2\r
wait amiga2:/ 60
sleep 1
send fdisk list\r
sleep 3
send mkdir /mnt/a; mount -t ffs ide0p1 /mnt/a; ls /mnt/a; sha256sum /mnt/a/Prefs/wbfile\r
sleep 15
send umount /mnt/a; echo zi-e""nd\r
wait zi-end 30
send sync\r
sleep 3
stop
EOS
    run $OUT/a3.fs-uae 600 $OUT/a3.txt $OUT/a3.log
    check "a: the menu's Zenix: a shell on the new root" "$(has $OUT/a3.log '^zi-end')"
    check "a: Zenix lists DH0, DH1" "$(has $OUT/a3.log 'ide0p1 +DH0')"
    check "a: Zenix reads AmigaOS's DH0" "$(has $OUT/a3.log "^$(sha256sum $OUT/wbfile | cut -d' ' -f1)")"
    $CK sums $OUT/a3.hdf DH1 >$OUT/a3.after
    check "a: DH1 still byte-identical after the boots" "$(grep -q "$(grep '^DH1' $OUT/a.before)" $OUT/a3.after && echo ok || echo bad)"
    # the way back: zinstall --undo in the new Zenix: the table as it was, every sector of it; AmigaOS starts directly, no menu
    cat >$OUT/a4.txt <<'EOS'
wait amiga2 login: 240
sleep 1
send root\r
wait Password: 30
send rootpw2\r
wait amiga2:/ 60
sleep 1
send zinstall --undo\r
wait Type undo 30
send no\r
wait Nothing was changed 10
sleep 1
send zinstall --undo\r
wait Type undo 30
send undo\r
wait the table is as it was 30
sleep 1
send sync\r
sleep 3
stop
EOS
    run $OUT/a3.fs-uae 400 $OUT/a4.txt $OUT/a4.log
    check "a: zinstall --undo says it is done" "$(has $OUT/a4.log 'the table is as it was')"
    check "a: the table is back, the first 64 sectors as before the install" "$([ "$($CK bytes $OUT/a3.hdf 0 64)" = "$(cat $OUT/a.table)" ] && echo ok || echo bad)"
    check "a: only DH0 and DH1 in it" "$([ "$($CK parts $OUT/a3.hdf | cut -d' ' -f1 | tr '\n' ' ')" = 'DH0 DH1 ' ] && echo ok || echo bad)"
    cat >$OUT/a5.txt <<'EOS'
wait nosuchkernel 120
sleep 1
stop
EOS
    fsconf $OUT/a5.fs-uae $OUT/a3.hdf 2048 8192 none
    run $OUT/a5.fs-uae 200 $OUT/a5.txt $OUT/a5.log
    check "a: and DH0 starts by itself" "$(has $OUT/a5.log 'nosuchkernel')"
    empty $OUT/a3.hdf $d
}

# ---- m, z: 2 MB of chip RAM alone ----
small() {   # NAME 1|2 ROOTTYPE MINKB
    local n=$1 d=$OUT/$1.hdf
    truncate -s 0 $d; truncate -s 300M $d
    fsconf $OUT/$n.fs-uae $d 2048 0 3
    cat >$OUT/$n.txt <<EOS
wait zenix:/# 180
sleep 1
send free\r
sleep 2
send $ACC_PREP\r
wait zi-prep 10
send zinstall $ACC_OPTS\r
wait Choose a number 60
send 1\r
wait Choose .Return = 1, ext2 60
send $2\r
wait To erase the whole disk 60
send ERASE ide0\r
wait (Done\. Zenix is on this disk|Stopped\.|Put install disk [0-9]) 2400
sleep 1
send skip\r
$INSTALLED
sleep 2
send free\r
sleep 2
send echo zi-e""nd\r
wait zi-end 30
stop
EOS
    run $OUT/$n.fs-uae 3000 $OUT/$n.txt $OUT/$n-1.log
    check "$n: the install says it is done" "$(has $OUT/$n-1.log '^Done\. Zenix is on this disk')"
    check "$n: optional packages left out, and said" "$(has $OUT/$n-1.log 'optional packages left out')"
    local low; low=$(grep -a 'Lowest free' $OUT/$n-1.log | sed 's/[^0-9]*\([0-9]*\) KB.*/\1/')
    echo "$n: lowest free memory while installing: ${low:-?} KB (chip RAM alone, 2 MB)"
    check "$n: the lowest free memory is at least $4 KB" "$([ "${low:-0}" -ge "$4" ] && echo ok || echo bad)"
    host_fs $n $d ZR0 $3
    fsconf $OUT/$n-2.fs-uae $d 2048 0 none
    boot_script $OUT/$n-2.txt amiga2 bea beapw2 rootpw2
    run $OUT/$n-2.fs-uae 900 $OUT/$n-2.txt $OUT/$n-2.log
    booted $n $OUT/$n-2.log $3 amiga2 bea
    grep -a -A1 'chip RAM free' $OUT/$n-2.log | head -2
    host_fs $n $d ZR0 $3
    empty $d
}
part_m() { small m "" ext2 150; }
part_z() { small z 2 zenixfs 100; }

# ---- c: the card's layout ----
part_c() {
    local d=$OUT/c.img
    mk_aos $OUT/c-rdb.hdf 0
    truncate -s 0 $d; truncate -s 600M $d
    head -c 100000 /dev/urandom | dd of=$d bs=512 seek=2048 conv=notrunc status=none
    head -c 100000 /dev/urandom | dd of=$d bs=512 seek=$((2048 + 60 * 2048)) conv=notrunc status=none
    local len=$(( $(stat -c %s $OUT/c-rdb.hdf) / 512 )) s2=$((2048 + 64 * 2048))
    dd if=$OUT/c-rdb.hdf of=$d bs=512 seek=$s2 conv=notrunc,sparse status=none
    sfdisk -q $d <<EOS
label: dos
2048,$((64 * 2048)),c,*
$s2,$len,76
EOS
    $CK sums $d slot0 slot1 DH0 DH1 >$OUT/c.before
    fsconf $OUT/c.fs-uae $d 2048 8192 4
    cat >$OUT/c.txt <<EOS
wait zenix:/# 120
sleep 1
send $ACC_PREP\r
wait zi-prep 10
send zinstall $ACC_OPTS\r
wait Choose a number 30
send \r
wait MB .Return 30
send \r
wait Choose .Return = 1, ext2 30
send \r
wait Type yes to go on 30
send yes\r
$INSTALLED
sleep 2
send echo zi-e""nd\r
wait zi-end 30
stop
EOS
    run $OUT/c.fs-uae 1800 $OUT/c.txt $OUT/c1.log
    check "c: the install says it is done" "$(has $OUT/c1.log '^Done\. Zenix is on this disk')"
    check "c: it says only Emu68 starts from such a table" "$(has $OUT/c1.log 'only Emu68')"
    check "c: and that the FAT partition is not touched" "$(has $OUT/c1.log 'FAT partition .* not touched')"
    $CK sums $d slot0 slot1 DH0 DH1 >$OUT/c.after
    check "c: the FAT partition, the 0x76 whole, DH0, DH1: byte-identical" "$(yes_if cmp $OUT/c.before $OUT/c.after)"
    check "c: the MBR's new entry is a 0x76" "$($CK parts $d | grep -q '^slot3 .* 76 ' && echo ok || echo bad)"
    boot_vol c $d $BD/kernel.bin
    host_fs c $d ZR0 ext2
    fsconf $OUT/c2.fs-uae $d 2048 8192 kernel:$BD/zenix.adf
    boot_script $OUT/c2.txt amiga2 bea beapw2 rootpw2
    run $OUT/c2.fs-uae 900 $OUT/c2.txt $OUT/c2.log
    booted c $OUT/c2.log ext2 amiga2 bea
    $CK sums $d slot0 slot1 DH0 DH1 >$OUT/c.after2
    check "c: unchanged after running on it" "$(yes_if cmp $OUT/c.before $OUT/c.after2)"
    empty $d
}

# ---- r: the refusals ----
part_r() {
    # a disk with no room and a bad table in one run each
    local d=$OUT/r1.hdf
    python3 tools/mkffs.py $OUT/r.img 32256K Full --intl >/dev/null
    python3 tools/mkrdb-multi.py $d $OUT/r.img:ffs3:DH0:boot >/dev/null
    cp $d $OUT/r1.orig
    fsconf $OUT/r1.fs-uae $d 2048 8192 1
    cat >$OUT/r1.txt <<'EOS'
wait zenix:/# 120
sleep 1
send zinstall\r
wait (nowhere|Where should Zenix go) 30
sleep 1
send q\r
sleep 2
send echo zi-e""nd\r
wait zi-end 30
stop
EOS
    run $OUT/r1.fs-uae 300 $OUT/r1.txt $OUT/r1.log
    check "r: a disk with no room (and 32 MB): DH0 can only be chosen by name, and q leaves it" "$(has $OUT/r1.log 'REPLACE partition ide0p1 DH0')"
    check "r: no free space offered" "$(hasnt $OUT/r1.log 'the free space')"
    check "r: the image is unchanged" "$(yes_if cmp $d $OUT/r1.orig)"
    # the wrong name for a partition; then a bad checksum in the table
    cp $OUT/r1.orig $d
    cat >$OUT/r2.txt <<'EOS'
wait zenix:/# 120
sleep 1
send zinstall\r
wait Choose a number 30
send \r
sleep 1
send 1\r
wait Choose .Return = 1, ext2 30
send \r
wait type its name exactly 30
send ide0p1\r
wait Nothing was changed 10
sleep 2
send echo zi-e""nd\r
wait zi-end 30
stop
EOS
    fsconf $OUT/r2.fs-uae $d 2048 8192 1
    run $OUT/r2.fs-uae 300 $OUT/r2.txt $OUT/r2.log
    check "r: Return alone does not choose a partition; a wrong name stops" "$(has $OUT/r2.log 'Nothing was changed')"
    check "r: the image is unchanged" "$(yes_if cmp $d $OUT/r1.orig)"
    # a bad checksum in a PART block
    python3 -I - $d <<'PY'
import sys
f = open(sys.argv[1], "r+b"); f.seek(512 + 100); f.write(b"\xAA")
PY
    cp $d $OUT/r3.orig
    cat >$OUT/r3.txt <<'EOS'
wait zenix:/# 120
sleep 1
send zinstall --list\r
sleep 3
send zinstall\r
wait (nowhere|Where should Zenix go) 30
sleep 1
send q\r
sleep 2
send echo zi-e""nd\r
wait zi-end 30
stop
EOS
    fsconf $OUT/r3.fs-uae $d 2048 8192 1
    run $OUT/r3.fs-uae 300 $OUT/r3.txt $OUT/r3.log
    check "r: a bad checksum: the table is called damaged" "$(has $OUT/r3.log 'cannot be changed: .*checksum')"
    check "r: the partition is not offered" "$(hasnt $OUT/r3.log 'REPLACE partition')"
    check "r: the image is unchanged" "$(yes_if cmp $d $OUT/r3.orig)"
    # the Pi's card with nothing free: FAT never, 0x76 never
    local m=$OUT/r4.img
    truncate -s 0 $m; truncate -s 256M $m
    mk_aos $OUT/r4-rdb.hdf 0
    local len=$(( $(stat -c %s $OUT/r4-rdb.hdf) / 512 )) rest
    dd if=$OUT/r4-rdb.hdf of=$m bs=512 seek=$((2048 + 64 * 2048)) conv=notrunc,sparse status=none
    rest=$(( $(stat -c %s $m) / 512 - 2048 - 64 * 2048 - len ))
    sfdisk -q $m <<EOS
label: dos
2048,$((64 * 2048)),c,*
$((2048 + 64 * 2048)),$len,76
$((2048 + 64 * 2048 + len)),$rest,83
EOS
    cp $m $OUT/r4.orig
    cat >$OUT/r4.txt <<'EOS'
wait zenix:/# 120
sleep 1
send zinstall --list\r
sleep 4
send echo zi-e""nd\r
wait zi-end 30
stop
EOS
    fsconf $OUT/r4.fs-uae $m 2048 8192 1
    run $OUT/r4.fs-uae 300 $OUT/r4.txt $OUT/r4.log
    check "r: the FAT partition is not offered" "$(has $OUT/r4.log 'ide0p1 is not offered: a FAT')"
    check "r: the 0x76 container is not offered" "$(has $OUT/r4.log 'ide0p2 is not offered: it holds')"
    check "r: the erase is not offered with a FAT partition" "$(has $OUT/r4.log 'erasing the disk is not offered: it has a FAT')"
    check "r: the image is unchanged" "$(yes_if cmp $m $OUT/r4.orig)"
    empty $OUT/r1.orig $OUT/r3.orig $OUT/r4.orig
}

# ---- n: from an existing Zenix, by zpkg ----
part_n() {
    command -v debugfs >/dev/null || { echo "FAIL debugfs"; pass=0; return; }
    unshare --map-root-user --map-auto -nm true 2>/dev/null || { echo "FAIL unprivileged namespaces"; pass=0; return; }
    local W=$OUT/www today KEY
    python3 tools/zsign.py testkey >/dev/null || { echo "FAIL the test key"; pass=0; return; }
    KEY=$(python3 tools/zsign.py testkey | cut -d' ' -f1)
    today=$(date +%Y%m%d)
    mkdir -p $W
    [ ! -e $W/r ] || mv $W/r $W/r.$$
    python3 tools/mkrepo.py add $W/r test $BD --key $KEY --date $today >$OUT/repo.log 2>&1 || { tail $OUT/repo.log; echo "FAIL the repository"; pass=0; return; }
    # the existing Zenix: an RDB with its root (ZX0, a copy of this build's) and free cylinders
    local d=$OUT/n.hdf rootimg=$OUT/n-root.img
    cp $BD/zenix.hdf $rootimg
    mkdir -p $OUT/nt
    printf 'auto eth0\niface eth0 inet dhcp\n    driver a2065\n' >$OUT/nt/interfaces
    debugfs -w -f - $rootimg >/dev/null 2>&1 <<EOS
mkdir /etc/network
sif /etc/network mode 040755
write $OUT/nt/interfaces /etc/network/interfaces
sif /etc/network/interfaces uid 0
sif /etc/network/interfaces gid 0
sif /etc/network/interfaces mode 0100644
EOS
    python3 tools/mkrdb-multi.py $d $rootimg:ext2:ZX0:nomount >/dev/null
    grow_rdb $d 200
    fsconf $OUT/n.fs-uae $d 2048 8192 kernel:$BD/zenix.adf "network_card = a2065"
    cat >$OUT/n.txt <<EOS
wait zenix:/# 180
sleep 1
send for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do ifconfig 2>/dev/null | grep -q 'inet 10.0.2.15' && break; sleep 1; done; printf 'channel test\nserver http://10.0.2.2:PORT/r\n' > /etc/zpkg.conf; ifconfig | head -3; echo zi-n""et\r
wait zi-net 60
send zinstall --list\r
sleep 4
send $ACC_PREP\r
wait zi-prep 10
send zinstall $ACC_OPTS\r
wait Files come from: the package server 30
wait Choose a number 30
send \r
wait MB .Return 30
send \r
wait Choose .Return = 1, ext2 30
send \r
wait Type yes to go on 30
send yes\r
$INSTALLED
sleep 2
send mount\r
sleep 3
send sync; echo zi-e""nd\r
wait zi-end 30
sleep 2
stop
EOS
    cat >$OUT/inner-n.sh <<EOS
#!/usr/bin/env bash
ip link set lo up
python3 -I tools/host/http-test-server.py $W $OUT/port-n & srv=\$!
for i in \$(seq 50); do [ -s $OUT/port-n ] && break; sleep 0.1; done
PORT=\$(head -1 $OUT/port-n)
sed -i "s/PORT/\$PORT/" $OUT/n.txt
ZENIX_SERIAL=$SER ZENIX_BUILD=$BD tools/run-test.sh $OUT/n.fs-uae 1800 $OUT/n.txt
rc=\$?
kill \$srv
exit \$rc
EOS
    chmod +x $OUT/inner-n.sh; : >$OUT/port-n
    $CK sums $d ZX0 >$OUT/n.before
    unshare --map-root-user --map-auto -nm $OUT/inner-n.sh 2>&1 | clean >$OUT/n1.log
    check "n: the existing Zenix: files from the server" "$(has $OUT/n1.log 'Files come from: the package server')"
    check "n: the running root is listed in use and not offered" "$(has $OUT/n1.log 'ide0p1 is not offered: it is in use')"
    check "n: the erase is not offered while running from the disk" "$(has $OUT/n1.log 'erasing the disk is not offered: something on this disk is in use')"
    check "n: the install says it is done" "$(has $OUT/n1.log '^Done\. Zenix is on this disk')"
    check "n: the system's bundle came through zpkg" "$(has $OUT/n1.log 'zpkg: zenix-[0-9]+\.zup')"
    check "n: the root it ran from is still in the table, same place" "$($CK parts $d | grep -q '^ZX0 ' && echo ok || echo bad)"
    boot_vol n $d $BD/kernel.bin
    host_fs n $d ZR0 ext2
    # the new one boots by itself, from its own boot partition, and is the one that comes up
    fsconf $OUT/n2.fs-uae $d 2048 8192 none
    boot_script $OUT/n2.txt amiga2 bea beapw2 rootpw2
    run $OUT/n2.fs-uae 900 $OUT/n2.txt $OUT/n2.log
    booted n $OUT/n2.log ext2 amiga2 bea
    check "n: the root is the new one, not the old" "$(has $OUT/n2.log '^ide0p3 +/ +ext2')"
    empty $rootimg $d
}

for p in $PARTS; do "part_$p"; done
[ $pass = 1 ] && echo "zinstall-test: all passed" || echo "zinstall-test: FAILED"
exit $((1 - pass))
