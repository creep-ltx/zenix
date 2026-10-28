# SPDX-License-Identifier: GPL-2.0-only
# The m68k-elf cross toolchain (gcc, binutils). Give its prefix with `make TOOLPREFIX=/path/to/bin/m68k-elf-` or in the
# environment; by default m68k-elf-gcc from PATH, else the default install place $(HOME)/opt/m68k-elf. Exported, so
# tools/mkzexe.py and tools/check-68020-60.sh use the same one.
ifeq ($(origin TOOLPREFIX),undefined)
TOOLPREFIX := $(if $(shell command -v m68k-elf-gcc 2>/dev/null),m68k-elf-,$(HOME)/opt/m68k-elf/bin/m68k-elf-)
endif
export TOOLPREFIX
CC      := $(TOOLPREFIX)gcc
OBJCOPY := $(TOOLPREFIX)objcopy
BUILD   := build

# -m68020-60: code that runs on every CPU from the 68020 to the 68060 (no 64-bit mulu.l/divu.l, which a 68060 lacks and
# nothing emulates under Zenix); tools/check-68020-60.sh proves it on every kernel, stage0 and program
CFLAGS  := -m68020-60 -msoft-float -ffreestanding -fno-builtin -nostdlib \
           -O2 -g -Wall -Wextra -fno-strict-aliasing -fno-common -ffunction-sections -fdata-sections
DIWV    ?= 0x2C
CFLAGS  += -DDIW_V=$(DIWV)
# the display's bitplanes (DESIGN D14): empty = chosen by the machine at boot; GFXDEPTH=1, 3 or 4 for the screenshot
# test builds (4 only on AGA). Not tracked as a dependency: use a build directory of its own (make BUILD=/tmp/x ...).
GFXDEPTH ?=
CFLAGS  += $(if $(GFXDEPTH),-DGFX_DEPTH=$(GFXDEPTH))
ASFLAGS := -m68020-60 -x assembler-with-cpp -c
# the version (kernel/version.h, DESIGN D15): a test build may give its own number and string (ZVNUM=2026102701
# ZVSTR=0.2-test, in a build directory of its own: not tracked as a dependency); tools/mkzup.py reads them too
ZVNUM ?=
ZVSTR ?=
CFLAGS  += $(if $(ZVNUM),-DZENIX_VNUM_SET=$(ZVNUM)u) $(if $(ZVSTR),'-DZENIX_VSTR_SET="$(ZVSTR)"')
export ZVNUM ZVSTR
# test builds only (audit A11/A12, docs/pi-first-boot.md): DIAG=-DDIAG_STOP=N hangs at boot stage N (its colour stays),
# DIAG=-DDIAG_PANIC=N panics there. Use a build directory of its own (make BUILD=/tmp/x DIAG=...): flags are not tracked.
DIAG    ?=
CFLAGS  += $(DIAG)
ASFLAGS += $(DIAG)
# the same for the programs: UDIAG=-DDIAG_SLOWZUP (boot's short zup time limit and a stage-2 zup that hangs: the
# update test's slowzup build)
UDIAG   ?=
# libgcc's 64-bit helpers (__udivdi3, __muldi3) in the default multilib use the 64-bit mulu.l/divu.l themselves; the
# 68060 multilib's do not and run on every 68020+, so that one is linked (the check below covers it too)
LIBGCC  := $(shell $(CC) -mcpu=68060 -msoft-float -print-libgcc-file-name 2>/dev/null)

KOBJS := $(BUILD)/start.o $(BUILD)/main.o $(BUILD)/serial.o $(BUILD)/mem.o $(BUILD)/ide.o $(BUILD)/emmc.o $(BUILD)/floppy.o $(BUILD)/input.o $(BUILD)/audio.o $(BUILD)/a3000.o $(BUILD)/blk.o $(BUILD)/rtc.o $(BUILD)/proc.o $(BUILD)/syscall.o $(BUILD)/vfs.o $(BUILD)/tty.o $(BUILD)/gfx.o $(BUILD)/term.o $(BUILD)/kbd.o $(BUILD)/ipc.o $(BUILD)/svc.o $(BUILD)/seg.o $(BUILD)/event.o $(BUILD)/signal.o $(BUILD)/random.o $(BUILD)/sock.o $(BUILD)/drv.o $(BUILD)/embed.o

UAPPS := diskd init sh ls cat echo pwd ps free wc grep uname hello spin crash badptr boot \
          mkdir rmdir rm cp mv touch msgpingd msgping socktestd socktest bench fdisk mkfs_ext2 fsck_ext2 df sync termtest \
          head tail sort uniq cut tr tee seq basename dirname yes sleep true false env printenv which clear \
          whoami hostname id uptime cmp hexdump stat expr find xargs du \
          sed diff chmod ln readlink date less edit man gzip tar input beep dmesg dd login su passwd chown sha256sum printf \
          tac rev nl fold paste comm expand mktemp realpath base64 md5sum cal time timeout kill pgrep watch tree mount umount mkfs_zenixfs fsck_zenixfs \
          nice renice priotest reboot cu awk svctest useradd userdel gpasswd sudo zup sigtest netstat nettest ifconfig route ping host telnet \
          rz cryptotest ssh ssh-keygen wget sntp
UCFLAGS := -m68020-60 -msoft-float -ffreestanding -fno-builtin -nostdlib \
           -O2 -g -Wall -Wextra -fno-strict-aliasing -fno-common -ffunction-sections -fdata-sections
UCFLAGS += $(UDIAG)
# Stack sizes written into each program's ZEXE header (the loader gives this much; no guard below it). Default 16 KB;
# these need more (measured with -fstack-usage, docs/limits.md): sh's recursive parser and runner (its own depth
# guard, STK_LIMIT, keeps to 16 KB and the rest is margin), gzip's deflate (about 12 KB of frames in one chain), edit.
STACK_DEFAULT := 16384
STACK_sh := 24576
STACK_gzip := 32768
STACK_edit := 32768
STACK_awk := 65536
# boot (pid 1, built into the kernel image) lives for ever: its frames are under 500 bytes (-fstack-usage, 2026-10-26),
# so 4 KB, the loader's least; it gave back the memory D15 cost in 1 MB of chip RAM (three sessions again)
STACK_boot := 4096
# the 2 MB diet (DESIGN D18): init and diskd live for ever too and use under 700 bytes of stack (measured 2026-10-26
# in FS-UAE: the deepest byte written after a boot, logins on two consoles, ls -l, df, mount); init's frames are its
# 512-byte /etc/console buffer and printf, diskd has no recursion: 4 KB and 8 KB (16 before)
STACK_init := 4096
STACK_diskd := 8192
# every program links only the parts of libz it calls (libz is built one section per function)
ULDFLAGS := -nostartfiles -T link/user.ld -Wl,--build-id=none -Wl,-q -Wl,--gc-sections
USERZX := $(UAPPS:%=$(BUILD)/%.zx)
# servers that live on the disk as files (DESIGN D19 step 1), installed as /sbin/NAME on every full root image and in
# the base package: the ZenixFS server, which mount registers as a file service (D11 6.4). Not commands: no page.
SBINAPPS := zfsd netd
SBINZX := $(SBINAPPS:%=$(BUILD)/%.zx)
# the network's drivers (DESIGN D19 steps 7-8): user processes netd starts as /sbin/net/NAME IFACE [ARGS], from
# net/drv/NAME.c, built as drv-NAME. NETDRVS are shipped (/sbin/net on every full root image and in the base package);
# TESTDRVS only built: the driver protocol's test driver, which tools/drv-test.sh puts on its own image
NETDRVS := a2065 genet
TESTDRVS := fake
NETDRVZX := $(NETDRVS:%=$(BUILD)/drv-%.zx)
# programs with shared code (ZEX2, below) in the test images' /tests only, not commands (tools/mkroot.sh, DESIGN D13)
TESTPZX := $(BUILD)/hello.pzx $(BUILD)/crash.pzx $(BUILD)/sigtest.pzx

