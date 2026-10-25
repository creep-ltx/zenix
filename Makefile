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
ASFLAGS := -m68020-60 -x assembler-with-cpp -c
# test builds only (audit A11/A12, docs/pi-first-boot.md): DIAG=-DDIAG_STOP=N hangs at boot stage N (its colour stays),
# DIAG=-DDIAG_PANIC=N panics there. Use a build directory of its own (make BUILD=/tmp/x DIAG=...): flags are not tracked.
DIAG    ?=
CFLAGS  += $(DIAG)
ASFLAGS += $(DIAG)
# libgcc's 64-bit helpers (__udivdi3, __muldi3) in the default multilib use the 64-bit mulu.l/divu.l themselves; the
# 68060 multilib's do not and run on every 68020+, so that one is linked (the check below covers it too)
LIBGCC  := $(shell $(CC) -mcpu=68060 -msoft-float -print-libgcc-file-name 2>/dev/null)

KOBJS := $(BUILD)/start.o $(BUILD)/main.o $(BUILD)/serial.o $(BUILD)/mem.o $(BUILD)/ide.o $(BUILD)/emmc.o $(BUILD)/floppy.o $(BUILD)/input.o $(BUILD)/audio.o $(BUILD)/a3000.o $(BUILD)/blk.o $(BUILD)/rtc.o $(BUILD)/proc.o $(BUILD)/syscall.o $(BUILD)/vfs.o $(BUILD)/tty.o $(BUILD)/gfx.o $(BUILD)/term.o $(BUILD)/kbd.o $(BUILD)/ipc.o $(BUILD)/svc.o $(BUILD)/embed.o

UAPPS := diskd init sh ls cat echo pwd ps free wc grep uname hello spin crash badptr boot \
          mkdir rmdir rm cp mv touch pingd ping bench fdisk mkfs_ext2 fsck_ext2 df sync termtest \
          head tail sort uniq cut tr tee seq basename dirname yes sleep true false env printenv which clear \
          whoami hostname id uptime cmp hexdump stat expr find xargs du \
          sed diff chmod ln readlink date less edit man gzip tar input beep dmesg dd login su passwd chown sha256sum printf \
          tac rev nl fold paste comm expand mktemp realpath base64 md5sum cal time timeout kill pgrep watch tree mount umount mkfs_zenixfs fsck_zenixfs \
          nice renice priotest reboot cu awk svctest
UCFLAGS := -m68020-60 -msoft-float -ffreestanding -fno-builtin -nostdlib \
           -O2 -g -Wall -Wextra -fno-strict-aliasing -fno-common -ffunction-sections -fdata-sections
# Stack sizes written into each program's ZEXE header (the loader gives this much; no guard below it). Default 16 KB;
# these need more (measured with -fstack-usage, docs/limits.md): sh's recursive parser and runner (its own depth
# guard, STK_LIMIT, keeps to 16 KB and the rest is margin), gzip's deflate (about 12 KB of frames in one chain), edit.
STACK_DEFAULT := 16384
STACK_sh := 24576
STACK_gzip := 32768
STACK_edit := 32768
STACK_awk := 65536
# every program links only the parts of libz it calls (libz is built one section per function)
ULDFLAGS := -nostartfiles -T link/user.ld -Wl,--build-id=none -Wl,-q -Wl,--gc-sections
USERZX := $(UAPPS:%=$(BUILD)/%.zx)

all: $(BUILD)/zenix.adf $(BUILD)/zenix.hdf

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
$(BUILD)/stage0.o: boot/stage0.c kernel/hwinfo.h boot/dtapi_amiga.h | $(BUILD)
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
$(BUILD)/zenixboot.elf: $(BUILD)/zenixboot-start.o $(BUILD)/zenixboot.bo link/user.ld
	$(CC) $(CFLAGS) -nostartfiles -T link/user.ld -Wl,--build-id=none -Wl,-q -Wl,--gc-sections \
	    $(BUILD)/zenixboot-start.o $(BUILD)/zenixboot.bo $(LIBGCC) -o $@
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
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/%.o: kernel/%.S | $(BUILD)
	$(CC) $(ASFLAGS) $< -o $@
$(BUILD)/kernel.elf: $(KOBJS) link/kernel.ld
	$(CC) $(CFLAGS) -nostartfiles -T link/kernel.ld -Wl,--build-id=none -Wl,-q \
	    $(KOBJS) $(LIBGCC) -o $@
