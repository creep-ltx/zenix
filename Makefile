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

KOBJS := $(BUILD)/start.o $(BUILD)/main.o $(BUILD)/serial.o $(BUILD)/mem.o $(BUILD)/ide.o $(BUILD)/emmc.o $(BUILD)/floppy.o $(BUILD)/input.o $(BUILD)/audio.o $(BUILD)/a3000.o $(BUILD)/blk.o $(BUILD)/rtc.o $(BUILD)/proc.o $(BUILD)/syscall.o $(BUILD)/vfs.o $(BUILD)/tty.o $(BUILD)/gfx.o $(BUILD)/term.o $(BUILD)/confont.o $(BUILD)/kbd.o $(BUILD)/ipc.o $(BUILD)/svc.o $(BUILD)/seg.o $(BUILD)/event.o $(BUILD)/signal.o $(BUILD)/random.o $(BUILD)/sock.o $(BUILD)/drv.o $(BUILD)/zine.o $(BUILD)/embed.o

UAPPS := diskd init sh ls cat echo pwd ps free wc grep uname hello spin crash badptr boot \
          mkdir rmdir rm cp mv touch msgpingd msgping socktestd socktest bench fdisk mkfs_ext2 fsck_ext2 df sync termtest \
          head tail sort uniq cut tr tee seq basename dirname yes sleep true false env printenv which clear \
          whoami hostname id uptime cmp hexdump stat expr find xargs du \
          sed diff chmod ln readlink date less edit man gzip tar input beep dmesg dd login su passwd chown sha256sum printf \
          tac rev nl fold paste comm expand mktemp realpath base64 md5sum cal time timeout kill pgrep watch tree mount umount mkfs_zenixfs fsck_zenixfs \
          nice renice priotest reboot cu awk svctest useradd userdel gpasswd sudo zup sigtest netstat nettest ifconfig route ping host telnet \
          rz cryptotest ssh ssh-keygen wget sntp zpkg sumi maki markdown ahiru ctop zine
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
# Markdown (ports/markdown): upstream's renderer recurses once per container inside a container, about 1 KB a level, to
# MAXDEPTH 24 (md.c), and upstream itself runs on a 32 KB stack (its STACKSZ): the same here
STACK_markdown := 32768
# boot (pid 1, built into the kernel image) lives for ever: its frames are under 500 bytes (-fstack-usage, 2026-10-26),
# so 4 KB, the loader's least; it gave back the memory D15 cost in 1 MB of chip RAM (three sessions again)
STACK_boot := 4096
# the 2 MB diet (DESIGN D18): init and diskd live for ever too and use under 700 bytes of stack (measured 2026-10-26
# in FS-UAE: the deepest byte written after a boot, logins on two consoles, ls -l, df, mount); init's frames are its
# 512-byte /etc/console buffer and printf, diskd has no recursion: 4 KB and 8 KB (16 before)
STACK_init := 4096
# init is cold code (it waits for its children all its life): -Os, which also pays for /etc/daemons (DESIGN D24 7)
$(BUILD)/init.uo: UCFLAGS += -Os
STACK_diskd := 8192
# every program links only the parts of libz it calls (libz is built one section per function)
ULDFLAGS := -nostartfiles -T link/user.ld -Wl,--build-id=none -Wl,-q -Wl,--gc-sections
USERZX := $(UAPPS:%=$(BUILD)/%.zx)
# servers that live on the disk as files (DESIGN D19 step 1), installed as /sbin/NAME on every full root image and in
# the base package: the ZenixFS server, which mount registers as a file service (D11 6.4), and the network server.
# Not commands: no page; wasabid (D24), the remote control daemon root starts, has docs/man/wasabid.txt; nfscd (D23),
# the server of an NFS mount, is described in docs/man/mount.txt.
SBINAPPS := zfsd netd wasabid nfscd
SBINZX := $(SBINAPPS:%=$(BUILD)/%.zx)
# the network's drivers (DESIGN D19 steps 7-8): user processes netd starts as /sbin/net/NAME IFACE [ARGS], from
# net/drv/NAME.c, built as drv-NAME. NETDRVS are shipped (/sbin/net on every full root image and in the base package);
# TESTDRVS only built: the driver protocol's test driver, which tools/drv-test.sh puts on its own image
NETDRVS := a2065 genet
TESTDRVS := fake
NETDRVZX := $(NETDRVS:%=$(BUILD)/drv-%.zx)
# programs with shared code (ZEX2, below) in the test images' /tests only, not commands (tools/mkroot.sh, DESIGN D13)
TESTPZX := $(BUILD)/hello.pzx $(BUILD)/crash.pzx $(BUILD)/sigtest.pzx
# the C library's test program (DESIGN D27 section 11, tools/libc-test.sh), in the test images' /tests: its ZEXE
# build and its ZEX2 one (libctestpure)
TESTLIBC := $(BUILD)/libctest.zx $(BUILD)/libctest.pzx $(BUILD)/tuitest.zx

all: $(BUILD)/zenix.adf $(BUILD)/zenix.hdf $(TESTDRVS:%=$(BUILD)/drv-%.zx) $(BUILD)/libc-pure.a

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
# so it needs no Amiga toolchain. It shares the hand-over code with the bootblock (boot/handoff.S). It checks a bundle's
# signature (DESIGN D21 6.4) with the crypto library's zc_sig_verify against the build's keys ($(BUILD)/zpkgkeys.h,
# below: the TEST key, or with RELEASE=1 the release keys).
$(BUILD)/zenixboot-start.o: boot/zenixboot-start.S boot/handoff.S | $(BUILD)
	$(CC) $(ASFLAGS) $< -o $@
$(BUILD)/zenixboot.bo: boot/zenixboot.c $(BUILD)/zpkgkeys.h crypto/zc/zc.h | $(BUILD)
	$(CC) $(CFLAGS) -Os -I$(BUILD) -c $< -o $@
$(BUILD)/zenixboot.bo: user/sha256.h
$(BUILD)/sha256.bo: user/sha256.c user/sha256.h | $(BUILD)
	$(CC) $(CFLAGS) -Os -c $< -o $@
$(BUILD)/zenixboot.elf: $(BUILD)/zenixboot-start.o $(BUILD)/zenixboot.bo $(BUILD)/sha256.bo $(BUILD)/libcrypto.a link/user.ld
	$(CC) $(CFLAGS) -nostartfiles -T link/user.ld -Wl,--build-id=none -Wl,-q -Wl,--gc-sections \
	    $(BUILD)/zenixboot-start.o $(BUILD)/zenixboot.bo $(BUILD)/sha256.bo $(BUILD)/libcrypto.a $(LIBGCC) -o $@
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
# DESIGN D28: the console's widths and the Unicode font's checks (cold, -Os; the width lookup keeps -O2 by an
# attribute), with the width table generated from the C library's own tables (picolibc's combining.t and wide.t)
KOPT_confont := -Os -I$(BUILD)
$(BUILD)/confont.o: $(BUILD)/conwidth.h
$(BUILD)/conwidth.h: tools/mkconfont.py libc/picolibc/newlib/libc/string/combining.t libc/picolibc/newlib/libc/string/wide.t | $(BUILD)
	python3 tools/mkconfont.py widths $@
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
# DESIGN D29 step 2: ZINE's kernel part (SYS_zine: the checks, the sync, the hand-over and the resume): cold, -Os
KOPT_zine := -Os
$(BUILD)/%.o: kernel/%.S | $(BUILD)
	$(CC) $(ASFLAGS) $< -o $@
