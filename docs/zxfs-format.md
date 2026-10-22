<!-- SPDX-License-Identifier: GPL-2.0-only -->
# ZXFS on-disk format, version 1 (draft, 2026-10-22; implemented on the host only)

Rationale and goals are in `zxfs.md`. This file is the exact format. **All integers are big-endian.** Because of
that, two keys compare correctly with a plain `memcmp`. A *block* is `1 << bshift` bytes (bshift 10, 11 or 12: 1, 2
or 4 KB). Block numbers are 32-bit and count from the start of the volume (partition), block 0 first.

## 1. Layout
```
block 0                 superblock (written by mkfs; rewritten only to change the label or feature flags)
blocks 1 .. 8           commit ring: 8 slots, each one block
blocks 9 .. 9+2*NB-1    free-space bitmap, NB pairs (copy 0, copy 1) of bitmap blocks
blocks 9+2*NB ..        everything else: tree blocks and file data, in any order
```
`NB = ceil(total_blocks / (8 * (B - 32)))`, B = block size. Each bitmap block describes `8 * (B - 32)` consecutive blocks
(4 KB: 32,512 blocks = 127 MB). Blocks 0 .. 9+2*NB-1 are permanently marked used.

## 2. Block header (32 bytes, every block except file data)
| off | size | field |
|---|---|---|
| 0 | 4 | `csum` checksum of the block (see 3), computed with this field zero |
| 4 | 2 | `magic` 0x5A42 |
| 6 | 1 | `type`: 1 super, 2 commit, 3 bitmap, 4 tree leaf, 5 tree node |
| 7 | 1 | `level`: 0 for leaf, +1 per level above |
| 8 | 4 | `self` this block's own number (catches a block written to the wrong place) |
| 12 | 4 | reserved, zero |
| 16 | 8 | `gen` generation (transaction number) that wrote the block |
| 24 | 2 | `nitems` (leaf, node) |
| 26 | 2 | `dataoff` (leaf: offset of the lowest item data; others 0) |
| 28 | 4 | reserved, zero |

## 3. Checksum
Over the whole block, as big-endian 32-bit words, with `csum` taken as 0:
`sum = 0x5A454E46; for each word w: sum = rol32(sum, 1) + w;` (68020: `rol.l #1` and `add.l`). Any change to a single word is
always detected; two changes cancel with probability 2^-32. A block is valid only if `csum`, `magic`, `type` and `self` all match.

## 4. Superblock (block 0, after the header)
| off | size | field |
|---|---|---|
| 32 | 4 | `sb_magic` 'ZXFS' |
| 36 | 4 | `version` = 1 |
| 40 | 4 | `bshift` |
| 44 | 4 | `total_blocks` |
| 48 | 4 | `nb` bitmap pairs |
| 52 | 4 | `reserve` blocks kept free for unlink on a full volume |
| 56 | 4 | `compat` flags: unknown bits are ignored |
| 60 | 4 | `rocompat` flags: unknown bits mean mount read-only |
| 64 | 4 | `incompat` flags: unknown bits mean refuse to mount |
| 68 | 16 | uuid |
| 84 | 8 | creation time (seconds, Unix epoch) |
| 92 | 32 | label, NUL padded |
| 124 | 4 | `charset` of the names, a hint only: 0 = Latin-1 (also when unset), 1 = UTF-8 |

Defined flags (version 1): none yet. Reserved names: incompat bit 0 = data checksums, rocompat bit 0 = snapshots (not built).

## 5. Commit record (blocks 1..8, type 2, after the header)
| off | size | field |
|---|---|---|
| 32 | 4 | `root` block number of the tree root |
| 36 | 4 | `height` of the tree (1 = root is a leaf) |
| 40 | 4 | `free_blocks` |
| 44 | 4 | `next_ino` next unused object number |
| 48 | 8 | `time` commit time |
| 56 | 4 | `nbm` number of valid bits that follow (= nb) |
| 60 | .. | `bmsel`: bit i (byte i/8, bit i%8 from the least significant) says which copy (0 or 1) of bitmap block i is current |

