# How Zenix works: three walk-throughs

Read architecture.md first for the parts. Everything here is *read* from the source (2026-10-22).

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
3. **`start.S`**: the kernel image is a ZEXE; the first code walks its own relocation table adding the real load
   address, flushes the instruction cache (the patched words must not run from a stale cache or an Emu68
   translation), clears BSS, and refuses to continue on a 68000 (serial message, orange over red).
4. **`kmain`** (`kernel/main.c`), in order: the kernel log (`klog_init`: a log left by the previous boot at the
   top of chip RAM is found, and this boot writes the other of the two slots there); serial port (so there is always a console); font pointer extracted
   *before* the memory manager touches RAM; vector table (`VBR` set; exceptions, the four interrupt levels and
   `TRAP #0`); memory manager built from bootinfo with the kernel, low chip RAM and the RAM-disk excluded; a
   memory self-test; instruction cache on; display (Copper list, bitplane, font); the previous boot's last lines
   if there was one (`klog_show_prev`); real-time clock if present;
   IDE probe with Gayle's interrupt sources switched off first (panic only if there is neither a disk nor a RAM
   disk).
5. `proc_init`, `tty_init`, then three processes **built into the kernel image** (so they need no disk):
   `diskd` (priv), `fsd`, `boot` (priv). Timer interrupt and interrupts are switched on. The main thread becomes
   the idle loop: reap orphans, `schedule()`, drain the serial buffer, `cpu_idle()` (stop until an interrupt).
6. `diskd` finds the disk and RDB and creates the `disk` port. `fsd` mounts a device through it and creates the
   `fs` port. `boot` polls for `fs` (up to ~30 s), then `spawn("/bin/init")`.
   `spawn` itself is a message to `fsd`: the kernel's `proc_spawn` reads the file via `fs_read_file`.
7. **`/bin/init`** prints `/etc/motd`, spawns `/bin/sh` with the tty as stdio, makes it the foreground process and
   waits; if the shell exits it starts another.

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
5. `cat` calls `read(fd, buf, 1024)`. After `user_ok` checks the buffer, the kernel sends `FS_READ{ino, offset,
   buf, len}`; **`fsd` copies from its cache directly into `cat`'s buffer** (no intermediate copy) and replies
   with the byte count.
6. `cat` calls `write(1, ...)` on the pipe: bytes go into the ring; a waiting `grep` is made ready. A full ring
   blocks the writer (`P_IO`); `wake_io()` re-checks blocked peers when either side changes.
7. `grep` reads the pipe, filters with the regex engine, writes to the tty. The kernel's `tty_write` passes the
   bytes to the terminal parser, which updates that workspace's grid; if the workspace is visible the glyphs are
   drawn with the blitter-assisted display code, and the focused tty is mirrored to the serial port.
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
4. If the exception was in **supervisor mode**, it's a kernel bug: print the vector, PC and fault address and
   `panic` (halt).

What this does **not** catch: a program that writes through a wild but *valid* pointer into someone else's
memory. With no MMU every address is valid for every program. See limits.md.

Note that on a 68020 odd-address word/long access does *not* fault, so crash tests must use `illegal`, not an odd
pointer.