$(BUILD)/kernel.elf: $(KOBJS) link/kernel.ld
	$(CC) $(CFLAGS) -nostartfiles -T link/kernel.ld -Wl,--build-id=none -Wl,-q -Wl,--gc-sections \
	    $(KOBJS) $(LIBGCC) -o $@
# The RAM floppy (zenix-ram.adf) holds the bootblock (1 KB), the kernel and the RAM disk (RAMDISK_K KB) in 880 KB: the
# kernel may grow to KERNEL_MAX bytes. Every build says how much is left; tools/mkadf.py refuses an image that is over.
# The mount tests' floppy (zenix-mount.adf) has a RAM disk of its own size (RAMDISK_MOUNT_K: it carries /sbin/zfsd,
# DESIGN D19 step 1), so its kernel limit is lower; the build says that one too. Its RAM disk carries no test
# scripts (NOTESTS=1: the mount tests run none of /tests); that made room for D20 step 1's 8 KB (3 blocks were free).
RAMDISK_K := 640
# 712 since DESIGN D28 step 2 (2026-10-29): the console's UTF-8 took about 9 KB of kernel; 33 blocks stay free there
RAMDISK_MOUNT_K := 712
KERNEL_MAX := $(shell echo $$((901120 - 1024 - $(RAMDISK_K) * 1024)))
KERNEL_MAX_MOUNT := $(shell echo $$((901120 - 1024 - $(RAMDISK_MOUNT_K) * 1024)))
$(BUILD)/kernel.bin: $(BUILD)/kernel.elf tools/mkzexe.py tools/check-68020-60.sh tools/ksize-check.sh tools/ksize-budget
	TOOLPREFIX=$(TOOLPREFIX) tools/check-68020-60.sh $<
	tools/mkzexe.py --delta $< $@
	@KSIZE_SKIP=$(if $(DIAG)$(GFXDEPTH),1) TOOLPREFIX=$(TOOLPREFIX) tools/ksize-check.sh $(BUILD) $@ $(KERNEL_MAX_MOUNT) || { rm -f $@; exit 1; }
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
	$(CC) $(CFLAGS) -nostartfiles -T link/kernel.ld -Wl,--build-id=none -Wl,-q -Wl,--gc-sections \
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
# the mount tests (tools/mounttest.sh) boot their own small floppy: the standard RAM image is full (passwd, whoami,
# uname and touch left out 2026-10-30, no mount test runs them: the allocator's code, about 1 KB more in each program,
# had taken the room the tests write into; 25 KB stay free, as before)
MOUNTAPPS := init login sh ls ln cmp seq cat echo pwd mkdir rm rmdir cp mv df sync fdisk mkfs_ext2 fsck_ext2 mount umount \
             mkfs_zenixfs fsck_zenixfs su id chmod wc head tail ps stat sleep true false env \
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

# zup (DESIGN D15, /sbin/zup): inflate (miniz), SHA-256 (sha.c) and the package formats' parsers (zpkgfmt.c, D22 4.2:
# one copy for zup, zpkg and the tests)
$(BUILD)/sha256.uo $(BUILD)/sha256.po: user/sha256.h
$(BUILD)/zpkgfmt.uo: user/zpkgfmt.c user/zpkgfmt.h user/sha256.h user/libz.h user/usys.h | $(BUILD)
	$(CC) $(UCFLAGS) -Os -c $< -o $@
$(BUILD)/zup.uo: user/zup.c user/zpkgfmt.h user/libz.h user/usys.h user/sha256.h kernel/fsproto.h | $(BUILD)
	$(CC) $(UCFLAGS) -Os $(MINIZ_CFLAGS) -c $< -o $@
$(BUILD)/zup.uelf: $(BUILD)/zup.uo $(BUILD)/zpkgfmt.uo $(BUILD)/miniz_tinfl.uo $(BUILD)/sha256.uo $(BUILD)/crt0.o $(BUILD)/libz.uo link/user.ld
	$(CC) $(UCFLAGS) $(ULDFLAGS) \
	    $(BUILD)/crt0.o $(BUILD)/zup.uo $(BUILD)/zpkgfmt.uo $(BUILD)/miniz_tinfl.uo $(BUILD)/sha256.uo $(BUILD)/libz.uo $(LIBGCC) -o $@

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
# wasabid (DESIGN D24, docs/man/wasabid.txt): the remote control daemon, /sbin/wasabid, its OS layer (user/wasabid.c)
# over its core in net/wasabi/ (no system calls, shared with the PC tests, tools/host/wasabi-test.sh) and regex.c
# (TEXT's patterns). All of it cold code: -Os (the main session, D24 step 0)
WSBOBJS := $(patsubst %,$(BUILD)/ws-%.no,core files info pure run screen regex)
$(BUILD)/ws-%.no: net/wasabi/%.c net/wasabi/wasabi.h net/wasabi/wasabi_int.h user/regex.h | $(BUILD)
	$(CC) $(UCFLAGS) -Os -c $< -o $@
$(BUILD)/ws-regex.no: user/regex.c user/regex.h user/libz.h | $(BUILD)
	$(CC) $(UCFLAGS) -Os -c $< -o $@
$(BUILD)/wasabid.uo: user/wasabid.c net/wasabi/wasabi.h user/socket.h kernel/netproto.h user/libz.h user/usys.h | $(BUILD)
	$(CC) $(UCFLAGS) -Os -c $< -o $@
$(BUILD)/wasabid.uelf: $(BUILD)/wasabid.uo $(WSBOBJS) $(BUILD)/crt0.o $(BUILD)/libz.uo link/user.ld
	$(CC) $(UCFLAGS) $(ULDFLAGS) $(BUILD)/crt0.o $(BUILD)/wasabid.uo $(WSBOBJS) $(BUILD)/libz.uo $(LIBGCC) -o $@
STACK_wasabid := 8192
# nfscd (DESIGN D23, docs/d23-nfs.md): the server of an NFS mount, /sbin/nfscd, its OS layer (user/nfscd.c) over the
# NFS client core in net/nfs/ (no system calls, shared with the PC tests, tools/host/nfs-test.sh). Cold code: -Os
NFSOBJS := $(patsubst %,$(BUILD)/nfs-%.no,core ops proto rpc table write)
$(BUILD)/nfs-%.no: net/nfs/%.c net/nfs/nfs.h net/nfs/nfs_int.h kernel/fsproto.h kernel/errno.h | $(BUILD)
	$(CC) $(UCFLAGS) -Os -c $< -o $@
$(BUILD)/nfscd.uo: user/nfscd.c net/nfs/nfs.h user/socket.h kernel/netproto.h kernel/fsproto.h user/libz.h user/usys.h | $(BUILD)
	$(CC) $(UCFLAGS) -Os -c $< -o $@
$(BUILD)/nfscd.uelf: $(BUILD)/nfscd.uo $(NFSOBJS) $(BUILD)/crt0.o $(BUILD)/libz.uo link/user.ld
	$(CC) $(UCFLAGS) $(ULDFLAGS) $(BUILD)/crt0.o $(BUILD)/nfscd.uo $(NFSOBJS) $(BUILD)/libz.uo $(LIBGCC) -o $@
STACK_nfscd := 8192
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
SNTPOBJS := $(BUILD)/cr/net/sntp/sntp.o $(BUILD)/clockfile.uo
# /var/lib/clock's reading and writing, one copy for sntp and reboot (reboot raises the floor from a trusted clock)
CLOCKOBJS := $(BUILD)/clockfile.uo $(BUILD)/cr/crypto/zc/zc_clock.o
$(BUILD)/clockfile.uo: user/clockfile.c user/clockfile.h crypto/zc/zc_clock.h user/libz.h user/usys.h | $(BUILD)
	$(CC) $(UCFLAGS) -Os -c $< -o $@
