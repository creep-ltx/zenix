# D32: PFS3, a read-only filesystem server (design note)

Status: **built on branch pfs3 (2026-10-31), third item of block 4 (ROADMAP "The order of work", item 4).** No code of PFS3's is
here. Where each fact came from: "Where the format's facts came from" below; the owner should read that before Zenix goes public.

## What it is
`mount -t pfs3 DEVICE DIR` serves a PFS3 volume, **only for reading**: `/sbin/pfs3d`, a file service like `/sbin/ffsd` (D31):
one process per mount, the protocol of `kernel/fsproto.h`. Nothing is added to the kernel, `diskd` or the small floppies' root.
`mount` finds it by itself for an RDB partition whose DosType is `PFS\1`..`PFS\3` or `PDS\1`..`PDS\3` (pfs3aio registers `PFS\1`,
`PDS\1`, `PFS\3`, `PDS\3`; `PFS\2` and `PDS\2` also taken), also inside an MBR partition of type 0x76 as on the owner's card.
The purpose: Zenix can read the owner's real AmigaOS, which lives on PFS3. The name: `pfs3d`, type `pfs3`.

## Decisions (made here, the owner delegated the details, 2026-10-31)
1. **Read-only, always, with no write path.** The source has no `DISK_WRITE` and no `DISK_FLUSH` (a test greps for them); there
   is no write code to get wrong. `-o rw` is refused: by `mount` with a line for the user and by the server if started some
   other way. Every request that would change something answers `EROFS`.
2. **What the root block names is the volume.** PFS3 has no journal to replay: changed metadata goes to new blocks, then one new
   root block commits them (the old locations are given back only after that). So whatever is on the disk, also after AmigaOS was
   cut off, is one whole committed state: the last root's. **A volume with pending changes:** the only thing pending in the format
   is a *postponed operation* in the rootblock extension, a delete or truncate whose freeing of blocks AmigaOS finishes at its next
   mount. It concerns which blocks are free, which this server never reads: the file is already gone from its directory (or already
   shorter). pfs3d mounts, reads the state on the disk, says so in one dmesg line, and does nothing about it. Data written into a
   file in place after the last root can be there (new bytes under the old size); that is what AmigaOS itself would read.
3. **Finding the root, and the block size.** The root is block 2 of the volume. Tried largest block first (4096 down to 512),
   needing `PFS\1` or `PFS\2` (also the older `muPF`, `AFS\1`, `muAF`: same layout by the format's own header, not tested) in the
   root and in block 0. No RDB parsing: the owner's DH2 and DH3 say 4 KB blocks in the table but are `PFS\1` volumes with 512-byte
   blocks inside (the handler falls back the same way), so the table would be the wrong thing to trust. `-o blocksize=` forces one.
4. **Refused at mount, with the reason:** options bits it does not know (a newer format), no hard-disk mode (an old floppy
   format), an original type with the newer format's features, impossible reserved block sizes, a reserved area outside the
   partition, a volume larger than its partition, a damaged extension, no root directory.
5. **Inode numbers are small positive numbers:** on the disk an anode number is two 16-bit parts (block, place in it) on current
   volumes; Zenix folds it to `block * anodes-per-block + place`, so it cannot look like an error in a reply (a sign bit set) and is
   stable across a restart. Files open at a restart answer `ESTALE` until closed, as ffsd's. A hard link answers its object's number.
6. **Names:** Latin-1 on the disk, UTF-8 to programs, compared as PFS3 does (always international: a-z, 0xE0-0xFE but 0xF7). A name
   over 63 bytes of UTF-8 does not fit `udirent`: listed as its start + `~` + its own anode in hex, which opens it, as ffsd's. Comments
   and the owner fields are not shown.
7. **Unix meaning:** as D31: owner and group from `-o uid=,gid=`, modes: the `r`, `w`, `e` bits select from `-o fmode=` (default 755),
   directories `-o dmode=`. Dates stored as UTC.
8. **The deleted-files directory** (`.deldir`) is not shown: it is virtual in the format, and is what AmigaOS has thrown away.
9. **Rollover files** read from where they start and round the end, as big as their virtual size. *Read* from the format, tested on
   `tools/mkpfs3.py`'s volumes only: AmigaOS has no command that makes one.
10. **Files of 4 GB or more** (the large-file mode's high size bits) are listed with the size 4294967295 and answer `EFBIG` at open;
    the protocol's offsets are 32 bits. Up to 4 GB they read; the kernel's `lseek` stops at 2 GB as for every filesystem here.
11. **Soft links:** the text is in the link's first data block, its length in the entry; read as D31 11 (`amigatext.h`, shared with
    ffsd): `Vol:a/b` on this volume or `:a/b` is the way up to this volume's root, a leading `/` is `../`. Another volume's name stays.
12. **Damage:** every reserved block must lie on the reserved area's grid and have its id, sequence number or owner anode. A
    directory's blocks are anodes of exactly one block (all three places the format makes them). Chains are walked with Brent's mark,
    the mark kept with the open file: a loop answers `EIO` however many reads it takes. A listing goes on past a damaged block and
    answers `EIO` at its end. Damage may fail a request, never crash or hang (`pfs3-host-test`, ASAN + UBSAN).

## The format, as this server reads it
All numbers big-endian. A **block** is the volume's logical block; a **reserved block** is `RB` bytes (1, 2 or 4 KB, at least a
block) inside the reserved area, on a grid of `RB`-sized steps from `firstreserved`.

