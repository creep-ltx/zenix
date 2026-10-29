# Owner decisions and approvals (the history)

<!-- Moved out of CLAUDE.md on 2026-10-28, word for word (CLAUDE.md points here). -->

Every feature the owner approved, in the order they came, with their conditions. CLAUDE.md "What is approved" names the rule; this file is the record. New approvals are added here, at the end of the list below, in the same style.

## The approvals, from the feature halt of 2026-10-22 on
**FEATURE HALT (decided by the owner, 2026-10-22): no new features - no new commands, system calls, drivers,
options or programs.** Only fixes from `docs/audit-2026-10-22.md`, tests that check them, and documentation
corrections. Merging work that was already built before the halt (the `drivers`, `zenixfs` and `floppy` branches) is
allowed. **Approved exceptions:** booting without a floppy (`zenixboot`, owner 2026-10-23); DESIGN D10 priorities (owner
2026-10-24), to start only after the audit fixing is done and the closing audit report is written.
The boot menu and the dual-boot SD card (owner 2026-10-24: a small boot partition with a menu, the owner's AmigaOS
on PFS3 partitions, Zenix root and /home); the menu program lives with `zenixboot` in `boot/`.
Rebooting between the systems (owner 2026-10-24): `bootzenix` in AmigaOS, `reboot [amigaos]` / `bootamigaos` in Zenix,
through a one-shot note the boot menu reads. Later (not now): a Wasabi-like remote-access daemon for Zenix (needs the network driver).
DESIGN D11 service manager (owner 2026-10-24: design reviewed twice and approved, docs/d11-service-manager.md).
`awk` (owner 2026-10-24: finish the parked `user/awk.c` now, while D11 is built).
**`useradd` and `userdel` approved (owner 2026-10-26):** small commands for root (ideas list), built at once.
**`sudo` approved (owner 2026-10-26, "with users added we also need a sudo command"):** the group wheel, own password, with `gpasswd` and `useradd -G`.
**Rescue when a boot repair fails approved (owner 2026-10-26):** after D16: a root that fsck.ext2 cannot repair is mounted read-only and boot offers 1 continue read-only (default after 30 s), 2 read-write anyway, 3 rescue shell with the repair commands listed, 4 reboot; `man rescue`. As AmigaOS does with a disk that cannot be validated.
**The 2 MB diet approved (owner 2026-10-26):** after D16: without fast RAM the console defaults to 8 colours (3 planes; 16 with fast RAM); a setting (2/4/8/16 colours, at the next boot, as Workbench's ScreenMode prefs) lets the user choose; the disk cache sized by the machine's memory; login steps aside for the shell. Measured 2026-10-26: Workbench 3.1 on a 2 MB chip-only A1200 leaves 1,737 KB free, Zenix with one login 1,126 KB. Halving the double-height screen buffer: decided after a real-68020 scroll test.
**The network (DESIGN D19) approved with all seven recommendations (owner 2026-10-27, "Yes"):** the stack and SLIP over the serial cable first, the CM4 Ethernet later; an A2065 driver early (FS-UAE tests the Ethernet path); SLIP may take the stock A1200's only serial port; `zenix-slirp` an ordinary Linux program in the repo; a ZenixFS root test-only until block 4; licence notices shipped in every image and bundle; lwIP 2.2 as the TCP/IP code.
**Telnet (DESIGN D20) approved with all seven recommendations (owner 2026-10-27, "Yes to all"):** the client first, a telnet server later (home network, off by default); 8 colours stay the default on the stock A1200; the board is told 80x32; a phone book later; Zmodem only; Amiga-board mode by default, `-p` for PC boards; Topaz only (no shipped Amiga fonts).
**Crypto, ssh and HTTPS (DESIGN D21) approved with all eight recommendations (owner 2026-10-27, "Yes"):** TLS 1.2 now, 1.3 only if a site needs it; the Amiga gets its own ssh key; no key passphrase by default (optional); the clock set from the internet when the network comes up (the battery clock first); HTTPS may take up to a minute on the stock A1200; `zpkg` over plain HTTP from the NAS with signed packages (HTTPS optional); unsigned update bundles refused once signing ships (test builds excepted); `scp`/`sftp` after ssh is in daily use.
**zpkg (DESIGN D22) approved (owner 2026-10-27: "I let you decide those things, but decide it good and professional"; the main session took all seven recommendations):** system updates through one slot file that `zenixboot MKSLOT` makes on the AmigaOS boot partition, its data blocks overwritten in place and nothing else there ever written; packages by family (about 40); the signed list expires 60 days after signing, the PC re-signs weekly; stable from GitHub Pages over HTTPS, testing from the NAS over plain HTTP; `zpkg -Syu` again after a system update restart (automatic later); no install scripts; the previous version of each package kept for `zpkg --back`.
**NFS (DESIGN D23) and the remote control (DESIGN D24) approved with their recommendations (owner 2026-10-28, "Yes"):** NFS: programs wait while the NAS is off (Ctrl-C or kill -9 stop them); a Synology folder `zenix` exported only to the Amiga's address, Zenix users mapped to the owner's NAS user. Remote control: Zenix speaks the owner's Wasabi protocol (version 1) and Wasabi gets the small additions D24 section 11 lists; unencrypted for now (its own key and port, home network only, off by default; an exception to "the A1200 only encrypted" while it runs Zenix), encryption later; later, a panic during an unattended test resets into AmigaOS after 30 s. **The demo floppy (DESIGN D25) is postponed (owner 2026-10-28, "skip demo floppy for now"):** its design stays; nothing of it is built until the owner says; its questions stay open. **The C library (DESIGN D27) and the console's UTF-8 and fonts (DESIGN D28) approved (owner 2026-10-28, "yes for all questions"):** picolibc as the C library (the main session's choice, for D27 3.3's reasons); Ahiru and the CFile core published in Zenix's tree under MIT, the owner's programs one source with a copy in Zenix; UTF-8 everywhere like Linux with Latin-1 text still readable, other alphabets as a box for now. Both are block 3, built after block 2's audit. **Wasabi 0.11b3 (the client's additions for Zenix, D24 section 11) pushed to Wasabi's public main with the owner's yes (2026-10-28), no release.** **Sumi as a command (main session, 2026-10-29, under the owner's yes above to "your own programs ... Zenix carries a copy"):** `/bin/sumi` in UAPPS, built from the pinned copy in `ports/sumi` (D27's "Sumi on Zenix").
**Colour console approved (owner 2026-10-25):** colours in the text console (ANSI colours, for `ls`, `grep`, the prompt,
`less`); a design note first (a DESIGN number, reviewed), then built. **The shell's memory** (a fix, audit 2026-10-25
'found while fixing'): sh grabs a fixed arena of 64-256 KB per shell and parses a whole script at once; make it grow on
demand, give memory back, and read scripts a piece at a time.
**The order of work is approved (owner 2026-10-25): docs/ROADMAP.md "The order of work" - always the next item when
one is done; and the versioning model there ("Versions and updates").**
**Pace (owner 2026-10-25):** steady progress every day, done properly, never hasty: "an absolute epic operating system
for the Amiga". Every day ends with something finished and merged, and a short status for the owner. Care matches risk:
two independent design reviews for deep core changes (kernel, loader, updates), one for features; every change has tests;
every merge gets the full suite; a focused audit after each big block, its findings fixed at once.
**The week of 2026-10-26 (owner at work until Friday 2026-10-30; the order, not the speed: when a step is done the next starts at once):** Sun D13 merged, shell memory fix started; Mon shell
fix merged, colour console designed; Tue colour console built; Wed self-updates design (two reviews); Thu self-updates
building starts, and the owner's Friday package ready (card update, DoM image, checklist, cable and cu instructions).
**Order after D13 (owner 2026-10-25: "fix things when we find them"):** first the shell's memory fix, then the
self-updates design note (then build), the colour console, then the network driver. Rule from then on: a problem found
during other work is fixed next (or now if small), not parked behind new features; the owner's weekend 2026-10-30/31: card update, serial cable and `cu`,
the DoM on the stock A1200 and the Apollo 060 (D13 measurements).
Next approved, in this order after D11 (owner 2026-10-24): **D13 shared libraries and shared program code** (one copy in
memory of libz and of a program several processes run, as AmigaOS libraries and pure programs; design note first,
reviewed, before code; it comes before the self-updates so the updater ships the final program format), then Zenix
installing updates by itself (no card swaps), then the network driver.
**Standing order (owner 2026-10-24, away from home):** when D11 is merged, first a **focused audit** of everything
changed since the audit closed (D10, the boot menu and reboot, D12/cu, awk, G1/G3/G4, D11): one independent reviewer,
findings fixed, tested and merged; then start D13 without waiting for the owner:
review the design note (an independent review, fixes, a second look), then build stage 1 step by step in FS-UAE. The
A1200 measurements (note section 8) wait until the owner is home; nothing is written to the owner's card meanwhile.
**The owner's own goal (2026-10-24): Claude in a terminal on Zenix on the A1200.** It sets the order of the work, but it
is the owner's goal, not Zenix's purpose (owner, 2026-10-24): never present it as Zenix's main goal in the docs. Approved now: a serial terminal program (Zenix to the PC over the null-modem cable, from
the weekend of 2026-10-31); then the same over the network; then a console good enough for Claude Code's screen
(colours, stand-ins for its Unicode symbols).
Only the owner lifts the halt; a session never decides on its own that it is over. Unfinished work stays
parked. Order of the fixes: batches A and B, then the first PiStorm boot, then C,
D and E.
1. **Batch A** - first-boot blockers (cache flushes, `cpusha`, `-m68020-60`, keyboard timing, Amiga Swedish keymap,
   the RAM-disk image, bootinfo/Gayle/floppy motor). Without these the PiStorm boot fails.
