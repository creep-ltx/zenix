#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The installer in FS-UAE (DESIGN D35), on image files only. A1200 with Kickstart 3.1; the dialogue typed through the serial line.
#   f  from the floppies onto an empty disk: questions answered wrong first (nothing written), the erase word; then a boot
#      with no floppy: the menu starts Zenix, login, the system's programs, zpkg knows the packages, /home mounted, a file
#      survives a restart; the host's e2fsck and amitools on what was written
#   a  beside an AmigaOS-style layout (an RDB with bootable DH0 and DH1, free cylinders): DH0 and DH1 byte-identical; the first boot
#      with no floppy: the menu's default starts DH0; with the menu's default changed on the image: Zenix, which reads DH0 by ffsd
#   m  the machine Zenix is made for (CLAUDE.md, 2026-11-04): a stock A1200 (68EC020) with 2 MB chip and 4 MB fast RAM: ext2,
#      the optional disk skipped, the lowest free memory held to a floor; it boots
#   z  the same with a ZenixFS root: it boots with its kernel; the host's fsck
#   b  best effort: 2 MB of chip RAM alone, ext2 on one partition: what it finds is said (NOTE lines), never a FAIL
#   c  the Emu68 card's layout (an MBR, FAT, 0x76 with an RDB, a free slot): FAT and the RDB's partitions unchanged; started from
#      the kernel floppy as the note says, the new root is found in the new 0x76
#   r  the refusals as a person meets them: a disk with no room, a table with a bad checksum, wrong words: nothing written
#   o  a card of the owner's kind (16 GB, PFS3 in the RDB): DH2 deleted in the partition step, Zenix in its place; DH0, DH1, the LSEG
#      chain the same; it boots and the user logs in; then zinstall --undo from the floppy gives the old table back, every sector
#   k  a disk with Zenix already: reinstall keeping /home and the users (the user's file is still there, the user logs in), then
#      reinstall erasing /home (the users kept, the file gone)
#   h  no floppy: from AmigaOS on a 68060 (zenixboot, the kernel and the RAM disk with the set, as files on DH0); --list, the install
#   n  from an existing Zenix: its root stays mounted on the same disk; the files through zpkg --fetch from a test server over
#      FS-UAE's A2065 into the free space; the new system boots, the old root unchanged
# usage: tools/zinstall-test.sh [PART...]   (ZENIX_SERIAL, ZENIX_OUT, ZENIX_BUILD as the other tests)
# Big images are emptied by truncate when a part ends; the output folder itself is check.sh's to clear.
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
OUT=${ZENIX_OUT:-/tmp/zenix-zinstall}; mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
SER=${ZENIX_SERIAL:-/tmp/zenix-zinstall-serial}
PARTS="${*:-f a m z b c r n o h k}"
make -s BUILD=$BD all install-floppies $BD/zenix.adf $BD/zenix-zfs.adf >"$OUT/make.log" 2>&1 || { tail "$OUT/make.log"; echo "FAIL the build"; exit 1; }
make -s -C zenixfs build/zenixfs-tool >/dev/null 2>&1 || { echo "FAIL the host tool"; exit 1; }
BD=$(cd "$BD" && pwd)
F=$BD/install
ND=$(ls $F/zenix-install-*.adf | wc -l)
Z=zenixfs/build/zenixfs-tool
CK="python3 -B tools/zinstall-check.py"
pass=1
BEST=
check() { if [ "$2" = ok ]; then echo "PASS $1"
          elif [ -n "$BEST" ]; then echo "NOTE (best effort, not a failure) $1"
          else echo "FAIL $1"; pass=0; fi; }
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
send zinstall --list; echo zi-li""st\r
wait zi-list 60
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
        if [ "$($CK parts $i | grep '^ZH' | head -1 | cut -d' ' -f4)" = 5a4e4601 ]; then check "$n: the host's fsck finds /home (ZenixFS) clean" "$(yes_if $Z fsck $OUT/$n-home.bin)"
        else check "$n: the host's e2fsck finds /home clean" "$(yes_if e2fsck -fn $OUT/$n-home.bin)"; fi
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
    check "$n: the disk server reads the table whole (no note about it)" "$(hasnt $l 'diskd: note: the partition table')"
    check "$n: --list on the running Zenix says so and offers nothing" "$(has $l 'Zenix is installed here .* and running from it now')"
    check "$n: --list's lines all fit 79 columns" "$(sed -n '/^The disk ide0/,/running from it now/p' $l | awk 'length($0) > 79 { bad = 1 } END { print bad ? "bad" : "ok" }')"
}
# the accounts as options (unattended): the password files are made on the RAM disk first
ACC_PREP='echo rootpw2 > /tmp/rp; echo beapw2 > /tmp/up; echo zi-pr""ep'
ACC_OPTS='--layout recommended --home-fs ext2 --hostname amiga2 --root-password-file /tmp/rp --user bea --user-password-file /tmp/up'
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
wait What zinstall could do here 30
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
wait Choose .Return = 1.: 30
send \r
wait Choose .Return = 1, ext2 30
send \r
wait Choose .Return = 1, ext2 30
send \r
wait To erase the whole disk 30
send yes\r
wait Nothing was changed 10
sleep 1
send zinstall\r
wait Choose a number 30
send 1\r
wait Choose .Return = 1.: 30
send 2\r
wait Choose .Return = 1, ext2 30
send \r
wait To erase the whole disk 30
send ide0\r
wait Nothing was changed 10
sleep 1
send sync; zinstall\r
wait Choose a number 30
send 1\r
wait Choose .Return = 1.: 30
send 3\r
wait Size of the system part 30
send 10\r
wait Between 32 and 30
send 200\r
wait Size for your files 30
send \r
wait Choose .Return = 1, ext2 30
send \r
wait filesystem for your files 30
wait Choose .Return = 1, ext2 30
send 2\r
wait To erase the whole disk 30
send ERASE ide0\r
wait The computer's name 1500
send Bad_Name\r
wait Use only a-z, 0-9 and -, at most 63 30
send amiga1\r
wait A password for root 30
send rootpw1\r
wait Type it again to check: 30
send rootpw1\r
wait Your user name for logging in 30
send Anna\r
wait Use only a-z, 0-9, _ and -, at most 31 30
send anna\r
wait Your full name 30
send Anna Andersson\r
wait A password for anna 30
send annapw1\r
wait Type it again to check: 30
send annapwX\r
wait did not match 30
wait A password for anna 30
send annapw1\r
wait Type it again to check: 30
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
    check "f: the passwords were not shown as they were typed" "$(hasnt $OUT/f1.log 'annapw|rootpw|shows as you type.: .')"
    check "f: a bad name and two passwords not the same are asked again" "$(has $OUT/f1.log 'did not match')"
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
    check "f: the plan showed the layout's sizes: root 200 MB, /home on ZenixFS" "$(grep -aq 'New ZR0 *200 MB' $OUT/f1.log && grep -aq '/home (ZenixFS)' $OUT/f1.log && echo ok || echo bad)"
    check "f: /home is the new partition, ZenixFS" "$(has $OUT/f2.log '^ide0p3 +/home +zenixfs')"
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

