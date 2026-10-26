# Future ideas (after the feature halt)

Ideas the owner wants kept for later (decided 2026-10-23). **None of them is built**, and none may be built while the
feature halt holds (CLAUDE.md), except the stack guard, which the owner moved into the hardening work as a safety fix.
Each idea is grounded in public documentation: the ROM Kernel Reference Manuals and the Hardware Reference Manual, as
searchable text in the knowledge base (`Knowledge/manuals/`, read its INDEX.md, then grep), or textbook practice. The
first list came from another session's study of a classic microkernel's design, passed on as ideas only (clean room).
Zenix code is never written from anything but these descriptions, the public manuals and Zenix's own needs.

Facts about Zenix below were read from main on 2026-10-23; check them again before acting.

## 1. Signal bits per process: wait for many things at once
**Today:** a process waits on one thing at a time: one port, a timer, or one child. The only combination is
receive-with-timeout. There is no `select`/`poll`, and the shell polls the tty's interrupt flag.

**Idea:** each process gets two 32-bit words, *pending* and *waiting for*. Every source it cares about gets one bit,
allocated per process: each port it owns, tty input, a timer, child exit, Ctrl-C.
- An event ORs its bit into *pending*. If *pending AND waiting-for* is non-zero and the process is blocked, it becomes
  ready.
- A wait sets *waiting for*. If the AND is already non-zero it returns at once, so no wake-up is lost; otherwise the
  process blocks. On return it hands back the bits that fired and clears them.

**Why:** one AND decides every wake-up, `select`/`poll` become a thin layer on top, a terminal can wait for the
keyboard and the network together, and the shell stops polling.

**Care:** allocating and freeing bits per process; reserved bits for Ctrl-C, child exit and the timer; the
check-then-block must stay atomic against interrupts (audit B5).

**Public source:** this is how AmigaOS Exec works. See *ROM Kernel Reference Manual: Exec*, chapter "Signals" (page 19 of
that volume): `AllocSignal`, `Signal`, `Wait` and the mask that `Wait` returns. Exact calls are in *Includes and
Autodocs* (`exec.library/Wait`, `exec.library/Signal`).

## 2. Wake only the process that waits
**Today:** `wake_io()` (proc.c) walks every process slot and readies every process waiting for I/O, on any pipe or tty
event. Each one wakes up, checks, and sleeps again.

**Idea:** each object (a pipe end, a tty) records which process waits on it, and which bit to raise if idea 1 is built.
An event then wakes exactly that process. Without idea 1, a small list of waiters per object does the same.

**Public source:** per-object wait queues are textbook design (any Unix kernel's sleep/wakeup on a wait channel).

## 3. Stack overflow check at every task switch (approved as a fix, 2026-10-23)
**Before batch F:** there was nothing below any stack. Kernel stacks are 4 KB and interrupts run on them.

**Idea:**
- Put a guard pattern at the bottom of every kernel stack and user stack.
- Check it in `schedule()`/`switch_to` and on trap entry, and compare the saved stack pointers against the known
  bounds.
- On failure, kill the process with "pid N overflowed its kernel/user stack", or panic if it is pid 0.

**Why:** an overflow becomes a clear message instead of random crashes. The cost is a compare or two per switch.

**Public source:** stack canaries and guard words are textbook practice.

**Done** (audit batch F, 2026-10-23): architecture.md section 6 ("Stack guard").

## 4. CPU time per process and an idle counter
**Today:** there is no per-process CPU time, no idle counter and no profiling hook.

**Idea:**
- At each switch, read `hr_time()` (the CIA-B E-clock counter) and add the difference to the outgoing process's CPU
  time.
- Count pid 0's time as idle, and count the switches.
- Show both through `ps`/`top` as CPU% and idle%.
- Later: one hook pointer at the switch for profilers.

**Care:** the counter wraps after about 1.7 hours, so add up differences rather than storing absolute values.

**Public source:** the CIA timers are in the *Hardware Reference Manual* (appendix on the 8520 CIA) and the CIA data
sheets listed in `manuals/INDEX.md`.

## 5. Shorter stretches with interrupts off
**Today:** `schedule()` scans all process slots for sleepers with interrupts masked, and all of `proc_exit` runs with
interrupts off.

**Idea:**
- (a) Keep sleepers in a list sorted by wake-up tick, so the tick only looks at the head.
- (b) Separate "no task switch" from "no interrupts". The kernel is not preemptive, so most of `proc_exit` only needs
  not to be switched away from. Mask interrupts only around the fields that interrupt handlers also touch.

**Why:** long masked stretches cost serial and keyboard latency.

## 6. Deferred work through the software interrupt (needs care)
**Today:** `kwork()` runs from four places: the idle loop, the end of every system call, the level-2 interrupt when
it hit user mode, and between pieces of a long terminal write.

**Idea:** when an interrupt queues work, it sets SOFTINT in INTREQ. The hardware delivers it at level 1 as soon as the
mask allows, and `kwork` runs or is scheduled from there. The same trick gives "reschedule as soon as allowed" from
places that cannot switch right now.

**Care:** `kwork`'s re-entry guard; whether `kwork` may run at level 1 in the middle of a system call, since the kernel
is not preemptive.

**Public source:** *Hardware Reference Manual*, the interrupt chapter: INTREQ bit 2, SOFTINT, level 1.

## Lessons to test for
- **Freed memory really comes back.** Allocate, free everything, and check that the free total is back where it
  started. Do this for the kernel's `mem.c` and for the large-block regions of the user `malloc`. (A bug class that
  goes unseen for years when nothing tests it.)