$(BUILD)/reboot.uo $(BUILD)/sntp.uo: user/clockfile.h
# reboot's floor is never below the build date (audit 2026-10-29 B2-2)
$(BUILD)/reboot.uo: user/reboot.c $(BUILD)/builddate.h crypto/zc/zc_clock.h user/libz.h user/usys.h | $(BUILD)
	$(CC) $(UCFLAGS) -I$(BUILD) -c $< -o $@
$(BUILD)/reboot.uelf: $(BUILD)/reboot.uo $(CLOCKOBJS) $(BUILD)/crt0.o $(BUILD)/libz.uo link/user.ld
	$(CC) $(UCFLAGS) $(ULDFLAGS) $(BUILD)/crt0.o $(BUILD)/reboot.uo $(CLOCKOBJS) $(BUILD)/libz.uo $(LIBGCC) -o $@
$(BUILD)/cr/user/wget/plan.o: user/wget/wget.h net/http/http.h
$(BUILD)/cr/net/http/http.o: net/http/http.h
$(BUILD)/cr/net/sntp/sntp.o: net/sntp/sntp.h
$(BUILD)/wget.uo $(BUILD)/sntp.uo: $(BUILD)/%.uo: user/%.c $(BUILD)/builddate.h crypto/zc/zc_clock.h user/socket.h user/libz.h user/usys.h | $(BUILD)
	$(CC) $(UCFLAGS) -Os -I$(BUILD) -Inet/port/shim $(CRYPTO_DEF) -c $< -o $@
$(BUILD)/wget.uo: user/wget/wget.h net/http/http.h crypto/zc/zc_tls.h crypto/zc/zc_x509.h
$(BUILD)/sntp.uo: net/sntp/sntp.h
# zpkg (DESIGN D22, docs/man/zpkg.txt) over its cores (user/zpkg/: the list and the trust rules, the resolver),
# net/http/, the package formats (zpkgfmt.c), Ed25519 from the library, the clock file's code; its trusted keys built
# in (tools/zsign.py cheader: the TEST key, or with RELEASE=1 tools/keys/zenix-release-*.pub, D21 6.3)
ZPKGOBJS := $(BUILD)/cr/user/zpkg/index.o $(BUILD)/cr/user/zpkg/resolve.o $(BUILD)/cr/net/http/http.o $(BUILD)/zpkgfmt.uo \
            $(BUILD)/sha256.uo $(BUILD)/cr/crypto/zc/zc_clock.o
# RELEASE_KEYS: where the release public keys are (default tools/keys; the tests give a folder of their own)
RELEASE_KEYS ?= tools/keys
ifneq ($(filter-out 1,$(RELEASE)),)
$(error RELEASE=1 makes a release build; leave it out for a test build (RELEASE=$(RELEASE) means neither))
endif
$(BUILD)/zpkgkeys.h: tools/zsign.py FORCE | $(BUILD)
	@python3 tools/zsign.py cheader $@ $(if $(RELEASE),--release $(RELEASE_KEYS))
$(BUILD)/cr/user/zpkg/index.o $(BUILD)/cr/user/zpkg/resolve.o: user/zpkg/zpkg.h
$(BUILD)/zpkg.uo: user/zpkg.c $(BUILD)/zpkgkeys.h $(BUILD)/builddate.h user/zpkg/zpkg.h user/zpkgfmt.h net/http/http.h crypto/zc/zc.h crypto/zc/zc_clock.h user/socket.h user/libz.h user/usys.h | $(BUILD)
	$(CC) $(UCFLAGS) -Os -I$(BUILD) -Inet/port/shim $(CRYPTO_DEF) -c $< -o $@
$(BUILD)/zpkg.uelf: $(BUILD)/zpkg.uo $(ZPKGOBJS) $(BUILD)/libcrypto.a $(BUILD)/crt0.o $(BUILD)/libz.uo link/user.ld
	$(CC) $(UCFLAGS) $(ULDFLAGS) $(BUILD)/crt0.o $(BUILD)/zpkg.uo $(ZPKGOBJS) $(BUILD)/libcrypto.a $(BUILD)/libz.uo $(LIBGCC) -o $@
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

# ---- the C library (DESIGN D27, docs/d27-c-library.md): picolibc 1.8.10 vendored in libc/picolibc (only the files
# built, LIBC_SRCS in its sources.mk; no meson), Zenix's port in libc/zenix, and the files it shares with libz
# (user/zshare.h). Two archives, as libz's two builds: libc.a for ZEXE programs, libc-pure.a (every C object through
# tools/purecc.sh) for ZEX2. -Os, the formatter included; -O2 for the string functions, m68k's memcpy/memset, the
# stdio buffer path and the allocator (section 4's golden path). Zenix's constants come from the kernel's headers
# (tools/libc-consts.sh); the port's headers go ahead of picolibc's. A C library program links $(BUILD)/libc.a before
# $(LIBGCC), with the long long printf (LIBC_LDFLAGS). ----
include libc/picolibc/sources.mk
# libz's files the C library builds too (user/zshare.h; libz.uo below): the allocator, the environment, the buffered
# output, the error texts, the signals, poll and select, the sockets, the helpers (paths, colours) and the passwd parser
LIBZ_SHARED := zmalloc zenv zout zerrtab zsig zpoll zsock zhelp zpw
# the port: the system layer, the start and the end, the streams, strerror, dprintf, directories, the 64-bit division
# (section 7: libgcc's is not pure), the terminal, processes, users, glob, regcomp (over user/regex.c, the one engine)
LIBC_PORT = libc/zenix/sys.c libc/zenix/start.c libc/zenix/stdio.c libc/zenix/strerror.c libc/zenix/dprintf.c libc/zenix/dirent.c \
             libc/zenix/divdi3.c libc/zenix/termios.c libc/zenix/spawn.c libc/zenix/pwd.c libc/zenix/glob.c libc/zenix/regcomp.c \
             libc/zenix/locale.c \
             user/regex.c $(LIBZ_SHARED:%=user/%.c)
LIBC_O2 := $(filter newlib/libc/machine/m68k/%,$(LIBC_SRCS)) \
           $(addprefix newlib/libc/string/,memchr.c memcmp.c memmove.c strcat.c strchr.c strcmp.c strcpy.c strcspn.c \
             strlen.c strncat.c strncmp.c strncpy.c strnlen.c strrchr.c strspn.c strstr.c) \
           $(addprefix newlib/libc/tinystdio/,bufio.c fgetc.c fputc.c fgets.c fputs.c fwrite.c fread.c getchar.c \
             putchar.c) \
           user/zmalloc.c
LIBC_INC := -Ilibc/zenix/include -I$(BUILD)/libc-inc -Ilibc/picolibc/newlib/libc/machine/m68k \
            -Ilibc/picolibc/newlib/libc/tinystdio -Ilibc/picolibc/newlib/libc/locale -Ilibc/picolibc \
            -Ilibc/picolibc/newlib/libc/include -Ilibc/picolibc/newlib/libm/common
LIBC_CFLAGS := -m68020-60 -msoft-float -ffreestanding -nostdlib -g -ffunction-sections -fdata-sections -fno-common \
               -frounding-math -fsignaling-nans -fno-builtin-copysignl -fno-stack-protector -D_LIBC -U_FORTIFY_SOURCE \
               $(LIBC_INC)