2. **Batch B** - memory foundations (libz `malloc`/`free`, `mem_alloc` overflow, interrupts only queue input, the
   lost wakeup, stack-hungry tools, the ZEXE loader checks).
3. Then the first boot on the PiStorm with `build/zenix-ram.adf`, following the checklist in the audit (Part 4).
**Text console = Unix/Linux style (owner, 2026-10-23):** ten virtual consoles on Amiga+F1..F10, each with its own login
prompt; no bar, no Amiga+Return, no workspaces outside the future desktop. Never add desktop features to the text console.
Mark each fixed item in the audit file in the same commit as the fix. When a batch is done, say so in ROADMAP.md.
After the first real boot, the next goals are: a terminal the owner can live in on the real machine, then ZenixFS as
root, then the desktop (ROADMAP phases).
**Amiga demos on Zenix approved and moved up (owner 2026-10-29, "I would like to be able to watch an Amiga demo in Zenix. That would be amazing. So yes, please move it up"):** ROADMAP block 3½, right after block 3's audit; a design note first, reviewed twice; bare-hardware demos first, demos needing AmigaOS's libraries later.
**The demo command is called `amiga` (owner 2026-10-29, asked "which one is the best option", the main session recommended it):** `amiga FILE.adf` boots a disk image, `amiga PROGRAM` runs an AmigaOS program file; Zenix also hands an AmigaOS program typed by name to `amiga` (its file magic), as Linux does with Wine; Ahiru later calls the same command.
**The music player, the IRC client and the e-mail client postponed (owner 2026-10-29: "Music player, IRC, email, can wait"):** out of block 3; block 3 is now the C library, UTF-8, the owner's tools (Sumi, Maki, Markdown, Ahiru, `ctop`) and its audit. Their new place in the order is the owner's to set.
