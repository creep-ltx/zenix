TOOLPREFIX := $(HOME)/opt/m68k-elf/bin/m68k-elf-
CC      := $(TOOLPREFIX)gcc
OBJCOPY := $(TOOLPREFIX)objcopy
STRIP   := $(TOOLPREFIX)strip
BUILD   := build

CFLAGS  := -mcpu=68030 -msoft-float -ffreestanding -fno-builtin -nostdlib \
           -O2 -g -Wall -Wextra -fno-strict-aliasing -fno-common
# fpu.c contains FPU inline asm; userland is hard-float (FPU is mandatory)
CFLAGS_FPU := $(subst -msoft-float,-m68881,$(CFLAGS))
# userland: hosted (newlib), hard-float
UCFLAGS := -mcpu=68030 -m68881 -O2 -g -Wall -Wextra -fno-common
ASFLAGS := -mcpu=68030 -x assembler-with-cpp -c
LIBGCC  := $(shell $(CC) $(CFLAGS) -print-libgcc-file-name 2>/dev/null)

KOBJS := $(BUILD)/start.o $(BUILD)/main.o $(BUILD)/serial.o \
         $(BUILD)/console.o $(BUILD)/kbd.o $(BUILD)/signal.o \
         $(BUILD)/pmm.o $(BUILD)/kmalloc.o $(BUILD)/mmu030.o \
         $(BUILD)/umem.o $(BUILD)/elf.o $(BUILD)/fpu.o \
         $(BUILD)/ide.o $(BUILD)/fat.o $(BUILD)/proc.o

UAPPS    := init sh ls cat echo hello crash spin segv keymap
USERELFS := $(UAPPS:%=$(BUILD)/%.elf)

all: $(BUILD)/zenix.adf $(BUILD)/zenix.hdf disk-sync

$(BUILD):
	mkdir -p $(BUILD)

# ---- bootblock ----
$(BUILD)/bootblock.o: boot/bootblock.S | $(BUILD)
	$(CC) $(ASFLAGS) $< -o $@
$(BUILD)/bootblock.elf: $(BUILD)/bootblock.o
	$(CC) $(CFLAGS) -nostartfiles -Wl,-Ttext=0 -Wl,--build-id=none $< -o $@
$(BUILD)/bootblock.bin: $(BUILD)/bootblock.elf
	$(OBJCOPY) -O binary $< $@

# ---- user programs (hard-float newlib ELF, loaded from disk) ----
$(BUILD)/crt0.o: user/crt0.S | $(BUILD)
	$(CC) $(ASFLAGS) $< -o $@
$(BUILD)/%.uo: user/%.c user/usys.h | $(BUILD)
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/%.elf: $(BUILD)/%.uo $(BUILD)/crt0.o $(BUILD)/syscalls.uo link/user.ld
	$(CC) $(UCFLAGS) -nostartfiles -T link/user.ld -Wl,--build-id=none \
	    $(BUILD)/crt0.o $(BUILD)/syscalls.uo $< -lm -o $@
	$(STRIP) -g $@

# ---- kernel ----
$(BUILD)/fpu.o: kernel/fpu.c $(wildcard kernel/*.h) | $(BUILD)
	$(CC) $(CFLAGS_FPU) -c $< -o $@
$(BUILD)/%.o: kernel/%.c $(wildcard kernel/*.h) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/%.o: kernel/%.S | $(BUILD)
	$(CC) $(ASFLAGS) $< -o $@
$(BUILD)/kernel.elf: $(KOBJS) link/kernel.ld
	$(CC) $(CFLAGS) -nostartfiles -T link/kernel.ld -Wl,--build-id=none \
	    $(KOBJS) $(LIBGCC) -o $@
$(BUILD)/kernel.bin: $(BUILD)/kernel.elf
	$(OBJCOPY) -O binary $< $@

# ---- bootable floppy image ----
$(BUILD)/zenix.adf: $(BUILD)/bootblock.bin $(BUILD)/kernel.bin tools/mkadf.py
	python3 tools/mkadf.py $(BUILD)/bootblock.bin $(BUILD)/kernel.bin $@

# ---- IDE test disk (created if missing; ELFs synced on every build) ----
$(BUILD)/zenix.hdf: | $(BUILD)
	tools/mkdisk.sh $@

disk-sync: $(BUILD)/zenix.hdf $(USERELFS)
	for a in $(UAPPS); do \
	    mcopy -o -i $(BUILD)/zenix.hdf $(BUILD)/$$a.elf ::$$(echo $$a | tr a-z A-Z).ELF || exit 1; \
	done

run: all
	fs-uae fsuae/a4000-dev.fs-uae

clean:
	rm -rf $(BUILD)

.PHONY: all run clean disk-sync
