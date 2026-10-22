TOOLPREFIX := $(HOME)/opt/m68k-elf/bin/m68k-elf-
CC      := $(TOOLPREFIX)gcc
OBJCOPY := $(TOOLPREFIX)objcopy
BUILD   := build

CFLAGS  := -mcpu=68020 -msoft-float -ffreestanding -fno-builtin -nostdlib \
           -O2 -g -Wall -Wextra -fno-strict-aliasing -fno-common
DIWV    ?= 0x2C
CFLAGS  += -DDIW_V=$(DIWV)
ASFLAGS := -mcpu=68020 -x assembler-with-cpp -c
LIBGCC  := $(shell $(CC) $(CFLAGS) -print-libgcc-file-name 2>/dev/null)

KOBJS := $(BUILD)/start.o $(BUILD)/main.o $(BUILD)/serial.o $(BUILD)/mem.o $(BUILD)/ide.o $(BUILD)/proc.o $(BUILD)/syscall.o $(BUILD)/vfs.o $(BUILD)/tty.o $(BUILD)/gfx.o $(BUILD)/term.o $(BUILD)/kbd.o $(BUILD)/ipc.o $(BUILD)/embed.o

UAPPS := diskd init sh ls cat echo pwd ps free wc grep uname hello spin crash badptr boot \
          mkdir rmdir rm cp mv touch pingd ping bench fdisk mkfs_ext2 fsck_ext2 df sync termtest \
          head tail sort uniq cut tr tee seq basename dirname yes sleep true false env printenv which clear \
          whoami hostname id uptime cmp hexdump stat expr find xargs du \
          sed diff chmod ln readlink date less edit man gzip tar
UCFLAGS := -mcpu=68020 -msoft-float -ffreestanding -fno-builtin -nostdlib \
           -O2 -g -Wall -Wextra -fno-strict-aliasing -fno-common
# programs that need more than the default 16 KB stack
STACK_sh := 40960
STACK_gzip := 32768
STACK_edit := 32768
USERZX := $(UAPPS:%=$(BUILD)/%.zx)

all: $(BUILD)/zenix.adf $(BUILD)/zenix.hdf

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/bootblock.o: boot/bootblock.S | $(BUILD)
	$(CC) $(ASFLAGS) $< -o $@
$(BUILD)/bootblock.elf: $(BUILD)/bootblock.o
	$(CC) $(CFLAGS) -nostartfiles -Wl,-Ttext=0 -Wl,--build-id=none $< -o $@
