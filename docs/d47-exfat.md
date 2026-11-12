# D47: exFAT, read-only (`/sbin/exfatd`)

Roadmap block 6, the USB item's next step (docs/d46-usb.md section 9 step 3 and section 10; the main session's decision
of 2026-11-12 in docs/decisions.md: "exFAT is the next item, its own design note"; the details delegated). Revision 1,
2026-11-13, branch `exfat`. Built and checked on the PC and in FS-UAE; **nothing here has read the partner's real
stick yet**: section 9 says what only it can show. No change of meaning to any interface: one new file service, one
new type name for `mount` and `/etc/fstab`, nothing in the kernel, `diskd` or any message.

## 1. In plain words

The partner's main USB stick is a 128 GB Corsair GTX formatted exFAT ("USB sticks mostly, and mainly 128GB Corsair
GTX (ExFAT)", the partner 2026-11-12). With D46 the stick shows up in the disk server as `usb0` and `usb0p1`; with
this, `mount usb0p1 /mnt` reads it: every file and directory, long names in any language, files up to 4 GB - 1 byte,
volumes of any size Zenix's disks can have (2 TB). Read-only: nothing is ever written to the stick. A file of 4 GB or
more (a film, a disk image) is listed but cannot be opened, because Zenix's file calls count bytes in 32 bits.

exFAT is not a kind of FAT for this purpose (D46 section 10): it has an allocation bitmap instead of free FAT entries,
files that need no FAT chain at all, directory entry sets with checksums, an up-case table for names, 64-bit sizes and
no 8.3 names. So it is its own server, `/sbin/exfatd`, beside `fatd`, sharing with it the one piece that really is
common: the UTF-16 names (`user/utf16name.h`).

| Part | What it does | Where |
|---|---|---|
| exfatd | reads exFAT; one file service per mount, as `fatd`, `pfs3d` and `iso9660d` | `user/exfatd.c`, `/sbin/exfatd` |
| names | UTF-16 to UTF-8 and back, case folding: one copy for `fatd` and `exfatd` | `user/utf16name.h` |
| mount | the type `exfat`, found by itself from the boot sector's "EXFAT   "; `-o rw` refused; allowed on a read-only device (a stick today) | `user/mount.c` |

## 2. Decisions (made here, the details delegated)

1. **Read-only, and only the reads.** `exfatd` sends the disk server nothing but `DISK_OPEN`, `DISK_INFOSZ`,
   `DISK_READ` and `DISK_UNMOUNT`; it has no code that writes. Every change answers `EROFS` after the answer a
   writable volume would give first (`ENOENT`, `EEXIST`, `EACCES`, `ENOTDIR`), as `iso9660d` and `pfs3d` do. `-o rw`
   is refused by `mount` and by the server. A volume marked dirty (VolumeDirty, spec 3.1.13.2: pulled out without
   being ejected) or with media failures recorded is read as it is, with a line in dmesg: reading cannot make it worse.