- **Fairness.** If priorities are added, test that processes of equal priority switch only at the end of a time slice.
  Done with D10 (`priotest fair`); it found that the slice only counted ticks taken in user mode.

## Other ideas noted on 2026-10-23
- **Shared libraries:** one copy in memory used by many programs (AmigaOS-style jump tables work without an MMU).
  First candidates: libz, the regex engine, the line editor, a text-user-interface library for porting Sumi and Maki.
  The ZEXE format left room for it (DESIGN D3).
- **newlib** as a fuller C library, to make porting small Unix and Minix programs easy.
- **PFS3 support** written from the on-disk format (facts, not code), or as a separate filesystem server under its own
  licence. PFS3's code is 4-clause BSD, which cannot be mixed into GPL code; its authors could be asked to drop the
  advertising clause.
- **SD card log area:** once the SD driver is proven on the owner's card, save the kernel log to a small area of that
  card, so it can be read on a PC.
- **Zenix as its own ROM: not pursued (owner's decision, 2026-10-23: "not for now, maybe never").** Zenix must run on
  Amigas in general, booted the normal way from a floppy or disk, not only on the owner's PiStorm. A clean-room hand-off
  note about ROM booting (2026-10-23) is kept with the owner's research notes, outside the repository. Two points from
  it still matter without a ROM:
  - Zenix borrows the Topaz 8 font from the running Kickstart at boot. Checked 2026-10-23: no Topaz data is copied into
    Zenix's files, and none may ever be (the font is Commodore's).
  - Emu68 also puts a standard flattened device tree (magic $D00DFEED) on its ROM board, a possible second way for
    stage0 to find the Pi's addresses.
- **The data cache without an MMU** on 68030/040/060 (audit Part 3 item 2): the transparent-translation registers
  (DTT0/DTT1 on the 040/060, TT0/TT1 on the 030, ACRs on the EC parts) can mark chip RAM non-cacheable and cache the
  rest, without paging. Public: the MC68040 and MC68030 user manuals in `Knowledge/manuals`, section "Transparent
  Translation". Needs measuring on hardware.
- **A boot menu on the small boot partition** (owner's idea, 2026-10-23): the Startup-Sequence runs a menu (Kickstart's
  own graphics, keyboard and mouse, a default with a countdown) offering Zenix (runs `zenixboot`) and one or more AmigaOS
  partitions. The reliable way to start another AmigaOS partition is to give it the highest boot priority for one boot
  and reboot (the menu restores the order next time), not to chain into its startup files. Until then, Kickstart's own
  Early Startup menu (hold both mouse buttons at power-on, Kickstart 2.0+) already chooses the boot partition. Pi-side
  choices (several FAT partitions, each with its own Emu68 and ROM, picked by the Pi firmware) are in the knowledge base
  note `amiga/emu68-and-pistorm.md`.