all: $(BUILD)/zenix.adf $(BUILD)/zenix.hdf $(TESTDRVS:%=$(BUILD)/drv-%.zx)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/bootblock.o: boot/bootblock.S boot/handoff.S | $(BUILD)
	$(CC) $(ASFLAGS) $< -o $@
$(BUILD)/bootblock.elf: $(BUILD)/bootblock.o
	$(CC) $(CFLAGS) -nostartfiles -Wl,-Ttext=0 -Wl,--build-id=none $< -o $@
$(BUILD)/bootblock.bin: $(BUILD)/bootblock.elf
	$(OBJCOPY) -O binary $< $@

# stage0: reads the Pi's hardware addresses from Emu68's device tree while AmigaOS is alive. Position
# independent (-mpcrel), no data, no relocations; embedded in the kernel image (start.S) and called by the bootblock.
$(BUILD)/stage0.o: boot/stage0.c boot/genet-stop.h net/drv/genet.h kernel/hwinfo.h boot/dtapi_amiga.h | $(BUILD)
	$(CC) -m68020-60 -msoft-float -ffreestanding -fno-builtin -nostdlib -Os -Wall -Wextra -mpcrel \
	    -fno-asynchronous-unwind-tables -c $< -o $@
	@test -z "$$($(TOOLPREFIX)readelf -r $@ | grep -E "R_68K_(32|16|8|GOT|PLT)")" || { echo "stage0 must have no absolute relocations"; exit 1; }
$(BUILD)/stage0.elf: $(BUILD)/stage0.o link/stage0.ld
	$(CC) -nostartfiles -nostdlib -T link/stage0.ld -Wl,--build-id=none $< $(LIBGCC) -o $@
$(BUILD)/stage0.bin: $(BUILD)/stage0.elf tools/check-68020-60.sh
	TOOLPREFIX=$(TOOLPREFIX) tools/check-68020-60.sh $<
	$(OBJCOPY) -O binary $< $@

# zenixboot: the AmigaOS program that boots Zenix from files on a hard disk, CF or SD card partition
# (docs/man/zenixboot.txt). Built with the same m68k-elf compiler as the kernel, freestanding (it calls AmigaOS through
# one assembly stub), linked like a Zenix program and then wrapped into an AmigaOS hunk executable by tools/mkhunk.py,
# so it needs no Amiga toolchain. It shares the hand-over code with the bootblock (boot/handoff.S).
$(BUILD)/zenixboot-start.o: boot/zenixboot-start.S boot/handoff.S | $(BUILD)
	$(CC) $(ASFLAGS) $< -o $@
$(BUILD)/zenixboot.bo: boot/zenixboot.c | $(BUILD)
	$(CC) $(CFLAGS) -Os -c $< -o $@
$(BUILD)/zenixboot.bo: user/sha256.h
$(BUILD)/sha256.bo: user/sha256.c user/sha256.h | $(BUILD)
	$(CC) $(CFLAGS) -Os -c $< -o $@
$(BUILD)/zenixboot.elf: $(BUILD)/zenixboot-start.o $(BUILD)/zenixboot.bo $(BUILD)/sha256.bo link/user.ld
	$(CC) $(CFLAGS) -nostartfiles -T link/user.ld -Wl,--build-id=none -Wl,-q -Wl,--gc-sections \
	    $(BUILD)/zenixboot-start.o $(BUILD)/zenixboot.bo $(BUILD)/sha256.bo $(LIBGCC) -o $@
$(BUILD)/zenixboot.zx: $(BUILD)/zenixboot.elf tools/mkzexe.py tools/check-68020-60.sh
	TOOLPREFIX=$(TOOLPREFIX) tools/check-68020-60.sh $<
	tools/mkzexe.py $< $@
$(BUILD)/zenixboot: $(BUILD)/zenixboot.zx tools/mkhunk.py
	python3 tools/mkhunk.py $< $@

# bootmenu: the boot menu of a disk with several systems (docs/man/bootmenu.txt), built the same way as zenixboot
$(BUILD)/bootmenu-start.o: boot/bootmenu-start.S | $(BUILD)
	$(CC) $(ASFLAGS) $< -o $@
$(BUILD)/bootmenu.bo: boot/bootmenu.c | $(BUILD)
	$(CC) $(CFLAGS) -Os -c $< -o $@
$(BUILD)/bootmenu.elf: $(BUILD)/bootmenu-start.o $(BUILD)/bootmenu.bo link/user.ld
	$(CC) $(CFLAGS) -nostartfiles -T link/user.ld -Wl,--build-id=none -Wl,-q -Wl,--gc-sections \
	    $(BUILD)/bootmenu-start.o $(BUILD)/bootmenu.bo $(LIBGCC) -o $@
$(BUILD)/bootmenu.zx: $(BUILD)/bootmenu.elf tools/mkzexe.py tools/check-68020-60.sh
	TOOLPREFIX=$(TOOLPREFIX) tools/check-68020-60.sh $<
	tools/mkzexe.py $< $@
$(BUILD)/bootmenu: $(BUILD)/bootmenu.zx tools/mkhunk.py
	python3 tools/mkhunk.py $< $@

$(BUILD)/start.o: kernel/start.S $(BUILD)/stage0.bin | $(BUILD)
	$(CC) $(ASFLAGS) -Wa,-I$(BUILD) $< -o $@
