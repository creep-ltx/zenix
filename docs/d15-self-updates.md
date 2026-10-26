# D15: Zenix updates itself (design note, revision 2)

Status: **revised after the first review (2026-10-26), awaiting the second.** Approved by the owner on 2026-10-24
("Zenix installing updates by itself, no card swaps"), in the order of work after D14 (ROADMAP). Built on the approved
versioning model (ROADMAP, "Versions and updates") and on D13's safe replacement of running programs. The short version
is in [DESIGN.md](DESIGN.md) D15. Revision 2 takes in every finding of the first review: a crash-safe ext2 rename first
(a bug found in today's code), the rule that the kernel is never older than the root, a rollback that works with older
kernels, no result block, careful FFS writes, the blob's memory given back, frozen kernel-written structs, a three-way
compare for settings files, and the boot-time cost of checking.

## 1. The goal

A new version of Zenix reaches the machine, installs itself, keeps the previous version, and the owner can boot back
to the previous one. No SD card leaves the A1200, no command runs on the PC as root, and the owner's settings
(passwords, fstab, ...) survive every update.

## 2. The idea: one bundle, two halves, one order rule

The kernel and the boot files live on BOOT: (AmigaDOS FFS), which Zenix cannot write; everything else on Zenix's ext2
root. Each half is written by the only program that can reach it:

1. **The bundle arrives on BOOT:** as `BOOT:zenix-update.zup`. Today it is copied from AmigaOS (the owner, or Wasabi
   with the owner's OK), always as `zenix-update.zup.part` and then renamed, so a half-copied file is never seen.
2. **zenixboot (in AmigaOS) does the boot half first:** it checks the bundle, installs the new kernel (section 6), and
   loads the root packages into memory for Zenix, in **their own bootinfo fields** (section 7).
3. **Zenix does the root half at boot:** `boot` (inside the kernel image) runs `/sbin/zup` before `/bin/init`; zup
   applies the packages transactionally (section 5). If zup crashes, `boot` carries on; the journal recovers at the next
   start.

**The order rule: the kernel is never older than the root's base.** The boot half always goes first, so a new kernel
may meet old programs, which the frozen-interface rules (section 4) make safe. A failed or rolled-back root half leaves
"new kernel, old root": legal, so nothing on BOOT: needs undoing. Base is never installed while the system runs (a new
`init` or `sh` on an older running kernel would break the rule); it waits for the next start.

**Re-applying is harmless:** zup sees "version V already installed" and does nothing. So no result needs to survive a
power cut (section 8).

## 3. The bundle (`.zup`)

- A header (magic `ZUP1`, format version, the bundle's version and date, the channel, the minimum installed version it
  applies to), a table of contents, then the parts: `boot/` (the kernel and zenixboot; **never bootmenu**, section 6)
  and `pkg/` (packages for the root).
- Every part carries its size and SHA-256; the table of contents its own. zenixboot parses the bundle while AmigaOS
  runs, so **every count, size and offset is range-checked** (CLAUDE.md: untrusted input).
- **Signatures (build step 7):** Ed25519 over the table of contents, two public keys built in (current and next, so a
  key can be rotated by a signed bundle), checked **once per transport**: by zenixboot for BOOT:, by `zpkg` for the
  network. **Until then, said plainly:** whoever can write BOOT: can already replace the kernel; a bundle opens no new
  hole, and checksums catch damage, not a deliberate fake.
- The private key lives on the owner's PC outside every repository (e.g. `~/.config/zenix-signing/`, mode 600), with
  an explicit backup and an offline copy (the NAS backup covers only the projects folder). A lost or stolen key is
  replaced by the trusted manual path: a new zenixboot copied to BOOT: through Wasabi.
- Size: about 600 KB for a full bundle; on a 2 MB chip-only A1200 the kernel home, the kernel file and the blob fit
  with a ceiling of about 1.1-1.2 MB per bundle (read from the code, not measured).

## 4. Packages, base, and what must never change

- **A package** = `tar` + `gzip` plus a manifest: name, version, files with SHA-256, owner and mode, dependencies
  (a minimum base version, other packages), and a **config** flag on files the owner may edit.
- **base** = the kernel, `boot`'s chain (built into the kernel image), `init`, `login`, `sh`, `mount`, `/sbin/zup`, the
  shipped service table. One version for the group.
- **Frozen interfaces** (written into CLAUDE.md as a rule, checked in every review):
  - system call numbers and their argument meanings; error numbers; message types and their argument meanings
    (fsproto.h, diskproto.h): append-only, never reused or changed;
  - **every struct the kernel or a server writes into a program's memory** (`ustat`, `uproc`, `ustatfs`, `udirent`,
    `umountinfo`, disk info, the `SYS_meminfo` words): frozen, or the caller passes its buffer size and gets at most
    that many bytes. *(Found by the review: D13 grew `struct uproc` by 4 bytes, so `SYS_procinfo` writes 44 bytes into an
    older `ps`'s 40-byte buffer. Fixed next, before D15: the size-passing calls.)*
  - the program start contract (the stack at entry, `a5`), the ZEXE and ZEX2 formats (the loader keeps both for ever);
  - the bootinfo layout and the klog/boot-note layout (zenixboot and bootmenu against the kernel);
  - the `/etc` formats base programs read (svctab, fstab, passwd, group).
  libz is linked into each program, so it needs no rule of its own.
- **zup itself stays a v1 ZEXE** using only calls and structs that existed when D15 shipped, so it runs on every
  D15-aware kernel (section 9).
- **The database** (`/var/lib/zpkg/`): plain text, one manifest per installed package.

## 5. Installing safely (the root half)

- **Before D15: ext2 rename-over made crash-safe** (a bug in today's code, found by the review): `ext2_rename` today
  unlinks the target first, so a power cut can lose `/bin/sh`, or leave two names sharing one inode that a later
  cleanup frees. Fixed the Linux way: raise the moving inode's link count, rewrite the target entry's inode in place
  (one block write), remove the source entry, lower the count, drop the old target inode. Tested by killing the server
  after each block write and running `e2fsck -fn` (at worst a leaked inode, never a missing or shared one). ZenixFS's
  rename is checked the same way.
- **The journal** (`/var/lib/zpkg/journal`), written and synced before anything changes: the files the update replaces,
  adds and removes. The previous version of each replaced or removed file is kept by **hard link** in
  `/var/cache/zpkg/prev/` (no copying; `FS_LINK` exists), so the space check counts only the new files and the journal.
- **Each new file:** written as `path.zup-new`, checked against its SHA-256, owner set first and then the mode
  (`ext2_chown` clears set-user-id), then **renamed over** the old one. D13: a rename never meets `ETXTBSY`, and running
  programs keep their code.
- **Config files (three-way compare):** if the current file's SHA equals the SHA in the installed manifest (the owner
  never changed it), it is replaced; otherwise the new one is written as `path.new` and the owner is told. A base change
  that needs a new config line must never depend on the owner merging a `.new`.
- **The volume is marked "needs a check"** (`ext2_mark_unclean`) during the install, and clean again at the commit.
- **A power cut:** at the next boot zup finds the journal and finishes (every new file complete and checked) or rolls
  back (the hard links put back). When both names point to the same inode, it deletes nothing. The previous `prev/` set
  is deleted only after the new install has committed.
- **Sync** once after the journal and once after the commit record (writes are already write-through).
- **The result is shown where the owner sees it:** init or login prints the one-line result after the motd until it has
  been seen (on console 1 it would otherwise scroll away).

## 6. The boot half (zenixboot), written carefully

- **FFS keeps metadata in its buffers:** after each write step zenixboot sends `ACTION_FLUSH` to BOOT:'s handler (DoPkt,
  AmigaDOS Manual 3rd ed., packet 27) and waits for the reply, and again before switching AmigaOS off. *(The knowledge
  base has a verified case of a closed file's header still in buffers.)*
- **The order, so a valid kernel is always findable:** write `zenix.new` from the bundle; read it back and check its
  SHA; flush; delete `zenix.old`; rename `zenix` -> `zenix.old`, flush; rename `zenix.new` -> `zenix`, flush. Free space on
  BOOT: is checked first.
- **The start rule:** try `zenix`; if it is missing or bad, a checked `zenix.new`, then `zenix.old`; the one used is
  named on screen and serial.
- **A bad bundle** is renamed `.bad`, the reason printed, and the boot carries on with the installed system.
- **zenixboot updates itself only** after running `zenixboot.new CHECK` successfully; **CHECK never writes**; a start
  with `RAM=` ignores any bundle (a RAM root would lose the root half).
- **bootmenu is never updated by a bundle:** it runs first on BOOT:, and a broken one would also block AmigaOS (only
  Kickstart's Early Startup menu would be left). It is updated by hand, with the owner, when needed.
- **A small state file on BOOT:** (`zenix-update.state`): the version being tried and a try counter (section 8).

## 7. The kernel's part, and the memory

- **The bundle has its own bootinfo fields** (+0x78 address, +0x7C size, +0x80 flags, inside the 1 KB table below
  hwinfo at +0x100), **not** the RAM-disk slot: an older kernel or diskd/rootpick would otherwise mount the bundle as the
  root. Old kernels ignore the new fields safely. `SYS_blk_ram` never returns the bundle.
- The kernel keeps the bundle's memory out of the pools (as the RAM disk's) and offers it read-only to `boot`/zup;
  when zup is done, **the range goes back to the memory pool** (`add_range`) and the fields are cleared. Without that a
  2 MB A1200 would lose about 600 KB until the next reset.
- zenixboot loads the bundle into **fast RAM first** (today's `alloc_safe` tries chip first; changed for the bundle).
- The flags carry "roll back" when the owner chose "Zenix (previous)".

## 8. Failures, and the way back

| Case | What happens |
|---|---|
| Bundle copy cut off | `.part` name or bad checksum: renamed `.bad`, said on screen and serial, the installed system boots |
| zenixboot cut off mid-install | the section 6 order and start rule: always a valid kernel, named on screen; BOOT: may need validation (FFS's validator, harmless) |
| Power cut during the root half | the journal plus the crash-safe rename: finished or rolled back at the next start |
| The new kernel panics | warm reset: zenixboot reads the klog header (reason PANIC; it already reserves that area) and offers "previous" as the default |
| The new kernel hangs, then a power cycle | the try counter on BOOT:: the first unconfirmed start offers "previous" with "new" as default, the second defaults to "previous" with rollback |
| The root half fails | zup rolls the root back and says so; new kernel with old root is legal; the try counter stops retrying after two |

- **An update note** of 16 bytes next to the boot note in the klog header: Zenix sets "V done" or "V failed". It
  survives warm resets and `reboot` (verified on the A1200 for the boot note, 2026-10-24); bootmenu leaves it alone when
  it clears its own note. zenixboot reads it at the next start, retires the bundle (renames it `.done`) and resets the
  counter. If a power cycle loses the note, the same bundle is offered again and zup does nothing (already installed).
- **"Zenix (previous)":** a line in the owner's `S:BootMenu.conf`, `Zenix (previous) = run SYS:zenixboot PREVIOUS`,
  added by hand in the owner's first D15 card package. zenixboot then uses `zenix.old`, sets the rollback flag, renames
  the bundle `.rejected`, and makes the old kernel the default again (`zenix` <- old, the rejected one to
  `zenix.rejected`); after that there is no "previous" until the next update. zup, run by that kernel's `boot`, decides
  **from the root's own database** whether anything needs rolling back, so it is safe whether or not the root half ran.
- **Said openly:** the way back works only to a kernel that is itself D15-aware. The first D15 update has no menu way
  back; the way back then is the card on the PC, as today.

## 9. Time on a stock 68020

SHA-256 costs about 1 ms per 64-byte block on a 68020 (the project's own estimate, user/sha.c), so hashing a 600 KB
bundle is about 10 s and re-checking 1.2 MB of files about 20 s; Ed25519 without a 64-bit multiply (the 68060 rule)
goes through libgcc and may take tens of seconds (to be measured before choosing an implementation). So: everything is
checked **only when an update is applied**, never at a normal start (zenixboot remembers what it applied in its state
file instead of hashing `zenix` every time); the signature is checked once per transport.

## 10. Later transports (same bundle, same zup)

- **Over the serial cable** (from 2026-10-31): `zpkg` receives a bundle with a checked transfer from the PC and keeps it
  on the root; non-base packages apply at once (running programs keep their code), base waits for the next start, where
  the boot half needs the bundle on BOOT: (the PC side also puts it there through Wasabi; a tiny read-only ext2 reader
  in zenixboot would need MBR/RDB parsing and Emu68's whole-card unit: not planned). Designed in detail when the cable
  works.
- **Over the network** (block 2): `zpkg -Syu` from static files on GitHub (the owner's NAS as the testing channel).
  Non-base packages live (with a warning to logged-in users), base at the next start.

## 11. Tests

- **Host:** the crash-safe rename (kill after each block write, `e2fsck -fn`); `mkzup.py` and zup's logic built for the
  PC: a wrong checksum, a truncated part, an older version, a missing dependency, all refused before any change; the
  journal finished or rolled back at every point of a simulated power cut; the three-way compare; "already installed"
  does nothing.
- **Emulator:** a bundle on the zenixboot-test disk: the kernel replaced with `.old` kept and the right order of FFS
  writes; the root packages installed with the owner's changed `/etc/passwd` kept and `passwd.new` written; the update
  note read at the next start and the bundle renamed `.done`; `PREVIOUS` rolls back both; a kernel that panics and one
  that hangs lead back as in section 8; a power cut (the emulator killed) during the root half; running shells keep
  working; the bundle's memory is back in the pool afterwards (`free`).
- The full suite after each step.

## 12. Order of work

0. Before D15 (found bugs, fixed first): the crash-safe ext2 rename (and ZenixFS checked); the size-passing calls for
   every kernel-written struct; the frozen-interface rule in CLAUDE.md.
1. The bundle format, `tools/mkzup.py`, packages from the build; host tests.
2. zup (journal, hard-link previous versions, rename-over, three-way compare, unclean mark, rollback, already-installed);
   host tests.
3. The kernel: the bundle's bootinfo fields, read-only access for `boot`/zup, the memory given back; `boot` runs zup.
4. zenixboot: check, the careful FFS order with flushes, the start rule, `.part`/`.bad`/`.done`/`.rejected`, the state
   file and try counter, the update note, `PREVIOUS`; fast RAM first.
5. The emulator tests.
6. Docs; the owner's first D15 card package (with the "previous" menu line), with the owner.
7. Signatures (Ed25519, a small public-domain implementation checked against its test vectors; measured on a 68020).

## 13. The first review's answers to the open questions

1. Result channel: none needed to survive a power cut; the update note in the klog header plus re-applying being
   harmless (section 8). The ext2 boot area and the memory copy's header were rejected (unwritable, or reused at reset).
2. Separate bootinfo fields, not a kind word in the RAM-disk slot (section 7).
3. Logged-in users: zup runs at boot, before any login; later, non-base live with a warning, base never live.
4. The key: section 3.

Last checked against the code: 2026-10-26.