# ---- m, z: the stock A1200 with 4 MB of fast RAM; b: 2 MB of chip RAM alone (best effort) ----
small() {   # NAME 1|2 ROOTTYPE MINKB FASTKB [MORE OPTIONS]
    local n=$1 d=$OUT/$1.hdf
    truncate -s 0 $d; truncate -s 300M $d
    fsconf $OUT/$n.fs-uae $d 2048 $5 3
    cat >$OUT/$n.txt <<EOS
wait zenix:/# 180
sleep 1
send free\r
sleep 2
send $ACC_PREP\r
wait zi-prep 10
send zinstall $ACC_OPTS $6\r
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
    check "$n: optional packages left out, and said" "$(has $OUT/$n-1.log 'extra packages were left out')"
    local low; low=$(grep -a 'Lowest free' $OUT/$n-1.log | sed 's/[^0-9]*\([0-9]*\) KB.*/\1/')
    echo "$n: lowest free memory while installing: ${low:-?} KB (2 MB chip, $5 KB fast)"
    check "$n: the lowest free memory is at least $4 KB" "$([ "${low:-0}" -ge "$4" ] && echo ok || echo bad)"
    host_fs $n $d ZR0 $3
    fsconf $OUT/$n-2.fs-uae $d 2048 $5 none
    boot_script $OUT/$n-2.txt amiga2 bea beapw2 rootpw2
    run $OUT/$n-2.fs-uae 900 $OUT/$n-2.txt $OUT/$n-2.log
    booted $n $OUT/$n-2.log $3 amiga2 bea
    grep -a -A1 'chip RAM free' $OUT/$n-2.log | head -2
    grep -a 'Not enough memory here' $OUT/$n-1.log | head -1
    host_fs $n $d ZR0 $3
    empty $d
}
part_m() {    # one partition: the root only, /home a folder on it
    small m "" ext2 2000 4096 "--layout one"
    check "m: the filesystem was checked after the copy" "$(hasnt $OUT/m-1.log 'Not enough memory here')"
    check "m: one partition: no /home partition, the plan says /home is a folder" "$(grep -aq '/home is a folder on the root' $OUT/m-1.log && ! grep -aqE '^ide0p[0-9]+ +/home ' $OUT/m-2.log && echo ok || echo bad)"
}
part_z() { small z 2 zenixfs 2000 4096; }
part_b() {    # best effort (CLAUDE.md 2026-11-04): 2 MB of chip RAM alone; fsck.ext2 does not fit beside zinstall there and is left for later
    BEST=1
    small b "" ext2 150 0 "--layout one"
    check "b: the check left for later is said" "$(has $OUT/b-1.log 'Not enough memory here to check')"
    BEST=
}

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
wait MB .Return 30
send \r
wait Choose .Return = 1.: 30
send 2\r
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
    check "r: a bad checksum: the table is called damaged" "$(has $OUT/r3.log 'will not change this disk.s partition table: .*checksum')"
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
wait Zenix will be copied from the package server 30
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
    check "n: the existing Zenix: files from the server" "$(has $OUT/n1.log 'Zenix will be copied from the package server')"
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

