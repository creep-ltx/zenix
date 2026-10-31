# D31: the Amiga filesystems OFS and FFS, a filesystem server (design note)

Status: **built on branch ffs (2026-10-31), second item of block 4 (ROADMAP "The order of work", item 4).** Format
facts are *read* from public sources: the knowledge base's notes (`amiga/disk-images.md`, `amiga/aos31-study/`), the Guru
Book, the NDK's `dos/dos.h` for the numbers of the protection bits. No other system's filesystem code was read.

## What it is
`mount -t ffs DEVICE DIR` serves an AmigaDOS volume: `/sbin/ffsd`, a file service like `/sbin/fatd` (D30): one process
per mount, the protocol of `kernel/fsproto.h`. Nothing is added to the kernel, `diskd` or the small floppies' root; `mount`
learns the type and finds it by itself for an RDB partition whose DosType is `DOS\0` .. `DOS\7`, also inside an MBR
partition of type 0x76 as on the owner's card (`diskd` lists those). A volume without a table is `mount -t ffs df0 DIR`
or `ide0`. The name: `ffs` stands for both OFS and FFS, one server, one type; PFS3 will be a server of its own.

## What it reads and writes
| DosType | | read | write (`-o rw`) |
|---|---|---|---|
| `DOS\0` `DOS\1` | OFS, FFS | yes | yes |
| `DOS\2` `DOS\3` | international OFS, FFS | yes | yes |
| `DOS\4` `DOS\5` | directory cache OFS, FFS | yes (the hash chains, as the validator does; the cache is not used) | no: read-only |
| `DOS\6` `DOS\7` | long names OFS, FFS | yes | no: read-only |

Block sizes 512 to 32768 bytes are read: from the RDB's `PART` block (DosEnvec `SizeBlock` x `SectorPerBlock`, and
`Reserved`). A volume without an RDB has 2 reserved blocks and its block size is found by trying each size until the root
block fits. **Only 512-byte blocks are written:** a larger block is several sectors, a cut can tear it, and a torn
directory or bitmap block fails its checksum, which takes others down with it. The crash order rests on one block being
one atomic write.

**Why a directory-cache volume is never written:** a cached copy of every entry lives in other blocks, and AmigaOS trusts
them while the bitmap is valid (aos31-study, dos-and-filesystems). Two places for one fact cannot be changed by one write,
so a cut would leave a listing that lies, and rule 5 below keeps the bitmap valid so nothing would ever repair it.
**Why long-name volumes are never written:** name and comment share one area with an overflow block, and names over 30 bytes
do not fit a Zenix path anyway once in UTF-8; reading is enough to get files off such a volume.

## Decisions (made here, the owner delegated the details, 2026-10-31)
1. **Read-only unless `-o rw`, every device, floppies too.** One rule to remember. `-o rw` on a DosType above runs as
   read-only with a line in dmesg.
2. **`-o rw` walks the whole volume first.** Every directory, chain, file, extension block and link; no block named twice;
   every named block marked in use; the bitmap's checksums. The strictness of `user/zpkg/bootfs.c`, because a cross-linked
   or falsely-free block is how one file's write destroys another's data. Anything wrong: mounted read-only with a line in
   dmesg naming what. It costs one block read per file or directory and memory of one bit per block for the moment of
   the walk. A volume mounted read-only is not walked: it mounts at once.
3. **A volume whose bitmap is marked invalid mounts read-only** (AmigaOS was cut off, or is validating). Only AmigaOS's
   validator fixes that; Zenix has no `-o force` here.
4. **Blocks marked in use that nothing names (leaks) are given back** at the first commit of a `-o rw` mount, the walk
   having proved nothing names them, as `bootfs.c` does. AmigaOS would not validate a valid volume, so nothing else
   recovers them.
5. **The root's "bitmap valid" flag is never cleared by Zenix.** (AmigaOS clears it during writes and validates after a
   cut; `bootfs.c` and CFile's writer never do, and that is verified.) The order of writes is what keeps the volume valid.
   A cut leaves at worst leaks and one change to finish, never a "needs validating" disk, so zenixboot's update flow never finds a volume it may
   not write.
6. **The crash order, enforced by one cache:** new blocks (data, new headers, new extension blocks), flush; the bitmap,
   flush; extension blocks and the blocks of a file's data list changed in place, flush; existing headers and
   directories, flush. A block freed: nothing on the disk names it before its bit is set free in the bitmap: freed files
   wait in a list that the end of a commit works off. Commit: 3 s after the first change, at `sync`, at `umount`.
7. **A single header write (one sector) is atomic and carries a file's size,** so a file being appended has its old size or its new
   one. A cut can leave an extension block or an OFS data block's "next" naming blocks past the size; AmigaOS and ffsd
   both go by the size. `zpkg`'s stricter check refuses such a file until it is removed.
