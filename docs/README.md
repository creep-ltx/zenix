# Zenix documentation

| Document | What it answers |
|---|---|
| [why.md](why.md) | Why Zenix works the way it does: the reason behind each big decision (no MMU, `spawn`, messages, zero-copy, priorities, ext2, booting through Kickstart, ...), in plain words. Start here. |
| [source-tree.md](source-tree.md) | What each folder and file in the source is, and where to start reading. |
| [architecture.md](architecture.md) | How Zenix is put together: the layers, the kernel, memory, processes, messages, the servers, the filesystems, the terminal and display, the build and the tests. |
| [how-it-works.md](how-it-works.md) | Walk-throughs: from power-on to a shell prompt, what happens inside `cat file | grep x`, what happens when a program crashes. |
| [features.md](features.md) | The one list of what Zenix can do, with the status of each feature (verified / read / planned). |
| [limits.md](limits.md) | What is missing, what is unmeasured, the hard numbers (limits), and the honest no-MMU limit. |
| [commands.md](commands.md) | The command index (every program, with a link to its manual page in `man/`, the pages `man` shows). |
| [vs-amigaos.md](vs-amigaos.md) | Zenix next to AmigaOS: what is the same, what is different, what Zenix gives up and gains. |
| [vs-unix-linux.md](vs-unix-linux.md) | Zenix next to Unix and Linux (and no-MMU Linux, Fuzix, Minix 3, QNX): where it behaves like a Unix and where it cannot. |
| [pi-first-boot.md](pi-first-boot.md) | How to try Zenix on a PiStorm A1200 for the first time: the boot colours, what the messages should say, what to report. |
| [zenixfs.md](zenixfs.md) | ZenixFS, the native filesystem: why, the design, the host tools, its status. |
| [zenixfs-format.md](zenixfs-format.md) | ZenixFS's exact on-disk format, version 1. |
| [d13-shared-code.md](d13-shared-code.md) | DESIGN D13's design note: one copy in memory of a program's code and of shared libraries, how (gcc's separate-data mode, a base register), the file format, the loader, safety, the measurements it must pass. Revision 3 after two reviews: ready to build. |
| [d14-colour-console.md](d14-colour-console.md) | DESIGN D14's design note: colours in the text console (bitplanes by machine, a setting, the ANSI codes, the programs that use colour, tests). Revision 2 after its review: ready to build. |
| [d15-self-updates.md](d15-self-updates.md) | DESIGN D15's design note: Zenix updates itself (the bundle on BOOT:, zenixboot's half and Zenix's installer, the journal and rollback, later serial and network). Revision 3 after two reviews: ready to build. |
| [d16-events-and-signals.md](d16-events-and-signals.md) | DESIGN D16's design note: waiting for many things at once (Exec-style event bits, `poll`/`select`) and Unix signals with handlers. Revision 3 after two reviews; built on branch d16 (section 15, "As built"). |
| [d17-ext2-clean-mark.md](d17-ext2-clean-mark.md) | DESIGN D17's design note: the ext2 clean mark while a volume is in use and the check of a marked root at boot (audit 2026-10-26 F5, F6). Revision 2 after two reviews; built. |
| [d19-network.md](d19-network.md) | DESIGN D19's design note: the network (netd with lwIP, driver processes, SLIP, BSD sockets as files, DHCP/DNS, random numbers, zfsd out of the kernel image to make room). Revision 2 after two reviews, the owner's answers; steps 0-5 built (section 14, "As built"). |
<<<<<<< HEAD
| [d20-telnet.md](d20-telnet.md) | DESIGN D20's design note: telnet for BBSes (the protocol, PC/Amiga/Unix modes, a per-console CP437 font and board rules, Zmodem by `rz`/`sz`) and the later pseudo-terminals, SIGHUP and telnet server. Revision 2 after four reviews; not built. |
=======
| [a1200-checklist-network.md](a1200-checklist-network.md) | The owner's checklist for the CM4's Ethernet on the PiStorm A1200 (D19 step 9): what to type, what the screen should say, the Emu68 cache check, the speed table. |
>>>>>>> n9
| [network-serial.md](network-serial.md) | How to connect an Amiga (the stock A1200 too) to a Linux PC with a null-modem cable: SLIP, `zenix-slirp` on the PC, `/etc/network/interfaces`, `ifup`, `ping`; plain words. |
| [DESIGN.md](DESIGN.md) | The numbered design decisions D1-D13 and where each one stands. |
| [d11-service-manager.md](d11-service-manager.md) | The full design note of DESIGN D11, the service manager (servers started on first use and started again when they die): approved; built: steps 0-5 (the core) merged, steps 6-9 (mounts, `/etc/svctab`, the servers' output in the kernel log) on branch d11; section 15 says where the code differs. |
| [ROADMAP.md](ROADMAP.md) | Vision, principles and the phased plan. |
| [audit-2026-10-22.md](audit-2026-10-22.md) | The audit of 2026-10-22: the work list of the hardening round (batches A-E), each item marked with its fix. |
| [audit-2026-10-26.md](audit-2026-10-26.md) | The focused audit closing roadmap block 1 (D13 to D15): the work list B1-B9 and T1, each marked with its fix. |
| [audit-2026-10-25.md](audit-2026-10-25.md) | The focused audit of 2026-10-25 (everything since the first audit closed, D11 included): the work list A1-A16 and P1, each marked with its fix. |
| [audit-closing-2026-10-24.md](audit-closing-2026-10-24.md) | The closing report of the audit round: what was fixed, what is verified on the real A1200, what is left. |
| [future-ideas.md](future-ideas.md) | Ideas kept for after the feature halt (signal bits, CPU accounting, shared libraries, newlib, PFS3, Zenix as its own ROM, ...), each with its public source. Nothing in it is built. |

How these documents are written: by reading the source tree. A statement about Zenix is *read* from the source unless
it says *verified* (a test checked it in FS-UAE; on real hardware it would say "verified on the A1200" with the date).
Statements about AmigaOS, Linux and other systems are general knowledge, not measured here. Zenix has run on a real
Amiga (the owner's PiStorm A1200) since 2026-10-23; what was checked there is listed in audit-closing-2026-10-24.md.

## Where things live (and the rule)
- All documentation is in `docs/`. The only others are `README.md` (short, points here) and `CLAUDE.md` (the rules for
  whoever works on the project). `archive/README` says what the old code in `archive/` is (not built) and its licence.
- Manual pages: `docs/man/NAME.txt`, one per program, plain text of at most 78 columns (the format `man` reads); the
  root disk gets them in `/usr/man`. Command index: `commands.md`. Features: `features.md`.
- Text that is not documentation stays where it is used: `rootfs/` (files that end up on the Zenix disk),
  `tools/tests/` (scripts typed into the emulator).
- **When a command is added, changed or removed, or a feature appears, the same commit updates its manual page,
  `commands.md` and `features.md`.** One fact lives in one place; other documents point to it.

`tools/docs-check.sh` checks this: every program has a page and a line in `commands.md`, every link there and in the
other documents leads to a file, this index lists every document, and the counts written in the documents ("N
programs", "N system calls", "N emulated setups") match the Makefile's `UAPPS`, `kernel/kernel.h` and the matrix
configs. `--staged` also checks what is about to be committed (a changed program needs its page changed),
`--commit REV` a commit already made.

## Testing
Nothing counts as done until a test checks it. The tests run FS-UAE headless (never on the screen), from scripts typed
into the serial port (`tools/serial-tap.py`: `sleep`, `send` and `wait REGEX [timeout]`, which waits for the output;
after a `send` that ends a line only the output after that line's echo counts, so waiting for the prompt waits for the
next one, not the one the line editor redrew while typing; `waitecho` lets the echo count too; `stop` kills the
emulator at once, a power cut or the end of a script). **A `wait` that times out fails the test** (audit 2026-10-26
F7), and so does a run that ends while a `wait` still waits: the tap prints `FAIL: serial-tap: ...`,
`tools/run-test.sh` exits 3, and `make check` fails the test whatever its script made of the output (the tap writes
the lines to `ZENIX_TIMEOUTS`, which check.sh sets for each job). Where the text not coming is a right answer (a
machine that refuses to start, a login that runs out of memory) the script says so with `waitopt REGEX [timeout]`,
and the test checks what happened instead.
Several FS-UAEs run side by side on one machine as long as each has **its own serial port and its own image files**
(runs that shared a port or an image spoiled each other; that, not FS-UAE, was why only one ran at a time before
2026-10-26). Every emulator run (`tools/run-test.sh`, and the cage rig's through `tools/fsuae_conf.py`) takes one of
`ZENIX_EMU_SLOTS` (default 6) slots, a lock file it holds until its run ends: slot 0 is `/tmp/zenix-emulator.lock`
(`ZENIX_EMU_LOCK`), slot N is `/tmp/zenix-emulator.lock.N`; a run takes the first free one, waits only while all are
taken (or while the machine's 1-minute load is above 16), never for another FS-UAE. Another project on the same PC
takes slot 0 with `flock /tmp/zenix-emulator.lock COMMAND` and keeps one place in the pool. Each run also gets its own
folder for FS-UAE's save states (a floppy's writes are kept there under the config's name, which runs share).

**The gate: `make check`** (also `make test`, `tools/check.sh`). It builds every image once and keeps a copy, runs
the host tests (seconds), then the emulator tests side by side, up to `ZENIX_EMU_SLOTS` (6) at once and the longest
first (`svc-test` and `matrix` as three and two jobs, each with some of their parts), each with its own serial port,
output directory and copy of the build directory (`ZENIX_BUILD`: the tests write to their disks, so none shares an
image file), and prints a table in the order of the list (test, PASS/FAIL, time, details; the logs stay in
`/tmp/zenix-check.*`, a failed test's images too). It fails if any test fails. A full run took 26 minutes on
2026-10-26 (the tests' own times add up to 2 h 44 min). Every test script reads its build directory from
`ZENIX_BUILD` (default `build/`) and its output directory from `ZENIX_OUT`, so it can run beside others.
`tools/check.sh host` runs only the host tests, `tools/check.sh NAME...` only the named ones (e.g. `matrix users-test`).
Not run by it: `stage0-68k-test` when the Python module `unicorn` is missing (shown as skipped).

| Test | What it checks |
|---|---|
| docs-check | the documentation rules above |
| ext2-host-test, disk-host-test | the ext2 server, `fsck.ext2`, `mkfs.ext2`, `fdisk` and `diskd` built for the PC, against image files and `e2fsck`, with a fuzz loop; the clean mark (DESIGN D17): a workload cut after each block write never leaves something on a volume that says clean, every block write comes after the mark, `fsck.ext2 -y` repairs each cut in one run and finishes a repair that was itself cut |
| zup-host-test | the updates (DESIGN D15) on the PC: the packages and the bundle of the build (`tools/mkzup.py`), every kind of damaged bundle refused; `zup` built for the PC: refusals before any change, a power cut before each change, the three-way compare of config files, the way back; `zup` on the real ext2 server with a power cut after each block write (at worst leaks, `tools/e2safe.py`; each repaired by one `fsck.ext2 -y`) |
| rootcheck-test | the ext2 clean mark and the check at boot (DESIGN D17): leaks on a marked root repaired at boot, the emulator killed while writing leaves the root "not clean" and the next start repairs it, killed after a quiet second or after `reboot` it is clean, a marked `/home` checked by `mount -a`; a root fsck.ext2 cannot repair: the four choices (read-only by default after 30 s, read-write, a rescue shell as root, reboot; man/rescue.txt) |
| zenixfs-test | `make -C zenixfs test`: the ZenixFS library on the PC (random operations, power cuts, damaged blocks) |
| mfm-test, emmc-test, genet-test, serial-test, term-test, mem-test, a3000-test, stage0-test, stage0-68k-test | the floppy track coding, the SD card, the serial copy of the console and `/dev/ttyS0`, the terminal's escape parser and colours (`kernel/term.c` against a model of the display), the kernel allocator around 1 GB (`MEM_LOW1G`), A3000 SCSI and Pi device-tree code against models; genet-test: the CM4's Ethernet driver's chip code (`net/drv/genet-core.c`) against a model of GENET v5, its rings and DMA, the BCM54213PE and the cache (`tools/genet-model.h`): bring-up, link changes, 3,000 frames with the indexes wrapping, a slow netd, bad frames, a stuck DMA, the quiesce program, the self-test with working and broken cache calls |
| tap-test | the serial tap's `wait` (`tools/serial-tap.py`, against a fake shell that redraws its prompt with each key): after a typed line it waits for the next prompt, not the one in the echo; `waitecho` and a half-typed line still match the echo; a `wait` that times out or is cut short by the end of the run is a FAIL line (and in `ZENIX_TIMEOUTS`), a `waitopt` that times out is not |
| usize-test | the frozen first sizes of the structs the kernel writes into a program (a size-0 caller gets each one's first size, also when the struct grows; every system call passes its frozen constant) |
| mkzexe-test | the ZEX2 program format of shared code (DESIGN D13): `tools/mkzexe.py` classifies each relocation by its target (a `.data` pointer into constants to code, a table slot of a BSS array to data) and refuses a reference from the shared code into data, a value equal to `text_size`, an undefined weak and an absolute symbol |
| matrix | boots every emulated machine (architecture.md section 13), a short session on each (including a stack overflow the guard must catch) |
| cmds-test, cmds2-test | the commands and the shell, output compared with GNU bash and tools; `e2fsck` after the run |
| users-test, zroot-test | users and permissions, the consoles; zroot-test the same on a ZenixFS root (with the test-only kernel kernel-zfs, DESIGN D19) |
| jobs-test, kbd-test | job control and history search, long typed lines (kept whole up to the limits, refused with a message past them); the keyboard (keymaps, repeat, consoles, scrollback) |
| signal-host-test | DESIGN D16's core on the PC (`kernel/event.c`, `kernel/signal.c` against a model of the scheduler): the wake rule, stale waiter bits, every interleaving of a producer and a waiter (no lost wake-up), sending and the defaults, groups, the frame, SA_RESTART, sigreturn's checks (a forged status register, an odd PC, a frame outside the process refused before it is read) |
| signal-test | events and signals (DESIGN D16) in FS-UAE: `sigtest`'s checks (docs/man/sigtest.txt; handlers, masks, restarts, alarm in a CPU-bound program, SIGCHLD, SIGPIPE, poll and select on the tty, a pipe and a port, groups, a stopped server, the stack room, forged frames), again as a program with shared code; the shell's job control with groups (^C to a pipeline, ^Z/bg/fg with handlers, kill %job), less and edit stopped and brought back |
| cu-test | `cu` and `/dev/ttyS0` (DESIGN D12): the other computer's line reaches cu, keys reach the port, `~.` ends it, the console's copy comes back |
| prio-test | priorities (D10): nice/renice for a user and for root, PRI/NI in `ps`, `priotest` (equal priorities take turns per slice, a higher one wins, wake-up, priority inheritance; a kernel built without inheritance must fail that check) |
| diag-test | the boot colours, the panic screens, the previous-boot log across a warm reset; the stack guard's kernel-stack checks (a `STACK_TEST` build) |
| colour-test | the colour console (DESIGN D14) by screenshots at 1, 2, 3 and 4 bitplanes, and `/etc/console`'s setting switching the display on the A1200 without fast RAM (4 and 16 colours, a wrong value refused): `termtest columns` (all 80 columns, the edges where one plane has them: FMODE 3's fetch window), the 16 colours against the palette, a screen after `termtest scroll` (blitter scrolls, region moves, background-colour erases, the copy-back) pixel-identical to a redraw from the grid, the scrollback keeping colours |
| floppy-test | two floppy drives: whole-disk reads, a write and its read-back, the image file on the host afterwards |
| memtest | the allocator, deep directory walks, corrupt program files refused |
| mounttest, zenixfstest | mounting ext2 and ZenixFS, `fsck`, nested mounts; zfsd started from `/sbin/zfsd`, again after a kill, and through the old name `@zfsd` |
| edit-regress, disktools, mbr-test | the editor; `fdisk`/`mkfs`/`fsck` on scratch disks; PC partition tables |
| zenixboot-test | booting with no floppy: Kickstart boots the FFS partition, zenixboot starts Zenix on its IDE root; zenixboot's error paths; Kickstart 3.2 and 2.05 only when `ZENIX_KICK32` / `ZENIX_KICK205` name those ROMs (else SKIP) |
| update-test | Zenix updating itself (DESIGN D15) in FS-UAE: zenixboot installs a bundle's kernel (the FFS steps in their order, flushed), zup the root half (the owner's passwd kept), the memory given back, shells working; the confirmation and `.done`; the bundle offered again; a cold start in the trial; a panic and a hang then the question and both halves back; PREVIOUS after a cut; a power cut during zup; damaged bundles |
| pure-test | shared program code (DESIGN D13): one copy for one, two and four processes (`free`, `ps`), the first ending and the last freeing it, two started at once, a crash in one, literals in system calls (a read into one refused), text file busy, an update renamed over a running program, a private copy of a file open for writing, a new generation after a server restart and after a remount, a damaged copy found and not used again, corrupt ZEX2 files refused; `sh` and `login` shared by four consoles' sessions and by `sh -c` shells, a session ending while the others run |
| random-host-test, random-test | the random devices (DESIGN D19 9.3): BLAKE2s and ChaCha20 against the RFCs' vectors, the crediting and seed-file rules, mkroot and mkzup refusing a seed (on the PC); in FS-UAE a fresh image's `/dev/random` waiting until keys are typed on the emulated keyboard, the seed file rewritten and credited at the next boot, two boots' bytes different, a RAM-disk root never crediting its seed |
| sock-host-test, sock-test | the kernel's socket layer (DESIGN D19 step 3): on the PC the rules of 4.3 in a model (a push at every point of the wait never lost, a stale bit costs one call, the server's end waking every waiter, -ENETDOWN never a peer's reset, NET_CLOSE as a no-start call, partial sends, accept's reservations, a restarted connect); in FS-UAE `socktest`'s checks against the test server `socktestd` (docs/man/socktest.txt) and ^C on a blocked recv |
| netd-test, slip-test | the network (DESIGN D19 steps 4 and 5) in FS-UAE: netd over the loopback with `nettest`'s checks (TCP and UDP echo, one big send, poll, `O_NONBLOCK`, names, the profiles' limits, the root-only ports, `SA_RESTART`, partial sends and SIGPIPE against netd, the assembler against C), `netstat`, ^C on a waiting accept, netd killed and started again, five kills (given up, root's `ifup` brings it back); then SLIP: Zenix's serial port on `zenix-slirp` (in a user and network namespace with test servers; it paces the pty to 115,200 baud), `ifup` at boot, `ifconfig`, `route`, `ping 10.0.2.2`, TCP, 100 KB checked, UDP, DNS through 10.0.2.3, `ifdown`, the memory on 2 MB and 1 MB of chip RAM; noise on the line (damaged frames counted, the 100 KB still whole) and a SYN flood from the PC (the pool limits hold, the machine stays responsive). slip-test needs libslirp and unprivileged user namespaces, as net-test |
| drv-test | the network's driver protocol (DESIGN D19 step 7) in FS-UAE with the A2065 board present and the test driver `fake` (answers ARP and ICMP itself): ping through a driver and through a DMA driver, the kernel's refusals (`SYS_drv` for netd's children only, a second claim, a quiesce step outside the window, cache calls outside the DMA region), wrong buffer numbers refused by netd, a driver killed mid-ping and started again, the DMA driver's claim released at its end, five ends (given up, `ifup` brings it back), netd killed (every claim released, the drivers end) |
| eth-test | Ethernet (DESIGN D19 step 8) in FS-UAE: the A2065 board (`network_card = a2065`, FS-UAE's own slirp) and its driver, in a user and network namespace with the PC's test servers: DHCP (address, route, name server), ping 10.0.2.2, TCP echo, 100 KB checked, UDP, names through DHCP's name server, the driver killed and started again (DHCP again), `ifdown`; part g: the CM4's Ethernet driver `genet` where there is no GENET (the emulator): it says so, touches nothing, netd gives it up after 5 tries and keeps serving (D19 step 9). Needs unprivileged user namespaces |
| svc-test | the service manager (DESIGN D11): boot's pids, restarts of diskd, fsd and zfsd, the bootable root, no root, the RAM floppy, diskd's saved table, open files across a restart, a dd copy, limits and back-off, the deadline, a call whose start fails (stop, kill, the loader's priority), reboot with a dead server and with one that hangs, a damaged table (a test build) |

What the tests need: FS-UAE, and the Kickstart ROM files in the directory named by `ZENIX_KICKSTART_DIR` (default
FS-UAE's own Kickstarts folder, `Documents/FS-UAE/Kickstarts` in your home directory); the configs in `fsuae/` name no path of their own (`tools/fsuae_conf.py`
fills it in, with the build directory and the serial port). kbd-test, cu-test, diag-test, colour-test, random-test and the screenshots
(`tools/shot.py`) also need the headless cage rig: `ZENIX_CAGE_RIG` names its directory. A single test
beside others: `ZENIX_SERIAL=/tmp/NAME ZENIX_BUILD=/tmp/NAME-build ZENIX_OUT=/tmp/NAME-out tools/users-test.sh` (its own
serial port, a copy of `build/` and an output folder; without `ZENIX_BUILD` it uses `build/` of the tree, which only
one test at a time may do). `make run` boots a window for a person, never for a test.

Last checked against the code: 2026-10-26.