# The RAM floppy (zenix-ram.adf) holds the bootblock (1 KB), the kernel and the RAM disk (RAMDISK_K KB) in 880 KB: the
# kernel may grow to KERNEL_MAX bytes. Every build says how much is left; tools/mkadf.py refuses an image that is over.
RAMDISK_K := 704
KERNEL_MAX := $(shell echo $$((901120 - 1024 - $(RAMDISK_K) * 1024)))
$(BUILD)/kernel.bin: $(BUILD)/kernel.elf tools/mkzexe.py tools/check-68020-60.sh
	TOOLPREFIX=$(TOOLPREFIX) tools/check-68020-60.sh $<
	tools/mkzexe.py $< $@
	@s=$$(wc -c < $@); echo "$@: $$s of $(KERNEL_MAX) bytes, $$(($(KERNEL_MAX) - s)) left for the kernel on the RAM floppy"; \
	    [ $$s -le $(KERNEL_MAX) ] || echo "$@: WARNING: $$((s - $(KERNEL_MAX))) bytes too big for the RAM floppy (zenix-ram.adf)"

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
             kill pgrep grep dmesg svctest

# development/test image: the same, plus a small root filesystem that the bootblock loads into RAM.
# For machines without an IDE disk (a Gotek boot) and for testing. Not part of the standard build.
$(BUILD)/root-ram.img: $(USERZX) $(shell find rootfs -type f) tools/mkroot.sh
	NOMAN=1 NOTESTS=1 tools/mkroot.sh $@ $(BUILD) "$(RAMAPPS)" $(RAMDISK_K)K 128

$(BUILD)/root-mount.img: $(USERZX) $(shell find rootfs -type f) tools/mkroot.sh Makefile
	NOMAN=1 tools/mkroot.sh $@ $(BUILD) "$(MOUNTAPPS)" $(RAMDISK_K)K 128

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

# programs that look up users and groups (/etc/passwd, /etc/group), and those that also check passwords
PWAPPS := ls id whoami chown renice
AUTHAPPS := login su passwd sha256sum
$(PWAPPS:%=$(BUILD)/%.uelf): $(BUILD)/%.uelf: $(BUILD)/%.uo $(BUILD)/pw.uo $(BUILD)/crt0.o $(BUILD)/libz.uo link/user.ld
	$(CC) $(UCFLAGS) $(ULDFLAGS) \
	    $(BUILD)/crt0.o $< $(BUILD)/pw.uo $(BUILD)/libz.uo $(LIBGCC) -o $@
$(AUTHAPPS:%=$(BUILD)/%.uelf): $(BUILD)/%.uelf: $(BUILD)/%.uo $(BUILD)/pw.uo $(BUILD)/sha.uo $(BUILD)/crt0.o $(BUILD)/libz.uo link/user.ld
	$(CC) $(UCFLAGS) $(ULDFLAGS) \
	    $(BUILD)/crt0.o $< $(BUILD)/pw.uo $(BUILD)/sha.uo $(BUILD)/libz.uo $(LIBGCC) -o $@
$(BUILD)/pw.uo $(BUILD)/sha.uo: $(BUILD)/%.uo: user/%.c user/pw.h user/libz.h user/usys.h | $(BUILD)
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
$(BUILD)/embed.o: kernel/embed.S $(BUILD)/fsd.zx $(BUILD)/boot.zx $(BUILD)/diskd.zx $(BUILD)/zfsd.zx | $(BUILD)
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

# root filesystem: ext2 (old revision 0 format), built from rootfs/ + the programs
$(BUILD)/zenix.hdf: $(USERZX) $(shell find rootfs -type f) tools/mkroot.sh tools/mkman.py
	tools/mkroot.sh $@ $(BUILD) "$(UAPPS)"

# the same disk wrapped in an RDB, as a real hard disk or CF card has it
$(BUILD)/zenix-rdb.hdf: $(BUILD)/zenix.hdf tools/mkrdb.py
	python3 tools/mkrdb.py $< $@

# the same root disk with ZenixFS as the root filesystem instead of ext2 (needs the host tool: make -C zenixfs build/zenixfs-tool)
$(BUILD)/zenix-zroot.hdf: $(USERZX) $(shell find rootfs -type f) tools/mkroot-zenixfs.sh tools/mkrdb.py tools/mkman.py $(wildcard zenixfs/*.c zenixfs/*.h zenixfs/tools/*.c)
	make -s -C zenixfs build/zenixfs-tool
	tools/mkroot-zenixfs.sh $@ $(BUILD) "$(UAPPS)" 8

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
$(BUILD)/root-card.img: $(USERZX) $(shell find rootfs -type f) tools/mkroot.sh tools/mkman.py
	tools/mkroot.sh $@ $(BUILD) "$(UAPPS)" 512M 8192
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
.PHONY: all run test check clean