$(BUILD)/bootblock.bin: $(BUILD)/bootblock.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/%.o: kernel/%.c $(wildcard kernel/*.h) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/%.o: kernel/%.S | $(BUILD)
	$(CC) $(ASFLAGS) $< -o $@
$(BUILD)/kernel.elf: $(KOBJS) link/kernel.ld
	$(CC) $(CFLAGS) -nostartfiles -T link/kernel.ld -Wl,--build-id=none -Wl,-q \
	    $(KOBJS) $(LIBGCC) -o $@
$(BUILD)/kernel.bin: $(BUILD)/kernel.elf tools/mkzexe.py
	tools/mkzexe.py $< $@

# the standard floppy carries only the bootblock and the kernel (the root filesystem is on the disk)
$(BUILD)/zenix.adf: $(BUILD)/bootblock.bin $(BUILD)/kernel.bin tools/mkadf.py
	python3 tools/mkadf.py $(BUILD)/bootblock.bin $(BUILD)/kernel.bin $@

# development/test image: the same, plus a small root filesystem that the bootblock loads into RAM.
# For machines without an IDE disk (a Gotek boot) and for testing. Not part of the standard build.
$(BUILD)/root-ram.img: $(USERZX) $(shell find rootfs -type f) tools/mkroot.sh
	tools/mkroot.sh $@ $(BUILD) "$(UAPPS)" 640K 128

$(BUILD)/zenix-ram.adf: $(BUILD)/bootblock.bin $(BUILD)/kernel.bin $(BUILD)/root-ram.img tools/mkadf.py
	python3 tools/mkadf.py $(BUILD)/bootblock.bin $(BUILD)/kernel.bin $@ $(BUILD)/root-ram.img

# ---- servers built into the kernel image ----
$(BUILD)/ext2_fsd.uo: kernel/ext2.c kernel/ext2.h user/fsenv.h user/libz.h | $(BUILD)
	$(CC) $(UCFLAGS) -DFSD -c $< -o $@
$(BUILD)/regex.uo: user/regex.c user/regex.h user/libz.h | $(BUILD)
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/sed.uelf: $(BUILD)/sed.uo $(BUILD)/regex.uo $(BUILD)/crt0.o $(BUILD)/libz.uo link/user.ld
	$(CC) $(UCFLAGS) -nostartfiles -T link/user.ld -Wl,--build-id=none -Wl,-q \
	    $(BUILD)/crt0.o $(BUILD)/sed.uo $(BUILD)/regex.uo $(BUILD)/libz.uo $(LIBGCC) -o $@

$(BUILD)/less.uelf: $(BUILD)/less.uo $(BUILD)/regex.uo $(BUILD)/crt0.o $(BUILD)/libz.uo link/user.ld
	$(CC) $(UCFLAGS) -nostartfiles -T link/user.ld -Wl,--build-id=none -Wl,-q \
	    $(BUILD)/crt0.o $(BUILD)/less.uo $(BUILD)/regex.uo $(BUILD)/libz.uo $(LIBGCC) -o $@

$(BUILD)/edit.uelf: $(BUILD)/edit.uo $(BUILD)/regex.uo $(BUILD)/crt0.o $(BUILD)/libz.uo link/user.ld
	$(CC) $(UCFLAGS) -nostartfiles -T link/user.ld -Wl,--build-id=none -Wl,-q \
	    $(BUILD)/crt0.o $(BUILD)/edit.uo $(BUILD)/regex.uo $(BUILD)/libz.uo $(LIBGCC) -o $@

MINIZ_CFLAGS := -Iuser/miniz/shim -Iuser/miniz -Wno-unused-parameter -Wno-sign-compare -Wno-implicit-fallthrough -Wno-misleading-indentation -Wno-unused-function
$(BUILD)/miniz_tdef.uo: user/miniz/miniz_tdef.c | $(BUILD)
	$(CC) $(UCFLAGS) $(MINIZ_CFLAGS) -c $< -o $@
$(BUILD)/miniz_tinfl.uo: user/miniz/miniz_tinfl.c | $(BUILD)
	$(CC) $(UCFLAGS) $(MINIZ_CFLAGS) -c $< -o $@
$(BUILD)/gzip.uo: user/gzip.c user/libz.h | $(BUILD)
	$(CC) $(UCFLAGS) $(MINIZ_CFLAGS) -c $< -o $@
$(BUILD)/gzip.uelf: $(BUILD)/gzip.uo $(BUILD)/miniz_tdef.uo $(BUILD)/miniz_tinfl.uo $(BUILD)/crt0.o $(BUILD)/libz.uo link/user.ld
	$(CC) $(UCFLAGS) -nostartfiles -T link/user.ld -Wl,--build-id=none -Wl,-q \
	    $(BUILD)/crt0.o $(BUILD)/gzip.uo $(BUILD)/miniz_tdef.uo $(BUILD)/miniz_tinfl.uo $(BUILD)/libz.uo $(LIBGCC) -o $@

$(BUILD)/grep.uelf: $(BUILD)/grep.uo $(BUILD)/regex.uo $(BUILD)/crt0.o $(BUILD)/libz.uo link/user.ld
	$(CC) $(UCFLAGS) -nostartfiles -T link/user.ld -Wl,--build-id=none -Wl,-q \
	    $(BUILD)/crt0.o $(BUILD)/grep.uo $(BUILD)/regex.uo $(BUILD)/libz.uo $(LIBGCC) -o $@

$(BUILD)/fsck_ext2.uelf: $(BUILD)/fsck_ext2.uo $(BUILD)/ext2_fsd.uo $(BUILD)/crt0.o $(BUILD)/libz.uo link/user.ld
	$(CC) $(UCFLAGS) -nostartfiles -T link/user.ld -Wl,--build-id=none -Wl,-q \
	    $(BUILD)/crt0.o $(BUILD)/fsck_ext2.uo $(BUILD)/ext2_fsd.uo $(BUILD)/libz.uo $(LIBGCC) -o $@

$(BUILD)/fsd.uelf: $(BUILD)/fsd.uo $(BUILD)/ext2_fsd.uo $(BUILD)/crt0.o $(BUILD)/libz.uo link/user.ld
	$(CC) $(UCFLAGS) -nostartfiles -T link/user.ld -Wl,--build-id=none -Wl,-q \
	    $(BUILD)/crt0.o $(BUILD)/fsd.uo $(BUILD)/ext2_fsd.uo $(BUILD)/libz.uo $(LIBGCC) -o $@
$(BUILD)/fsd.zx: $(BUILD)/fsd.uelf tools/mkzexe.py
	tools/mkzexe.py $< $@ 16384
$(BUILD)/embed.o: kernel/embed.S $(BUILD)/fsd.zx $(BUILD)/boot.zx $(BUILD)/diskd.zx | $(BUILD)
	$(CC) $(ASFLAGS) $< -o $@

# ---- user programs: ZEXE, relocated by the kernel at load ----
$(BUILD)/crt0.o: user/crt0.S | $(BUILD)
	$(CC) $(ASFLAGS) $< -o $@
$(BUILD)/libz.uo: user/libz.c user/libz.h user/usys.h | $(BUILD)
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/%.uo: user/%.c user/libz.h user/usys.h | $(BUILD)
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/%.uelf: $(BUILD)/%.uo $(BUILD)/crt0.o $(BUILD)/libz.uo link/user.ld
	$(CC) $(UCFLAGS) -nostartfiles -T link/user.ld -Wl,--build-id=none -Wl,-q \
	    $(BUILD)/crt0.o $< $(BUILD)/libz.uo $(LIBGCC) -o $@
$(BUILD)/%.zx: $(BUILD)/%.uelf tools/mkzexe.py
	tools/mkzexe.py $< $@ 16384

# root filesystem: ext2 (old revision 0 format), built from rootfs/ + the programs
$(BUILD)/zenix.hdf: $(USERZX) $(shell find rootfs -type f) tools/mkroot.sh tools/mkman.py
	tools/mkroot.sh $@ $(BUILD) "$(UAPPS)"

# the same disk wrapped in an RDB, as a real hard disk or CF card has it
$(BUILD)/zenix-rdb.hdf: $(BUILD)/zenix.hdf tools/mkrdb.py
	python3 tools/mkrdb.py $< $@

run: all
	fs-uae fsuae/a1200-020.fs-uae

# boot headless, print the serial log
test: all
	rm -f $(BUILD)/zenix.hdf; $(MAKE) $(BUILD)/zenix.hdf
	tools/run-test.sh fsuae/a1200-020.fs-uae 20

clean:
	rm -rf $(BUILD)

.SECONDARY:
.PHONY: all run test clean
