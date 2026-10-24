# Zenix

**A Unix-like operating system for the Amiga. Real processes, a real shell, a real filesystem, on a 68020 with no MMU.**

Zenix boots on the Amiga itself and owns the whole machine: no AmigaOS underneath, no Linux, no emulation layer. It
borrows Kickstart only long enough to load its kernel, then switches AmigaOS off and runs its own scheduler, memory
manager, disk servers and ten text consoles.

```
 Zenix - a small Unix-like system for the Amiga
 68020+, no MMU needed. Type help.

zenix:~# uname
Zenix 0.2 m68k (68020+, no MMU)
zenix:~# echo "hello from $(uname | cut -d' ' -f1)"
hello from Zenix
```

> **Status (2026-10-24): runs on a real Amiga.** It boots on an A1200 (PiStorm32-lite, Emu68, Kickstart 3.2.3) from
> its SD card, with no floppy, next to AmigaOS on the same card, through a boot menu. Every change is tested first on a
> set of emulated Amigas (FS-UAE), then on the real one. It is young (started 2026-10-22), changing
> every day, and not yet released.

## Why it's interesting

- 🧠 **No MMU needed.** Any Amiga with a 68020 or better can run it, a stock A1200 included. Programs are relocated as they
  load, and a crashing program is killed and its memory freed while the rest keep running.
- ✉️ **Built on messages, like Exec and QNX.** The disk and the filesystems are separate servers. A call to one passes
  a pointer, not a copy, so reading a file copies the data exactly once.
- ⚖️ **Fair and responsive.** 32 priority levels with `nice`, 50 ms time slices, and priority inheritance: a server
  always works at the speed of whoever is waiting for it.
- 💾 **Talks to the rest of the world.** Its filesystem is ext2, which Linux reads, writes and checks. It reads Amiga
  RDB and PC partition tables and keeps AmigaOS partitions untouched. Plus ZenixFS, its own copy-on-write filesystem.
- 🖥️ **A proper Unix at the keyboard.** A bash-like shell with job control, history and completion, 113 programs with
  manual pages, users and permissions, ten consoles on Amiga+F1 to F10, an ANSI/VT100 terminal with scrollback.
- 🔁 **Lives next to AmigaOS.** A boot menu picks the system; `bootzenix` in AmigaOS and `bootamigaos` in Zenix
  switch between them with one command.

## How it fits together

```
   sh   ls   grep   edit   less   cu   ...                  programs (user mode, loaded anywhere in RAM)
     \    |     |    /                                       system calls (TRAP #0), every pointer checked
 ┌──────────────────────────────────────────────────────┐
 │ kernel: scheduler · memory · message ports · files,   │
 │ pipes, tty · console (Copper + blitter) · drivers     │
 └──────────────────────────────────────────────────────┘
     |               |                 |                     messages, zero-copy
   fsd (ext2)    zfsd (ZenixFS)    diskd (disks, partitions)   servers (user mode)
```

The full picture is in [docs/architecture.md](docs/architecture.md). The reasons behind it (why no `fork`, why
messages, why ext2, why it boots through Kickstart) are in [docs/why.md](docs/why.md).

## Try it

You need the `m68k-elf` cross toolchain (gcc and binutils), Python 3, e2fsprogs and `fakeroot`. For the tests you also
need FS-UAE and your own Kickstart ROMs.

```sh
make              # build/zenix.adf (boot floppy) and build/zenix.hdf (root disk)
make run          # boot it in an FS-UAE window
make check        # every test, host and emulator (about 1.5 hours)
```

For a PiStorm SD card, `tools/mkdualcard.sh` builds a card image with the boot menu, an AmigaOS partition and Zenix.
See [docs/pi-first-boot.md](docs/pi-first-boot.md).

> ⚠️ **Not locked as shipped.** Root logs in by itself on console 1 and nobody has a password. Set them with `passwd`
> and remove `/etc/autologin`. And with no MMU, permissions guard against mistakes, not against a program written to
> cheat ([docs/limits.md](docs/limits.md)).

## Read more

| | |
|---|---|
| [Why it works this way](docs/why.md) | The thinking behind each big decision, in plain words |
| [Architecture](docs/architecture.md) | Every part, how it works and where it lives |
| [How it works](docs/how-it-works.md) | Walk-throughs: power-on to prompt, a pipe, a crash |
| [Source tree](docs/source-tree.md) | What each folder and file is |
| [Features](docs/features.md) · [Limits](docs/limits.md) | What it can do, what it can't (yet) |
| [Commands](docs/commands.md) | All 113 programs, each with its manual page |
| [Roadmap](docs/ROADMAP.md) · [Design decisions](docs/DESIGN.md) | Where it's going, and what was decided |
| [Next to AmigaOS](docs/vs-amigaos.md) · [Next to Unix](docs/vs-unix-linux.md) | How it compares |

All documents: [docs/README.md](docs/README.md).

## Licence

GPL-2.0-only ([LICENSE](LICENSE)). Code taken from other projects keeps its own licence: miniz and `user/regex.c`
are MIT.