LIBC_HDRS := $(BUILD)/libc-inc/zenix-consts.h libc/picolibc/picolibc.h $(wildcard libc/zenix/include/*.h libc/zenix/include/*/*.h) \
             user/socket.h user/zpw.h user/regex.h
LIBC_OBJS = $(patsubst %,$(BUILD)/lc/%.o,$(basename $(LIBC_SRCS))) $(patsubst %.c,$(BUILD)/lcz/%.o,$(LIBC_PORT))
LIBC_POBJS = $(patsubst %,$(BUILD)/lcp/%.o,$(basename $(LIBC_SRCS))) $(patsubst %.c,$(BUILD)/lczp/%.o,$(LIBC_PORT))
LIBC_OPT = $(if $(filter $(1),$(LIBC_O2)),-O2,-Os)
LIBC_NOBUILTIN = $(if $(filter newlib/libc/machine/m68k/%,$(1)),-fno-builtin)
# mkostemps' mask names O_SYNC, which Zenix does not define (a caller cannot pass it): 0 there
LIBC_DEF_newlib/libc/tinystdio/mktemp.c := -DO_SYNC=0
# DESIGN D28 11: picolibc's setlocale ignores the environment for ""; the port's (libc/zenix/locale.c) reads LC_ALL,
# LC_CTYPE and LANG and calls picolibc's under this name
LIBC_DEF_newlib/libc/locale/setlocale.c := -Dsetlocale=__pico_setlocale
$(BUILD)/lc/newlib/libc/locale/setlocale.o $(BUILD)/lcp/newlib/libc/locale/setlocale.o: Makefile
LIBC_LDFLAGS := -Wl,--defsym=vfprintf=__l_vfprintf -Wl,--defsym=vfscanf=__l_vfscanf
$(BUILD)/libc-inc/zenix-consts.h: tools/libc-consts.sh kernel/kernel.h kernel/fsproto.h kernel/ext2.h kernel/netproto.h user/usys.h
	@mkdir -p $(dir $@)
	tools/libc-consts.sh $@