$(BUILD)/%.o: kernel/%.c $(wildcard kernel/*.h) | $(BUILD)
	$(CC) $(CFLAGS) $(KOPT_$*) -c $< -o $@
# -Os for the kernel files that are truly cold (DESIGN D13, docs/d13-shared-code.md section 10: room for the shared
# code): every wait in them is timed by the E-clock or a timer, none is on a hot path. The floppy's track coding
# (mfm.h) keeps -O2 through a pragma in floppy.c. Everything else stays -O2.
KOPT_floppy := -Os
KOPT_emmc := -Os
KOPT_a3000 := -Os
KOPT_rtc := -Os
KOPT_kbd := -Os
KOPT_gfx := -Os
KOPT_term := -Os
# DESIGN D16 rule 14 (room for events and signals, measured 2026-10-26): tty.c (line discipline at typing speed) and
# vfs.c (dispatch and path routing; the time goes to the servers) at -Os save 4048 bytes of the RAM floppy's kernel; the
# pipes' copy loops (pipe_read, pipe_write) keep -O2 through an attribute (136 bytes). The scheduler, ipc.c and the
# interrupt entries stay -O2.
KOPT_tty := -Os
KOPT_vfs := -Os
# DESIGN D16's own files (rule 14): events and signals at -Os; only the one-word test on the return paths is inline
KOPT_event := -Os
KOPT_signal := -Os
# DESIGN D19 9.3: the entropy pool and the random devices: cold, -Os in their own file
KOPT_random := -Os
# DESIGN D19 4.1-4.3: the socket layer (the time goes to the network server): -Os in its own file
KOPT_sock := -Os
# DESIGN D19 step 7: the drivers' system call (claims, DMA memory, quiesce, the blocks to netd): cold, -Os
KOPT_drv := -Os
$(BUILD)/%.o: kernel/%.S | $(BUILD)
	$(CC) $(ASFLAGS) $< -o $@
$(BUILD)/kernel.elf: $(KOBJS) link/kernel.ld
	$(CC) $(CFLAGS) -nostartfiles -T link/kernel.ld -Wl,--build-id=none -Wl,-q \
	    $(KOBJS) $(LIBGCC) -o $@
# The RAM floppy (zenix-ram.adf) holds the bootblock (1 KB), the kernel and the RAM disk (RAMDISK_K KB) in 880 KB: the
# kernel may grow to KERNEL_MAX bytes. Every build says how much is left; tools/mkadf.py refuses an image that is over.
# The mount tests' floppy (zenix-mount.adf) has a RAM disk of its own size (RAMDISK_MOUNT_K: it carries /sbin/zfsd,
# DESIGN D19 step 1), so its kernel limit is lower; the build says that one too. Its RAM disk carries no test
# scripts (NOTESTS=1: the mount tests run none of /tests); that made room for D20 step 1's 8 KB (3 blocks were free).
RAMDISK_K := 704
RAMDISK_MOUNT_K := 720
KERNEL_MAX := $(shell echo $$((901120 - 1024 - $(RAMDISK_K) * 1024)))
KERNEL_MAX_MOUNT := $(shell echo $$((901120 - 1024 - $(RAMDISK_MOUNT_K) * 1024)))
$(BUILD)/kernel.bin: $(BUILD)/kernel.elf tools/mkzexe.py tools/check-68020-60.sh
	TOOLPREFIX=$(TOOLPREFIX) tools/check-68020-60.sh $<
	tools/mkzexe.py --delta $< $@
	@s=$$(wc -c < $@); echo "$@: $$s of $(KERNEL_MAX) bytes, $$(($(KERNEL_MAX) - s)) left for the kernel on the RAM floppy" \
	    "($$(($(KERNEL_MAX_MOUNT) - s)) on the mount tests' floppy)"; \
	    [ $$s -le $(KERNEL_MAX) ] || echo "$@: WARNING: $$((s - $(KERNEL_MAX))) bytes too big for the RAM floppy (zenix-ram.adf)"; \
	    [ $$s -le $(KERNEL_MAX_MOUNT) ] || echo "$@: WARNING: $$((s - $(KERNEL_MAX_MOUNT))) bytes too big for the mount tests' floppy (zenix-mount.adf)"

# The test-only kernel (DESIGN D19 step 1): the same kernel with the ZenixFS server built in, for the emulator tests
# that boot a ZenixFS root (zroot-test, svc-test z and zo). A ZenixFS root needs its server before any file can be read,
# and the shipped kernel no longer carries one. Too big for a RAM floppy: on a kernel-only floppy, zenix-zfs.adf.
# Updates never install it over a ZenixFS root's kernel by mistake: zenixboot reads the kernel's root list (ZSRV).
KZOBJS := $(subst $(BUILD)/embed.o,$(BUILD)/zfs-embed.o,$(subst $(BUILD)/svc.o,$(BUILD)/zfs-svc.o,$(subst $(BUILD)/syscall.o,$(BUILD)/zfs-syscall.o,$(KOBJS))))
$(BUILD)/zfs-%.o: kernel/%.c $(wildcard kernel/*.h) | $(BUILD)
	$(CC) $(CFLAGS) $(KOPT_$*) -DKERNEL_ZFSD -c $< -o $@
$(BUILD)/zfs-embed.o: kernel/embed.S $(BUILD)/fsd.zx $(BUILD)/boot.zx $(BUILD)/diskd.zx $(BUILD)/zfsd.zx | $(BUILD)
	$(CC) $(ASFLAGS) -DKERNEL_ZFSD -Wa,-I$(BUILD) $< -o $@
$(BUILD)/kernel-zfs.elf: $(KZOBJS) link/kernel.ld
	$(CC) $(CFLAGS) -nostartfiles -T link/kernel.ld -Wl,--build-id=none -Wl,-q \
	    $(KZOBJS) $(LIBGCC) -o $@
$(BUILD)/kernel-zfs.bin: $(BUILD)/kernel-zfs.elf tools/mkzexe.py tools/check-68020-60.sh
	TOOLPREFIX=$(TOOLPREFIX) tools/check-68020-60.sh $<
	tools/mkzexe.py --delta $< $@
$(BUILD)/zenix-zfs.adf: $(BUILD)/bootblock.bin $(BUILD)/kernel-zfs.bin tools/mkadf.py
	python3 tools/mkadf.py $(BUILD)/bootblock.bin $(BUILD)/kernel-zfs.bin $@

# the standard floppy carries only the bootblock and the kernel (the root filesystem is on the disk)
$(BUILD)/zenix.adf: $(BUILD)/bootblock.bin $(BUILD)/kernel.bin tools/mkadf.py
	python3 tools/mkadf.py $(BUILD)/bootblock.bin $(BUILD)/kernel.bin $@

# The floppy has room for about 780 KB of root filesystem after the kernel. The RAM-disk image only serves the RAM-boot
# and disk-tool tests, so it gets a fixed short list of programs instead of everything (a new program does not need
# adding here, and cannot overflow the image).
RAMAPPS := init login sh ls cat echo pwd ps free wc hello crash uname df fdisk mkfs_ext2 fsck_ext2 sync mkdir rmdir rm cp mv \
           touch head tail sleep true false chmod id whoami su passwd chown ln stat cmp date env printenv dmesg
# the mount tests (tools/mounttest.sh) boot their own small floppy: the standard RAM image is full
MOUNTAPPS := init login sh ls ln cmp seq cat echo pwd mkdir rm rmdir cp mv df sync fdisk mkfs_ext2 fsck_ext2 mount umount \
             mkfs_zenixfs fsck_zenixfs su id whoami chmod touch wc head tail ps stat uname sleep true false env passwd \
             kill pgrep grep dmesg svctest zfsd readlink realpath tar tree

# development/test image: the same, plus a small root filesystem that the bootblock loads into RAM.
# For machines without an IDE disk (a Gotek boot) and for testing. Not part of the standard build.
$(BUILD)/root-ram.img: $(USERZX) $(shell find rootfs -type f) tools/mkroot.sh
	NOMAN=1 NOTESTS=1 tools/mkroot.sh $@ $(BUILD) "$(RAMAPPS)" $(RAMDISK_K)K 128

$(BUILD)/root-mount.img: $(USERZX) $(SBINZX) $(shell find rootfs -type f) tools/mkroot.sh Makefile
	NOMAN=1 NOPURE=1 NOTESTS=1 tools/mkroot.sh $@ $(BUILD) "$(MOUNTAPPS)" $(RAMDISK_MOUNT_K)K 128

$(BUILD)/zenix-mount.adf: $(BUILD)/bootblock.bin $(BUILD)/kernel.bin $(BUILD)/root-mount.img tools/mkadf.py
	python3 tools/mkadf.py $(BUILD)/bootblock.bin $(BUILD)/kernel.bin $@ $(BUILD)/root-mount.img

$(BUILD)/zenix-ram.adf: $(BUILD)/bootblock.bin $(BUILD)/kernel.bin $(BUILD)/root-ram.img tools/mkadf.py
	python3 tools/mkadf.py $(BUILD)/bootblock.bin $(BUILD)/kernel.bin $@ $(BUILD)/root-ram.img

# ---- servers built into the kernel image ----
$(BUILD)/ext2_fsd.uo: kernel/ext2.c kernel/ext2.h kernel/fsperm.h user/fsenv.h user/libz.h | $(BUILD)
	$(CC) $(UCFLAGS) -DFSD -c $< -o $@
$(BUILD)/regex.uo: user/regex.c user/regex.h user/libz.h | $(BUILD)
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/sed.uelf: $(BUILD)/sed.uo $(BUILD)/regex.uo $(BUILD)/crt0.o $(BUILD)/libz.uo link/user.ld
	$(CC) $(UCFLAGS) $(ULDFLAGS) \
	    $(BUILD)/crt0.o $(BUILD)/sed.uo $(BUILD)/regex.uo $(BUILD)/libz.uo $(LIBGCC) -o $@

$(BUILD)/less.uelf: $(BUILD)/less.uo $(BUILD)/regex.uo $(BUILD)/crt0.o $(BUILD)/libz.uo link/user.ld
	$(CC) $(UCFLAGS) $(ULDFLAGS) \
	    $(BUILD)/crt0.o $(BUILD)/less.uo $(BUILD)/regex.uo $(BUILD)/libz.uo $(LIBGCC) -o $@

$(BUILD)/edit.uelf: $(BUILD)/edit.uo $(BUILD)/regex.uo $(BUILD)/crt0.o $(BUILD)/libz.uo link/user.ld
	$(CC) $(UCFLAGS) $(ULDFLAGS) \
	    $(BUILD)/crt0.o $(BUILD)/edit.uo $(BUILD)/regex.uo $(BUILD)/libz.uo $(LIBGCC) -o $@

MINIZ_CFLAGS := -Iuser/miniz/shim -Iuser/miniz -Wno-unused-parameter -Wno-sign-compare -Wno-implicit-fallthrough -Wno-misleading-indentation -Wno-unused-function
$(BUILD)/miniz_tdef.uo: user/miniz/miniz_tdef.c | $(BUILD)
	$(CC) $(UCFLAGS) $(MINIZ_CFLAGS) -c $< -o $@
$(BUILD)/miniz_tinfl.uo: user/miniz/miniz_tinfl.c | $(BUILD)
	$(CC) $(UCFLAGS) $(MINIZ_CFLAGS) -c $< -o $@
$(BUILD)/gzip.uo: user/gzip.c user/libz.h | $(BUILD)
	$(CC) $(UCFLAGS) $(MINIZ_CFLAGS) -c $< -o $@
$(BUILD)/gzip.uelf: $(BUILD)/gzip.uo $(BUILD)/miniz_tdef.uo $(BUILD)/miniz_tinfl.uo $(BUILD)/crt0.o $(BUILD)/libz.uo link/user.ld
	$(CC) $(UCFLAGS) $(ULDFLAGS) \
	    $(BUILD)/crt0.o $(BUILD)/gzip.uo $(BUILD)/miniz_tdef.uo $(BUILD)/miniz_tinfl.uo $(BUILD)/libz.uo $(LIBGCC) -o $@

# zup (DESIGN D15, /sbin/zup): inflate (miniz) and SHA-256 (sha.c)
$(BUILD)/sha256.uo $(BUILD)/sha256.po: user/sha256.h
$(BUILD)/zup.uo: user/zup.c user/libz.h user/usys.h user/sha256.h kernel/fsproto.h | $(BUILD)
	$(CC) $(UCFLAGS) $(MINIZ_CFLAGS) -c $< -o $@
$(BUILD)/zup.uelf: $(BUILD)/zup.uo $(BUILD)/miniz_tinfl.uo $(BUILD)/sha256.uo $(BUILD)/crt0.o $(BUILD)/libz.uo link/user.ld
	$(CC) $(UCFLAGS) $(ULDFLAGS) \
	    $(BUILD)/crt0.o $(BUILD)/zup.uo $(BUILD)/miniz_tinfl.uo $(BUILD)/sha256.uo $(BUILD)/libz.uo $(LIBGCC) -o $@

# programs that look up users and groups (/etc/passwd, /etc/group), and those that also check passwords
PWAPPS := ls id whoami chown renice useradd userdel gpasswd netstat
AUTHAPPS := login su sudo passwd sha256sum
$(PWAPPS:%=$(BUILD)/%.uelf): $(BUILD)/%.uelf: $(BUILD)/%.uo $(BUILD)/pw.uo $(BUILD)/crt0.o $(BUILD)/libz.uo link/user.ld
	$(CC) $(UCFLAGS) $(ULDFLAGS) \
	    $(BUILD)/crt0.o $< $(BUILD)/pw.uo $(BUILD)/libz.uo $(LIBGCC) -o $@
$(AUTHAPPS:%=$(BUILD)/%.uelf): $(BUILD)/%.uelf: $(BUILD)/%.uo $(BUILD)/pw.uo $(BUILD)/sha.uo $(BUILD)/sha256.uo $(BUILD)/crt0.o $(BUILD)/libz.uo link/user.ld
	$(CC) $(UCFLAGS) $(ULDFLAGS) \
	    $(BUILD)/crt0.o $< $(BUILD)/pw.uo $(BUILD)/sha.uo $(BUILD)/sha256.uo $(BUILD)/libz.uo $(LIBGCC) -o $@
$(BUILD)/pw.uo $(BUILD)/sha.uo: $(BUILD)/%.uo: user/%.c user/pw.h user/sha256.h user/libz.h user/usys.h | $(BUILD)
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/grep.uelf: $(BUILD)/grep.uo $(BUILD)/regex.uo $(BUILD)/crt0.o $(BUILD)/libz.uo link/user.ld
	$(CC) $(UCFLAGS) $(ULDFLAGS) \
	    $(BUILD)/crt0.o $(BUILD)/grep.uo $(BUILD)/regex.uo $(BUILD)/libz.uo $(LIBGCC) -o $@

$(BUILD)/awk.uelf: $(BUILD)/awk.uo $(BUILD)/regex.uo $(BUILD)/crt0.o $(BUILD)/libz.uo link/user.ld
	$(CC) $(UCFLAGS) $(ULDFLAGS) \
	    $(BUILD)/crt0.o $(BUILD)/awk.uo $(BUILD)/regex.uo $(BUILD)/libz.uo $(LIBGCC) -o $@

$(BUILD)/expr.uelf: $(BUILD)/expr.uo $(BUILD)/regex.uo $(BUILD)/crt0.o $(BUILD)/libz.uo link/user.ld
	$(CC) $(UCFLAGS) $(ULDFLAGS) \
	    $(BUILD)/crt0.o $(BUILD)/expr.uo $(BUILD)/regex.uo $(BUILD)/libz.uo $(LIBGCC) -o $@

$(BUILD)/fsck_ext2.uelf: $(BUILD)/fsck_ext2.uo $(BUILD)/ext2_fsd.uo $(BUILD)/crt0.o $(BUILD)/libz.uo link/user.ld
	$(CC) $(UCFLAGS) $(ULDFLAGS) \
	    $(BUILD)/crt0.o $(BUILD)/fsck_ext2.uo $(BUILD)/ext2_fsd.uo $(BUILD)/libz.uo $(LIBGCC) -o $@

$(BUILD)/fsd.uelf: $(BUILD)/fsd.uo $(BUILD)/ext2_fsd.uo $(BUILD)/crt0.o $(BUILD)/libz.uo link/user.ld
	$(CC) $(UCFLAGS) $(ULDFLAGS) \
	    $(BUILD)/crt0.o $(BUILD)/fsd.uo $(BUILD)/ext2_fsd.uo $(BUILD)/libz.uo $(LIBGCC) -o $@
$(BUILD)/fsd.zx: $(BUILD)/fsd.uelf tools/mkzexe.py tools/check-68020-60.sh
	TOOLPREFIX=$(TOOLPREFIX) tools/check-68020-60.sh $<
	tools/mkzexe.py $< $@ 16384
$(BUILD)/boot.uo: user/rootpick.h user/volid.h user/rdb.h kernel/diskproto.h
$(BUILD)/diskd.uo: user/say.h user/rdb.h kernel/diskproto.h
$(BUILD)/fsd.uo: user/volid.h user/diskcall.h kernel/diskproto.h kernel/fsproto.h
$(BUILD)/embed.o: kernel/embed.S $(BUILD)/fsd.zx $(BUILD)/boot.zx $(BUILD)/diskd.zx | $(BUILD)
	$(CC) $(ASFLAGS) -Wa,-I$(BUILD) $< -o $@

# ---- ZenixFS: the library (zenixfs/zenixfs.c, also built for the host) as a Zenix program part, its tools and server ----
$(BUILD)/zenixfs_lib.uo: zenixfs/zenixfs.c zenixfs/zenixfs.h user/zenixfs_env.h | $(BUILD)
	$(CC) $(UCFLAGS) -Os -ffunction-sections -fdata-sections -DZENIXFS_FREESTANDING -Iuser -c $< -o $@
ZENIXFS_OBJS := $(BUILD)/zenixfs_lib.uo $(BUILD)/zenixfs_disk.uo
$(BUILD)/zenixfs_disk.uo: user/zenixfs_disk.c user/zenixfs_disk.h user/diskcall.h zenixfs/zenixfs.h user/libz.h | $(BUILD)
	$(CC) $(UCFLAGS) -Os -ffunction-sections -c $< -o $@
$(BUILD)/zenixfsd.uo: user/zenixfsd.c user/volid.h user/rdb.h kernel/fsperm.h kernel/fsproto.h user/zenixfs_disk.h zenixfs/zenixfs.h user/libz.h | $(BUILD)
	$(CC) $(UCFLAGS) -Os -ffunction-sections -c $< -o $@
$(BUILD)/zenixfsd.uelf: $(BUILD)/zenixfsd.uo $(ZENIXFS_OBJS) $(BUILD)/crt0.o $(BUILD)/libz.uo link/user.ld
	$(CC) $(UCFLAGS) $(ULDFLAGS) \
	    $(BUILD)/crt0.o $(BUILD)/zenixfsd.uo $(ZENIXFS_OBJS) $(BUILD)/libz.uo $(LIBGCC) -o $@
$(BUILD)/zfsd.zx: $(BUILD)/zenixfsd.uelf tools/mkzexe.py tools/check-68020-60.sh
	TOOLPREFIX=$(TOOLPREFIX) tools/check-68020-60.sh $<
	tools/mkzexe.py $< $@ 32768
$(BUILD)/mkfs_zenixfs.uelf: $(BUILD)/mkfs_zenixfs.uo $(ZENIXFS_OBJS) $(BUILD)/crt0.o $(BUILD)/libz.uo link/user.ld
	$(CC) $(UCFLAGS) $(ULDFLAGS) \
	    $(BUILD)/crt0.o $(BUILD)/mkfs_zenixfs.uo $(ZENIXFS_OBJS) $(BUILD)/libz.uo $(LIBGCC) -o $@
$(BUILD)/fsck_zenixfs.uelf: $(BUILD)/fsck_zenixfs.uo $(ZENIXFS_OBJS) $(BUILD)/crt0.o $(BUILD)/libz.uo link/user.ld
	$(CC) $(UCFLAGS) $(ULDFLAGS) \
	    $(BUILD)/crt0.o $(BUILD)/fsck_zenixfs.uo $(ZENIXFS_OBJS) $(BUILD)/libz.uo $(LIBGCC) -o $@
$(BUILD)/mkfs_zenixfs.zx $(BUILD)/fsck_zenixfs.zx: %.zx: %.uelf tools/mkzexe.py tools/check-68020-60.sh
	TOOLPREFIX=$(TOOLPREFIX) tools/check-68020-60.sh $<
	tools/mkzexe.py $< $@ 32768

# ---- netd (DESIGN D19 step 4, docs/man/netd.txt): the network server, /sbin/netd, loaded from the disk the first time
# a program opens a socket. lwIP 2.2.1 (net/lwip, BSD-3: its notice ships as /usr/share/licenses/lwIP), its port
# (net/port: lwipopts.h, the random numbers, the checksum and the copy in 68020 assembler) and netd's core and main
# program (net/netd). Cold code at -Os; SLIP's byte loop (slipif.c, iface.c) at -O2 (the golden path). ----
LWIPSRC := net/lwip/src
LWIP_CORE := def dns inet_chksum init ip mem memp netif pbuf raw stats sys tcp tcp_in tcp_out timeouts udp
LWIP_IPV4 := dhcp etharp icmp ip4 ip4_addr ip4_frag
NETDOBJS := $(LWIP_CORE:%=$(BUILD)/lw-%.no) $(LWIP_IPV4:%=$(BUILD)/lw-%.no) $(BUILD)/lw-ethernet.no $(BUILD)/lw-slipif.no \
            $(BUILD)/np-port.no $(BUILD)/np-rand.no $(BUILD)/np-m68k.no \
            $(BUILD)/nd-core.no $(BUILD)/nd-sock.no $(BUILD)/nd-iface.no $(BUILD)/nd-config.no $(BUILD)/nd-main.no
NETCFLAGS := $(UCFLAGS) -Os -Inet/port/shim -Inet/port -I$(LWIPSRC)/include -Wno-unused-parameter -Wno-sign-compare \
             -Wno-implicit-fallthrough
NETHDRS := $(wildcard net/port/*.h net/port/arch/*.h net/netd/*.h $(LWIPSRC)/include/lwip/*.h \
                      $(LWIPSRC)/include/lwip/priv/*.h $(LWIPSRC)/include/lwip/prot/*.h $(LWIPSRC)/include/netif/*.h) \
           kernel/netproto.h user/libz.h user/usys.h
NOPT_slipif := -O2
NOPT_iface := -O2
$(BUILD)/lw-%.no: $(LWIPSRC)/core/%.c $(NETHDRS) | $(BUILD)
	$(CC) $(NETCFLAGS) $(NOPT_$*) -c $< -o $@
$(BUILD)/lw-%.no: $(LWIPSRC)/core/ipv4/%.c $(NETHDRS) | $(BUILD)
	$(CC) $(NETCFLAGS) $(NOPT_$*) -c $< -o $@
$(BUILD)/lw-%.no: $(LWIPSRC)/netif/%.c $(NETHDRS) | $(BUILD)
	$(CC) $(NETCFLAGS) $(NOPT_$*) -c $< -o $@
$(BUILD)/np-%.no: net/port/%.c $(NETHDRS) | $(BUILD)
	$(CC) $(NETCFLAGS) $(NOPT_$*) -c $< -o $@
$(BUILD)/np-m68k.no: net/port/m68k.S | $(BUILD)
	$(CC) $(ASFLAGS) $< -o $@
$(BUILD)/nd-%.no: net/netd/%.c $(NETHDRS) | $(BUILD)
	$(CC) $(NETCFLAGS) $(NOPT_$*) -c $< -o $@
$(BUILD)/netd.uelf: $(NETDOBJS) $(BUILD)/crt0.o $(BUILD)/libz.uo link/user.ld
	$(CC) $(UCFLAGS) $(ULDFLAGS) $(BUILD)/crt0.o $(NETDOBJS) $(BUILD)/libz.uo $(LIBGCC) -o $@
# nettest checks netd's assembler against C (nettest asm)
$(BUILD)/nettest.uelf: $(BUILD)/nettest.uo $(BUILD)/np-m68k.no $(BUILD)/crt0.o $(BUILD)/libz.uo link/user.ld
	$(CC) $(UCFLAGS) $(ULDFLAGS) $(BUILD)/crt0.o $(BUILD)/nettest.uo $(BUILD)/np-m68k.no $(BUILD)/libz.uo $(LIBGCC) -o $@
$(BUILD)/nettest.uo $(BUILD)/netstat.uo $(BUILD)/ifconfig.uo $(BUILD)/route.uo $(BUILD)/ping.uo $(BUILD)/host.uo: user/socket.h kernel/netproto.h
# telnet (DESIGN D20, docs/man/telnet.txt) over telnet's core in net/telnet/ (no system calls, shared with the PC tests,
# tools/host/telnet-test.sh): the protocol, the keys and the tables cold (-Os), the stream filter every byte (-O2)
TNOBJS := $(BUILD)/tn-proto.no $(BUILD)/tn-filter.no $(BUILD)/tn-keys.no $(BUILD)/tn-cp437.no
TNOPT_filter := -O2
$(BUILD)/tn-%.no: net/telnet/%.c net/telnet/telnet.h | $(BUILD)
	$(CC) $(UCFLAGS) $(if $(TNOPT_$*),$(TNOPT_$*),-Os) -c $< -o $@
$(BUILD)/telnet.uo: user/telnet.c net/telnet/telnet.h user/xfer/xfer.h user/socket.h kernel/netproto.h user/libz.h user/usys.h | $(BUILD)
	$(CC) $(UCFLAGS) -Iuser -Os -c $< -o $@
$(BUILD)/telnet.uelf: $(BUILD)/telnet.uo $(TNOBJS) $(BUILD)/xf-detect.no $(BUILD)/crt0.o $(BUILD)/libz.uo link/user.ld
	$(CC) $(UCFLAGS) $(ULDFLAGS) $(BUILD)/crt0.o $(BUILD)/telnet.uo $(TNOBJS) $(BUILD)/xf-detect.no $(BUILD)/libz.uo $(LIBGCC) -o $@
# the crypto library (DESIGN D21 3.5, docs/d21-crypto-ssh-https.md): BearSSL and Monocypher (only the files used,
# crypto/*/README) and Zenix's crypto/zc/, as one archive linked with --gc-sections into the programs that use it
# (cryptotest, ssh, ssh-keygen). Cold code at -Os; the files a handshake waits on at -O2 (D21 3.6 step 1: the curve25519
# and Ed25519 arithmetic, the i15 Montgomery multiply, P-256, ChaCha20, Poly1305, SHA-2)
CRYPTO_SRCS := $(sort $(wildcard crypto/zc/*.c)) crypto/monocypher/monocypher.c crypto/monocypher/monocypher-ed25519.c \
               $(sort $(shell find crypto/bearssl/src -name '*.c')) crypto/openssh/blowfish.c crypto/openssh/bcrypt_pbkdf.c
CRYPTO_O2 := crypto/monocypher/monocypher.c crypto/monocypher/monocypher-ed25519.c crypto/bearssl/src/ec/ec_c25519_m15.c \
             crypto/bearssl/src/int/i15_montmul.c crypto/bearssl/src/ec/ec_p256_m15.c crypto/bearssl/src/symcipher/chacha20_ct.c \
             crypto/bearssl/src/symcipher/poly1305_ctmul32.c crypto/bearssl/src/hash/sha2small.c crypto/bearssl/src/hash/sha2big.c
# BR_USE_UNIX_TIME=0: x509_minimal never calls time() and loses its 64-bit division (D21 3.5, 8.3: the date comes
# from zc_clock's callback; newlib's headers would switch it on)
CRYPTO_DEF := -DBR_LE_UNALIGNED=0 -DBR_BE_UNALIGNED=0 -DBR_USE_URANDOM=0 -DBR_USE_GETENTROPY=0 -DBR_USE_UNIX_TIME=0 \
              -Icrypto/bearssl/inc -Icrypto/bearssl/src -Icrypto/openssh
CRYPTO_OBJS := $(patsubst %.c,$(BUILD)/cr/%.o,$(CRYPTO_SRCS))
$(BUILD)/cr/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) -m68020-60 -msoft-float -ffunction-sections -fdata-sections -fno-common -g $(CRYPTO_DEF) \
	    $(if $(filter $<,$(CRYPTO_O2)),-O2,-Os) -c $< -o $@
$(BUILD)/libcrypto.a: $(CRYPTO_OBJS) Makefile
	rm -f $@
	$(TOOLPREFIX)ar rcs $@ $(CRYPTO_OBJS)
$(BUILD)/cryptotest.uo: user/cryptotest.c tools/host/crypto-vectors.h crypto/zc/zc.h user/libz.h user/usys.h | $(BUILD)
	$(CC) $(UCFLAGS) -Os -Inet/port/shim $(CRYPTO_DEF) -c $< -o $@
$(BUILD)/cryptotest.uelf: $(BUILD)/cryptotest.uo $(BUILD)/libcrypto.a $(BUILD)/crt0.o $(BUILD)/libz.uo link/user.ld
	$(CC) $(UCFLAGS) $(ULDFLAGS) $(BUILD)/crt0.o $(BUILD)/cryptotest.uo $(BUILD)/libcrypto.a $(BUILD)/libz.uo $(LIBGCC) -o $@
STACK_cryptotest := 24576
# ssh and ssh-keygen (DESIGN D21 4, docs/man/ssh.txt): ssh's core in net/ssh/ (system-call free, shared with the PC
# tests) over the library, and telnet's Unix-mode filter and keys (net/telnet/)
SSHCORE := auth channel config keys knownhosts transport wire
SSHOBJS := $(SSHCORE:%=$(BUILD)/cr/net/ssh/%.o)
$(BUILD)/cr/net/ssh/%.o: net/ssh/%.c net/ssh/ssh.h net/ssh/ssh_int.h
	@mkdir -p $(dir $@)
	$(CC) -m68020-60 -msoft-float -ffunction-sections -fdata-sections -fno-common -g $(CRYPTO_DEF) -Os -c $< -o $@
$(BUILD)/ssh.uo $(BUILD)/ssh-keygen.uo: $(BUILD)/%.uo: user/%.c net/ssh/ssh.h user/libz.h user/usys.h | $(BUILD)
	$(CC) $(UCFLAGS) -Os -Inet/port/shim $(CRYPTO_DEF) -c $< -o $@
$(BUILD)/ssh.uelf: $(BUILD)/ssh.uo $(BUILD)/pw.uo $(SSHOBJS) $(TNOBJS) $(BUILD)/libcrypto.a $(BUILD)/crt0.o $(BUILD)/libz.uo link/user.ld
	$(CC) $(UCFLAGS) $(ULDFLAGS) $(BUILD)/crt0.o $(BUILD)/ssh.uo $(BUILD)/pw.uo $(SSHOBJS) $(TNOBJS) $(BUILD)/libcrypto.a $(BUILD)/libz.uo $(LIBGCC) -o $@
$(BUILD)/ssh-keygen.uelf: $(BUILD)/ssh-keygen.uo $(SSHOBJS) $(BUILD)/libcrypto.a $(BUILD)/crt0.o $(BUILD)/libz.uo link/user.ld
	$(CC) $(UCFLAGS) $(ULDFLAGS) $(BUILD)/crt0.o $(BUILD)/ssh-keygen.uo $(SSHOBJS) $(BUILD)/libcrypto.a $(BUILD)/libz.uo $(LIBGCC) -o $@
STACK_ssh := 16384
# ssh is a program with shared code (D13, D21 3.5: one copy of its ~100 KB for every session): pure builds of the
# library, the core and telnet's filter (tools/purecc.sh), linked as ssh.pzx and installed as ssh (PUREAPPS)
CRYPTO_POBJS := $(patsubst %.c,$(BUILD)/crp/%.o,$(CRYPTO_SRCS))
SSHPOBJS := $(SSHCORE:%=$(BUILD)/crp/net/ssh/%.o) $(patsubst %,$(BUILD)/crp/net/telnet/%.o,proto filter keys cp437)
$(BUILD)/crp/%.o: %.c tools/purecc.sh
	@mkdir -p $(dir $@)
	tools/purecc.sh $(CC) -m68020-60 -msoft-float -ffunction-sections -fdata-sections -fno-common -g $(CRYPTO_DEF) \
	    $(if $(filter $<,$(CRYPTO_O2) net/telnet/filter.c),-O2,-Os) -c $< -o $@
$(BUILD)/libcrypto-pure.a: $(CRYPTO_POBJS) Makefile
	rm -f $@
	$(TOOLPREFIX)ar rcs $@ $(CRYPTO_POBJS)
$(BUILD)/ssh.po: user/ssh.c net/ssh/ssh.h user/pw.h user/libz.h user/usys.h tools/purecc.sh | $(BUILD)
	tools/purecc.sh $(CC) $(UCFLAGS) -Os -Inet/port/shim $(CRYPTO_DEF) -c $< -o $@
$(BUILD)/ssh.puelf: $(BUILD)/ssh.po $(BUILD)/pw.po $(SSHPOBJS) $(BUILD)/libcrypto-pure.a $(BUILD)/crt0-pure.o $(BUILD)/libz.po link/pure.ld
	$(CC) $(UCFLAGS) $(PULDFLAGS) $(BUILD)/crt0-pure.o $(BUILD)/ssh.po $(BUILD)/pw.po $(SSHPOBJS) $(BUILD)/libcrypto-pure.a $(BUILD)/libz.po $(LIBGCC) -o $@
STACK_ssh-keygen := 16384
# the define above is not tracked: the one file it changes is rebuilt when the Makefile changes
$(BUILD)/cr/crypto/bearssl/src/x509/x509_minimal.o $(BUILD)/crp/crypto/bearssl/src/x509/x509_minimal.o: Makefile
# the build date (DESIGN D21 8.3: the floor certificate dates are never judged below): the last commit's time, so
# one commit always builds the same programs; SOURCE_DATE_EPOCH when given (a build outside git), else 0. The header
# changes only when the date does
BUILD_DATE := $(or $(SOURCE_DATE_EPOCH),$(shell git log -1 --format=%ct 2>/dev/null),0)
$(BUILD)/builddate.h: FORCE | $(BUILD)
	@printf '#define ZENIX_BUILD_DATE %su\n' '$(BUILD_DATE)' > $@.new; cmp -s $@.new $@ || mv $@.new $@; rm -f $@.new
FORCE:
# wget (DESIGN D21 8, docs/man/wget.txt) over its core (user/wget/), net/http/ (shared with zpkg) and the library's
# TLS glue (crypto/zc/zc_tls.c), a plain program (not pure: D21 3.5); sntp (D21 8.3, docs/man/sntp.txt) over
# net/sntp/ and the clock file's code (crypto/zc/zc_clock.c). The cores are system-call free (tools/host/tls-test.sh)
WGETOBJS := $(BUILD)/cr/user/wget/plan.o $(BUILD)/cr/net/http/http.o
SNTPOBJS := $(BUILD)/cr/net/sntp/sntp.o
$(BUILD)/cr/user/wget/plan.o: user/wget/wget.h net/http/http.h
$(BUILD)/cr/net/http/http.o: net/http/http.h
$(BUILD)/cr/net/sntp/sntp.o: net/sntp/sntp.h
$(BUILD)/wget.uo $(BUILD)/sntp.uo: $(BUILD)/%.uo: user/%.c $(BUILD)/builddate.h crypto/zc/zc_clock.h user/socket.h user/libz.h user/usys.h | $(BUILD)
	$(CC) $(UCFLAGS) -Os -I$(BUILD) -Inet/port/shim $(CRYPTO_DEF) -c $< -o $@
$(BUILD)/wget.uo: user/wget/wget.h net/http/http.h crypto/zc/zc_tls.h crypto/zc/zc_x509.h
$(BUILD)/sntp.uo: net/sntp/sntp.h
$(BUILD)/wget.uelf: $(BUILD)/wget.uo $(WGETOBJS) $(BUILD)/libcrypto.a $(BUILD)/crt0.o $(BUILD)/libz.uo link/user.ld
	$(CC) $(UCFLAGS) $(ULDFLAGS) $(BUILD)/crt0.o $(BUILD)/wget.uo $(WGETOBJS) $(BUILD)/libcrypto.a $(BUILD)/libz.uo $(LIBGCC) -o $@
$(BUILD)/sntp.uelf: $(BUILD)/sntp.uo $(SNTPOBJS) $(BUILD)/libcrypto.a $(BUILD)/crt0.o $(BUILD)/libz.uo link/user.ld
	$(CC) $(UCFLAGS) $(ULDFLAGS) $(BUILD)/crt0.o $(BUILD)/sntp.uo $(SNTPOBJS) $(BUILD)/libcrypto.a $(BUILD)/libz.uo $(LIBGCC) -o $@
# rz and sz (DESIGN D20 5, docs/man/rz.txt; sz is a link to rz): the Zmodem engine (user/xfer/, CTerm's, MIT) at -O2
# (every byte of a transfer goes through it), the download rules (rzrules.c, shared with the PC tests) and rz.c cold
XFEROBJS := $(BUILD)/xf-zmodem.no $(BUILD)/xf-detect.no $(BUILD)/xf-rzrules.no
XFOPT_zmodem := -O2
$(BUILD)/xf-%.no: user/xfer/%.c user/xfer/xfer.h user/xfer/rzrules.h | $(BUILD)
	$(CC) $(UCFLAGS) -Inet/port/shim $(if $(XFOPT_$*),$(XFOPT_$*),-Os) -c $< -o $@
$(BUILD)/rz.uo: user/rz.c user/xfer/xfer.h user/xfer/rzrules.h user/libz.h user/usys.h | $(BUILD)
	$(CC) $(UCFLAGS) -Iuser -Os -c $< -o $@
$(BUILD)/rz.uelf: $(BUILD)/rz.uo $(XFEROBJS) $(BUILD)/crt0.o $(BUILD)/libz.uo link/user.ld
	$(CC) $(UCFLAGS) $(ULDFLAGS) $(BUILD)/crt0.o $(BUILD)/rz.uo $(XFEROBJS) $(BUILD)/libz.uo $(LIBGCC) -o $@
$(BUILD)/drv-%.uo: net/drv/%.c net/drv/drv.h kernel/netproto.h kernel/hwinfo.h user/libz.h user/usys.h | $(BUILD)
	$(CC) $(UCFLAGS) -Os -c $< -o $@
# a driver's frames are its stack's few hundred bytes (the transfer block is static): 8 KB
STACK_drv-fake := 8192
STACK_drv-a2065 := 8192
STACK_drv-genet := 8192
# genet's chip and PHY code is genet-core.c, included by genet.c (and by the host test against the model)
$(BUILD)/drv-genet.uo: net/drv/genet-core.c net/drv/genet.h
# netd's stack: lwIP's deepest chain (a segment in, its answer out) and the log's formatting (D19 7.3: 8 KB)
STACK_netd := 8192

# ---- user programs: ZEXE, relocated by the kernel at load ----
$(BUILD)/crt0.o: user/crt0.S | $(BUILD)
	$(CC) $(ASFLAGS) $< -o $@
$(BUILD)/libz.uo: user/libz.c user/libz.h user/usys.h | $(BUILD)
	$(CC) $(UCFLAGS) -ffunction-sections -fdata-sections -c $< -o $@
$(BUILD)/%.uo: user/%.c user/libz.h user/usys.h | $(BUILD)
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/%.uelf: $(BUILD)/%.uo $(BUILD)/crt0.o $(BUILD)/libz.uo link/user.ld
	$(CC) $(UCFLAGS) $(ULDFLAGS) \
	    $(BUILD)/crt0.o $< $(BUILD)/libz.uo $(LIBGCC) -o $@
$(BUILD)/%.zx: $(BUILD)/%.uelf tools/mkzexe.py tools/check-68020-60.sh Makefile
	TOOLPREFIX=$(TOOLPREFIX) tools/check-68020-60.sh $<
	tools/mkzexe.py $< $@ $(or $(STACK_$*),$(STACK_DEFAULT))

# ---- programs with shared code (DESIGN D13, ZEX2: docs/d13-shared-code.md): one copy of the code for every process
# running it. Every object is compiled with -msep-data through tools/purecc.sh (the 16-bit table offsets) - libz and
# crt0 too, so a pure program has its own builds of them (.po) - and linked with link/pure.ld. NAME.pzx is the
# program; nothing in `all` uses one yet. ----
PULDFLAGS := -nostartfiles -T link/pure.ld -Wl,--build-id=none -Wl,-q -Wl,--gc-sections
$(BUILD)/crt0-pure.o: user/crt0.S | $(BUILD)
	$(CC) $(ASFLAGS) -DZEX2 $< -o $@
$(BUILD)/%.po: user/%.c user/libz.h user/usys.h tools/purecc.sh | $(BUILD)
	tools/purecc.sh $(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/%.puelf: $(BUILD)/%.po $(BUILD)/crt0-pure.o $(BUILD)/libz.po link/pure.ld
	$(CC) $(UCFLAGS) $(PULDFLAGS) $(BUILD)/crt0-pure.o $< $(BUILD)/libz.po $(LIBGCC) -o $@
$(BUILD)/%.pzx: $(BUILD)/%.puelf tools/mkzexe.py tools/check-68020-60.sh Makefile
	TOOLPREFIX=$(TOOLPREFIX) tools/check-68020-60.sh $<
	tools/mkzexe.py $< $@ $(or $(STACK_$*),$(STACK_DEFAULT))
# login also links the user and password parts (pure builds of them: pw.po, sha.po)
$(BUILD)/login.puelf: $(BUILD)/login.po $(BUILD)/pw.po $(BUILD)/sha.po $(BUILD)/sha256.po $(BUILD)/crt0-pure.o $(BUILD)/libz.po link/pure.ld
	$(CC) $(UCFLAGS) $(PULDFLAGS) $(BUILD)/crt0-pure.o $(BUILD)/login.po $(BUILD)/pw.po $(BUILD)/sha.po $(BUILD)/sha256.po $(BUILD)/libz.po $(LIBGCC) -o $@
$(BUILD)/login.po $(BUILD)/pw.po $(BUILD)/sha.po: user/pw.h
# DESIGN D13 stage 1: these commands are installed as programs with shared code (one copy of sh's and login's code
# for every console's session; ssh: one copy for every ssh session, D21 3.5). Nothing they do sends raw messages
# (limits.md); sh.c, login.c and ssh.c were checked with -Wwrite-strings: no write into a literal.
PUREAPPS := sh login ssh
$(PUREAPPS:%=$(BUILD)/%.zx): $(BUILD)/%.zx: $(BUILD)/%.pzx
	cp $< $@

# ---- updates (DESIGN D15, docs/d15-self-updates.md): the packages of a build and the bundle zenixboot installs ----
# build/pkg/NAME.manifest + NAME.tgz for the standard packages (tools/mkzup.py says which files each holds); the full
# root images get their manifests in /var/lib/zpkg (tools/mkroot.sh), so the first update knows what is installed.
packages: $(BUILD)/pkg/list
# the PC font (DESIGN D20 4.2: telnet's PC boards load it with CON_FONT): from Linux's font_8x8.c, on the full root
# images and in the base package as /usr/share/consolefonts/cp437-8x8.psf with its notice (tools/font_8x8.COPYING)
$(BUILD)/cp437-8x8.psf: tools/font_8x8.c tools/mkpsf.py | $(BUILD)
	python3 tools/mkpsf.py tools/font_8x8.c $@
$(BUILD)/pkg/list: $(BUILD)/cp437-8x8.psf $(USERZX) $(SBINZX) $(NETDRVZX) $(shell find rootfs/etc -type f) $(wildcard docs/man/*.txt) tools/mkzup.py tools/mkman.py kernel/version.h
	NETDRVS="$(NETDRVS)" python3 tools/mkzup.py packages $(BUILD) "$(UAPPS)"
# the bundle: the kernel and every package (copied to BOOT:Zenix/zenix-update.zup, docs/man/zenixboot.txt)
bundle: $(BUILD)/zenix-update.zup
$(BUILD)/zenix-update.zup: $(BUILD)/kernel.bin $(BUILD)/pkg/list tools/mkzup.py
	python3 tools/mkzup.py bundle $@ --kernel $(BUILD)/kernel.bin $(BUILD)/pkg $$(cat $(BUILD)/pkg/list)

# root filesystem: ext2 (old revision 0 format), built from rootfs/ + the programs; 1,024 inodes (512 ran out with the
# network's files in D19 step 8: cmds2-test makes 150 files)
$(BUILD)/zenix.hdf: $(BUILD)/cp437-8x8.psf $(USERZX) $(SBINZX) $(NETDRVZX) $(TESTPZX) $(shell find rootfs -type f) tools/mkroot.sh tools/mkman.py $(BUILD)/pkg/list
	PKGDB=1 tools/mkroot.sh $@ $(BUILD) "$(UAPPS) $(SBINAPPS) $(NETDRVS:%=drv-%)" 8M 1024

# the same disk wrapped in an RDB, as a real hard disk or CF card has it
$(BUILD)/zenix-rdb.hdf: $(BUILD)/zenix.hdf tools/mkrdb.py
	python3 tools/mkrdb.py $< $@

# the same root disk with ZenixFS as the root filesystem instead of ext2 (needs the host tool: make -C zenixfs build/zenixfs-tool)
$(BUILD)/zenix-zroot.hdf: $(USERZX) $(SBINZX) $(NETDRVZX) $(shell find rootfs -type f) tools/mkroot-zenixfs.sh tools/mkrdb.py tools/mkman.py $(wildcard zenixfs/*.c zenixfs/*.h zenixfs/tools/*.c)
	make -s -C zenixfs build/zenixfs-tool
	tools/mkroot-zenixfs.sh $@ $(BUILD) "$(UAPPS) $(SBINAPPS) $(NETDRVS:%=drv-%)" 8

# ---- booting without a floppy (zenixboot; docs/how-it-works.md, docs/pi-first-boot.md) ----
# The AmigaDOS boot volume Kickstart starts from: S/Startup-Sequence (one line), zenixboot, the kernel as "zenix".
# 33 RDB cylinders of 504 KB (16 heads x 63 sectors): the FFS volume fills its partition exactly.
BOOTFS_SIZE := 16632K
$(BUILD)/zenix-bootfs.img: $(BUILD)/zenixboot $(BUILD)/kernel.bin tools/mkffs.py
	printf 'SYS:zenixboot\n' > $(BUILD)/startup-sequence
	python3 tools/mkffs.py $@ $(BOOTFS_SIZE) ZenixBoot $(BUILD)/startup-sequence=S/Startup-Sequence \
	    $(BUILD)/zenixboot=zenixboot $(BUILD)/kernel.bin=zenix
# a hard disk / CF card of any Amiga: an RDB with the boot volume and the ext2 root (the same root as zenix.hdf)
$(BUILD)/zenix-boot.hdf: $(BUILD)/zenix-bootfs.img $(BUILD)/zenix.hdf tools/mkcard.py tools/mkrdb-multi.py
	python3 tools/mkcard.py ide $@ $(BUILD)/zenix-bootfs.img $(BUILD)/zenix.hdf
# the SD card of an Emu68 Amiga: MBR, p1 FAT for Emu68 (empty here), p2 0x76 RDB with the boot volume, p3 a 512 MB
# ext2 root with every program (written to a card by tools/write-card.sh)
$(BUILD)/root-card.img: $(BUILD)/cp437-8x8.psf $(USERZX) $(SBINZX) $(NETDRVZX) $(TESTPZX) $(shell find rootfs -type f) tools/mkroot.sh tools/mkman.py $(BUILD)/pkg/list
	PKGDB=1 tools/mkroot.sh $@ $(BUILD) "$(UAPPS) $(SBINAPPS) $(NETDRVS:%=drv-%)" 512M 8192
$(BUILD)/zenix-card.img: $(BUILD)/zenix-bootfs.img $(BUILD)/root-card.img tools/mkcard.py tools/mkrdb-multi.py
	python3 tools/mkcard.py sd $@ $(BUILD)/zenix-bootfs.img $(BUILD)/root-card.img

# boots in a window on this screen (for a person, never for a test; tools/fsuae_conf.py fills in the Kickstart directory,
# ZENIX_KICKSTART_DIR)
run: all
	ZENIX_BUILD=$(abspath $(BUILD)) python3 tools/fsuae_conf.py fsuae/a1200-020.fs-uae $(BUILD)/run.fs-uae
	fs-uae $(BUILD)/run.fs-uae

# the gate: build every image, run the host tests and then every strong emulator test one at a time, print a table, fail
# if anything failed (tools/check.sh; about 1.5 hours; docs/README.md "Testing"). Uses build/ of this tree.
check:
	tools/check.sh
test: check

clean:
	rm -rf $(BUILD)

.SECONDARY:
.DELETE_ON_ERROR:           # a recipe that fails leaves no half-written target behind for the next make to trust
.PHONY: all run test check clean packages bundle FORCE