# ---- o: the owner's kind of card: 16 GB, PFS3 in the RDB, DH2 split into Zenix and a new empty DH2; then --undo ----
part_o() {
    local d=$OUT/o.hdf
    truncate -s 0 $d
    python3 -I - $d <<'PY'
import hashlib, struct, sys
p = sys.argv[1]
MB = 2048
s = 16 * 1024 * MB; cb = 128 * 64; cyls = s // cb
def blk(L):
    L = list(L) + [0] * (128 - len(L)); L[2] = 0; L[2] = (-sum(L[:L[1]])) & 0xFFFFFFFF
    return struct.pack(">128I", *L)
f = open(p, "wb"); f.truncate(s * 512)
R = [0x5244534B, 64, 0, 7, 512, 0x12, 0xFFFFFFFF, 123, 1, 0xFFFFFFFF] + [0xFFFFFFFF] * 6
R += [cyls, 64, 128, 0, cyls, 0, 0, 0, cyls, cyls, cyls, 0, 0, 0, 0, 0, 0, 16383, 2, cyls - 1, cb, 0, 125]
f.seek(0); f.write(blk(R))
# a filesystem in the RDB whose dostype no partition has: Kickstart loads nothing of it (the data is no program)
f.seek(512); f.write(blk([0x46534844, 64, 0, 7, 0xFFFFFFFF, 0, 0, 0, 0x50465309, 0x13002] + [0] * 8 + [2]))
for i in range(2, 123):
    f.seek(i * 512); f.write(blk([0x4C534547, 64, 0, 7, i + 1 if i < 122 else 0xFFFFFFFF] + [i] * 100))
def part(name, nxt, lo, hi, flags):
    nm = bytes([len(name)]) + name.encode() + b"\0" * (31 - len(name))
    L = [0x50415254, 64, 0, 7, nxt, flags, 0, 0, 0] + list(struct.unpack(">8I", nm)) + [0] * 15
    L += [16, 128, 0, 128, 1, 64, 2, 0, 0, lo, hi, 80, 0, 0xFFFFFF, 0xFFFFFFFE, 0, 0x50465303]
    return blk(L)
# PFS3 partitions with no PFS3 here: no mount (flags 2), as the test machine has no handler for them
for k, (nm, lo, hi) in enumerate([("DH0", 2, 257), ("DH1", 258, 769), ("DH2", 770, cyls - 1)]):     # 1 GB, 2 GB, the rest
    f.seek((123 + k) * 512); f.write(part(nm, 124 + k if k < 2 else 0xFFFFFFFF, lo, hi, 2))
for nm, lo, n in (("dh0", 2, 256), ("dh1", 258, 512)):
    f.seek(lo * cb * 512); f.write(hashlib.sha256(nm.encode()).digest() * 2048)
PY
    $CK sums $d DH0 DH1 >$OUT/o.before
    $CK bytes $d 1 124 >$OUT/o.rdb
    $CK bytes $d 0 256 >$OUT/o.table
    fsconf $OUT/o.fs-uae $d 2048 8192 4
    cat >$OUT/o.txt <<EOS
wait zenix:/# 120
sleep 1
send $ACC_PREP\r
wait zi-prep 10
send zinstall --delete DH2 $ACC_OPTS --layout custom --root-size 200 --home-size 300\r
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
    run $OUT/o.fs-uae 1800 $OUT/o.txt $OUT/o1.log
    check "o: the install says it is done" "$(has $OUT/o1.log '^Done\. Zenix is on this disk')"
    check "o: the plan says DH2 is deleted, what is on it lost" "$(has $OUT/o1.log 'Partition DH2 .* is DELETED')"
    $CK sums $d DH0 DH1 >$OUT/o.after
    check "o: DH0 and DH1 byte-identical" "$(yes_if cmp $OUT/o.before $OUT/o.after)"
    check "o: the FileSysHeader, its 121 LSEG blocks, DH0's and DH1's PART blocks: byte-identical" "$([ "$($CK bytes $d 1 124)" = "$(cat $OUT/o.rdb)" ] && echo ok || echo bad)"
    $CK parts $d >$OUT/o.parts
    check "o: the table: DH0, DH1, then Zenix's three where DH2 was" "$([ "$(cut -d' ' -f1 $OUT/o.parts | tr '\n' ' ')" = 'DH0 DH1 ZB0 ZR0 ZH0 ' ] && echo ok || echo bad)"
    # it starts, and the new user logs in
    fsconf $OUT/o3.fs-uae $d 2048 8192 none
    boot_script $OUT/o3.txt amiga2 bea beapw2 rootpw2
    run $OUT/o3.fs-uae 900 $OUT/o3.txt $OUT/o3.log
    booted o $OUT/o3.log ext2 amiga2 bea
    host_fs o $d ZR0 ext2
    # the way back, from the floppy: the copy of the table in the new root
    cat >$OUT/o2.txt <<'EOS'
wait zenix:/# 120
sleep 1
send mkdir -p /mnt/x; mount -t ext2 ide0p4 /mnt/x; zinstall --undo /mnt/x/var/lib/zinstall/table-before\r
wait Type undo 30
send undo\r
wait the table is as it was 30
sleep 1
send umount /mnt/x; sync; echo zi-e""nd\r
wait zi-end 30
stop
EOS
    run $OUT/o.fs-uae 600 $OUT/o2.txt $OUT/o2.log
    check "o: --undo says the deleted partition comes back, to be formatted" "$(has $OUT/o2.log 'split or deleted partitions')"
    check "o: --undo: the first 256 sectors exactly as before the install" "$([ "$($CK bytes $d 0 256)" = "$(cat $OUT/o.table)" ] && echo ok || echo bad)"
    check "o: and DH0, DH1 still the same" "$($CK sums $d DH0 DH1 | cmp -s - $OUT/o.before && echo ok || echo bad)"
    empty $d
}

