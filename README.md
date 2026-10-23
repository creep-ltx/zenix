# Zenix

A small Unix-like operating system for Amigas with a 68020 or better. **No MMU is needed.** Zenix runs on the Amiga
itself: the floppy's bootblock borrows Kickstart to load the kernel, then switches AmigaOS off and owns the machine.
No AmigaOS, no Linux and no emulation layer underneath.

**Status (2026-10-23):** what is listed below is *verified in FS-UAE* (an emulator: a stock A1200 and the other machines
of the test matrix) unless it says otherwise.
**Nothing has run on a real Amiga yet**; the first boot on the owner's PiStorm A1200 is next ([docs/pi-first-boot.md](docs/pi-first-boot.md)).
New features are on hold until then (hardening round, [docs/audit-2026-10-22.md](docs/audit-2026-10-22.md)). Emulator timing is not hardware
timing, so no speed is claimed.

## What it has
| | |
|---|---|
| **Kernel** | Relocating kernel and programs (no MMU), preemptive scheduler, chip and fast RAM with an owner for every block, `spawn` instead of `fork`, synchronous zero-copy message ports, system calls with every pointer checked, errors as errno numbers. A crashing program is killed and its memory freed; the rest keeps running. |
| **Servers** | The disk (`diskd`) and the filesystems (ext2 and ZenixFS) are user-mode servers behind message ports. |
| **Filesystems** | ext2 read and write (Linux and `e2fsck` agree with it; ext3 read-only, ext4 refused), ZenixFS (our own: copy-on-write commits, checksummed metadata), either one as the root; `mount`, `/etc/fstab`; RDB and PC (MBR) partition tables. |
| **Disks** | Gayle IDE (A600/A1200/A4000), A3000 SCSI, the PiStorm's SD card (written, not yet run on a Pi), floppy drives (experimental), a RAM disk from the boot floppy. |
| **Console** | Ten text consoles on Amiga+F1..F10, each with its own login (like Linux); ANSI/VT100 terminal, 80x32 on PAL (80x25 on NTSC), scrollback, the ROM's Topaz font, blitter and Copper scrolling; Amiga keyboard with the Swedish and US keymaps, key repeat. |
| **Users** | Users and groups, file permissions checked by the filesystem servers, `login`, `su`, `passwd`, set-user-id programs. |
| **Shell** | A bash-like `sh`: line editing, history, completion, variables, quoting, wildcards, pipes, redirects, here-documents, `if/while/for/case`, functions, aliases, arithmetic, command substitution, job control. Checked against bash. |
| **Commands** | The usual Unix tools, each with a manual page: files, text (`grep sed sort diff ...`), `tar`, `gzip`, a pager, an editor, `man`, disk tools (`fdisk mkfs fsck`), processes and users. List: [docs/commands.md](docs/commands.md). |
| **Diagnostics** | Boot messages on the screen and in `dmesg`, a screen colour for every boot step, and the previous boot's log kept across a warm reset. |

The full list with the status of each feature is [docs/features.md](docs/features.md); what is missing is [docs/limits.md](docs/limits.md).

## Not locked as shipped
**Root is logged in automatically on console 1, and nobody has a password** (root, user and guest). `/etc/autologin`
does that (the tests rely on it); the other consoles ask for a name, with no password. `login` warns about it. Before
trusting the system, set passwords with `passwd` and remove `/etc/autologin`. Even then, with no MMU the permissions
protect against honest mistakes, not against a program written to cheat ([docs/limits.md](docs/limits.md)).

## Build and test
Needs: the `m68k-elf` cross toolchain (gcc and binutils; `make TOOLPREFIX=/path/to/bin/m68k-elf-`, by default
`m68k-elf-gcc` from PATH), Python 3, `mke2fs`/`debugfs`/`e2fsck` (e2fsprogs) and `fakeroot`. For the tests: FS-UAE and
the Kickstart ROM files, in the directory named by `ZENIX_KICKSTART_DIR` (tools/fsuae_conf.py).

- `make` builds `build/zenix.adf` (the floppy: bootblock and kernel) and `build/zenix.hdf` (the root disk);
  `make build/zenix-ram.adf` the floppy with a small root filesystem in RAM, for a machine with no disk (a Gotek boot).
- `make check` (also `make test`) builds everything and runs every strong test, host and emulator, one after another,
  and prints a table; about 1.5 hours. How to run single tests: [docs/README.md](docs/README.md), "Testing".

## Documentation
[docs/README.md](docs/README.md) is the index: architecture, walk-throughs, comparisons with AmigaOS and Unix, the command manuals, the
feature list, the design decisions ([docs/DESIGN.md](docs/DESIGN.md)) and the plan ([docs/ROADMAP.md](docs/ROADMAP.md)).

## Licence
GPL-2.0-only ([LICENSE](LICENSE); [docs/DESIGN.md](docs/DESIGN.md) D9). Files taken from other projects keep their own licence: miniz and
`user/regex.c` (from CUtils) are MIT.

Last checked against the code: 2026-10-23.
