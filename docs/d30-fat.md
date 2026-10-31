# D30: FAT, a filesystem server (design note)

Status: **built on branch fat (2026-10-31), first item of block 4 (ROADMAP "The order of work", item 4).** Format facts
are *read* from Microsoft's FAT specification (published) and general knowledge; no other system's code was read.

## What it is
`mount -t fat DEVICE DIR` serves a FAT12, FAT16 or FAT32 volume with VFAT long names, read and write. The server is
`/sbin/fatd`, a file service like `/sbin/zfsd` (D11 6.4, D19 step 1): one process per mount, the same protocol
(`kernel/fsproto.h`). Nothing is added to the kernel, `diskd` or the small floppies' root: only `mount` learns the type
(about 200 bytes), and it finds the type by itself for an MBR partition of a FAT type (01 04 06 0B 0C 0E, the hidden
11 14 16 1B 1C 1E, and EF). A volume without a partition table is `mount -t fat ide0 DIR` or `df1` (`diskd` lists the
whole device; it already tells a bare FAT volume from an MBR, D-mbr-test).

## Decisions (made here, the owner delegated the details, 2026-10-31)
- **Names:** UTF-8 to programs, UCS-2 on disk (surrogate pairs kept, so UTF-16 from Windows reads right). Old 8.3 names:
  **CP437** (the table is 256 bytes). A new file always gets an ASCII alias (others become `_`), so the codepage matters
  only for reading old volumes; å ä ö are at the same place in CP850. Lookups fold case for ASCII, Latin-1, Latin
  Extended-A, Greek and Cyrillic; other scripts compare exactly (no 128 KB up-case table). A long name that does not fit
  `udirent` (63 bytes of UTF-8) is listed by its 8.3 alias, so nothing is hidden.
- **Inode numbers:** FAT has none. Root 1; a directory `0x40000000 | first cluster` (stateless: `..` gives the
  parent's, a rename keeps it); a file its entry's place (`sector * 16 + index`, 30 bits). An open file has a slot (64,
  the kernel's NFILE) that keeps its number, first cluster and size across a rename; a new file that would collide with
  a slot takes a number from `0x60000000 |` a counter. After a server restart the kernel's open files answer `ESTALE`
  until closed, as `nfscd`.
- **Unix meaning:** owner, group and modes come from `-o uid=,gid=,fmode=,dmode=` (defaults root, root, 644, 755). The
  read-only attribute is the one mode bit on disk: `chmod a-w` sets it, `chmod u+w` clears it; any other change, `chown`
  to someone else, `ln` and `ln -s` answer `EPERM`. Names with `" * / : < > ? \ |`, control characters or a trailing dot
  or space answer `EINVAL` (Windows could not open them). Set-user-id bits cannot be set at all.
- **Crash order (the owner's card):** three rules, enforced by one cache of 24 sectors:
  data reaches the disk before the FAT, the FAT before the directory that points to it, and a cluster goes free only
  after the directory that named it is on the disk. So freed chains wait in a list and are zeroed in the FAT at the end
  of a commit. Rename writes the new entry first, then removes the old one (overwriting a file changes its short entry
  in place: one 32-byte write). The worst a power cut leaves is lost clusters, or a chain longer than the file's size,
  and two names for one file for a rename: never another file's data. All FAT copies are written in turn, with a disk
  flush between them. Commit: 3 s after the first change, at `sync` and at `umount` (as zfsd).
- **The boot sector is never written.** A torn write there would lose the volume (FAT12/16 have no backup). So the
  dirty mark is the specification's clean-shutdown bit in `FAT[1]` (FAT16, FAT32; both FAT copies), set before the
  first change and cleared 5 s after the last commit or at `sync` and `umount`. FAT12 has no such bit. At mount a volume
  whose `FAT[1]` bit or boot sector byte says dirty (other systems set the latter) is **mounted read-only with a line
  in dmesg**; `-o force` mounts it writable and takes over the `FAT[1]` bit. Zenix has no `fsck.fat`.
- **FSInfo (FAT32):** free count and next-free hint read at mount if they make sense, written when the volume goes
  clean. Without it the first `df` counts the FAT once.
- **Damage:** the BPB is checked whole at mount (sizes, FAT long enough for its clusters, inside the device; only
  512-byte sectors). Chains are checked per step: out of range, free or bad in a chain is `EIO`; walks are bounded by the
  file's size, directories by the format's 2 MB, so a loop ends in `EIO`. LFN parts are checked (order, checksum);
  bad ones are ignored and the 8.3 name shows, which is what the format asks.
- **The service text:** `/sbin/fatd DEV -id START,SIZE [-o OPTIONS]`: a volume id without the uuid (`volid.h`'s place
  rule; FAT has only a 4-byte serial and `vol_look` must not grow, it is in the kernel's fsd), so a floppy is refused at a restart.
  `mount` hands the option text through and `fatd` checks it, so `mount` stays small; a bad option is a line in dmesg.

## Limits
Primary MBR partitions (`diskd`); 512-byte logical sectors; files under 4 GB (the format's own limit); no sparse
files (a write past the end writes zeros); access dates are never written; no `fsck.fat` on Zenix; times are stored as
UTC. An Amiga floppy drive reads only the 880 KB Amiga track format, so a real PC floppy cannot be read in `df0`; an
880 KB image formatted FAT12 can.

## Tests
`fat-host-test` (the server on the PC against `mkfs.fat` images, `fsck.fat` and `mtools`; power cuts after each sector
written; a fuzz run with ASAN and UBSAN), `fat-test` (FS-UAE, an MBR partition and a superfloppy).