- **Remote access for Zenix, like Wasabi** (owner's idea, 2026-10-24): a daemon that lets the PC (and Claude) run
  commands, move files and see the screen on a running Zenix, as `wasabid` does on AmigaOS. Needs the network driver
  (GENET) and a TCP/IP stack first; until then the serial cable is the way in.
- **`zc`, Zenix Commander** (owner's idea, 2026-10-23): a text-mode two-panel file manager in the spirit of Midnight
  Commander - copy, move, delete, rename, view (with the pager) and edit (with the editor), a command line under the
  panels. Fits the owner's rules: works in 80x32 / 80x25 (no wide-screen layout), every key configurable (as Ahiru and
  CFile do), and loaded parts only when used. Natural after the shared text-interface library (and newlib); the owner's
  Ahiru and CFile (AmigaOS) have file-manager experience to borrow ideas from, but Zenix needs its own code (their
  shared core is built on AmigaOS).

## The kernel: the best idea from each system (owner's goal, 2026-10-23: "an outstanding kernel")
The owner wants the best of Exec, Linux, BSD, Minix, Fuzix, QNX, the Hurd and Darwin in one kernel. Principle 2 ("one way to do
a thing") still rules: for each area **one** idea is taken, the one that fits a 68020+ Amiga without an MMU, not all of
them. Ideas only: code may come only from GPL-2-compatible sources (Linux, Fuzix, BSD, Minix); Exec and QNX are
proprietary, so from them only the documented behaviour (the ROM Kernel Manuals; QNX's published papers).

| Area | Taken from | The idea | Zenix today |
|---|---|---|---|
| Scheduling | **Exec** | Priorities: the highest ready process runs, round robin within a priority. First thing to build after the halt (small, makes the terminal and servers feel quick); with a fairness test | **done** (D10, 2026-10-24) |
| Servers and priorities | **QNX** | Priority inheritance: a server handling a message runs at its client's priority, so a high-priority program is never stuck behind a low one's request | **done** (D10, 2026-10-24) |
| Waiting | **Exec** | Signal bits per process (section 1 above); `select`/`poll` built on them | one source at a time |
| Event API for programs | **BSD** | kqueue-style: register many events once, get the ready ones in one call (a thin layer on the signal bits) | - |
| Messages | **QNX / Minix 3** | Synchronous send/receive/reply, pointers not copies | **done** |
| Reliability | **Minix 3** | The reincarnation server: a supervisor restarts a crashed server or driver (fsd, diskd, the network server) instead of the system stopping | a dead server stays dead |
| Devices | **Exec** | Servers loaded on first use (like `OpenDevice` loading from `DEVS:`), unloaded when unused | all start at boot |
| User-level servers | **the Hurd** | Translators: a program can serve a file or directory (an FTP site, an archive, `/proc`) and any user may attach one to their own files | mount is root-only |
| `/proc` and `/dev` | **Linux / Plan 9 via the Hurd idea** | System information as files, served by a translator, not by special system calls | special calls (`procinfo`, `meminfo`) |
| Memory | **Linux/BSD slab + Exec pools** | Slab caches for kernel objects (no fragmentation from many small objects); pools for programs | general allocator; size classes in libz |
| No-MMU discipline | **uClinux / Fuzix** | Relocated flat binaries, `vfork`/spawn, careful fragmentation rules; Fuzix's habit of keeping everything small | **done** (ZEXE, spawn) |
| Shared libraries | **Exec** | Libraries with jump tables, opened by name, one copy in memory (works without an MMU) | static only |
| Timers | **Linux** | One ordered timer list (or a timer wheel); sleeping processes cost nothing until due | scan of every process |
| Port security | **Darwin (Mach)** | Port rights: a process may only send to a port it holds a send right for, handed out by the kernel or by another holder. Replaces batch D's reserved names and kernel flag with one clean rule | reserved names + a kernel-only flag |
| Starting and keeping servers | **Darwin (launchd)** | One service manager that creates a server's port at boot but starts the server only on the first message, and restarts it if it dies (joins the Exec "on first use" and the Minix 3 restart rows) | all start at boot |
| Drivers | **Darwin (IOKit)** | A registry of hardware found (Zorro boards, the Pi's device tree) and drivers matched to it by properties, instead of each driver probing for itself | each driver probes |
| Users and permissions | **Unix/BSD** | The classic model, with the honest no-MMU limit | **done** (batch D) |


**How these get built without making a mess (owner, 2026-10-24: "keep it nice and tidy"):**
- **One idea per area.** Where two rows overlap they become one mechanism: Exec's "on first use", Minix 3's restart and
  launchd are **one** service manager (grown from today's `boot`); the signal bits are the base and `select`/`poll`
  and the kqueue-style call are thin layers on them, not three systems.
- **A new mechanism replaces an old one** in the same step: port rights replace the reserved names and the kernel
  flag; the service manager replaces "start everything at boot"; the ordered timer list replaces the scan. Nothing is
  kept "for now" next to its replacement.
- **A design note first:** each idea gets a numbered decision in DESIGN.md (what, why, what it replaces, what it costs)
  before code, and the owner agrees to it.
- **Measured and tested:** each one comes with checking tests in `make check`, and a before/after measurement on the
  real A1200 (kernel size, switch cost, message cost).
- **An audit after every batch,** as on 2026-10-22, before the next one starts.

What stays deliberately out: `fork` (needs an MMU), Linux's size and its thousands of system calls, Exec's lack of
users and of resource tracking (Zenix keeps per-process ownership: everything a process held is freed when it dies).

Last checked: 2026-10-24.

## The software list (owner, 2026-10-24: "an operating system isn't better than its software")
A list of programs Zenix could have, by area, from a read-only survey on 2026-10-24 and the owner's goal. **None of
it is approved** unless marked: the feature halt still holds, and each item needs the owner's OK, a design note where
it touches the architecture, tests, and a build on the A1200. Marks: **[built]**, **[approved]**, **[owner]** (the owner's
own wish: a terminal good enough to run Claude Code from the A1200), otherwise an idea. Where an idea needs a foundation first, the
foundation is named.

**Foundations that most of the list depends on** (each is its own decision):
newlib, a fuller C library (porting) · signal bits and `select`/`poll` (item 1 above) · shared libraries · a shared
text-UI library (full-screen tools) · the TCP/IP server [approved, after D11 and self-updates] · a display server and a
sound server (phase 5) · the service manager [approved: D11, being built].

**1. Shell and terminal life:** `zc` Zenix Commander · ports of the owner's Sumi (editor), Maki (pager), Markdown
(viewer) · `top`/`ctop` (needs CPU time per process, item 4) · FuzzyFind (`fzf`, `locate`, `updatedb`) · tmux/screen-style
panes and detachable sessions · an `ncdu`-style disk browser · `htop`-style process tree, `pstree` · `script` and
`ttyrec` · `bc`/`dc`, `units` · fish-style suggestions, a `.zenixrc` setup wizard · `cron`/`at` · `logger` and a syslog
server · `banner`, `figlet`, `cowsay`, `fortune`.

**2. Text and data:** finish `awk` (parked in user/awk.c) · `join`, `split`, `csplit`, `column`, `fmt`, `pr`, `shuf`,
`od`, `strings`, `iconv` · `patch`, `diff3` · `bzip2`, `xz`, `zip`/`unzip`, `cpio` · **`lha`/`lzx` unpackers** (Aminet
archives) · a `jq`-style JSON tool · a small database or key-value store · `file` (Amiga formats too) · `sha1sum`,
`crc32`, `b2sum`.

**3. Programming on the machine:** an assembler and a C compiler that output ZEXE (vasm/vbcc/tcc-like) · `make` · a
debugger `zdb` on the crash handler · an `strace`-style tracer (the message design suits it) · a profiler on the switch
hook · `objdump`/`nm`/`size` for ZEXE, a 68k disassembler · Lua, Forth, Scheme, MicroPython · ARexx-like scripting through
programs' command ports · a small version-control tool.

**4. Networking (after TCP/IP):** `ifconfig`/`ip`, real `ping`, `route`, `netstat`, a DHCP client, `/etc/hosts` and DNS ·
network cards as servers (the Pi's GENET first [approved], Zorro cards, SLIP/PPP over serial) · the Wasabi-like remote
daemon [owner's idea, later] · `telnet` [owner], `ssh` client/server, `nc` · `wget`/`curl`, `ftp`, `tftp`, rsync-lite ·
an NFS client (the owner's AmiNFSv3 knowledge), an SMB client for the NAS · a tiny `httpd` status page · IRC, Gemini,
Gopher, a lynx-style browser · a BBS client with ANSI and Zmodem (`sz`/`rz`), BBX's door protocol · NTP · a mutt-style
mail reader · **Claude on Zenix** [owner: `cu` over serial **[built]**, then over the network].

**5. Desktop (phase 5):** the display server (planar, blitter, Copper) · a terminal window first · workspaces and an
i3-style bar · a tiling window manager (the Sprawl idea) · an Ahiru-style file manager · picture viewer (IFF ILBM, PNG,
QOI, GIF, JPEG) · text-art viewer (ANSI, Amiga ASCII) · clock, calendar, meters · screenshot, screen blanker · a
Deluxe Paint-lite, a font editor · one preferences program (ControlPanel-style).

**6. Sound and music (a Paula server):** `play` (WAV, 8SVX, QOA, FLAC) · a MOD/tracker player (the owner's bbmp
knowledge) · AHX/HivelyTracker, SID later · `mixer`, a notify sound · a small tracker or sequencer (much later).

**7. Games and fun:** rogue/nethack-lite, a Z-machine (Zork), tetris, snake, 2048, minesweeper, chess, sokoban · after
the display server: blitter/sprite/Copper games · demos that run on Zenix.

**8. Amiga compatibility:** an FFS/OFS reader (read-only first), PFS3 from its format description (licence, above) · `adf`
tools and writing a floppy from an image · a FAT reader/writer (the Pi's boot partition, USB sticks) · ISO 9660 · IFF
tools · an AmigaOS CLI-program runner through a dos.library translation layer (large; needs a design decision first).

**9. Administration and reliability:** the service manager [approved: D11] · `useradd`, `userdel`, `groupadd`,
`chpasswd`, shadow passwords · `/proc`-style information, `lsof`, `fuser`, `vmstat`, `iostat` · `lsdev` (Zorro boards,
Gayle, the Pi) · an installer (partition, format, copy, boot menu) · **a package manager like Arch's pacman** (owner's wish, 2026-10-24, see below) · backups, ZenixFS
snapshots · disk health, `badblocks` · Zenix installing its own updates [approved, after D11] · serial tools: `getty`
on serial, `stty`, a minicom-style terminal (`cu` **[built]**), file transfer over serial.

**10. Drivers (as servers):** USB (keyboard, mouse, memory sticks) · RTG graphics cards · faster IDE, CF, CD drives ·
the clock on every model · joysticks, the parallel port (printer, sampler) · a printing service.

**The order proposed on 2026-10-24:** the owner's wish first (TCP/IP, `telnet`, a better console), then newlib,
shared libraries and the text-UI library (they unlock most of the rest), then the owner's own tools (Sumi, Maki, `zc`),
`lha`/FFS/FAT and an installer, then the display and sound servers with a MOD player and games.

**More ideas (100, added by the owner 2026-10-24; ideas only, same rules as the list above).** The owner's marks the
same day: the ten under *Claude and the terminal* are **wanted** (the first five are also the owner's own wish); the rest were
skimmed, "yes" for the list, to be looked at properly before any is built. Instead of porting csh: **Zenix's own `sh`
is to be excellent from the start** - the best of bash, zsh and fish, as the owner's csh was designed.
- *Claude and the terminal* (**wanted**): a `claude` command that connects to Claude on the PC (serial or network) [owner] · a console
  mode tuned for Claude Code's screen (status line, symbol stand-ins) [owner] · UTF-8 in the console with Topaz stand-ins
  [owner] · a 640x512 interlaced or 132-column console [owner] · a colour console (2-4 bitplanes, ANSI colours) [owner] ·
  `ansiview` for text-art files · a clipboard shared by the consoles (Amiga+C / Amiga+V) · mouse text selection · search
  in the scrollback · saving a console's history to a file.
- *The owner's programs ported:* CTerm's tabs and layouts in the console · CUtils' `ls` colours
  and `ctop` look · Ahiru in text mode · the utf8 and tui libraries as Zenix shared libraries · a `json` command and
  library · Charcoal's text-art viewer · a Wasabi-compatible daemon so the existing `wasabi` tool works with Zenix.
- *BBS and the scene:* a BBX client · a small BBS running on Zenix (serial or telnet) · message-network tools (FidoNet,
  AmigaNet packets, a QWK reader) · door games · a text-mode ANSI editor (a small Charcoal) · an NFO/DIZ viewer with
  Amiga fonts · a scene-news reader (Pouet, Demozoo) · a bootblock and cracktro viewer that runs old intro code safely.
- *Music:* a Modland/HVSC browser streaming from the NAS mirror · a ProTracker-style tracker in the text console · a
  MOD info tool · text-mode VU meters · a background playlist daemon · a network remote for the player (PC, phone).
- *Everyday use:* a to-do list · notes with Markdown and search · a calendar with reminders at login · weather · an RSS
  reader · a pomodoro timer · an encrypted password store · unit and currency conversion · a world clock · a small
  spreadsheet (sc-style) · a word processor with page layout · an address book · a dashboard console (clock, CPU,
  memory, network, news).
- *For drivers (the owner's DropPoint domain):* a route and stop list · a trip log (km, fuel, notes) with totals · an EU
  driving-hours calculator.
- *System and safety:* a health screen (Pi temperature and voltage, as `wasabi health`) · a crash reporter (after a panic,
  the log saved to the card and shown at the next boot) · a disk check at boot after an unclean stop · a safe-mode boot
  entry in the menu · snapshots before updates, rollback from the boot menu · a firewall in the network server · login
  history (`last`, `who`, failed logins) · disk quotas · a memory-map viewer (owners, fragmentation) · a live monitor of
  the messages between servers · a benchmark suite that keeps the A1200 numbers over time · a hardware information
  screen (CPU, Emu68 version, Zorro boards, memory).
- *Amiga bridges:* reading the AmigaOS partitions on the same card (PFS3, read-only) · a shared FAT exchange partition ·
  an AmigaOS filesystem handler for Zenix's ext2 and ZenixFS · running Zenix inside AmigaOS in a window, for testing
  without a reboot · an AmigaGuide viewer · a `.info` icon viewer and converter · a Kickstart ROM information tool · a
  Workbench-style drawer view in `zc`.
- *Programming and learning:* a 68k assembly tutorial in a sandboxed process · a BASIC interpreter (AMOS/Blitz spirit) ·
  Logo (after the display server) · a C playground (with the compiler) · manual pages for the kernel interface (system
  calls, messages) · a "write your own server" kit with a template · a 68k instruction reference · a read-only Amiga
  hardware register browser (root).
- *Fun and the demoscene:* ASCII demos in the console · a Copper rainbow screen blanker · a cracktro-style "about Zenix"
  screen · a boot logo in the boot menu · two-player Pong on one keyboard · Hunt the Wumpus, Lunar Lander, Star Trek ·
  a daily puzzle (crossword, sudoku) · a MUD client, later a small MUD server.
- *Networking extras:* an NFS server so the PC can mount Zenix's files · `talk`/`write` between Zenix machines · a small
  web server for files (download to the phone) · wake-on-LAN and remote reboot of the PC · a Home Assistant / MQTT
  client · a packet sniffer · the console in a web browser on the phone.
- *Distribution:* a live floppy for any A1200 · an Aminet release with an installer · a website served by Zenix itself ·
  a first-boot tour for new users · translated messages (Swedish first) · a ready FS-UAE image.
- *Long term:* an optional MMU mode on 68030/040/060 (open question in DESIGN.md) · letting the Pi's ARM side do jobs
  for Zenix (compression, networking) · Zenix on other 68k machines (Atari ST/TT, 68k Macs, Sun 3).

**A pacman-like package system (the owner's wish, 2026-10-24).** Installing and updating programs, and Zenix itself, the
way Arch Linux's `pacman` does: a repository with a package database, `zpkg -S name` to install with its dependencies, `zpkg -Ss word` to search the repository (names and descriptions),
`zpkg -Syu` to bring the whole system up to date, `zpkg -R` to remove (`-Rns`: also the dependencies nothing else needs, and the package's saved settings files, as pacman), a record of which package owns which file. The
repository lives on the owner's NAS (served over the network: HTTP or NFS, once Zenix has TCP/IP), built on the PC from
the Zenix build. It must be **one mechanism with the approved self-updates** (Zenix installing updates by itself, after
D11): the self-update is designed as the first part of it (the kernel, the boot files and the system as packages, with
the old version kept to boot back to from the menu), so there is never a second way to update. Needs: the network,
checksums on every package (sha256sum exists), and a design note before any code.

## Amiga demos on Zenix (the owner's wish, 2026-10-26; an idea, not approved as a feature)

Choosing a demo from one's own collection in Zenix and watching it run on the real hardware. A demo does not want an
operating system, it wants the machine: Zenix's part is to hand it over cleanly and to get it back. After block 4 (Zenix
must first read Amiga partitions and LhA/LZX archives, where demos live). Three steps, smallest first:

1. **Executable demos** (the files in a demo archive). At start-up they ask AmigaOS for a few things (memory, loading
   their files, the graphics chips: `OwnBlitter`, `LoadView(NULL)`, `WaitTOF`, `Forbid`/`Disable`), then switch the
   system off and use the hardware directly. A small *demo layer* answers those calls (dozens of functions, not the
   thousands a full AmigaOS layer needs), moves Zenix's screen, copper list and interrupts out of the way, and gives the
   demo the chip RAM. A clean-room job: written from the public interface documentation (the autodocs and manuals),
   never from AmigaOS source.
2. **Disk demos** (ADF images with their own track loaders). They read the floppy hardware directly, so running one from
   a file needs a patch that sends its disk reads to memory. WHDLoad has such patches ("slaves") for thousands of demos
   and games and their interface is published: Zenix could offer that interface and use the patches the user downloads.
   A bigger job. (On a machine with a floppy or a Gotek, booting the disk itself already works without Zenix.)
3. **A demo browser**: a text-UI list of the demos found, with their group, year and machine needs (AGA, memory), and
   one key to run.

Getting back: a well-behaved executable demo returns to the system just as it returns to Workbench under AmigaOS: it
gets its chip RAM through `AllocMem` (the demo layer hands it out of Zenix's own free memory, so Zenix's screen and code
are never given away), saves the system's view, interrupts and DMA settings, and puts them back at the end, which puts
Zenix's console back. While the demo runs Zenix is paused (the clock is caught up after; serial input in that time may
be lost). Only demos that also do not return under AmigaOS (writing anywhere in memory without asking, ending in a
reset, booting from their own disk) need a reboot into Zenix to get back (seconds on a PiStorm). Demos that fail on
very fast CPUs (Emu68) fail the same way under AmigaOS; Zenix cannot change that. The wider question, AmigaOS shell
programs (a dos/exec layer, as amitools' vamos does on the PC) and window programs (a whole second system, AROS-sized),
was discussed the same day: shell programs are realistic later, window programs are left to the dual boot.

## Amiga programs on Zenix: the Wine route (the owner's wish, 2026-10-26; a long-term idea, not approved as a feature)

The owner: not the highest priority, but even a small share (10%, 40%: examples, not goals) of Amiga programs
working unlocks a large software library.
After the demos above (easier, first). The idea, as Wine does for Windows: rewrite only the core parts that clash with
Zenix and load everything else unchanged from the machine. MUI, ReAction and GadTools sit on top of intuition,
graphics, layers, exec and dos; graphics, layers, intuition and the input/timer devices are in the owner's own
Kickstart ROM and mostly talk to the chips, which are really there (no emulation). Zenix writes:

- **its own exec** (tasks as Zenix processes, memory, messages, libraries and devices, signals on D16's event bits),
  laid out like the real one inside, since ROM modules and programs read ExecBase fields, not only call functions;
- **files:** its own dos, or the ROM's dos with a small filesystem handler that serves Zenix files (likely smaller);
- **the glue:** the ROM modules started from their resident tags on that exec; the interrupt server lists shared with
  Zenix's interrupts; the screen and keyboard switched between the Zenix console and the Amiga screens (as Amiga-M).

No memory protection between programs, as on AmigaOS, so shared structures and messages work as expected. Clean room:
written from the public autodocs and manuals, the user's own ROM at run time, never AmigaOS source. Steps, each
visible: (1) a shell program runs (exec and dos only; the "shell programs" level discussed the same day); (2) the
ROM's graphics opens a screen and draws; (3) intuition opens a window, the mouse works; (4) GadTools, ReAction, MUI.
Hard parts: exec's private fields, interrupt sharing, the screen switch, Kickstart versions (3.1, 3.2, 3.2.3).
Size estimate (proposed): 15,000-25,000 lines, about half of Zenix's 37,500 lines of system code on 2026-10-26; the
long part is the compatibility hunt afterwards, months of small fixes, as with Wine. MorphOS runs old Amiga programs in
a similar way.

## The desktop: notes from a talk with the owner (2026-10-26; ideas, nothing decided)

Block 5, "much later, when the foundation is rock solid" (the owner). **Two desktops on one base:** a tiling one (the
owner's taste, i3-like) and a classic one with icons and floating windows for everyone else. Both on the same window
server and toolkit; only the window manager's policy differs, so every program runs in both.

- **Tiling on 640x256 PAL hires:** one full-screen program per workspace with tabs (i3's tabbed mode) as the default;
  splits possible (two 45-column terminals with a 7-pixel font) but not the default; requesters and file dialogs float
  briefly. Non-overlapping windows are also the cheap design: no hidden parts to keep or redraw.
- **Workspace = its own screen + layout + font:** each workspace has its own Amiga screen mode (e.g. shells at 640x256 8
  colours with a 7-pixel font; a BBS at 640x512 interlaced 16 colours, flicker-free with an Indivision; pictures in a
  256-colour mode). The bar is its own thin screen above, joined by the copper; where the modes cannot share a frame
  (interlace, other scan rates) the bar is redrawn in the workspace's mode. On 2 MB only the visible workspace keeps its
  bitmap (programs redraw on a switch); with fast RAM recent ones stay ready. On a graphics card each workspace is a
  card screen and the bar is drawn on each.
- **Toolkit: our own, small, not from zero.** Borrow microui's minimal core (MIT), EmuTOS GEM's bitplane drawing know-how
  (GPL-2+), MUI's box/group layout as an idea (no code), and the owner's tui.library model, so the block-3 text-UI
  toolkit and the window toolkit share one way of building programs. Two drawing backends (bitplanes + blitter; chunky
  pixels for graphics cards), keyboard first, configurable keys, designed for 4-8 colours; a shared library (D13 stage
  2), estimate 30-50 KB of code. Ported toolkits (Nuklear, LVGL, Nano-X) assume chunky pixels or floating point.
- **Not Intuition from ROM** for Zenix's own desktop (it needs the Wine-route exec, puts AmigaOS back underneath, is
  tied to ROM versions, overlaps windows, needs P96/CGX on cards); ROM Intuition is for running Amiga programs. Ask again
  only if that layer one day works well.
- **First program:** a terminal (day one is an i3 full of terminals), then a file manager, settings, an editor, telnet
  for BBSes in a window and the demo browser.
