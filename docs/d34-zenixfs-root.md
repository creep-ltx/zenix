<!-- SPDX-License-Identifier: GPL-2.0-only -->
# D34: ZenixFS as the root on a real machine (design note)

Status: **built on branch zroot (2026-11-01), the next item of block 4 (ROADMAP "The order of work", item 4). Everything here is
verified in FS-UAE and on the host, on test images. Since 2026-11-04 it has run on the real A1200 (the partner's choice): the
root on a CF card of the A1200 with the Apollo 1260 (d37's measurement ran on it), and the PiStorm card's root and /home
(reported). Nothing here changes what a card boots by itself.** The trial's steps and risks are in section 7.

## 1. What was missing
`zroot-test` passed, but what it tests is not a machine anyone could use:

| Gap | Now |
|---|---|
| A repair that destroyed data: `zenixfs_rebuild_bitmap` on a tree with a damaged node marked everything under the node free | Refuses; new inner nodes instead (section 2) |
| A mount that took a root or map copy newer than its commit record (a state of mixed generations, found as "1 prob" in the lab) | Checked; such a state is read-only at most (section 2) |
| `fsck.zenixfs` could only report, on Zenix | `-y` repairs what can be repaired without guessing |
| The root image: `mkroot-zenixfs.sh` had drifted from `mkroot.sh` (no package database, no fonts, certificates, ZINE, `/tests` programs) | One tree: `mkroot.sh` with `ROOTFS=zenixfs` |
| No update had ever run on a ZenixFS root (`update-test zfsroot` runs kernel-zfs on an **ext2** root) | `update-test zfsreal`, `zfscut`; `sysupdate-test z` |
| `zpkg` refused to do anything on a ZenixFS root | Programs live; the system by the list's `systemz` line |
| Nothing boot could do with a damaged root but panic | The rescue choices (section 4) |
| The card's layout never tried (a ZenixFS root in an MBR partition of type 0x83) | `zroot-card-test c` |
| 2 MB of chip RAM alone never measured | `zroot-card-test m`, section 5 |
| No power cut on a whole running system, only in the host's replay | `zroot-card-test p` |

## 2. The repair failure of 2026-10-31, and what it showed
`corrupt-lab.py --check` failed once in one worktree: one inner node garbaged, repair left 220 of 949 files, one file wrong. Cause, in
three layers, each measured (`zenixfs/tests/corrupt-lab.py`, the sweep below):

1. **Why it depended on the bytes of the tree.** A node of 2 KB holds 126 children. The lab copied the source tree into its volume, so
   the number of leaves followed the files: `git archive` of the branch 125 leaves, with `__pycache__` 126 (2 levels: the only inner node
   is the root), the failing worktree 129 (3 levels). Until the tree grew a level the lab's "one inner node" was always the root, which
   the commit before saves. It had never damaged a real inner node. **A flaw of the lab**, and it hid the rest.
2. **"Lost" 77%, and the wrong file.** Nothing was lost: `salvage` brought back all 949. `get -r`, the lab's way of reading, stopped at
   the first error (220 files were simply where it stopped), and it left the file it was writing, empty, under its real name: that was
   the "wrong" one (`tools/signal-host-test.c`, 0 bytes). The volume never returned wrong data. **A flaw of the tool.** `get` now goes on,
   removes a file it could not read whole and exits 1.
3. **A real weakness underneath: repair made it worse.** A damaged inner node is shared with the older commits (copy-on-write only
   rewrites the path of what changed), so no older commit saves it, whatever the old table said. Then `zenixfs_rebuild_bitmap` walked what
   it could reach, marked the rest free and committed: `salvage` before the repair found 949 files, after it 476. The next write would
   have overwritten the others.

And a second real one the new lab found: with the two newest commit records destroyed, mount took the third's root block, which a later
commit had reused for itself. The checksum and block number are right; the map was another generation's. It looked like all files
present, "1 problem". A write would have overwritten live blocks.

**What the code does now:**
- `zenixfs_rebuild_bitmap` refuses a tree it could not read whole, and changes nothing.
- `zenixfs_repair` (the policy, shared by `zenixfs-tool repair` and `fsck.zenixfs -y`): the tree whole: the map follows it. Only inner
  nodes damaged: **new inner nodes over the leaves, nothing lost**: every leaf says what it is (checksum, its own block number), and the
  commit's map says exactly which blocks are live, so old copies of leaves are not taken for live ones. **Everything is decided before
  anything is written**, by walking the leaves as if they were the tree: the links check out, no block they reach is marked free, and every
  used block they do not reach is a valid old node or one of the blocks the old tree's walk named damaged. One more means a leaf is gone
  (perhaps with a file's extents, which link counts do not see), and it refuses, having written nothing. Otherwise the commit before, if
  its tree is whole and it is the one before. Otherwise it refuses, changes nothing, and `salvage` still has everything it had. Only a
  destroyed superblock is rewritten first (and only if block 0 really fails its checks, and the records' map size fits the device).
- A destroyed leaf is not repaired in place: its contents exist nowhere else in the current state. `salvage` and `rescue` are the way.
- Generations are checked: a root newer than its commit, a tree block read from the disk newer than the state in hand (every read), a block
  newer than its parent (only `fsck`'s walk). Cost: compares on a read from the disk, nothing per request.
- `mount_back` (a new statistic): how far behind the newest commit that may have existed this one is, counting unreadable slots after the
  newest valid one, also a zeroed slot on a volume past its first turn of the ring (it used to pass silently). **The newest valid record is
  safe** (a torn newer one is only a cut). **Past a valid record (its root or map is bad), 2 or more back, or a map copy newer than the
  record: an ordinary mount, also a server's restart, is read-only.** The commit before is whole only until a later transaction starts
  writing (format rule 3), and without data checksums a repair that goes back one commit cannot prove the data of older files: it says so.

**Format:** nothing changes. These are rules for reading what version 1 already writes (`zenixfs-format.md` section 8).

**The review (one independent read of this diff, 2026-11-01)** found the first version wrote its new nodes before deciding, so a refusal was followed by a rollback onto overwritten data; `-y` took any failed mount for a missing superblock and rewrote it; a restart mount ignored the read-only rule; `--restore` refused a broken p3; and more. All fixed but those named open in section 8. One trap from the fix: the checksum of a superblock passes at a smaller block size too, because 256 rotations of a 32-bit sum over zeros change nothing: a block size is only proven by the superblock's own field.

**Test.** `--check` no longer reads the source tree: the tree comes from a seed, and it insists on a tree of three levels and one of two
(a volume of another shape fails the check instead of quietly testing something else). Every inner node in turn; a failure prints the line
that replays it. A refusal must leave the image bit for bit; a repair that says clean must take a new file and stay clean; a file with
wrong contents under its real name fails, from the plain tools and from repair. `--sweep N` for many shapes.

## 3. Which kernel: a second system bundle, not a boot module
A ZenixFS root needs its server before any file can be read, and the shipped kernel has none (D19 step 1 saved every machine 36 KB). D19
3.3 left two ways open: a variant in the bundle, or a boot module (zenixboot hands the server to the kernel). **Decided: the variant,
`kernel-zfs.bin`, as a second system bundle.** It needs no new hand-over and no kernel code on any other machine; zenixboot's refusal (a
machine on a ZenixFS kernel never takes a kernel without the server, D19 step 1) already makes a wrong bundle harmless; a boot module
would add a frozen bootinfo field and kernel code on every machine for the few with this root. Against it: 215 KB more per release in the
repository, on the PC's side only. kernel-zfs serves ext2 too, so a machine can have both.

- `tools/mkrepo.py add`: with `BUILD/kernel-zfs.bin` it also writes `pool/zenixz-VNUM.zup` (the same base group, that kernel) and the
  list's line `systemz VNUM VSTR FILE SIZE SHA256 min MIN`, signed with the list. An older zpkg ignores it (D22: other header words are
  growth). `zenixz` may not be a package's name.
- A change only in kernel-zfs is a new system too. A build without `kernel-zfs.bin` publishes a list without the line, with a printed note; those machines are told, nothing installed.
- `zpkg` on a ZenixFS root (`SYS_mountinfo` says `zenixfs`) takes that line for the system; a list without it: "no system for a ZenixFS
  root", nothing installed. Programs install live as on ext2 (D22 4.3's refusal is gone; zup's journal and rename work the same).
- `boot` is built twice (`bootz.zx`): the code for a damaged ZenixFS root is only in that kernel: the shipped one grew by 56 bytes.

The name "test-only" goes: `kernel-zfs` is the kernel for a ZenixFS root. A machine has it only if someone chose that.

## 4. Boot, the clean mark, rescue
- **No check at boot, no clean mark (D17 section 9 stands):** a cut leaves the last commit. `zfscut`: killed while zup installs, the host's
  fsck is clean at once, the next start needs no check, zup's journal finishes or rolls back.
- **`zfsd -root` on a state it can only read** (section 2): a new request `FS_HEALTH` (26; claims.md) says so to boot. boot stops
  that server, starts it again with `-fsck` (read-only, the device shared, the same word as fsd's), and offers man rescue's choices without
  "read-write anyway": writing on such a state could overwrite files. In the shell `fsck.zenixfs -y` (one commit back at most) or `--old`
  (the newest readable state, on the owner's word, older files may be wrong); when `fsck.zenixfs` says clean, the shell's exit starts the
  writable server, and boot asks again. An update is not installed on a read-only root (as for ext2).
- **A root that cannot be mounted at all:** the leftmost inner node damaged (a mount looks there for orphans), or with a damaged superblock boot cannot tell the type and fsd says there is no filesystem; with
  all records gone zfsd says what to do. Either way the service's panic stops the machine, nothing written. The floppy `zenix-mount.adf`
  (`fsck.zenixfs -y DEVICE`) or the PC's tool repairs it.
- **`df`, `sync`:** as on any mount: 2 KB or 4 KB blocks in `df`; `sync` commits. The commit timer: at most 3 s after the first change.
- **The partition:** `rootpick` takes the first ext2 or zenixfs partition; an MBR's type 0x83 counts as ext2 and boot reads the superblock,
  so the card's layout needs nothing new. An RDB says `ZNF\1` or the same way by superblock.

## 5. Memory and speed
Measured in FS-UAE (memory only; no speed from here):

| | ext2 root | ZenixFS root |
|---|---|---|
| Kernel image | 169,312 bytes | 209,484 bytes (`bootz` and `zfsd` built in) |
| Server's cache, chip-only | 24 KB | 96 KB of blocks (128 KB at 4 KB blocks: the library's floor of 32), 2 map pages |
| Server's cache, fast RAM | 48 blocks | 48 blocks, 8 map pages |
| Free with one login, 2 MB chip alone | 1323 KB | 1135 KB (2 KB blocks), 1071 KB (4 KB blocks): 188-252 KB less: the kernel 40, zfsd's image and its running copy 80, the cache and scratch blocks the rest |

The cache: a commit comes when half of it is dirty, so on chip alone a large write commits every 48 KB (each commit two flushes). What a
real 68020 feels is not known: no read-ahead yet (a block at a time, one tree search per block), writes one disk command per block; counted,
not timed. CLAUDE.md's rule stands: docs state speeds measured on real hardware only.

`fsck.zenixfs` needs one bit per block plus about 40 bytes per inode; `-y` also 28 bytes per leaf, and it reads every used block. On a volume
too big for the machine it says "not enough memory" and writes nothing (an allocation that failed used to crash `fsck`: no check; fixed).

## 6. Tests
`make -C zenixfs test` (the lab's `--check` as above); `tools/zroot-card-test.sh` c, p, r, m; `update-test` zfsreal, zfscut;
`sysupdate-test` z; `zroot-test`, `zenixfstest`, `mounttest`, `rootcheck-test`, `paths-test`, `svc-test` unchanged in what they check.

**The fault test (`p`)** kills the emulator at five moments while files are written, renamed over and deleted, with a file deleted while
open. After each cut the PC's fsck must be clean; every round synced before the cut compares exactly; later rounds are whole or a prefix,
never other bytes; the renamed-over file is one whole version. **Found while building it:** FS-UAE keeps a disk's last write back until the
next command, so a kill loses it whatever the flush said. So a round counts as synced only if a disk command followed. That is the emulator,
not a real disk's behaviour; what proves the order of writes and flushes is the host's replay (`ztest powercut`: every prefix and reorderings
inside each flush), which this does not replace. No real power has been cut (section 7).

## 7. A real-hardware trial
**The partner chose it and did it (2026-11-04: the PiStorm card's root and /home are ZenixFS, reported); the steps stay
here as the way to do it.** It changes partition 3 of the card and one file on `BOOT:`, and all of it can be put back.

1. On the PC, the root image, in the size of p3 or less (SIZE in MB; sizes of 256 MB and up get 4 KB blocks):
   `make build/kernel-zfs.bin build/pkg/list`, then
   `PKGDB=1 RAWIMG=1 ZENIX_RELEASE=1 tools/mkroot-zenixfs.sh build/root-z.img build "$(make -s --eval 'pa:;@echo $(UAPPS) $(SBINAPPS) $(NETDRVS:%=drv-%)' pa)" SIZE`
2. The card in the PC. `lsblk -o NAME,SIZE,MODEL,TRAN` before and after it goes in: the new line is the card, never guessed. Then
   `sudo ZENIX_WRITE_ROOT_ZENIXFS=1 bash tools/write-root.sh /dev/sdX build/root-z.img ~/zenix-backups`
   It checks the card (p2 the RDB, p3 Zenix's root), saves p3 and checks the copy, carries users, passwords, groups, fstab (with `/home`), autologin, waits 5 seconds, writes p3 only, reads back, checks. **Note the name of the saved file.**
3. The card back in the A1200. Start AmigaOS. `Rename BOOT:Zenix/zenix BOOT:Zenix/zenix-ext2`, and copy `build/kernel-zfs.bin` as `BOOT:Zenix/zenix` (AmigaOS names ignore case; the menu file is not touched).
4. Choose Zenix in the menu. `mount`: `/` must say `zenixfs`, `/home` `ext2`. `df`, `free`, a file, `sync`, `reboot`: the file is there. `/sbin/zup -l` lists the packages.
5. The fault test on real hardware: `seq 1 20000 > /x; cp /x /y` and power off during it, with the disk active. Start again: no check, no message, `fsck.zenixfs` from the rescue floppy or the PC finds it clean; the last few seconds may be gone.
6. Note `free`, how a program's start and a big `cp` feel, labelled "PiStorm/Emu68": the first real figures.
7. **The way back:** the card in the PC: `sudo bash tools/write-root.sh /dev/sdX --restore ~/zenix-backups/zenix-root-DATE.img.zst`, and in AmigaOS `Delete BOOT:Zenix/zenix`, `Rename BOOT:Zenix/zenix-ext2 BOOT:Zenix/zenix`. Files written on the ZenixFS root in between are lost unless copied to `/home` first.

**What the trial risks.**
- p1 (Emu68), p2 (AmigaOS, BOOT): the write in step 2 goes through the byte range the card's own table gives p3, with the checks `write-root.sh` has always had; run as written, it writes nothing else (tested on a card image: `zroot-card-test w`). The wrong device name is what it is built to refuse; it refuses non-removable disks and disks outside 1-64 GB. In Zenix `diskd` refuses a write outside a partition's range, and ZenixFS writes only inside its volume.
- p4 (`/home`): served by the same ext2 server as today, nothing changes.
- p3: replaced for the trial, restored by step 7. A wrong kernel only costs a reboot into AmigaOS through the menu ("Zenix (previous)" takes `zenix.old` if there is one; with the shipped kernel a ZenixFS root stops with a message, harmless).
- Updates: while kernel-zfs is installed zenixboot refuses a bundle with the shipped kernel (renames it `.bad`). `zpkg -Syu` asks for `systemz`.

**What cannot be known from here:** how the SD path behaves with this write pattern (2-4 KB transfers, each commit two flushes; ext2 writes 1 KB at a time); whether the Emu68 SD path's flush is a real one (`zfsd` prints a line if the disk says it cannot flush); speed. **A real power cut has never been tried on ZenixFS.**

## 8. Open
- Known, documented, not fixed: after a repair by rollback, the abandoned newer records stay valid: if the first commit after it is lost too, mount takes one of them. Two faults; zeroing them would make the ring look damaged later. The commit before as "whole": see section 2; the real fix is keeping freed blocks one commit longer (Z8 says so, the code does not) or data checksums, both after measurements.
- The trial itself, the owner's. The stock A1200 and the 060 (a DoM image with a ZenixFS root: `tools/mkdom.sh` is ext2 only).
- `zenix-mount.adf` as the floppy for a root that cannot be mounted: it is 2 MB+ safe only by its own RAM disk; tested with fast RAM here.
- Speed on real hardware; read-ahead, several blocks in one disk command (docs/zenixfs.md "not done"): only after the first measurements.
- The installer (the next item) will have to offer the choice.
