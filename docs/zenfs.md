# ZenFS - the native Zenix filesystem (plan, 2026-10-22)

**Status (2026-10-22):** phases Z-0 and Z-1 are done on the host: the exact format is in `zenfs-format.md`, the library is in `zenfs/` (branch `zenfs`),
and `make -C zenfs test` runs the tests. *Verified* (run, 60 random seeds, with AddressSanitizer and UBSan): ~3000 random operations compared with a model, `fsck` clean,
remount; the power-cut replay (every prefix of the recorded writes plus random subsets inside each flush epoch) always mounts, passes fsck and equals a committed state, and
is never older than a durable commit; 600 random bit flips in metadata are detected or harmless, none silent; full volume and orphan cases. The tests were checked by mutation:
removing the flush before the commit record, or reusing blocks freed in the same transaction, makes the power-cut test fail.
Not done: the Zenix port (needs the multi-filesystem VFS), windowed bitmap (the library keeps the whole bitmap in RAM), delayed allocation (data is placed when written),
hash-order `readdir` only, no `keep=N`, no data checksums, no defragmenter, no real-hardware measurements. Known limits of the library: a write that spans several blocks is
not atomic across a commit that the cache pressure triggers in the middle of it (each block step is consistent, the whole write is not); a mount that had to fall back to an older commit
is reported in `zfs_stats.mount_damage` and the caller must tell the user.

Status of every statement: **proposed**. Numbers marked "estimate" are not measured; measure on a real
A1200 (030 or 020, IDE or CF-in-IDE) before believing them. Background and sources: `Knowledge/amiga/zenix-filesystems-research.md`.

## 1. Goal and the machine it is for
A filesystem that is **safe** (no check after a crash, no replay, silent damage is noticed), **fast on slow
hardware** and **small**: 68020/030 at 14-50 MHz, a few MB of RAM (cache = tens to hundreds of KB, no MMU),
PIO IDE at about 1-3 MB/s (estimate) or a CF card behind it (no seeks, slow small writes). It has the Unix
semantics Zenix needs and none of the Amiga limits (107-char names, 104 GB, mtime only).
ext2 stays for Linux compatibility. Amiga volumes (OFS/FFS/SFS/PFS3) come later as separate read-only drivers.

Design rule: **the disk is the bottleneck, not the CPU.** Spend CPU (a checksum, a hash, a sort) to save disk
accesses; never the other way round. Few, large, sequential transfers.

## 2. What we take from where (ideas only; no code is copied, Zenix is GPL-2.0 only)
| From | Idea we take |
|---|---|
| PFS3 | Atomic commit by switching one root; metadata clustered; the deleted-files directory; little CPU per block |
| SFS | Hash-table directories, many entries per block, extent B+tree, read-ahead cache, online defragmenter, always-free reserve |
| ext2 | Plain inode semantics: modes, owners, hard + symbolic links, 255-byte names |
| btrfs/ZFS | Copy-on-write tree, per-block checksum with own-address check, snapshots as a free by-product |
| LittleFS / F2FS | A COW design that fits in a few KB of RAM and survives power loss; flash-friendly writes |
PFS3 is 4-clause BSD (not GPL-compatible as code): ideas only. SFS source is GPL (version unclear): read, do not paste.

## 3. Decisions
**Z1. Copy-on-write, atomic commit, no journal.** Nothing live is overwritten. A change writes new blocks;
one small *commit record* then makes them current. A crash leaves the previous commit. No replay code, no
log space, no fsck needed for crash recovery. *Why not a journal (SFS style):* a journal writes metadata
twice and needs replay and reserved space; COW writes it once and PFS3 shows COW is fast enough on this class of machine.

**Z2. Everything is in one B+tree of blocks**, keyed `(object number, kind, key)`: inodes, directory entries,
extents, small-file data, symlink targets, extended attributes. One tree implementation = less code, less RAM, one
thing to test. Fan-out about 30-40 with 4 KB blocks, so 3 levels cover tens of thousands of files (estimate).

**Z3. Block size is chosen at mkfs: 1, 2 or 4 KB** (default 4 KB; 1 KB for floppies and tiny RAM disks). 32-bit
block numbers, so 4 TB at 1 KB, 16 TB at 4 KB. IDE transfers are made in multi-sector commands. 4 KB also
matches CF erase-page behaviour better than 512 B.