The header `gen` of the record is the commit's generation. **Mount reads all 8 slots, ignores invalid ones, and uses the one with the
highest `gen`.** Commit number `g` is written to slot `1 + (g % 8)`. The bitmap needs `nb` bits, so `nb <= 8 * (B - 60)`.

## 6. Bitmap blocks (type 3)
After the header: `8 * (B - 32)` bits, bit `j` of block `i` (byte j/8, bit j%8) = block `i * 8 * (B-32) + j`; 1 = used, 0 = free.
A commit writes each changed bitmap block to the copy that is *not* current, then flips its bit in the new commit record. So the old commit's bitmaps are never overwritten.

## 7. The tree
One B+tree holds everything. **Key** = 12 bytes: `obj` (4), `kind` (1), 3 bytes zero, `off` (4). Order = memcmp order.

| kind | meaning | off | value |
|---|---|---|---|
| 1 INODE | the object | 0 | inode (below) |
| 2 DIRENT | names in directory `obj` | hash32(name) | one or more entries (below) |
| 3 EXTENT | file data run | first logical block of the run | `phys` (4), `len` (4) blocks |
| 4 INLINE | file data or symlink target, whole content, up to 192 bytes | 0 | the bytes |
| 5 ORPHAN | `obj` = 0: an inode unlinked while open | the inode number | empty |

**Leaf** (type 4): after the header, an array of `nitems` item headers (16 bytes: key 12, `off` 2, `size` 2) in key order, growing up; the
item values are stored from the end of the block downward. `off` is the value's offset in the block; `dataoff` is the lowest used offset.
**Node** (type 5): after the header, `nitems` entries of 16 bytes: the smallest key in the child (12) and the child's block number (4).
A search follows the last entry whose key is `<=` the search key. Maximum item value size: `(B - 32) / 4 - 16`.
Leaves are split when full and merged or freed when empty. A change to any block writes a new copy (copy-on-write) the first time in a
transaction; the parents are updated up to the root, and the new root goes in the commit record.

**Inode value** (60 bytes): `mode` 4 (Unix; type bits as usual), `uid` 4, `gid` 4, `nlink` 4, `size` 8, `mtime` 8, `ctime` 8, `flags` 4
(bit 0 = data is INLINE, bits 8..15 = Amiga protection bits), `parent` 4 (directories: the parent, for `..`), `rdev` 4.
Root directory = object 1. Object 0 is reserved for ORPHAN items. A directory's `nlink` is 2 plus its subdirectories.

**Directory entries**: a DIRENT value is a list of entries, each `ino` 4, `type` 1 (Unix file type), `namelen` 1, then the name (no NUL);
several entries only when two names have the same `hash32` (rare). `hash32`: FNV-1a over the name bytes, never changed in version 1.
A name is 1..255 arbitrary bytes except NUL and `/`; the filesystem does not interpret them. The superblock `charset` field says which encoding the volume's creator used (Latin-1 or UTF-8), so the operating system can convert at the boundary; the two encodings can even be mixed on one volume, since names are compared byte for byte.

**Files**: if `flags` bit 0 is set the content is the INLINE item. Otherwise it is the EXTENT items; a block range with no extent reads as zeros (sparse).
`size` is the length in bytes; bytes past it in the last block are zero.

## 8. Rules that make it safe
1. A transaction only ever writes blocks that were free in the last commit; it never writes any block the last commit uses.
2. Commit order: file data, tree blocks (children first), bitmap blocks, **flush**, the commit record, **flush**.
3. Blocks freed in a transaction are cleared in the bitmap of the same commit, but not reused until that commit is durable.
4. On mount: pick the newest valid commit record, check its root block. If the root fails its checksum, try the next older valid record (best effort: its blocks may have been reused).
5. `reserve` blocks are never given to a write that grows the volume's use; only unlink, rmdir and truncate may use them.
6. After mount: free the inodes listed as ORPHAN items (the open-but-unlinked files of a crash).