# ---- h: no floppy at all: from AmigaOS on an A1200 with a 68060, as the owner's Apollo 1260 has it: zenixboot loads the kernel and the
#      installer's RAM disk (install/from-amigaos/, the whole set inside) from files on DH0; zinstall --list, then the install into free
#      space; DH0 (which the files came from) and DH1 byte-identical ----
part_h() {
    local d=$OUT/h.hdf A=$F/from-amigaos
    printf 'SYS:Zenix-Install/zenixboot SYS:Zenix-Install/zenix RAM=SYS:Zenix-Install/zinstall-ram.img\n' >$OUT/h-ss
    python3 tools/mkffs.py $OUT/h-dh0.img 32256K Workbench --intl $OUT/h-ss=S/Startup-Sequence $A/zenixboot=Zenix-Install/zenixboot \
        $A/zenix=Zenix-Install/zenix $A/zinstall-ram.img=Zenix-Install/zinstall-ram.img >/dev/null || { echo "FAIL h: the AmigaOS disk"; pass=0; return; }
    head -c 300000 /dev/urandom >$OUT/h-work
    python3 tools/mkffs.py $OUT/h-dh1.img 8064K Work --intl $OUT/h-work=Data/work >/dev/null
    python3 tools/mkrdb-multi.py $d $OUT/h-dh0.img:ffs3:DH0:boot $OUT/h-dh1.img:ffs3:DH1:- >/dev/null
    grow_rdb $d 300
    $CK sums $d DH0 DH1 >$OUT/h.before
    fsconf $OUT/h.fs-uae $d 2048 8192 none "uae_cpu_model = 68060"
    cat >$OUT/h.txt <<EOS
wait zenix:/# 240
sleep 1
send cat /etc/motd; free\r
sleep 2
send zinstall --list\r
wait What zinstall could do here 30
sleep 2
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
    run $OUT/h.fs-uae 1800 $OUT/h.txt $OUT/h1.log
    check "h: Kickstart ran zenixboot from DH0, which loaded the kernel and the RAM disk from files" "$(has $OUT/h1.log 'zenixboot [0-9.b]*: loading')"
    # FS-UAE's 68060 with Kickstart 3.1 and no 68060.library: exec sets the 68040 flags (a real 1260 loads its library first)
    check "h: the CPU is FS-UAE's 68060 (exec's flags say 68040 without 68060.library)" "$(has $OUT/h1.log '^cpu: 680[46]0')"
    check "h: no floppy: the files come from the RAM disk" "$(has $OUT/h1.log "Zenix will be copied from the installer's own files")"
    check "h: --list shows the table with cylinders and dostypes" "$(grep -aqE '^  ide0p1 +DH0 .* DOS.3 +1-' $OUT/h1.log && echo ok || echo bad)"
    check "h: the install says it is done" "$(has $OUT/h1.log '^Done\. Zenix is on this disk')"
    $CK sums $d DH0 DH1 >$OUT/h.after
    check "h: DH0 (where the installer came from) and DH1 byte-identical" "$(yes_if cmp $OUT/h.before $OUT/h.after)"
    host_fs h $d ZR0 ext2
    boot_vol h $d $BD/kernel.bin
    empty $d $OUT/h-dh0.img
}