**Z4. Big-endian on disk.** The CPU's own order, no swapping, simple fields. (The Amiga handler later is easier too.)

**Z5. Names are opaque bytes**, 1-255, only NUL and `/` forbidden, case-sensitive. Zenix convention today is
Latin-1 (keyboard, font, AmigaOS agree); UTF-8 later changes nothing here. See the Latin-1 note in the research file.

**Z6. Directories are hashed.** A directory entry is a tree item with key `(dir, hash32(name), n)`; lookup is one
tree descent; no scan. `readdir` walks in key order and the cookie is the hash, so it stays valid while the
directory changes. Many small entries share a block (SFS idea, comes free from the tree). Hash: a cheap one
(FNV-style or a 32-bit multiply-rotate), fixed forever in the format.

**Z7. Files are extents** `(logical block, physical start, length)`, 32-bit each. Files under about 200 bytes live
**inline** in the tree item (saves a block and a seek; most of `/etc`, scripts, symlinks). Larger files: extents.
Data is also copy-on-write, written in the commit order below, with **delayed allocation** so a file written in
pieces is placed in one contiguous run. After a crash a file is entirely old or entirely new per commit; never
garbage inside a size the metadata claims.

**Z8. Free space = bitmap, windowed**, one bit per block, with a per-bitmap-block free count so the allocator
finds long runs without scanning. 4 GB at 4 KB = 128 KB of bitmap = 32 blocks, cached a few at a time. Blocks freed
in a transaction may be **reused only after the next commit is durable** (else a crash would revive a tree that points
to overwritten blocks). One generation of freed blocks is deliberately kept (see Z11).

**Z9. Commit procedure** (all dirty blocks are collected in RAM and written together):
1. write dirty data extents; 2. write dirty tree blocks, children before parents; 3. **flush the drive cache**
(ATA FLUSH CACHE; CF cards and modern IDE disks reorder writes, so the barrier is essential - check that Gayle-era
drives accept it, fall back to "wait" otherwise); 4. write the **commit record** (generation number, tree root,
bitmap root, checksum) into the next slot of a small ring of 8 slots at fixed places (spreads wear, and mount takes
the valid slot with the highest generation); 5. flush again. Until step 4 completes the old state is the truth.
Triggers: `sync`/`fsync`, a timer (about 5 s, adjustable), the dirty set reaching half the cache, unmount.
Because dirty blocks are bounded by the cache, commit never needs more RAM than it has.

**Z10. Integrity.** Every metadata block starts with: type, **its own block number** (catches misdirected writes),
generation, checksum. Checked on every read. Checksum = a cheap 32-bit sum over longwords (Fletcher/rotate-add
style), about 1k longwords per 4 KB block, far below the disk time; **not CRC32** unless measured to be cheap.
Data blocks are not checksummed (cost, and the extent map already protects structure); optional later per-file.

**Z11. Self-healing.** (a) Previous generation stays intact until the one after next commit, so a bad current root
is survived by mounting the older commit slot. (b) `mkfs` option `-m dup`: metadata blocks written twice at
different places (btrfs DUP), for CF/disks with bad sectors; off by default because it doubles metadata writes.
(c) `scrub` walks the tree in the background, verifies checksums and rebuilds a bad bitmap from the extents. (d) `fsck.zenfs`
= verify tree, rebuild bitmap, count links; never needed after a plain crash.

**Z12. Never wedges when full.** About 2 % (at least 64 blocks) is reserved so `unlink` and `truncate` work on a
full disk (COW needs room to write the new tree before it can free the old). Normal users see ENOSPC earlier.

**Z13. Time and metadata.** Inode: mode, uid, gid, nlink, size (64-bit), mtime and ctime (64-bit seconds, Unix
epoch, so host tools and ext2 agree), 8 bits of Amiga flags (so a future AmigaOS handler can map the protection
bits). **No atime** (a read must never cause a write). Hard links = several directory items to one inode number.

**Z14. Deleted files and undo.** Because old generations exist, a cheap undo is possible: `mount -o keep=N` keeps the
last N commits' blocks unreclaimed (PFS3 "deldir" spirit, in space the user chooses). Snapshots (a named old root)
are the same mechanism. Later; not in the first format, but the format leaves room (a root list in the commit record).