8. **Rename keeps a name at every moment.** The same hash chain (a change of case, or a name that hashes alike): one header
   write. Else the header's one "next" field serves one chain only, and taking it out first would lose the file, and for
   a directory its whole tree (the review's finding). So the new name goes in first, **as a hard link**: link block, the
   file's list, the new chain, each on the disk before the next. Then the old file of that name goes, if any; then the
   header leaves its chain, is rewritten with the new name and directory, joins the new chain, and the link goes. A cut
   leaves the file under its old name, its new name or both. The next `-o rw` mount finishes a rename left half done (one at
   most can be), after the whole volume has proved good. So it costs about eight writes and needs one free block (a
   rename on a full disk answers `ENOSPC`). A soft link is copied to a new block: nothing points at one. After a cut a
   rename over an existing name can leave both for a moment, the lower block found first.
9. **Hash chains stay in ascending block order** (Guru Book 15.3.4.8: AmigaOS's listing relies on it).
10. **Hard links:** `ln` makes them (files only), as the rename above does. Removing the original of a file with hard
    links makes the file's header take over the first link's name, the same way: no pointer to the header has to change.
11. **Soft link text:** on disk an Amiga path. `Vol:a/b` on this volume reads as a relative path up to this volume's root
    (the server does not know the mount point), a leading `/` as `../`, `a//b` as `a/../b`, Latin-1 as UTF-8. Another
    volume's name stays as text: a link that leads nowhere. `ln -s` takes relative targets and writes the Amiga form;
    an absolute target answers `EINVAL` (it would mean something else on the Amiga).
12. **Names:** Latin-1 on the disk, UTF-8 to programs, case-insensitive as the DosType says (ASCII, or Latin-1 from
    international on). A name outside Latin-1 answers `EILSEQ`; `/`, `:`, control characters, over 30 characters answer
    an error. A long name over 63 bytes of UTF-8 does not fit `udirent` and is listed as its start + `~` + its block number
    in hex, which opens it (long-name volumes only).
13. **Inode numbers:** the header's block number. Stable across a rename and across a restart, so a restarted server
    knows its open files' numbers; files open at a restart answer `ESTALE` until closed, as fatd's (the slots must
    remember a removed file). A hard link answers its file's number.
14. **Unix meaning:** owner and group from `-o uid=,gid=`; AmigaOS has no such thing on a normal volume. Modes: the `r`,
    `w`, `e` protection bits select from the mount's `-o fmode=` (default 755), so `----rwed` is `rwxr-xr-x`; directories
    are `-o dmode=` (default 755). `chmod` sets `r`, `w`, `e`; a `w` cleared also sets "delete protected" and a `w` set
    clears it, so `chmod a-w` is the Amiga's read-only and `chmod u+w` undoes both. The `s`, `p`, `a`, `h` bits and the
    comment are kept and never shown (the protocol has no place for a comment). The archive bit is cleared on a change,
    as AmigaOS does. A delete-protected entry answers `EPERM` to `rm`, also for root.
15. **Dates:** stored as UTC, as FAT (AmigaOS's are local; Zenix has no zone at this level).
16. **Damage:** the root and the block sizes are checked at mount. Every block read is checked by type, own number,
    checksum, and an entry also by parent. Block numbers out of range, a bad OFS data block (type, header, sequence, size,
    checksum) answer `EIO`. Hash chains and extension chains are walked with Brent's mark, so a loop ends in `EIO` with
    no memory; an extension chain is checked at a file's first open, as fatd's chains. A listing goes on past a damaged
    chain and answers `EIO` at its end, so what can be read is seen.

## The long-name format
Read from the knowledge base's note, then *verified*: volumes OS 3.2's Format and FFS made in FS-UAE read right.

## Limits
Files under 2 GB (AmigaDOS's own). 64 open files a mount. Comments are not shown. Reads of a `DOS\4/5` volume cost one block
per entry like any other, the cache is not used. A rename across chains needs one free block. Never run on a real Amiga yet.

## Tests
`ffs-host-test` (the server on the PC against images from `tools/mkffs.py` and `xdftool`, judged by `xdfscan` and an
own walker; power cuts after each sector written; a fuzz run with ASAN and UBSAN aimed at chains, extension blocks, the
bitmap and links), `ffs-test` (FS-UAE, an RDB partition inside an MBR type 0x76 partition, and a floppy).
`ffs-aos-test` (AmigaOS 3.1 and 3.2's own filesystems in FS-UAE read what ffsd wrote, ffsd reads what they wrote, the validator rebuilds the same bitmap).