2. **Names as `fatd`'s long names** (D30): UTF-8 for programs, UTF-16 on the disk, surrogate pairs kept (a character
   past U+FFFF, an emoji, reads right). The console draws UTF-8 (D28), so Swedish, Greek, Cyrillic and Japanese names
   show as written where the font has them. A character no Unix name can hold (a control character, `/`, half a
   surrogate pair: damage, or another system's mistake) is listed as `_`, and `_` finds it again. A name over 63
   bytes of UTF-8 (a `udirent`'s room) is listed as its start, `~` and its inode number in hex, which opens it, as
   `iso9660d`, `ffsd` and `pfs3d` do (exFAT has no 8.3 alias to show instead, as FAT has).
3. **Case: the folding `fatd` uses**, now shared (`u16_fold`): ASCII, Latin-1, Latin Extended-A, Greek and Cyrillic
   are found in any case; other scripts compare exactly. The volume's up-case table (128 KB when whole) is not read:
   the spec lets an implementation ignore it beyond its first 128 mappings, which it fixes (7.2.5); this one folds
   more than those (`fatd`'s set) and compares the rest exactly. Its checksum is therefore not checked either; its
   entry must be there, once, and in range.
4. **Programs see 32-bit sizes** (`struct ustat`'s `size`, `FS_READ`'s offset). A file of 4 GB or more is listed,
   `stat` says 4294967295, and `open` answers `EFBIG` ("file too large"): the answer `pfs3d` and the NFS client give
   already, so the system says one thing. A file of 4 GB - 1 byte reads to its end in order (the kernel's `lseek` takes
   offsets below 2 GB only, so a seek past 2 GB is refused there, for every filesystem). Nothing is cut or wrapped: no
   file is read as a shorter one.
5. **Every entry set's checksum is checked before the set is used** (spec 6.3.3: "Implementations shall verify..."). A
   set whose checksum is wrong is never followed: it is not listed, cannot be opened, and the listing goes on past
   it and answers `EIO` at its end, so what can be read is listed. The same for a set of the wrong shape (its stream
   entry missing or not first, too few name entries, cut by another primary entry).
6. **Every number on the disk is range-checked before use** (section 4): the boot sector whole, cluster numbers,
   lengths, contiguous runs inside the cluster heap, chains step by step (free, bad, end too early, past the heap:
   `EIO`), directories at most the format's 256 MB, the root's chain bounded. A damaged volume fails a request, never
   crashes or hangs the server (the host test's aimed damage and fuzz run, ASAN and UBSAN).
7. **Memory: nothing whole.** The FAT of a 128 GB stick at 4 KB clusters would be 128 MB, its bitmap 4 MB; neither
   is loaded. FAT and directory sectors go through a cache of 16 sectors (8 KB), a file's small reads through a
   window of 16 sectors (8 KB); the bitmap is only read for `df`, once, through the window, and its count kept.
8. **Speed: whole sectors straight into the caller's buffer**, and a run of clusters that lie one after the other on
   the disk as one request, cut only where the disk server's largest request ends (`diskinfo.maxsect`): a 64 KB read
   of a contiguous file at a sector boundary is one `DISK_READ` of 128 sectors (measured in the host test). A
   FAT-chained file is read in its runs: a cursor keeps where the last read was, so reading in order costs one FAT
   entry per cluster, never a walk from the start.
9. **Inode numbers as `fatd`'s:** an object's number is the place of its File entry (its sector x 16 + its index:
   30 bits, so on a volume over 32 GB two places 32 GB apart give one number); the root's is 1. Every object the
   kernel holds open has a slot (64, the kernel's `NFILE`) that keeps its number and its stream; a new open whose
   place number a slot holds for another object takes the first number of a fixed sequence above `0x40000000` that no
   slot holds, so it gets the same number every time while the open objects stay the same (its `~` name keeps opening
   it). So two open files never share a number, and `stat`, `open` and the listing agree. A file open when the server was restarted
   answers "stale file handle" until it is closed (as `fatd`'s, `ffsd`'s and `pfs3d`'s).
10. **Owners and modes from the mount**, as FAT's: `-o uid=,gid=,fmode=,dmode=` (defaults root, root, 644, 755); the
    read-only attribute takes the write bits away. A program on the stick runs only with `-o fmode=755`.
11. **Times:** the LastModified timestamp, its 10 ms count and its UTC offset when the offset is marked valid (spec
    7.4.10); without one the time is taken as UTC (as `fatd` takes FAT's).

## 3. The format as read (Microsoft's exFAT specification, 2019; section numbers are its)

All numbers little-endian.

- **Boot sector** (3.1): jump `EB 76 90`, `EXFAT   ` at byte 3, bytes 11-63 zero, PartitionOffset 64, VolumeLength
  72 (64-bit, sectors), FatOffset 80, FatLength 84, ClusterHeapOffset 88, ClusterCount 92, FirstClusterOfRootDirectory
  96, FileSystemRevision 104 (major at 105: only 1), VolumeFlags 106 (bit 0 the active FAT, 1 dirty, 2 media failure),
  BytesPerSectorShift 108 (9-12), SectorsPerClusterShift 109 (at most 25 minus that: 32 MB clusters), NumberOfFats 110
  (1 or 2), PercentInUse 112, `55 AA` at 510. Checked: all of these in their ranges, the FAT long enough for the
  clusters and before the heap, the heap inside the volume, the volume inside the device, the root cluster in the heap.
- **Boot checksum** (3.4): the rotate-right-and-add sum of the first 11 sectors of the region without bytes 106, 107
  and 112, repeated through the 12th. A main region that fails any check (its name too) is replaced by the backup at
  sector 12 (of the sector size the backup itself says), with a line in dmesg; both bad: refused with the main one's
  reason. (`mount`'s own probe looks only at sector 0: a stick whose main boot sector is gone needs `-t exfat`.)
- **FAT** (4.1): 32-bit entries; 2 .. ClusterCount + 1 the next cluster, FFFFFFF7h bad, FFFFFFFFh the end. The second
  FAT (TexFAT) is used when VolumeFlags says it is the active one.
- **Directory entries** (6, 7): 32 bytes; type 00h the end of the directory, 01h-7Fh unused (a deleted set's), 80h
  invalid. 81h the allocation bitmap (its FirstCluster 20, DataLength 24; flag bit 0 which FAT it belongs to), 82h the
  up-case table, 83h the volume label, 85h a File entry; A0h-BFh benign primary entries (the volume GUID, TexFAT's
  padding), passed over with their SecondaryCount; any other critical primary entry in the root refuses the volume
  (8.2), in another directory is damage.
- **A File entry set** (7.4, 7.6, 7.7): the File entry (SecondaryCount 1, SetChecksum 2, FileAttributes 4: 01h
  read-only, 10h directory; LastModifiedTimestamp 12, its 10 ms 21, its UTC offset 23), then exactly one Stream
  Extension (C0h: flags 1 - bit 0 AllocationPossible, must be 1; bit 1 NoFatChain -, NameLength 3, NameHash 4,
  ValidDataLength 8, FirstCluster 20, DataLength 24), then NameLength / 15 rounded up File Name entries (C1h, 15 UTF-16
  units from byte 2), then any benign secondary entries (E0h vendor extension, E1h vendor allocation: in the checksum,
  otherwise ignored). An unknown critical secondary entry (C2h-DFh) makes the set one this server only lists (8.2).
  The SetChecksum is the 16-bit rotate-and-add over the whole set without bytes 2 and 3 (6.3.3; 16 bits at every step).
- **A stream:** FirstCluster 0 only with DataLength 0; ValidDataLength at most DataLength; a NoFatChain stream is
  `DataLength / cluster size` rounded up clusters from FirstCluster, all inside the heap; past ValidDataLength a
  reader returns zeros (7.6.5). A directory's DataLength is its whole allocation, at most 256 MB, a multiple of 32
  here. The root directory has no entry: its length is its FAT chain's (3.1.10).
- **NameHash** (7.6.4): the same sum over the up-cased name's bytes. Used only to pass over sets quickly: for a name
  asked for in ASCII (whose up-casing the spec fixes, Table 24) a set whose hash differs cannot be it; any other name
  is compared after its length.

## 4. exfatd (`user/exfatd.c`)

`mount -t exfat DEV DIR` (or a device whose boot sector says `EXFAT   `) registers `fs.DEV` ->
`/sbin/exfatd DEV -id START,SIZE [-o OPTIONS]` at nice -10 (a file service, D11 6.4), in the `disktools` package
beside `fatd`, `ffsd`, `pfs3d` and `iso9660d`. One process per mount, the protocol of `kernel/fsproto.h`; exFAT has no
links, so it never answers `FS_LINKED`. Requests from anyone but the kernel and root are refused, as every server's.

**The mount:** the device's size and largest request (`DISK_INFOSZ`); at least 1 MB; the boot region (section 3)
or its backup; the root's chain walked once for its length (at most 256 MB, so a chain that loops ends); the root's
entries read once: the active FAT's bitmap entry and exactly one up-case entry must be there, in range, and nothing
unknown and critical. Refused with the reason in dmesg (`exfatd: DEV: ...`).

**Walking a path:** `.` and `..` are taken out of the text first (exFAT has no links and no `..` entries; the kernel
sends absolute paths with them taken out already, D26 5.6: so `nosuch/..` is the directory itself, as the kernel's
text rule makes it for every path); then each name is looked up in its directory: only sets
of the name's length (and, for an ASCII name, its hash) are read whole, checksum first, then compared in folded case.
A relative path from an open directory (only a raw request has one) cannot climb above it: `ENOENT`.

**Reading** (section 2.8): a cache of 16 FAT and directory sectors, a window of 16 of a file's sectors for small
reads, whole sectors straight into the caller's buffer in runs. A file's slot keeps a cursor in its chain, left at the
cluster of the last byte used, so small reads in order never walk the chain again. A listing's place that is not a
multiple of 32 (a program's `lseek` on the directory) answers `EINVAL` and is never read. A
directory's slot does the same for listings, so a listing is one pass. The bitmap's free clusters are counted at
the first `df` (a nibble table, 128 KB of bitmap for a 128 GB stick at 128 KB clusters) and kept: nothing changes
on a read-only mount.

**Sizes (measured, `m68k-elf-size`):** `exfatd.zx` 20,780 bytes on the disk (text 18,877 with the C library's
pieces, data 24, bss 8,792 of which 3.3 KB the 64 slots and 2.9 KB five entry sets with their 255-unit names). **A
mount's RAM: about 53 KB** - the program (19 KB), its bss (8.6 KB), the cache and the window (16 KB, malloc'd at the
mount) and its 8 KB stack. Nothing grows with the volume's size or the number of files. `fatd.zx` 28,524 -> 28,544
bytes (+20) for the shared names; `mount.zx` 16,256 -> 16,552 (+296). Nothing in the kernel, `diskd` or the floppies'
root changes (`exfatd` is in `disktools`, like `iso9660d`, not on the demo floppies).

**Options:** `uid=`, `gid=`, `fmode=`, `dmode=` (octal), `ro`, `defaults`, `noauto`; `rw` refused. They travel in the
service's text (80 bytes, frozen), as `iso9660d`'s.

## 5. mount

The type `exfat` (`-t`, `/etc/fstab`). Without `-t`, after the partition types (`fat` for FAT's MBR types, `ffs`,
`pfs3`, ...), a device whose sector 0 has `EXFAT   ` at byte 3 is `exfat`, so both a partition of MBR type 07 (which
NTFS shares, so the type alone says nothing) and a stick formatted without a partition table (`usb0`) mount by
themselves. `-o rw` is refused for `exfat` as for `pfs3` and `iso9660`. A device with `DISKCAP_RO` (D44's CD, D46's
stick until its writes, U4) accepts `-t iso9660` and `-t exfat`.

## 6. Limits (also in docs/limits.md)

Read-only. Files of 4 GB or more listed, not opened (section 2.4). Volumes up to Zenix's 2 TB (32-bit sectors); any
sector size 512-4096 and cluster size 512 bytes-32 MB. Case folded for Latin, Greek and Cyrillic only. Inode numbers
wrap every 32 GB of the volume (section 2.9): `stat` may show one number for two files on a big stick, never two open
ones. A set with an unknown critical secondary entry is listed but not opened. The up-case table, the volume label
and the GUID, access and creation times and the 10 ms parts beyond the second are not shown. The FAT chain of a
damaged file that loops inside its length reads repeated data, bounded by its length (no loop check; `fsck.exfat` on a
PC finds it). A file open across a restart of the server is stale until closed.

## 7. Tests

- **`tools/exfat-host-test.py`** (host, ASAN + UBSAN; SKIP without exfatprogs). The PC formats with `mkfs.exfat` and
  `tools/exfat-img.py` writes the files from the spec (no exFAT writer runs here without root: no FUSE, no loop
  mount); `fsck.exfat -n` checks every volume it wrote, so the writer is checked by the PC's own tool, which also
  verifies checksums and names. Then: every file read in odd pieces against its size and CRC (sizes 0 to 300,001,
  contiguous, one-run chains, fragmented chains, a valid data length below the size, read-only, a UTC offset); a
  300-entry and a 200-entry fragmented directory; Swedish, Greek, Cyrillic, Japanese and astral names, found in any
  case where folded; a long name by its whole name and by its `~` form; a name with `/` and a control character;
  every change refused with the right answer first; the options and permissions; raw IPC refused; inode numbers;
  the speed rule (a 64 KB read of a contiguous file: 1 request of 128 sectors; a one-run chain: 1; a fragmented chain:
  one per run; the second small read from the window: none; 20 small reads in order near the end of a 40 MB fragmented
  chain: a few requests each, and the whole file right); listing places from `lseek`; 512-byte and 32 MB clusters, 4096-byte sectors; a 128 GB
  volume (sparse): a file in its last cluster, two files 32 GB apart (one number for stat, two when open), a file of
  4 GB - 1 read at its start and its end, a 5 GB file capped and refused, a long name's `~` form stable while an open
  file holds its place number, `df` over a million clusters; 14 volumes refused at the
  mount (each check of the boot sector in both regions, both checksums bad, an unknown critical root entry, no bitmap,
  a root chain that loops) and one mounted from its backup boot region; 17 kinds of aimed damage (a wrong checksum, every stream
  field, a set's shape, a chain free, bad, past the heap, cut short or looping, a directory's length, a broken
  directory chain), each also checking that what the damaged file gives is its own data; a fuzz run (600 volumes by default) over the boot region, the FAT, the root and four directories,
  half the time with the checksums made to match so the damage gets inside: no crash, no hang.
- **`tools/exfat-test.sh`** (FS-UAE, the A1200 with 4 MB of fast RAM): an MBR card, partition 1 of type 07 formatted
  by `mkfs.exfat` and filled by `exfat-img.py`, partition 2 the root. `mount ide0p1 /mnt` by itself, its size and free
  space; a file, in any case; a Swedish name in other case; contiguous, fragmented, chained and partly-valid files and
  a program against the PC's SHA-256; a long name; a deep path; 120 entries; writes, `mkdir` and `rm` refused; the
  server killed and reading on; `-o rw` refused; `-o uid=5,fmode=755` and the program run from it; afterwards on the
  PC the partition unchanged to the byte and `fsck.exfat` clean.
- Run for what this touches: `fat-host-test`, `fat-test` (the shared names), `mounttest` (mount), `cd-test` (mount's
  probe).

## 8. Not done

Writing (a later item, after the stick has been read right on the real machine; it would need the bitmap, the FAT,
the checksums and the dirty mark kept in step, spec 8.1); the up-case table's full case folding; TexFAT's
transactions (a second FAT is only read when it is the active one); showing the volume label (`fsinfo` has no field
for it).

## 9. What only the real stick can show

On the PiStorm A1200 with D46's USB, the partner's 128 GB Corsair GTX plugged in (read-only: nothing here writes):

1. `fdisk list` shows `usb0` and `usb0p1` (type 07), or only `usb0` if the stick has no partition table.
2. `mount usb0p1 /mnt` (or `mount usb0 /mnt`); `mount` lists it as `exfat` with the stick's size; the first `df`
   counts the bitmap (128 KB at 128 KB clusters: time it).
3. `ls -l /mnt`, the partner's own folders and names (Swedish letters); `sha256sum` of a few files whose sums are taken
   on the PC; a file over 4 GB, if there is one: listed, `cat` says "file too large".
4. Speed: `time cat /mnt/BIGFILE > /dev/null` (no figure from FS-UAE, CLAUDE.md).
5. What only Windows-written volumes show: NoFatChain directories, vendor entries, a dirty mark from a stick pulled
   without ejecting (read as it is, a line in dmesg).

## 10. Review

One independent review (2026-11-13), read against the spec and with crafted volumes. Found, and fixed with a test
each: the read-ahead window's length wrapped for a file of almost 4 GB, so a small read at its start returned another
file's bytes (the window's count is now taken without the wrap; the test reads the 4 GB - 1 file's start after another
file); a listing's place from a program's `lseek` inside an entry let a set be read past its cache sector (`EINVAL`
now); a number given at a collision changed at every call, so a long name's `~` form never opened (a fixed sequence
now); unaligned reads of a FAT-chained file walked the chain from its start every time (the cursor now stays at the
last byte used; a 40 MB chain read in 5,000-byte pieces near its end is checked); the damage tests did not check that
what is read is the file's own data (now they do), and the image writer wrote three equal times (now three different
ones). Also: a 64-bit multiply in the boot check (a shift now), a leftover read, the backup boot region now tried
whatever failed, days past a month's end refused. Left as said: `..` is taken on the text (section 4); seeking past
2 GB is the kernel's limit (section 2.4).

## 11. Sources

- Microsoft, "exFAT file system specification" (learn.microsoft.com, published 2019), the only source of the format:
  sections 3 (boot region, checksum), 4 (FAT), 6 and 7 (directory entries, checksums, NameHash, up-case table), 8.2
  (unrecognised entries), 9 (limits). No other implementation's code was read.
- exfatprogs 1.4.3 (`mkfs.exfat`, `fsck.exfat`, `dump.exfat`) as tools only, to make and check test volumes.
- Knowledge base: `amiga/zenix-filesystems-research.md` section 3; Zenix's own D30 (`fatd`), D44 (`iso9660d`), D46.