**Z15. Defragmentation is online and restartable.** Copy an extent to a better place, switch the tree pointer in one
commit; can stop at any moment (SFS idea). Allocation prefers the old neighbourhood of a file to avoid fragmenting
in the first place. Later phase.

**Z16. RAM budget:** fixed at mount: cache size (default 128 KB, `-o cache=`), maximum one tree path pinned per
open file, no allocation proportional to the disk size. A mounted volume's own state is a few KB.

## 4. What is deliberately not in version 1
Compression, encryption, deduplication, quotas, data checksums, ACLs, atime, multiple devices, online grow
(shrink never), journal-style `fsync` fast path, snapshots UI. The format has a feature-flag word
(compat / read-only-compat / incompat, like ext2) so these can be added without breaking old volumes.

## 5. How we build and test it (the safe way)
1. **A host library first.** `zenfs` as portable C (no Zenix calls; a `blockio` interface: read, write, flush) compiled on
   Linux against a disk-image file. The same source later goes into the Zenix fs server unchanged.
2. **Tools on the host:** `mkfs.zenfs`, `fsck.zenfs`, `zenfs-dump` (print the tree), and a **FUSE mount** so ordinary Linux tools (fsstress,
   fsx, rsync, diff -r) can hammer it. Same approach as checking ext2 with the host's e2fsck.
3. **Power-cut test, deterministic:** record every write and flush of a test run; for each prefix (and for random subsets of writes inside a
   flush epoch, to model reordering) rebuild the image, mount it, run `fsck.zenfs`, and compare the tree with a model of what must be there.
   This proves the commit order instead of hoping. Plus: flip random bits in metadata and check the damage is detected, never silently read.
4. **Benchmarks before claims:** one script (sequential read/write of a large file, create 1000 small files, `ls` of a big directory, `rm -r`,
   untar a tree) run on the same real hardware against Zenix ext2, and under AmigaOS against FFS/PFS3/SFS. Docs state only
   figures measured on a real Amiga with a real drive; the host/emulator figures are labelled as such.
5. **Fuzz** the mounted image (one fuzzer at a time on this PC, memory limits per the machine notes).

## 5b. What can and cannot be measured here (owner, 2026-10-22)
The only Amiga available is an A1200 with a PiStorm: the "disk" is the Pi's SD card (about 17 MB/s) behind Emu68, and the CPU is a JIT, not a real 68030.
So **speed figures from it are not valid for real Amigas** and must never be quoted as such. What we can do:
- **Correctness and crash safety are fully testable** on the host: format, power-cut replay, bit-flip detection, fsck. This is where ZenFS earns its name.
- **Counted costs instead of timings:** the library counts disk reads, writes, flushes, commits and cache hits (`zfs_get_stats`). Fewer, larger, sequential
  transfers can be shown by counts on the host; they are the same on every machine.
- **Design from known hardware costs**, with the numbers in section 6b labelled as estimates.
- **A benchmark tool that others can run** (small, one binary on the Zenix disk, prints a table) so people with real IDE/SCSI Amigas can send results;
  defaults get tuned from those, not from the PiStorm.
- On the PiStorm, only "does it work" and the CPU-bound case (fast media) are meaningful, and only labelled "PiStorm/Emu68".

## 6. Phases
- **Z-0 Spec (done):** write `docs/zenfs-format.md` (every field, offsets, checksums, worked examples) from this plan; review it.
- **Z-1 Host library (done):** tree, inode, inline files, extents, bitmap, commit, mkfs, fsck, dump; unit tests; the power-cut tester.
- **Z-2 FUSE + stress** on the host; fix what the tests find.
- **Z-3 Zenix server:** after main has the multi-filesystem VFS (mount table, several servers). Runs beside ext2; boot from ZenFS later. Measure on the A1200.
- **Z-4 Speed and safety extras:** hash directory tuning, read-ahead, defragmenter, scrub, dup metadata, keep=N / snapshots.
- **Z-5 AmigaOS handler** (read-only first) so the other half of a dual-boot machine can read Zenix disks (Latin-1 names, big-endian: easy).

Dependencies on the other branches: VFS mount table (main), partition type id for RDB (here: DosType `ZEN\1`), `FS_*` message set in
`kernel/fsproto.h` already fits (open, read, write, readdir, rename, link, symlink, statfs, sync); add `FS_FSYNC`.

