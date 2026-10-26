# How Zenix works: three walk-throughs

Read architecture.md first for the parts. Everything here is *read* from the source; last checked against the code
2026-10-23.

## 1. From power-on to a shell prompt

1. **Kickstart** (the Amiga ROM) boots from the floppy: it loads the 1024-byte bootblock, checks its checksum
   and jumps to offset 12 with `a1` = the trackdisk request and `a6` = ExecBase. Zenix is still running *inside*
   AmigaOS at this point.
2. **Bootblock** (`boot/bootblock.S`): gets a 1 KB block for the boot tables and a chip buffer for the kernel,
   reads the kernel (+ optional RAM-disk image) with `DoIO`, switches the floppy motor off, then collects what
   only a live Exec knows: `AttnFlags` (which CPU), the memory list (each region with chip/fast attribute),
   `GfxBase` (to find the ROM Topaz font later), and under Emu68 the Pi's hardware addresses (stage0). It writes
   these into its block (**bootinfo** and **hwinfo**). It chooses the kernel's home (largest fast region, 256 KB
   aligned; else chip `0x40000`), disables interrupts and DMA, copies the kernel there, copies the block to
   `0x1000` (free only now that the OS is dead), flushes the instruction cache and jumps to the kernel. There is
   no way back to AmigaOS. Its first instructions switch the display to one plain colour, and every step from here
   to the display paints the screen its own colour (blue, cyan, magenta in the bootblock; architecture.md "Boot
   stage colours"); it also reserves the top 16.5 KB of chip RAM (`AllocAbs`) for the kernel log.
   **Without a floppy** (a hard disk, CF card or, under Emu68, the SD card): Kickstart boots a small AmigaDOS
   partition instead and runs its `S:Startup-Sequence`, whose one line starts **`zenixboot`** (`boot/zenixboot.c`), an
   AmigaOS program. It reads the kernel from the file `zenix` next to it (and a RAM-disk image if given `RAM=FILE`),
   checks the image, and only then does what the bootblock does, with the bootblock's own code (`boot/handoff.S`):
   the same tables, stage0, the same kernel home, the same colours (blue while reading, cyan, magenta). A problem
   before that point is printed and leaves AmigaOS running. Because a running AmigaOS may have more switched on than
   a fresh floppy boot, it also stops the floppy motors, shows no screen, resets the display registers and switches a
   68040/060's data cache and MMU off. With the kernel in a directory `Zenix/` next to it, zenixboot also does **the
   boot half of an update** (DESIGN D15, man/zenixboot.txt "UPDATES"): a bundle `Zenix/zenix-update.zup` is checked
   whole, its kernel installed as `zenix` with the old one kept as `zenix.old` (each step flushed to the disk), and
   the bundle left in memory for the new kernel (bootinfo `+0x78`); at the next starts it reads the update note and
   the previous boot's log to end the trial, or to offer the previous version after a crash. From step 3 on the two
   paths are the same.
3. **`start.S`**: the kernel image is a ZEXE; the first code walks its own relocation table adding the real load
   address, flushes the instruction cache (the patched words must not run from a stale cache or an Emu68
   translation), clears BSS, and refuses to continue on a 68000 (serial message, orange over red).
4. **`kmain`** (`kernel/main.c`), in order: the kernel log (`klog_init`: a log left by the previous boot at the
   top of chip RAM is found, and this boot writes the other of the two slots there); serial port (so there is always a console); font pointer extracted
   *before* the memory manager touches RAM; vector table (`VBR` set; exceptions, the four interrupt levels and
   `TRAP #0`); memory manager built from bootinfo with the kernel, low chip RAM and the RAM-disk excluded (and an
   update bundle zenixboot left, checked and reserved as a block in use); a
   memory self-test; instruction cache on; the E-clock counter (PAL or NTSC crystal, from the Agnus/Alice id);
   display (the beam's line count picks 32 or 25 text rows; Copper list, bitplane, font); the previous boot's last lines
   if there was one (`klog_show_prev`); real-time clock if present;
   IDE probe on this machine's port only - a Ramsey means the A4000 port, Gayle ID 0xD the A600/A1200 one with its
   interrupt sources switched off first, anything else no IDE - then the SD card, then the A3000's SCSI (panic only
   if there is neither a disk nor a RAM disk).
5. `proc_init`, `tty_init`, then one process **built into the kernel image**: `boot` (pid 1, priv). Timer and keyboard interrupts (latch from the
   E-clock; one keyboard handshake in case it still waits for one) and interrupts are switched on. The main thread becomes
   the idle loop: reap orphans, `schedule()`, drain the serial buffer, `cpu_idle()` (stop until an interrupt).
6. `boot` starts the servers by using them (DESIGN D11, the service manager; architecture.md section 9). It registers
   the service `disk` -> `@diskd` (nice -10): the name now has a *held* port with no server. Its first question to it
   (the device list, for choosing the root) makes boot itself load `diskd` from the kernel image (pid 2, priv); diskd
   finds the disk and its partition table and takes its port, and the question is answered (on the zenixboot SD card
   it lists the FAT partition, the 0x76 container, the FFS boot partition inside it and the ext2 partition; only the
   last one can be a root). boot picks the root (a RAM disk; else the bootable partition; else the first ext2 or
   ZenixFS one; else the bare disk), reads its superblock to tell ext2 from ZenixFS and its volume id, and registers
   `fs` -> `@fsd DEVICE fs -root -id ID` or `@zfsd ...`. Asking for the root inode starts that server (pid 3); from
   then on the kernel log is no longer mirrored on the screen. boot runs `/sbin/zup` and waits (D15: the root half of
   an update, or what a power cut left; with nothing to do it ends at once) and gives the bundle's memory back. boot
   registers the services of `/etc/svctab` (nothing
   is started for them yet), then `spawn("/bin/init")` and waits for it.
   `spawn` itself is messages to `fsd`: the kernel's `proc_spawn` opens the file (`fs_exec_open`) and reads it
   straight into the new process's memory.
7. **`/bin/init`** prints `/etc/motd`, runs `mount -a` if there is an `/etc/fstab`, and starts `/bin/login` on
   console 1 (`/dev/tty1` as its stdio, so that console becomes its controlling tty). `login` logs in the user named in
   `/etc/autologin` (console 1 only) or asks, then runs the user's shell and waits. init then waits for two kinds of
   event: a login that ended (it starts a new one on that console) and a console shown with nobody on it (Amiga+F2:
   it starts a login there).

## 2. Life of `cat notes | grep zen`

The shell parses the line and builds a pipeline:

1. `pipe(fds)` - the kernel allocates a 2 KB ring buffer and two entries in the open-file table.
2. `spawn("/bin/cat", argv, stdio={0, pipe_w, 2})` - the syscall copies the path and argv into kernel buffers,
   asks the filesystem server (message `FS_OPEN`, then repeated `FS_READ` into a kernel buffer) for the file,
   then `proc_load`: allocate **one block** for image+BSS+stack+arguments (owner = new pid), copy the image,
   apply the relocation table, put `argc/argv` at the top of the stack, build a first kernel-stack frame, give the
   child fds 0-2 duplicated from the parent's chosen descriptors, and push it on the run queue.
3. The same for `grep`, with stdin = the pipe's read end.
4. `cat` calls `open("notes")`. The kernel copies the path, sends `FS_OPEN{path, cwd inode, flags}` to `fs`, and
   blocks in state `P_MSG`. `fsd` wakes, walks the directory inodes through its block cache and replies with an
   inode number. The kernel stores that in a `struct file`.
5. `cat` calls `read(fd, buf, 1024)`. After `user_ok_wr` checks the buffer, the kernel sends `FS_READ{ino, offset,
   buf, len}`; **`fsd` copies from its cache directly into `cat`'s buffer** (no intermediate copy) and replies
   with the byte count.
6. `cat` calls `write(1, ...)` on the pipe: bytes go into the ring; a waiting `grep` is made ready. A full ring
   blocks the writer (`P_IO`); `wake_io()` re-checks blocked peers when either side changes.
7. `grep` reads the pipe, filters with the regex engine, writes to the tty. The kernel's `tty_write` passes the
   bytes to the terminal parser, which updates that console's grid; if the console is on the screen the glyphs are
   drawn with the blitter-assisted display code, and its output is mirrored to the serial port.
8. `cat` exits: `proc_exit` closes its fds (the pipe loses its last writer), frees all memory owned by its pid,
   becomes a zombie and wakes the shell's `wait`. `grep` then reads EOF, exits too. The shell collects both exit
   codes.

Context switches happen at those blocking points and at the 50 ms time slice, in `schedule()` / `switch_to()`.

## 3. A program crashes

`crash` executes an illegal instruction.

1. The CPU takes the exception through the vector table to `exc_common` (`start.S`), which saves registers and
   calls `exc_c`.
2. `exc_c` looks at the saved status register. If it was **user mode** and not pid 0: it prints
   `pid N (name) killed: vector V pc=... addr=...` and calls `proc_exit(139)`.
3. `proc_exit` closes the files, releases the tty, closes the process's ports (so a crashing *server* fails its
   clients with an error instead of hanging them), `mem_free_owner(pid)` returns every block it owned, and the
   parent's `wait` returns 139. Everyone else keeps running (verified: `crash` killed, `spin` continued).
   **If the process was a server of a service** (diskd, a filesystem server, an `/etc/svctab` service: DESIGN D11),
   its port is not closed but goes back to *held*: the request it was serving fails (-ECONNRESET; a program's file
   call sees -EIO), the requests queued behind it stay queued, and the first of those clients loads the program again
   in its own context; the new server takes the port and serves them as if nothing happened. The kernel log says
   `svc: NAME: PROG (pid N) ended, status 139, during request T for pid C` and `start K when next used`. A restarted
   diskd gets its device table back from the kernel, a restarted filesystem server its open files; repeated crashes
   wait 1, 2, 4 s before the next start, and 5 ends within a minute give the service up (for diskd or the root
   filesystem: a panic with the reason).
4. If the exception was in **supervisor mode**, it's a kernel bug: print the vector, PC and fault address and
   `panic` (halt).

What this does **not** catch: a program that writes through a wild but *valid* pointer into someone else's
memory. With no MMU every address is valid for every program. See limits.md.

Note that on a 68020 odd-address word/long access does *not* fault, so crash tests must use `illegal`, not an odd
pointer.
