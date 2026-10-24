# Why Zenix works the way it does

Every big choice in Zenix answers one question: *how do you build a real Unix on a machine that was never meant to
have one?* This page gives the reasons in plain words. The exact rules are in [DESIGN.md](DESIGN.md) (the numbered
decisions), the parts in [architecture.md](architecture.md).

## The machine sets the rules

An A1200 has a 14 MHz 68EC020, 2 MB of chip RAM and no MMU. Many accelerator cards have a full CPU with an MMU, but many
others do not (the EC versions of the 68020, 030 and 040), and the PiStorm's Emu68 offers none. Unix was designed around an MMU. Each
program gets its own private memory, `fork` copies a process cheaply, and a bad pointer only hurts its owner.

Zenix keeps what a Unix *user* sees (processes, a shell, pipes, files, users, permissions) and finds another way to
build each piece that would need an MMU. The sections below are those other ways.

## No MMU: all programs share one memory

**What we do.** Every program and the kernel live in the same memory. A program file (ZEXE) carries a relocation
table, and the loader patches the program for wherever it lands, as AmigaOS does with its hunks. Every block of memory
has an owner, so when a program ends or crashes, everything it had is given back.

**Why.** It is the only way to run on a stock A1200. Requiring an MMU would rule out most Amigas, and the target is
real Amigas ([ROADMAP](ROADMAP.md), principle 6).

**The price, said openly.** A program can write anywhere, so a wild pointer can damage another program. Zenix limits
the damage: CPU faults kill only the faulting program, a guard pattern below every stack catches overflows, and a
check word on every memory block catches heap overruns. But it cannot prevent a deliberate cheat. Permissions protect
against mistakes, not against attacks ([limits.md](limits.md)). AmigaOS has the same trust level.

## `spawn` instead of `fork`

**What we do.** A new program is started in one step: `spawn(path, arguments, which files it gets)`.

**Why.** `fork` makes a copy of the running process. With an MMU the copy is free (both share pages until one
writes). Without one, the copy would land at another address, and every pointer inside it would still point into the
original. There is no way to fix that in general. No-MMU Linux has the same problem and uses `vfork`. `spawn` (as in
POSIX `posix_spawn`) is the clean answer: it never copies anything, so it is also faster.

## A small kernel and servers that talk in messages

**What we do.** The kernel schedules, manages memory, handles interrupts and passes messages. The disk (`diskd`) and
the filesystems (`fsd` for ext2, `zfsd` for ZenixFS) are separate programs, *servers*, behind named message ports.
When `cat` reads a file, the kernel sends the ext2 server a message and waits for the reply.

**Why.**
- **A crash stays small.** A filesystem bug kills the server, not the whole machine. Its clients get an error instead
  of hanging, and D11 will restart it.
- **Parts can be replaced.** ZenixFS was added as a second server speaking the same protocol, without touching the
  kernel. The network and the display will come the same way.
- **It's the Amiga way.** Exec, the heart of AmigaOS, is built on message ports, and QNX proved the idea for a
  professional Unix-like system. Zenix takes the best of both.

## Messages pass pointers, not copies

**What we do.** A message carries the address of the caller's buffer. The ext2 server copies the data from its cache
straight into the program's buffer: one copy, no matter how many layers are in between.

**Why.** A classic microkernel copies every message between private memories, and that is why such systems got a
reputation for being slow. Zenix has no private memories, so the copy is not needed. On a 14 MHz CPU every copy you
leave out is time you get back. What made the MMU-less design costly elsewhere is an advantage here.

## Calls wait for their answer (synchronous messages)

**What we do.** A program sends a message and sleeps until the server replies. There are no message queues to fill up.

**Why.** It is simple to reason about, nothing can pile up, and the server always knows exactly who is waiting for it.
That last point makes priority inheritance (next section) almost free.

## Priorities with inheritance

**What we do.** 32 levels, shown to users as the Unix `nice` scale. The highest ready level runs, and equal levels take
turns in 50 ms slices. The disk and filesystem servers sit above normal programs. A server always works at least at
the level of the client it is serving ([DESIGN](DESIGN.md) D10).

**Why.** That's Exec's rule, which makes the Amiga feel quick. Inheritance solves the classic trap: a low-priority
server serving an important program could be held up by a busy middle-priority one ("priority inversion"). The test
shows the difference: 50 calls took 87 ms with inheritance and 1481 ms without (FS-UAE).

## The disk servers are built into the kernel image