$(BUILD)/lc/%.o: libc/picolibc/%.c $(LIBC_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(LIBC_CFLAGS) $(LIBC_DEF_$*.c) $(call LIBC_NOBUILTIN,$*.c) $(call LIBC_OPT,$*.c) -c $< -o $@
$(BUILD)/lc/%.o: libc/picolibc/%.S
	@mkdir -p $(dir $@)
	$(CC) $(LIBC_CFLAGS) -fno-builtin -O2 -c $< -o $@
$(BUILD)/lcz/%.o: %.c $(LIBC_HDRS) user/zshare.h user/usys.h
	@mkdir -p $(dir $@)
	$(CC) $(LIBC_CFLAGS) -Wall -Wextra -DZENIX_LIBC -Iuser -Ilibc/picolibc/newlib/libc/tinystdio $(call LIBC_OPT,$*.c) $(UDIAG) -c $< -o $@
$(BUILD)/lcp/%.o: libc/picolibc/%.c $(LIBC_HDRS) tools/purecc.sh
	@mkdir -p $(dir $@)
	tools/purecc.sh $(CC) $(LIBC_CFLAGS) $(LIBC_DEF_$*.c) $(call LIBC_NOBUILTIN,$*.c) $(call LIBC_OPT,$*.c) -c $< -o $@
$(BUILD)/lcp/%.o: libc/picolibc/%.S
	@mkdir -p $(dir $@)
	$(CC) $(LIBC_CFLAGS) -fno-builtin -O2 -c $< -o $@
$(BUILD)/lczp/%.o: %.c $(LIBC_HDRS) user/zshare.h user/usys.h tools/purecc.sh
	@mkdir -p $(dir $@)
	tools/purecc.sh $(CC) $(LIBC_CFLAGS) -Wall -Wextra -DZENIX_LIBC -Iuser $(call LIBC_OPT,$*.c) $(UDIAG) -c $< -o $@
$(BUILD)/libc.a: $(LIBC_OBJS) libc/picolibc/sources.mk tools/check-68020-60.sh
	rm -f $@
	$(TOOLPREFIX)ar rcs $@ $(LIBC_OBJS)
	TOOLPREFIX=$(TOOLPREFIX) tools/check-68020-60.sh $@
# C library programs (LIBCAPPS; the size table's test programs, tools/libc-size.sh, in $(BUILD)/lcs): compiled against
# the port's and picolibc's headers, linked with libz's crt0 and libc.a, the long long printf unless the program
# names the floating point one (LIBC_FLOAT_name = 1: printf's %f, %g, scanf's too)
LIBCAPPS := libctest
LIBC_PFLAGS := -m68020-60 -msoft-float -ffreestanding -nostdlib -g -Os -Wall -Wextra -fno-common -ffunction-sections \
               -fdata-sections $(LIBC_INC)
LIBC_FLOATLD := -Wl,--defsym=vfprintf=__d_vfprintf -Wl,--defsym=vfscanf=__d_vfscanf
LIBC_FLOAT_float := 1
$(LIBCAPPS:%=$(BUILD)/%.co): $(BUILD)/%.co: user/%.c $(LIBC_HDRS) | $(BUILD)
	$(CC) $(LIBC_PFLAGS) -c $< -o $@
$(BUILD)/lcs/%.co: tools/tests/libc-size/%.c $(LIBC_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(LIBC_PFLAGS) -c $< -o $@
$(BUILD)/%.celf: $(BUILD)/%.co $(BUILD)/crt0.o $(BUILD)/libc.a link/user.ld
	$(CC) $(LIBC_PFLAGS) $(ULDFLAGS) $(if $(LIBC_FLOAT_$(notdir $*)),$(LIBC_FLOATLD),$(LIBC_LDFLAGS)) \
	    $(BUILD)/crt0.o $< $(BUILD)/libc.a $(LIBGCC) -o $@
$(LIBCAPPS:%=$(BUILD)/%.zx): $(BUILD)/%.zx: $(BUILD)/%.celf tools/mkzexe.py tools/check-68020-60.sh
	TOOLPREFIX=$(TOOLPREFIX) tools/check-68020-60.sh $<
	tools/mkzexe.py $< $@ $(or $(STACK_$*),$(STACK_DEFAULT))
$(BUILD)/lcs/%.zx: $(BUILD)/lcs/%.celf tools/mkzexe.py tools/check-68020-60.sh
	TOOLPREFIX=$(TOOLPREFIX) tools/check-68020-60.sh $<
	tools/mkzexe.py $< $@ $(STACK_DEFAULT)
$(BUILD)/libc-pure.a: $(LIBC_POBJS) libc/picolibc/sources.mk tools/check-68020-60.sh
	rm -f $@
	$(TOOLPREFIX)ar rcs $@ $(LIBC_POBJS)
	TOOLPREFIX=$(TOOLPREFIX) tools/check-68020-60.sh $@
# C library programs with shared code (ZEX2, DESIGN D27 section 7): compiled through tools/purecc.sh, linked with the
# pure crt0, link/pure.ld and libc-pure.a (its own 64-bit division ahead of libgcc's, which a pure program cannot use);
# libctest.pzx is /tests/libctestpure on the test images (tools/libc-test.sh runs it on two consoles at once)
$(LIBCAPPS:%=$(BUILD)/%.cpo): $(BUILD)/%.cpo: user/%.c $(LIBC_HDRS) tools/purecc.sh | $(BUILD)
	tools/purecc.sh $(CC) $(LIBC_PFLAGS) -c $< -o $@
$(BUILD)/%.cpuelf: $(BUILD)/%.cpo $(BUILD)/crt0-pure.o $(BUILD)/libc-pure.a link/pure.ld
	$(CC) $(LIBC_PFLAGS) $(PULDFLAGS) $(LIBC_LDFLAGS) $(BUILD)/crt0-pure.o $< $(BUILD)/libc-pure.a $(LIBGCC) -o $@
$(LIBCAPPS:%=$(BUILD)/%.pzx): $(BUILD)/%.pzx: $(BUILD)/%.cpuelf tools/mkzexe.py tools/check-68020-60.sh
	TOOLPREFIX=$(TOOLPREFIX) tools/check-68020-60.sh $<
	tools/mkzexe.py $< $@ $(or $(STACK_$*),$(STACK_DEFAULT))

# ---- the text-UI library (roadmap block 3; DESIGN D27 sections 8-9: linked into each program, no shared library):
# the owner's tui.library, its upstream source pinned in ports/tui/upstream (tools/tui-vendor.sh, unchanged) and
# compiled through Zenix's layer in ports/tui/zenix (ports/tui/README). TUI_UTF8=1 (the default since the console's
# UTF-8, DESIGN D28) is UTF-8 output and the C library's wcwidth; TUI_UTF8=0 the Latin-1 build of before. tuitest is
# its test program (/tests/tuitest, tools/tui-test.sh), a C library program linked with libtui.a.
TUI_UTF8 ?= 1
TUI_CFLAGS := $(LIBC_PFLAGS) -Iports/tui/zenix/include -Iports/tui/upstream/include -DTUI_STATIC \
              $(if $(filter 1,$(TUI_UTF8)),-DZENIX_TUI_UTF8)
TUI_HDRS := $(LIBC_HDRS) $(wildcard ports/tui/zenix/include/*.h ports/tui/zenix/include/*/*.h) \
            $(wildcard ports/tui/upstream/include/*.h ports/tui/upstream/include/*/*.h) Makefile
# (Makefile: TUI_UTF8's default lives here, and the objects must follow it)
$(BUILD)/tui/%.o: ports/tui/zenix/%.c ports/tui/upstream/src/%.c $(TUI_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(TUI_CFLAGS) -c $< -o $@
# (core-other.o: the other setting's build of the same file, compiled with every build so the switch keeps building;
# not used)
$(BUILD)/tui/core-other.o: ports/tui/zenix/core.c ports/tui/upstream/src/core.c $(TUI_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -DZENIX_TUI_UTF8,$(TUI_CFLAGS)) $(if $(filter 1,$(TUI_UTF8)),,-DZENIX_TUI_UTF8) -c $< -o $@
# (sigfd.o: TuiWait's extra signals none, a member of its own: linked only when the program gives none, ports/common)
$(BUILD)/tui/sigfd.o: ports/tui/zenix/sigfd.c $(TUI_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(TUI_CFLAGS) -c $< -o $@
$(BUILD)/libtui.a: $(BUILD)/tui/core.o $(BUILD)/tui/glue.o $(BUILD)/tui/sigfd.o $(BUILD)/tui/core-other.o tools/check-68020-60.sh
	rm -f $@
	$(TOOLPREFIX)ar rcs $@ $(BUILD)/tui/core.o $(BUILD)/tui/glue.o $(BUILD)/tui/sigfd.o
	TOOLPREFIX=$(TOOLPREFIX) tools/check-68020-60.sh $@
$(BUILD)/tuitest.co: user/tuitest.c $(TUI_HDRS) user/usys.h | $(BUILD)
	$(CC) $(TUI_CFLAGS) -Iuser -c $< -o $@
$(BUILD)/tuitest.celf: $(BUILD)/tuitest.co $(BUILD)/crt0.o $(BUILD)/libtui.a $(BUILD)/libc.a link/user.ld
	$(CC) $(LIBC_PFLAGS) $(ULDFLAGS) $(LIBC_LDFLAGS) $(BUILD)/crt0.o $< $(BUILD)/libtui.a $(BUILD)/libc.a $(LIBGCC) -o $@
$(BUILD)/tuitest.zx: $(BUILD)/tuitest.celf tools/mkzexe.py tools/check-68020-60.sh
	TOOLPREFIX=$(TOOLPREFIX) tools/check-68020-60.sh $<
	tools/mkzexe.py $< $@ $(STACK_DEFAULT)
# the pure build of the library (D27 9 rule 4), for the programs with shared code that use it (Sumi, Maki, Markdown)
$(BUILD)/tuip/%.o: ports/tui/zenix/%.c ports/tui/upstream/src/%.c $(TUI_HDRS) tools/purecc.sh
	@mkdir -p $(dir $@)
	tools/purecc.sh $(CC) $(TUI_CFLAGS) -c $< -o $@
$(BUILD)/tuip/sigfd.o: ports/tui/zenix/sigfd.c $(TUI_HDRS) tools/purecc.sh
	@mkdir -p $(dir $@)
	tools/purecc.sh $(CC) $(TUI_CFLAGS) -c $< -o $@
$(BUILD)/libtui-pure.a: $(BUILD)/tuip/core.o $(BUILD)/tuip/glue.o $(BUILD)/tuip/sigfd.o tools/check-68020-60.sh
	rm -f $@
	$(TOOLPREFIX)ar rcs $@ $(BUILD)/tuip/core.o $(BUILD)/tuip/glue.o $(BUILD)/tuip/sigfd.o
	TOOLPREFIX=$(TOOLPREFIX) tools/check-68020-60.sh $@

# ---- The owner's terminal programs (roadmap block 3; the owner's TextTools: Sumi the editor, Maki the pager,
# Markdown the viewer; ports/PROG/README): upstream's files pinned in ports/PROG/upstream (tools/texttools-vendor.sh,
# unchanged), compiled over Zenix's AmigaOS layer shared by the three (ports/common: the headers and dos.c, compiled
# for each program with the settings names it maps) and linked with the text-UI library and the C library, as
# programs with shared code (ZEX2: a second one costs only its data). /bin/sumi, /bin/maki, /bin/markdown (UAPPS; no
# user/PROG.c: these rules build them). TT_NAMES_PROG: the names after ENVARC: (and ENV:) the program's own literals
# use, mapped to $HOME/.config/NAME; the first is the program's, also for T:NAME- (/tmp/NAME-). ----
TEXTTOOLS := sumi maki markdown
TT_SRCS_sumi := main buf file syntax clip keys
TT_SRCS_maki := main src keys drawer filecol
TT_SRCS_markdown := main buf file syntax clip keys md mdedit view
TT_NAMES_sumi := "Sumi"
TT_NAMES_maki := "Maki","FileColours"
TT_NAMES_markdown := "Markdown"
TT_CFLAGS := $(LIBC_PFLAGS) -Iports/common/include -Iports/tui/upstream/include -DTUI_STATIC
TT_HDRS := $(LIBC_HDRS) $(wildcard ports/common/include/*.h ports/common/include/*/*.h ports/tui/upstream/include/*.h \
           ports/tui/upstream/include/*/*.h)
define TEXTTOOL_RULES
$$(BUILD)/$(1)/%.o: ports/$(1)/upstream/src/%.c $$(TT_HDRS) $$(wildcard ports/$(1)/upstream/src/*.h) tools/purecc.sh
	@mkdir -p $$(dir $$@)
	tools/purecc.sh $$(CC) $$(TT_CFLAGS) -Wno-sign-compare -Wno-unused-parameter -Wno-unused-but-set-variable -c $$< -o $$@
$$(BUILD)/$(1)/zenix-dos.o: ports/common/dos.c $$(TT_HDRS) tools/purecc.sh Makefile
	@mkdir -p $$(dir $$@)
	tools/purecc.sh $$(CC) $$(TT_CFLAGS) '-DZENIX_TT_NAMES=$$(TT_NAMES_$(1))' -c $$< -o $$@
$$(BUILD)/$(1).cpuelf: $$(TT_SRCS_$(1):%=$$(BUILD)/$(1)/%.o) $$(BUILD)/$(1)/zenix-dos.o $$(BUILD)/crt0-pure.o \
                       $$(BUILD)/libtui-pure.a $$(BUILD)/libc-pure.a link/pure.ld
	$$(CC) $$(LIBC_PFLAGS) $$(PULDFLAGS) $$(LIBC_LDFLAGS) $$(BUILD)/crt0-pure.o $$(TT_SRCS_$(1):%=$$(BUILD)/$(1)/%.o) \
	    $$(BUILD)/$(1)/zenix-dos.o $$(BUILD)/libtui-pure.a $$(BUILD)/libc-pure.a $$(LIBGCC) -o $$@
$$(BUILD)/$(1).pzx: $$(BUILD)/$(1).cpuelf tools/mkzexe.py tools/check-68020-60.sh
	TOOLPREFIX=$$(TOOLPREFIX) tools/check-68020-60.sh $$<
	tools/mkzexe.py $$< $$@ $$(or $$(STACK_$(1)),$$(STACK_DEFAULT))
$$(BUILD)/$(1).zx: $$(BUILD)/$(1).pzx
	cp $$< $$@
endef
$(foreach p,$(TEXTTOOLS),$(eval $(call TEXTTOOL_RULES,$(p))))

# ---- Ahiru, the owner's file manager (roadmap block 3; ports/ahiru/README): its upstream files and the CFile core's
# under it (the owner's CFile repository: ahiru/src, core/, fs/), pinned in ports/ahiru/upstream (tools/ahiru-vendor.sh,
# unchanged but for the MIT notice Zenix's copy adds), compiled over the AmigaOS layer of the owner's programs
# (ports/common: dos.c with Ahiru's settings names, gui.c: the screen calls, which never open) and Ahiru's own Zenix
# files (ports/ahiru/zenix: the core's OS layer os.c, the parts with no place on Zenix none.c, and term.c and list.c,
# upstream's compiled with one function each of Zenix's), linked with the text-UI library and the C library as a
# program with shared code (ZEX2). /bin/ahiru (UAPPS). Each directory's objects in a folder of their own (fs/ops.c and
# ahiru/src/ops.c, core/sums.c and fs/sums.c share names). ----
AHIRU_UP := main ui preview ops fe keys more more2 filecol
AHIRU_CORE := core util arc lha lzx zip adf adfw iso dms version rename sums
AHIRU_FS := find place ops icon kind arcfs arcw keymap sync renames usercmd sums
AHIRU_ZX := os none term list
AHIRU_OBJS := $(AHIRU_UP:%=$(BUILD)/ahiru/ahiru/%.o) $(AHIRU_CORE:%=$(BUILD)/ahiru/core/%.o) \
              $(AHIRU_FS:%=$(BUILD)/ahiru/fs/%.o) $(AHIRU_ZX:%=$(BUILD)/ahiru/zenix/%.o) \
              $(BUILD)/ahiru/zenix-dos.o $(BUILD)/ahiru/zenix-gui.o
AHIRU_CFLAGS := $(TT_CFLAGS) -Iports/ahiru/upstream -Iports/ahiru/upstream/src -Iports/ahiru/upstream/ahiru/src \
                -Wno-sign-compare -Wno-unused-parameter -Wno-unused-but-set-variable -Wno-pointer-sign \
                -Wno-missing-field-initializers
AHIRU_HDRS := $(TT_HDRS) $(wildcard ports/ahiru/upstream/*/*.h ports/ahiru/upstream/ahiru/src/*.h)
$(BUILD)/ahiru/ahiru/%.o: ports/ahiru/upstream/ahiru/src/%.c $(AHIRU_HDRS) tools/purecc.sh
	@mkdir -p $(dir $@)
	tools/purecc.sh $(CC) $(AHIRU_CFLAGS) -c $< -o $@
$(BUILD)/ahiru/core/%.o: ports/ahiru/upstream/core/%.c $(AHIRU_HDRS) tools/purecc.sh
	@mkdir -p $(dir $@)
	tools/purecc.sh $(CC) $(AHIRU_CFLAGS) -c $< -o $@
$(BUILD)/ahiru/fs/%.o: ports/ahiru/upstream/fs/%.c $(AHIRU_HDRS) tools/purecc.sh
	@mkdir -p $(dir $@)
	tools/purecc.sh $(CC) $(AHIRU_CFLAGS) -c $< -o $@
$(BUILD)/ahiru/zenix/%.o: ports/ahiru/zenix/%.c $(AHIRU_HDRS) $(wildcard ports/ahiru/upstream/ahiru/src/*.c) tools/purecc.sh
	@mkdir -p $(dir $@)
	tools/purecc.sh $(CC) $(AHIRU_CFLAGS) -c $< -o $@
$(BUILD)/ahiru/zenix-dos.o: ports/common/dos.c $(TT_HDRS) tools/purecc.sh Makefile
	@mkdir -p $(dir $@)
	tools/purecc.sh $(CC) $(TT_CFLAGS) '-DZENIX_TT_NAMES="Ahiru","FileColours"' -c $< -o $@
$(BUILD)/ahiru/zenix-gui.o: ports/common/gui.c $(TT_HDRS) tools/purecc.sh
	@mkdir -p $(dir $@)
	tools/purecc.sh $(CC) $(TT_CFLAGS) -c $< -o $@
$(BUILD)/ahiru.cpuelf: $(AHIRU_OBJS) $(BUILD)/crt0-pure.o $(BUILD)/libtui-pure.a $(BUILD)/libc-pure.a link/pure.ld
	$(CC) $(LIBC_PFLAGS) $(PULDFLAGS) $(LIBC_LDFLAGS) $(BUILD)/crt0-pure.o $(AHIRU_OBJS) $(BUILD)/libtui-pure.a \
	    $(BUILD)/libc-pure.a $(LIBGCC) -o $@
$(BUILD)/ahiru.pzx: $(BUILD)/ahiru.cpuelf tools/mkzexe.py tools/check-68020-60.sh
	TOOLPREFIX=$(TOOLPREFIX) tools/check-68020-60.sh $<
	tools/mkzexe.py $< $@ $(or $(STACK_ahiru),$(STACK_DEFAULT))
$(BUILD)/ahiru.zx: $(BUILD)/ahiru.pzx
	cp $< $@

# ---- ctop, the owner's btop-like monitor (roadmap block 3; ports/ctop/README): upstream's ctop.c pinned in
# ports/ctop/upstream (tools/ctop-vendor.sh, unchanged), compiled over the AmigaOS layer of the owner's programs
# (ports/common: dos.c with ctop's settings name) and ctop's own Zenix files (ports/ctop/zenix: the part of CUtils'
# library it calls, cu.h and cu.c, and bsdsocket.library's base, sockbase.S), linked with the text-UI library and the
# C library as a program with shared code (ZEX2). /bin/ctop (UAPPS). ----
CTOP_OBJS := $(BUILD)/ctop/ctop.o $(BUILD)/ctop/cu.o $(BUILD)/ctop/sockbase.o $(BUILD)/ctop/zenix-dos.o
CTOP_HDRS := $(TT_HDRS) ports/ctop/zenix/cu.h
# upstream calls bsdsocket.library itself (inline asm: the base in a6, registers named without %): the assembler is
# told so, and nothing is inlined, so that no function holding those calls keeps its frame in a6 (this GCC's frame
# pointer; AmigaOS's GCC frames with a5): an inlined call wrote ctop's locals into its code (found 2026-10-30)
CTOP_FIX := -Wa,--register-prefix-optional -fno-inline
$(BUILD)/ctop/ctop.o: ports/ctop/upstream/src/ctop.c $(CTOP_HDRS) tools/purecc.sh Makefile
	@mkdir -p $(dir $@)
	tools/purecc.sh $(CC) $(TT_CFLAGS) -Iports/ctop/zenix -Wno-sign-compare -Wno-unused-parameter $(CTOP_FIX) -c $< -o $@
$(BUILD)/ctop/cu.o: ports/ctop/zenix/cu.c $(CTOP_HDRS) user/usys.h kernel/netproto.h tools/purecc.sh
	@mkdir -p $(dir $@)
	tools/purecc.sh $(CC) $(TT_CFLAGS) -Iports/ctop/zenix -c $< -o $@
$(BUILD)/ctop/sockbase.o: ports/ctop/zenix/sockbase.S | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) $< -o $@
$(BUILD)/ctop/zenix-dos.o: ports/common/dos.c $(TT_HDRS) tools/purecc.sh Makefile
	@mkdir -p $(dir $@)
	tools/purecc.sh $(CC) $(TT_CFLAGS) '-DZENIX_TT_NAMES="ctop"' -c $< -o $@
$(BUILD)/ctop.cpuelf: $(CTOP_OBJS) $(BUILD)/crt0-pure.o $(BUILD)/libtui-pure.a $(BUILD)/libc-pure.a link/pure.ld
	$(CC) $(LIBC_PFLAGS) $(PULDFLAGS) $(LIBC_LDFLAGS) $(BUILD)/crt0-pure.o $(CTOP_OBJS) $(BUILD)/libtui-pure.a \
	    $(BUILD)/libc-pure.a $(LIBGCC) -o $@
$(BUILD)/ctop.pzx: $(BUILD)/ctop.cpuelf tools/mkzexe.py tools/check-68020-60.sh
	TOOLPREFIX=$(TOOLPREFIX) tools/check-68020-60.sh $<
	tools/mkzexe.py $< $@ $(or $(STACK_ctop),$(STACK_DEFAULT))
$(BUILD)/ctop.zx: $(BUILD)/ctop.pzx
	cp $< $@

# ---- user programs: ZEXE, relocated by the kernel at load ----
$(BUILD)/crt0.o: user/crt0.S | $(BUILD)
	$(CC) $(ASFLAGS) $< -o $@
# libz.uo is user/libz.c and the files it shares with the C library (DESIGN D27 section 6 step 1, user/zshare.h),
# joined into one object (ld -r keeps every function's section, so --gc-sections still takes only what is called)
$(BUILD)/libz-main.uo: user/libz.c user/libz.h user/zshare.h user/usys.h kernel/cwidth.h $(BUILD)/conwidth.h | $(BUILD)
	$(CC) $(UCFLAGS) -I$(BUILD) -c $< -o $@
$(LIBZ_SHARED:%=$(BUILD)/%.uo) $(LIBZ_SHARED:%=$(BUILD)/%.po): user/zshare.h user/zpw.h
$(BUILD)/zsock.uo $(BUILD)/zsock.po: user/socket.h kernel/netproto.h
$(BUILD)/libz.uo: $(BUILD)/libz-main.uo $(LIBZ_SHARED:%=$(BUILD)/%.uo)
	$(CC) -m68020-60 -nostdlib -r $^ -o $@
$(BUILD)/libz-main.po: user/libz.c user/libz.h user/zshare.h user/usys.h kernel/cwidth.h $(BUILD)/conwidth.h tools/purecc.sh | $(BUILD)
	tools/purecc.sh $(CC) $(UCFLAGS) -I$(BUILD) -c $< -o $@
$(BUILD)/libz.po: $(BUILD)/libz-main.po $(LIBZ_SHARED:%=$(BUILD)/%.po)
	$(CC) -m68020-60 -nostdlib -r $^ -o $@
$(BUILD)/%.uo: user/%.c user/libz.h user/usys.h | $(BUILD)
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/%.uelf: $(BUILD)/%.uo $(BUILD)/crt0.o $(BUILD)/libz.uo link/user.ld
	$(CC) $(UCFLAGS) $(ULDFLAGS) \
	    $(BUILD)/crt0.o $< $(BUILD)/libz.uo $(LIBGCC) -o $@
$(BUILD)/%.zx: $(BUILD)/%.uelf tools/mkzexe.py tools/check-68020-60.sh Makefile
	TOOLPREFIX=$(TOOLPREFIX) tools/check-68020-60.sh $<
	tools/mkzexe.py $< $@ $(or $(STACK_$*),$(STACK_DEFAULT))

# ---- ZINE (DESIGN D29, docs/d29-zine.md): /bin/zine, its blob /lib/zine/zine.blob (the small AmigaOS, run by the
# kernel in supervisor mode; a ZEXE image zine relocates, with a "ZINB" header and a CRC: tools/mkzblob.py) and the
# test programs, AmigaOS hunk executables written for the purpose (tools/tests/zine/*.S, tools/mkhunktest.py), in
# the test images' /tests/zine. The cores of user/zine/ are shared by zine and the blob (and the host tests). ----
ZINEH := kernel/zine.h $(wildcard user/zine/*.h)
$(BUILD)/zc-%.uo: user/zine/%.c $(ZINEH) | $(BUILD)
	$(CC) $(UCFLAGS) -Os -c $< -o $@
$(BUILD)/zine.uo: user/zine.c $(ZINEH) user/libz.h user/usys.h | $(BUILD)
	$(CC) $(UCFLAGS) -Os -c $< -o $@
ZINECMD := $(BUILD)/zine.uo $(BUILD)/zc-hunk.uo $(BUILD)/zc-zvol.uo $(BUILD)/zc-lvo.uo
$(BUILD)/zine.uelf: $(ZINECMD) $(BUILD)/crt0.o $(BUILD)/libz.uo link/user.ld
	$(CC) $(UCFLAGS) $(ULDFLAGS) $(BUILD)/crt0.o $(ZINECMD) $(BUILD)/libz.uo $(LIBGCC) -o $@
ZBLOBOBJS := $(BUILD)/zb-blobs.o $(BUILD)/zb-blob.o $(BUILD)/zb-blibs.o $(BUILD)/zc-hunk.uo $(BUILD)/zc-zvol.uo $(BUILD)/zc-zplan.uo
$(BUILD)/zb-blobs.o: user/zine/blobs.S | $(BUILD)
	$(CC) $(ASFLAGS) $< -o $@
# the blob writes low memory (the vectors at 0, ExecBase at 0x800): addresses near 0 are not null pointers to it
ZBFLAGS := -Os -fno-delete-null-pointer-checks --param=min-pagesize=0
$(BUILD)/zb-%.o: user/zine/%.c $(ZINEH) | $(BUILD)
	$(CC) $(UCFLAGS) $(ZBFLAGS) -c $< -o $@
$(BUILD)/zine-blob.elf: $(ZBLOBOBJS) link/user.ld
	$(CC) $(UCFLAGS) $(ULDFLAGS) $(ZBLOBOBJS) $(LIBGCC) -o $@
$(BUILD)/zine.blob: $(BUILD)/zine-blob.elf tools/mkzexe.py tools/mkzblob.py tools/check-68020-60.sh
	TOOLPREFIX=$(TOOLPREFIX) tools/check-68020-60.sh $<
	tools/mkzexe.py $< $@.zx
	python3 tools/mkzblob.py $@.zx $@
# the trace build (tests only, /tests/zine/trace.blob): every library call named on the serial port
ZTBLOBOBJS := $(BUILD)/zt-blobs.o $(BUILD)/zb-blob.o $(BUILD)/zb-blibs.o $(BUILD)/zc-hunk.uo $(BUILD)/zc-zvol.uo $(BUILD)/zc-zplan.uo
$(BUILD)/zt-blobs.o: user/zine/blobs.S | $(BUILD)
	$(CC) $(ASFLAGS) -DZINE_TRACE $< -o $@
$(BUILD)/zine-trace.elf: $(ZTBLOBOBJS) link/user.ld
	$(CC) $(UCFLAGS) $(ULDFLAGS) $(ZTBLOBOBJS) $(LIBGCC) -o $@
$(BUILD)/zine-trace.blob: $(BUILD)/zine-trace.elf tools/mkzexe.py tools/mkzblob.py
	tools/mkzexe.py $< $@.zx
	python3 tools/mkzblob.py $@.zx $@
ZINETESTS := zexit zhello ztake zcrash zbadcall zalloc zscreen
# the null blob (step 2's tests): the pause and the resume with nothing in between (user/zine/nullblob.S)
$(BUILD)/zb-nullblob.o: user/zine/nullblob.S | $(BUILD)
	$(CC) $(ASFLAGS) $< -o $@
$(BUILD)/zine-null.elf: $(BUILD)/zb-nullblob.o link/user.ld
	$(CC) $(UCFLAGS) $(ULDFLAGS) $(BUILD)/zb-nullblob.o -o $@
$(BUILD)/zine-null.blob: $(BUILD)/zine-null.elf tools/mkzexe.py tools/mkzblob.py
	tools/mkzexe.py $< $@.zx
	python3 tools/mkzblob.py $@.zx $@
$(BUILD)/zinetests/%: tools/tests/zine/%.S tools/tests/zine/zt.i tools/mkhunktest.py | $(BUILD)
	@mkdir -p $(BUILD)/zinetests
	$(CC) $(ASFLAGS) -Itools/tests/zine $< -o $(BUILD)/zinetests/$*.o
	python3 tools/mkhunktest.py $(BUILD)/zinetests/$*.o $@
zine-tests: $(ZINETESTS:%=$(BUILD)/zinetests/%)
$(BUILD)/zenix.hdf $(BUILD)/root-card.img: $(BUILD)/zine.blob $(BUILD)/zine-null.blob $(BUILD)/zine-trace.blob $(ZINETESTS:%=$(BUILD)/zinetests/%)

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
# build/pkg/NAME.manifest + NAME.tgz for the standard packages (tools/mkzup.py says which files each holds: the base
# group, the families and base-man, DESIGN D22 8.4.4); the full
# root images get their manifests in /var/lib/zpkg (tools/mkroot.sh), so the first update knows what is installed.
packages: $(BUILD)/pkg/list
# the PC font (DESIGN D20 4.2: telnet's PC boards load it with CON_FONT): from Linux's font_8x8.c, on the full root
# images and in the base package as /usr/share/consolefonts/cp437-8x8.psf with its notice (tools/font_8x8.COPYING)
# the root certificates (DESIGN D21 8.1): /etc/ssl/anchors on the full images and in the base package
$(BUILD)/anchors: tools/certdata tools/mkanchors.py | $(BUILD)
	python3 tools/mkanchors.py $@ >/dev/null
$(BUILD)/cp437-8x8.psf: tools/font_8x8.c tools/mkpsf.py | $(BUILD)
	python3 tools/mkpsf.py tools/font_8x8.c $@
# the console's Unicode font (DESIGN D28 6, ZCF1), /usr/share/consolefonts/zenix-8x8.zcf on the full root images and in
# the base package: box drawing, blocks and braille generated, CP437's symbols from font_8x8.c, Zenix's own glyphs
# and stand-ins (tools/consolefont/); init loads it at boot (/etc/console's font line)
UFONT_SRC := tools/font_8x8.c tools/mkconfont.py $(wildcard tools/consolefont/*)
$(BUILD)/zenix-8x8.zcf: $(UFONT_SRC) | $(BUILD)
	python3 tools/mkconfont.py font $@
$(BUILD)/pkg/list: $(BUILD)/cp437-8x8.psf $(BUILD)/zine.blob $(BUILD)/zenix-8x8.zcf $(BUILD)/anchors $(USERZX) $(SBINZX) $(NETDRVZX) $(shell find rootfs/etc -type f) $(wildcard docs/man/*.txt) tools/mkzup.py tools/mkman.py kernel/version.h
	NETDRVS="$(NETDRVS)" python3 tools/mkzup.py packages $(BUILD) "$(UAPPS)"
# the system bundle: the kernel and the base group only (DESIGN D22 3.1 and 4.4: everything else is installed live,
# zup -f; copied to BOOT:Zenix/zenix-update.zup, docs/man/zenixboot.txt)
# Signed (DESIGN D21 6.3, tools/zsign.py) into zenix-update.zup.sig: a test build's bundle carries the channel "test"
# and the TEST key's signature (build/sign-test, made when missing, never committed); with RELEASE=1 the channel
# CHANNEL (default testing) and the release key SIGNKEY (default zsign.py's, outside every repository).
BASEPKGS := zup base zpkg
CHANNEL ?= testing
SIGNKEY ?=
bundle: $(BUILD)/zenix-update.zup
$(BUILD)/zenix-update.zup: $(BUILD)/kernel.bin $(BUILD)/pkg/list tools/mkzup.py tools/zsign.py
	python3 tools/mkzup.py bundle $@ --base-only --kernel $(BUILD)/kernel.bin $(if $(RELEASE),--channel $(CHANNEL)) \
	    $(BUILD)/pkg $(BASEPKGS)
	k="$(if $(RELEASE),$(SIGNKEY),$$(python3 tools/zsign.py testkey | cut -d' ' -f1))"; \
	    [ -n "$(RELEASE)" ] || [ -n "$$k" ] || { echo "the test key is missing" >&2; exit 1; }; \
	    python3 tools/zsign.py sign $${k:+--key "$$k"} $@ >/dev/null

# root filesystem: ext2 (old revision 0 format), built from rootfs/ + the programs; 1,024 inodes (512 ran out with the
# network's files in D19 step 8: cmds2-test makes 150 files)
$(BUILD)/zenix.hdf: $(BUILD)/cp437-8x8.psf $(BUILD)/zenix-8x8.zcf $(BUILD)/anchors $(USERZX) $(SBINZX) $(NETDRVZX) $(TESTPZX) $(TESTLIBC) $(shell find rootfs -type f) tools/mkroot.sh tools/mkman.py $(BUILD)/pkg/list
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
$(BUILD)/root-card.img: $(BUILD)/cp437-8x8.psf $(BUILD)/zenix-8x8.zcf $(BUILD)/anchors $(USERZX) $(SBINZX) $(NETDRVZX) $(TESTPZX) $(TESTLIBC) $(shell find rootfs -type f) tools/mkroot.sh tools/mkman.py $(BUILD)/pkg/list
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
