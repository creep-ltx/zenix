# D15: Zenix updates itself (design note, revision 1)

Status: **proposed, awaiting two independent reviews** (deep core: it replaces the kernel and the system). Approved by
the owner on 2026-10-24 ("Zenix installing updates by itself, no card swaps"), in the order of work after D14
(ROADMAP). Built on the versioning model the owner approved on 2026-10-25 (ROADMAP, "Versions and updates") and on
D13's safe replacement of running programs (`ETXTBSY`, rename-over). The short version is in [DESIGN.md](DESIGN.md) D15.

## 1. The goal

A new version of Zenix reaches the machine, installs itself, keeps the previous version, and the owner can always boot
back to the previous one from the boot menu. No SD card leaves the A1200, no command runs on the PC as root.

Today an update means: the card out of the A1200, a `dd` of the root partition on the PC (which also resets the owner's
settings, such as passwords), and a kernel copy to BOOT: through Wasabi. D15 ends all three.

## 2. The idea: one update bundle, two halves of the work

The kernel and the boot files live on BOOT:, an AmigaDOS (FFS) partition that Zenix cannot write (and today cannot
read). Everything else lives on Zenix's ext2 root. So the update is done by the two programs that can each reach their
half, before and after the hand-over:

1. **The bundle arrives on BOOT:** as one file, `BOOT:zenix-update.zup`. Today it gets there from AmigaOS (the owner, or
   Wasabi with the owner's OK, copies it); later Zenix fetches bundles itself over the serial cable or the network
   (`zpkg`, section 9) and writes them to its own disk instead (section 8).
2. **`zenixboot` (AmigaOS side) does the boot half** while AmigaOS still runs: it finds the bundle, checks it, installs
   the new kernel the AmigaOS way (`zenix` -> `zenix.old`, the new one -> `zenix`), and loads the rest of the bundle
   into memory for Zenix, the same way it already loads a RAM-disk image (`RAM=FILE`, bootinfo `ram_pa`/`ram_size`).
3. **Zenix does the root half** at boot, before `init` starts logins: a small installer (`/sbin/zup`, started by `boot`
   or `init`) unpacks the packages from the memory copy onto the root filesystem, transactionally (section 5), and
   writes the result where `zenixboot` will see it next time.
4. **`zenixboot` closes the circle** at the next start: it reads the result (section 6), and only then removes the
   bundle (renames it `zenix-update.done`), or, if the root half failed, puts the old kernel back and says so.

Nothing new is needed in the kernel: the hand-over of a memory blob already exists (the RAM disk). The RAM-disk slot
gets a "kind" word so the kernel passes an update blob to user space instead of mounting it (section 7).

## 3. The bundle (`.zup`)

- A header (magic `ZUP1`, format version, the bundle's own version and date, the channel `testing`/`stable`, the
  minimum running version it may be applied to), a table of contents, then the parts:
  - `boot/`: files for BOOT: (`zenix`, `zenixboot`, `bootmenu`) - handled by zenixboot;
  - `pkg/`: packages for the root (section 4) - handled by Zenix's installer.
- Every part carries its size and its SHA-256; the table of contents carries the SHA-256 of the whole.
- **Signature (step 2 of the build, section 11):** an Ed25519 signature over the table of contents, checked by both
  zenixboot and the installer against a public key built into them. Until signing exists, a bundle is accepted only
  with its checksums correct and only from BOOT: (which only the owner and Wasabi with the owner's OK can write).
- Size: the whole system is about 1.2 MB, a full bundle about half that compressed; zenixboot loads it into fast RAM
  when there is any (chip RAM on a chip-only machine; a 2 MB A1200 has room).

## 4. Packages (the approved model)

- **A package** = a compressed archive (`tar` + `gzip`, both already in Zenix) plus a manifest: name, version, the
  files with their SHA-256 and mode, dependencies (a minimum base version, other packages), and whether it is part of
  **base**.
- **base** = the kernel, `init`, `boot`'s chain (the built-in servers live inside the kernel image), `login`, `sh`, the
  service table. One version for the group, always updated together.
- **Compatibility rule (enforced by review, written in CLAUDE.md):** system call numbers and message types are
  append-only - never reused or changed - so an older program keeps working on a newer kernel. A package needs only
  "base at least X".
- **The database** (`/var/lib/zpkg/`): plain text, one file per installed package (its manifest, as installed), easy to
  read with `cat` and `grep`.
- **User files are never overwritten:** files under `/etc` that the owner may change (passwd, group, fstab, svctab,
  motd, profile) are installed as `name.new` when the owner's copy differs, and the installer says so (as pacman's
  `.pacnew`). So an update never resets passwords again.

## 5. Installing safely (the root half)

- **A transaction:** the installer writes a journal (`/var/lib/zpkg/journal`) before it changes anything: the list of
  files to install and the previous version of each. Every new file is written as `path.zup-new`, checked against its
  SHA-256, then **renamed over** the old one (atomic on ext2 and ZenixFS; D13's `ETXTBSY` never blocks a rename, and
  running programs keep their old code). The previous version of each replaced file goes to `/var/cache/zpkg/prev/`.
- **A power cut in the middle:** at the next boot the installer finds the journal and either finishes (all new files
  are complete and checked) or rolls back (puts the previous files back), then removes the journal. Never half and half.
- **Space:** the installer checks free space first (new files plus the previous copies); not enough is an error before
  anything changes.
- **Order:** base first, then the other packages; `sync` at the end; then the result file (section 6), then `sync`.

## 6. The result, and the way back

- The installer writes a small result file to a place zenixboot can read before Zenix runs: **the boot note area at the
  top of chip RAM survives only a warm reset, not a power cut, so it is not enough.** Proposed: the result goes into the
  bundle's memory copy's header *and* into a one-sector "result block" at a fixed place on the root partition's
  reserved area (ext2's first 1024 bytes are free for boot use), which zenixboot reads with the trackdisk-style device
  calls it already uses. (Open question 1.)
- **The way back:** the boot menu gets an entry "Zenix (previous)": zenixboot with `zenix.old`, and a flag the kernel
  passes to `init` so the installer **rolls the root back** to the previous versions from `/var/cache/zpkg/prev/` before
  the logins start. One previous version is kept (more later, if the owner wants).
- If the new kernel does not even boot (no result appears), the next start of zenixboot sees "update applied, no
  result" and offers the previous kernel automatically (a countdown, as the boot menu's).

## 7. What changes where

| Part | Change |
|---|---|
| `zenixboot` (AmigaOS side) | find and check a bundle on BOOT:; install `boot/` files (with `.old` copies); load `pkg/` into memory with kind "update"; read the result block; finish or undo; messages on screen and serial |
| `bootmenu` | the "Zenix (previous)" entry (runs zenixboot with the previous kernel and the rollback flag) |
| kernel | bootinfo gets a kind for the memory blob (RAM disk or update) and a rollback flag; the update blob is offered read-only to `boot` (a `/dev/update`-like device, or an existing call) instead of being mounted; nothing else |
| `boot`/`init` | run `/sbin/zup` when an update blob or the rollback flag is present, before the logins; its output on the console and in `dmesg` |
| new `zup` (installer) and `zpkg` (later, the package manager) | user-space programs; `zpkg -Q` lists installed packages from the start |
| the build (PC side) | `make packages` makes the packages and a bundle (`tools/mkzup.py`), with checksums (and later the signature) |

## 8. Later transports (same bundle, same installer)

- **Over the serial cable** (from 2026-10-31): `zpkg` receives a bundle with a simple checked transfer from the PC and
  writes it to the root partition (`/var/cache/zpkg/incoming.zup`); the installer applies the root half at once (running
  programs keep their code), and the kernel/boot half waits for the next start (zenixboot then needs to read the root
  partition: a tiny read-only ext2 reader in zenixboot, or the PC side also puts the boot half on BOOT: through Wasabi).
  Designed in detail when the cable works.
- **Over the network** (block 2): `zpkg -Syu` fetches the repository's index and the packages (static files on GitHub;
  the owner's NAS as the testing channel), checks the signatures, and installs exactly the same way.

## 9. `zpkg`, the package manager (built after this, block 2)

`zpkg -S name` install, `-Syu` update everything, `-Ss word` search, `-R name` remove, `-Rns` with unneeded
dependencies and saved settings, `-Q` list; repositories in `/etc/zpkg.conf`; the same database, journal and rollback
as section 5. Only `zup`'s local half is part of D15.

## 10. Tests

- Host: `mkzup.py` and the installer's logic built for the PC (like the ext2/disk host tests): a bundle with a wrong
  checksum, a truncated part, an older version, a missing dependency - all refused before any change; the journal
  finished and rolled back at every point of a simulated power cut (the installer killed after each step, then rerun).
- Emulator: zenixboot with a bundle on the FFS partition (the zenixboot-test disk): the kernel replaced with `.old`
  kept, the root packages installed, `/etc/passwd` changed by the user is kept and `passwd.new` written, the result
  read at the next boot and the bundle renamed; "Zenix (previous)" rolls back both halves; a bundle whose new kernel
  does not boot leads back to the old one; a power cut (the emulator killed) during the root half is finished or rolled
  back at the next boot; running shells keep working during an update (D13).
- The full suite after each step.

## 11. Order of work

1. The bundle format and `tools/mkzup.py`; packages from the build; the host test.
2. `zup`, the installer (journal, rename-over, `.new` for user files, rollback), first tested on the host.
3. The kernel's blob kind and rollback flag; `boot`/`init` starting `zup`.
4. zenixboot's half (check, install `boot/` files, load, read the result, finish or undo) and the boot menu's
   "previous" entry.
5. The emulator tests of section 10.
6. Signatures (Ed25519: a small, well-known public-domain implementation, checked against its test vectors) in
   zenixboot and `zup`; the public key in the build; the private key never in the repository.
7. Docs; then the owner's first real update on the A1200 (with the owner).

## 12. Open questions for the reviews

1. Where does the result live so zenixboot can read it after a power cut: ext2's boot area, a file on BOOT: written
   by the next zenixboot run from the memory copy, or a tiny ext2 reader in zenixboot?
2. Is a dedicated kind of RAM blob better than a separate memory region in bootinfo (so a RAM disk and an update can
   coexist)?
3. Should `zup` refuse to apply an update while more than one user is logged in (a running multi-user system), or
   always apply (running programs keep their code)?
4. The private key: where it lives (the owner's PC, outside every repository), and how a lost key is handled.

Last checked against the code: 2026-10-26.