**What we do.** `diskd`, `fsd`, `zfsd` and the starter `boot` are inside the kernel file and start before any disk is
read. Everything else is loaded from the disk.

**Why.** To read programs from a disk you need the disk and filesystem servers, so they cannot come from that disk.
They still run as ordinary user-mode processes with their own ports. Only their *delivery* is special.

## ext2 as the main filesystem

**What we do.** The root filesystem is ext2, the classic Linux one, written from its public specification.

**Why.** It is a real Unix filesystem (owners, permissions, links, long names) that is simple enough for a small
system. Linux can read, write and *check* it, so every test ends with the PC's `e2fsck`, an independent judge that
catches our mistakes. And you can mount a Zenix card on the PC to copy files over. ZenixFS, the native filesystem,
adds copy-on-write commits and checksums, so a power cut never leaves a half-written state ([zenixfs.md](zenixfs.md)).

## Booting through Kickstart, never a custom ROM

**What we do.** Kickstart boots normally, from the floppy's bootblock or from a small AmigaOS partition running
`zenixboot`. That code asks the still-running AmigaOS what the machine has (CPU, memory, the ROM font), loads the
kernel, then switches AmigaOS off and jumps in. The boot menu picks AmigaOS or Zenix at power-on.

**Why.** Every Amiga can do it with no hardware changes and no ROM swap. Kickstart already knows how to read floppies,
hard disks and the PiStorm's SD card, and the memory map it hands over is exactly right for this machine. AmigaOS stays
installed and one keypress away, so Zenix never takes the machine away from you.

## A boot log that survives a reset

**What we do.** The kernel keeps its log in a reserved spot at the top of chip RAM, and each boot step paints the
screen its own colour. After a warm reset (Ctrl-Amiga-Amiga) the next boot finds the old log and shows it
(`dmesg -p`).

**Why.** On a real Amiga, an early crash leaves only a frozen screen, and there may be no serial cable attached. The
colour tells you *where* it stopped. The surviving log tells you *what* happened. That's how the first boots on the
real A1200 were debugged.

## Text consoles first, like Linux

**What we do.** Ten text consoles on Amiga+F1 to F10, each with its own login, drawn with the Copper and the blitter
in the ROM's Topaz font.

**Why.** A terminal you can live in is the foundation. It is what a Unix user needs first. The windowed desktop comes later as its own server (D4) and stays out of the text
console, so neither gets in the other's way.

## Multi-user, with an honest limit

**What we do.** Users, groups, file permissions, `login`, `su`, `passwd`, set-user-id programs, as on any Unix. The
filesystem servers check the permissions on every request ([DESIGN](DESIGN.md) D8).

**Why.** A Unix without users isn't a Unix: separate home directories, files that belong to someone, root versus a
normal user. Without an MMU it can't be a security wall, and the documents say so, so nobody relies on it as one.

## The serial port as a terminal

**What we do.** `/dev/ttyS0` belongs to one program at a time, and `cu` connects the console to another computer.
While it is in use, the kernel's own copy of the console stays off the wire (D12).

**Why.** It's Zenix's first link to another computer, before there is a network driver: a terminal session on
a PC or a server, from the Amiga's own keyboard.

## Coming next: servers that come back (D11)

**What we'll do.** A service manager that starts a server when it is first needed and restarts it if it crashes. The
clients' open files and mounts carry over, so a program notices only a short pause (its design note, `docs/d11-service-manager.md`, is on the `d11` branch
while it is being built).

**Why.** Servers were chosen so a crash stays small. D11 finishes that thought: a crash should not even be noticed.

## GPL-2.0-only

**Why.** Nobody can take Zenix and close it. And GPL-2 code from Linux, Fuzix and U-Boot can be used where it helps
(D9). AmigaOS's own source is never used. Zenix is written from public manuals and specifications only.

## How it is built: carefully

**What we do.** Nothing counts as done until a test checks it. `make check` boots a set of emulated Amigas (68020 to
68060, chip-only, with fast RAM, several Kickstarts) and compares the output of every command with GNU bash and the GNU
tools. Every change that matters is then tried on the real A1200. A new direction starts as a written design that is
reviewed before any code is written.

**Why.** Zenix grew very fast in its first two days and needed an audit and a feature halt to become solid
([audit-closing-2026-10-24.md](audit-closing-2026-10-24.md)). The way of working is the lesson from that: slower
steps, each proven, so the system you boot is one you can trust.

Last checked against the code: 2026-10-24.