part_k() {
    local d=$OUT/k.hdf
    truncate -s 0 $d; truncate -s 300M $d
    fsconf $OUT/k.fs-uae $d 2048 8192 4
    cat >$OUT/k.txt <<EOS
wait zenix:/# 120
sleep 1
send $ACC_PREP\r
wait zi-prep 10
send zinstall $ACC_OPTS\r
wait Choose a number 30
send 1\r
wait Choose .Return = 1, ext2 30
send \r
wait To erase the whole disk 30
send ERASE ide0\r
$INSTALLED
sleep 1
stop
EOS
    run $OUT/k.fs-uae 1800 $OUT/k.txt $OUT/k1.log
    check "k: the first install is done" "$(has $OUT/k1.log '^Done\. Zenix is on this disk')"
    fsconf $OUT/k2.fs-uae $d 2048 8192 none
    boot_script $OUT/k2.txt amiga2 bea beapw2 rootpw2          # bea writes /home/bea/note
    run $OUT/k2.fs-uae 900 $OUT/k2.txt $OUT/k2.log
    check "k: the first system boots, bea's file written" "$(has $OUT/k2.log '^was-here$')"
    $CK sums $d ZH0 >/dev/null
    # reinstall, keeping /home and the users: the zpkg advice, the plan says what is kept
    cat >$OUT/k3.txt <<EOS
wait zenix:/# 120
sleep 1
send zinstall --list\r
sleep 3
send zinstall\r
wait This disk has Zenix already 30
wait Choose a number .Return = 1 30
send \r
wait Keep the users 30
send \r
wait Choose .Return = 1, ext2 30
send \r
wait Type yes to go on 30
send yes\r
$INSTALLED
sleep 1
stop
EOS
    run $OUT/k.fs-uae 1800 $OUT/k3.txt $OUT/k3.log
    check "k: the reinstall offers itself, with zpkg -Syu named for updates" "$(grep -aq 'zpkg -Syu in Zenix instead' $OUT/k3.log && echo ok || echo bad)"
    check "k: the plan: ZR0 formatted, ZH0 kept" "$(grep -aq 'ZR0 (the system part) is formatted again' $OUT/k3.log && grep -aq 'ZH0 (your files, /home) is KEPT' $OUT/k3.log && echo ok || echo bad)"
    check "k: the reinstall is done, the users kept" "$(grep -aq '^Done\. Zenix is on this disk' $OUT/k3.log && grep -aq 'users and the computer.s name of the old Zenix are kept' $OUT/k3.log && echo ok || echo bad)"
    cat >$OUT/k4.txt <<'EOS'
wait amiga2 login: 240
sleep 1
send bea\r
wait Password: 30
send beapw2\r
wait amiga2: 60
sleep 1
send cat /home/bea/note; echo zi-k""4\r
wait zi-k4 30
send exit\r
sleep 2
stop
EOS
    run $OUT/k2.fs-uae 600 $OUT/k4.txt $OUT/k4.log
    check "k: after the reinstall bea logs in with her old password, her file is still there" "$(has $OUT/k4.log '^was-here$')"
    host_fs k $d ZR0 ext2
    # reinstall erasing /home: the users kept, the file gone
    cat >$OUT/k5.txt <<EOS
wait zenix:/# 120
sleep 1
send zinstall --reinstall --erase-home --keep-users --root-fs ext2 --home-fs ext2\r
wait Type yes to go on 30
send yes\r
$INSTALLED
sleep 1
stop
EOS
    run $OUT/k.fs-uae 1800 $OUT/k5.txt $OUT/k5.log
    check "k: the plan says /home is erased" "$(has $OUT/k5.log 'ZH0 .your files, /home. is ERASED')"
    check "k: the second reinstall is done" "$(has $OUT/k5.log '^Done\. Zenix is on this disk')"
    cat >$OUT/k6.txt <<'EOS'
wait amiga2 login: 240
sleep 1
send bea\r
wait Password: 30
send beapw2\r
wait amiga2: 60
sleep 1
send ls -a /home/bea; cat /home/bea/note; echo zi-k""6\r
wait zi-k6 30
send exit\r
sleep 2
stop
EOS
    run $OUT/k2.fs-uae 600 $OUT/k6.txt $OUT/k6.log
    check "k: after erasing /home bea still logs in; her file is gone" "$(grep -aq '^zi-k6' $OUT/k6.log && ! grep -aq '^was-here$' $OUT/k6.log && echo ok || echo bad)"
    host_fs k $d ZR0 ext2
    empty $d
}

for p in $PARTS; do "part_$p"; done
[ $pass = 1 ] && echo "zinstall-test: all passed" || echo "zinstall-test: FAILED"
exit $((1 - pass))