**Root** (block 2; the facts below are all in its first 512 bytes), by byte: 0 type, 4 options, 8 datestamp, 12 creation day,
minute, tick (16 bits each), 18 protection, 20 name (length byte, then up to 31), 52 last reserved block, 56 first reserved block,
60 reserved free, 64 `RB` (16 bits), 66 sectors in the root's cluster (16 bits), 68 blocks free, 72 always free, 76 roving, 80 old
deleted-files pointer, 84 size of the disk in blocks, 88 the extension's block, 92 unused, 96 five bitmap index blocks, 116 the 99
index blocks of a volume without super blocks. Block 0 holds the type too.

**Option bits:** 1 hard disk, 2 split anodes, 4 directory extra fields, 8 deleted-files directory, 16 size field, 32 extension,
64 datestamps, 128 super blocks, 256 big deleted-files directory, 512 extended roving, 1024 long names, 2048 large files, 4096
stored geometry.

**Extension** (id `EX`), by byte: 0 id, 4 options, 8 datestamp, 12 version, 16 the root directory's date (three 16-bit), 22 the
volume's date, 28 the postponed operation (four longs: which, three arguments; 1 and 2 free a file's blocks, 3 an anode chain), 44
reserved roving, 48 roving bit, 50 anode roving, 52 deldir roving, 54 deldir size, 56 file name size, 64 the 16 super blocks.

**Blocks with a sequence number**, all: 0 id (16 bits), 2 unused, 4 datestamp, 8 sequence number, 12 on: longs. `IB` an index block
(block numbers of anode blocks), `SB` a super block (block numbers of index blocks), `AB` an anode block, where the anodes start
at byte 16. Anodes in a block: `(RB - 16) / 12`; longs in an index block: `(RB - 12) / 4`.

**Anode** (12 bytes): cluster size, first block, next anode number (0: end). An anode number is, with split anodes, block sequence in
the high 16 bits and place in the low 16; without, one number divided by anodes per block. Then: anode block sequence / longs per
index block = which index block, the rest = which long in it. With super blocks that index number is divided again the same way:
which of the extension's 16, which long in it. Without, the root's list of 99. Anode 5 is the root directory; 0-4 are reserved.

**A file's data:** the chain from its anode: each anode is that many blocks in a row (an empty file has an anode of cluster 0).

**Directory:** a chain of anodes of one block each, each a reserved block with id `DB`: 0 id, 4 datestamp, 8 unused, 12 the directory's
own first anode, 16 its parent's, 20 on: entries. **Entry:** 0 its size (even; 0 ends the block), 1 type (signed: 2 directory, 3 soft
link, 4 hard link to a directory, -3 file, -4 hard link to a file, -16 rollover file), 2 anode, 6 size, 10 day, minute, tick (16 bits
each), 16 protection (low 8 bits), 17 name length, 18 name, then the comment's length byte and the comment. With directory extra
fields, the **extra fields** are at the very end of the entry, read downward: the last 16 bits are flags, and for each flag set (bit 0
first) one more 16-bit word going down: link high, link low, uid, gid, protection high, protection low, virtual size high, low, roll
pointer high, low, file size bits 32-47.

**Links:** a hard link's entry has an anode of its own, a list element: its cluster-size field is the anode of the directory holding
the object, its block field the link's own directory, its next the next link of that object. The link's extra field names the object's
anode; the object's extra field names the first list element. So finding an object means searching its directory for that anode. A
soft link is a file of one block holding the text.

## Where the format's facts came from
This is a report point, as the licence asks (PFS3's code is 4-clause BSD; docs/future-ideas.md: facts, not code):
- **The public pfs3aio source (github.com/tonioni/pfs3aio), read for facts:** `blocks.h` (layouts, ids, options), and for the
  numbering, the links, the root checks and the extra fields passages of `anodes.c`, `directory.c`, `volume.c`, `format.c`, `update.c`,
  `disk.c`, `assroutines.c`. Nothing of it is in the repository: no code, no struct definitions, no tables written as code; the
  server's names, layout and walking are its own, in the shape of `user/ffsd.c`. The numbers and layouts above are the format's facts,
  written out in words here because the server and the image maker were written from them.
- **Examining the bytes:** the root blocks and every other structure of volumes made by the real handler in FS-UAE (`pfs3-aos-test`),
  and read-only of the image of the owner's own card (four PFS3 partitions). That the reading is right is *verified* against those.
- **Not used:** any other PFS3 reader's code, the AmigaOS sources (clean room).
- The handler (Aminet `disk/misc/pfs3aio.lha`, 3.1) ran only inside FS-UAE and is not in the repository, nor are volumes made with it.

## Limits
Read-only. Files under 4 GB (decision 10). 64 open files a mount. A cache of 16 reserved blocks of 1 KB (at least 8 of any size) and one
data block. The system's path limits (`docs/limits.md`) matter on real volumes: over 63 bytes in one system call, 159 absolute. On the owner's card most names lie deeper than 63 bytes from `/mnt`: `cd` gets there, `cp -r` and `find` stop there with an error. Not changed here: it is the kernel's limit, for every filesystem. The largest formats (`PFS\2`, reserved blocks over
1 KB, blocks over 512 bytes, large files) are *read from the format and tested on `tools/mkpfs3.py`'s volumes only* unless the
hand-back of the branch says otherwise: the Aminet handler makes `PFS\1`. Comments and `.deldir` are not shown. Never run on a real Amiga yet.

## Tests
`pfs3-host-test` (the server on the PC against `tools/mkpfs3.py`: ten layouts, names, links, refusals, a fuzz run with ASAN and UBSAN
aimed at root, extension, index and anode blocks, chains, directory entries, extra fields, links), `pfs3-test` (FS-UAE: an RDB inside
an MBR type 0x76, `PFS\3`, `PDS\3`, 4 KB in the table, damage; afterwards not one byte changed). `pfs3-aos-test` (not in `make check`,
SKIP without the machines and the handler: AmigaOS 3.2 and pfs3aio make the volumes; every file compared).
