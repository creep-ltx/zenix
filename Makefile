TOOLPREFIX := $(HOME)/opt/m68k-elf/bin/m68k-elf-
CC      := $(TOOLPREFIX)gcc
OBJCOPY := $(TOOLPREFIX)objcopy
BUILD   := build

CFLAGS  := -mcpu=68020 -msoft-float -ffreestanding -fno-builtin -nostdlib \
           -O2 -g -Wall -Wextra -fno-strict-aliasing -fno-common
ASFLAGS := -mcpu=68020 -x assembler-with-cpp -c
LIBGCC  := $(shell $(CC) $(CFLAGS) -print-libgcc-file-name 2>/dev/null)

KOBJS := $(BUILD)/start.o $(BUILD)/main.o $(BUILD)/serial.o $(BUILD)/mem.o $(BUILD)/ide.o $(BUILD)/proc.o $(BUILD)/syscall.o

UAPPS := init hello badptr crash spin
UCFLAGS := -mcpu=68020 -msoft-float -ffreestanding -fno-builtin -nostdlib \
           -O2 -g -Wall -Wextra -fno-strict-aliasing -fno-common
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

$(BUILD)/zenix.adf: $(BUILD)/bootblock.bin $(BUILD)/kernel.bin tools/mkadf.py
	python3 tools/mkadf.py $(BUILD)/bootblock.bin $(BUILD)/kernel.bin $@

# ---- user programs: ZEXE, relocated by the kernel at load ----
$(BUILD)/crt0.o: user/crt0.S | $(BUILD)
	$(CC) $(ASFLAGS) $< -o $@
$(BUILD)/%.uo: user/%.c user/usys.h | $(BUILD)
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/%.uelf: $(BUILD)/%.uo $(BUILD)/crt0.o link/user.ld
	$(CC) $(UCFLAGS) -nostartfiles -T link/user.ld -Wl,--build-id=none -Wl,-q \
	    $(BUILD)/crt0.o $< $(LIBGCC) -o $@
$(BUILD)/%.zx: $(BUILD)/%.uelf tools/mkzexe.py
	tools/mkzexe.py $< $@ 16384

$(BUILD)/zenix.hdf: $(USERZX) tools/mkdisk.py
	python3 tools/mkdisk.py $@ $(USERZX)

run: all
	fs-uae fsuae/a1200-020.fs-uae

# boot headless, print the serial log
test: all
	tools/run-test.sh fsuae/a1200-020.fs-uae 20

clean:
	rm -rf $(BUILD)

.SECONDARY:
.PHONY: all run test clean