## 6b. Storage varies a lot (owner's reminder, 2026-10-22): design for the whole range
Amiga disks hang on very different hardware (from general knowledge, to be checked per machine): Gayle IDE (A600/A1200) and A4000 IDE with
a CF card or a disk, fast-ATA add-on cards and IDE-fixes, Zorro/accelerator SCSI (e.g. Blizzard SCSI kits, some with **DMA**), the A3000/A4000T on-board
SCSI, PCMCIA CF, and PiStorm's SD card (about 17 MB/s, measured elsewhere in the notes). Speed spans roughly 1 MB/s to 20 MB/s, and the bottleneck moves:
slow PIO IDE = the disk is the limit and the CPU is half idle; DMA SCSI or SD = the **CPU is the limit** and the disk waits.
So ZenFS must not be tuned for one case:
- **Rule 1: low CPU per block everywhere.** Cheap checksum, no per-byte work on file data, copy as little as possible (messages pass pointers).
  Data checksums stay **off by default** until measured; with PIO they ride along with the copy, with DMA they cost real CPU time.
- **Rule 2: the filesystem asks the disk what it is.** Extend the disk protocol (`kernel/diskproto.h` today has READ/WRITE/INFO only, 512-byte sectors, no flush) with:
  `DISK_FLUSH` (ATA FLUSH CACHE / SCSI SYNCHRONIZE CACHE; essential for Z9, a driver that cannot flush says so), and capability fields in `diskinfo`: maximum sectors per
  command, scatter/gather or DMA yes/no, memory the device can reach (some DMA cards reach only certain RAM), required alignment, physical sector or erase size (CF/SD
  4 KB or more), "rotating" or "flash" (seeks matter or not), a rough speed class. The fs reads these at mount.
- **Rule 3: tuning follows the capabilities, the format does not.** The on-disk format is identical on every medium. What adapts at mount: block size default (flash with a large
  erase size -> 4 KB aligned), write batch size (as large as the device accepts in one command), read-ahead depth, commit interval, whether the allocator works hard for
  contiguous files (rotating disk: yes; flash: only alignment), and cache size. All overridable with mount options.
- **Rule 4: no hardware assumed in the filesystem code.** It speaks only the `blockio` interface (read, write, flush, info). Gayle, SCSI, SD, a RAM disk and the host's file image are all just
  drivers behind it; the same ZenFS code is tested on the host image and on the real machines.
- **Rule 5: the flush barrier is the one thing we cannot fake.** If a device cannot flush, ZenFS mounts it but says "no crash guarantee on this device" (like ext2 `nobarrier`).
  Test by really cutting power on each kind of device we claim to support, not only in the simulator.
- Measure each kind (Gayle+CF, Gayle+disk, SCSI with/without DMA, SD) with the same benchmark script and write the table; the defaults come from that table.

## 7. Decisions by the owner (2026-10-22)
1. **Name: ZenFS.** Decided.
2. **Block size:** the owner asked for pros and cons; recommendation: `mkfs.zenfs` picks by volume size (under 16 MB: 1 KB,
   under 256 MB: 2 KB, larger: 4 KB) and `-b` overrides. Reasons in the table below.
3. **Undo (`keep=N`, snapshots):** not in the first version. Reserve the room in the format now (root list in the commit
   record, feature flag); build it in phase Z-4, after the power-cut tests pass, because it changes when blocks may be reused.
4. **Data checksums:** reserve now, build later. A per-inode flag plus a feature flag; a directory flag is inherited by new
   files. The checksums are stored as tree items next to the extents (4 bytes per block). Decide the default after measuring on hardware.

| Block size | Good | Bad |
|---|---|---|
| 1 KB | Little waste with many small files; more blocks fit in a small cache | More IDE commands and more tree levels; worse on CF cards (writes smaller than the card's erase page); bigger bitmap |
| 2 KB | Middle | Middle |
| 4 KB | Fewest disk commands, shallowest tree, fastest large files, best match to CF cards | Wastes on average half a block per file (about 2 KB); fewer blocks in a small cache; one small change rewrites a bigger block |
Small files under about 200 bytes are stored inside the tree and do not use a block, so the waste only matters for files of a few hundred bytes to a few KB on a small volume.
