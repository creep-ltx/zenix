# ZenFS - the native Zenix filesystem (proposed, 2026-10-22; nothing is built)

Goal: a filesystem designed for slow machines and slow disks that never needs a check after a crash and
repairs what it can by itself. ext2 stays for Linux compatibility. The ideas below come from how PFS3 and SFS
are said to work (atomic commit, extents, indexed directories, a big metadata cache, delayed grouped writes);
I have not read their sources, and ZenFS is designed from the ideas, not from their code or formats. Read
their documentation and any open source before locking the format.

## Principles
- **Crash safety by atomic commit, not by a log.** Metadata is never overwritten in place: a change writes new
  blocks and then switches one root block. A crash leaves the old, consistent state. No journal replay, no check.
- **Self-healing.** Every metadata block has a checksum. Two root blocks (alternating, with a sequence number)
  so one bad root is survivable. The free-space map can be rebuilt from the extents. A background `scrub`
  verifies checksums and repairs from redundancy where it can.
- **Extents.** A file is a short list of (start, length) runs, not a block list. Fast for big files, small
  metadata.
- **Indexed directories.** Names found by hash or sorted blocks, not by scanning every entry.
- **Big metadata cache, delayed grouped writes.** Changes collect in RAM and commit together every few
  seconds or when asked (`sync`); one commit = one atomic root switch. Fewer, larger disk writes.
- **Native byte order.** Big-endian on disk, the CPU's own: no swapping.
- **Unix semantics:** permissions, owners, symbolic and hard links, 255-byte names, 32-bit unsigned seconds
  (good to 2106) or 64-bit.
- **Tools:** `mkfs.zenfs`, `fsck.zenfs` (verify; repair from redundancy), `scrub`, shown in `df`.

## Open design questions
- Block size (1-4 KB), maximum volume size, and how the free-space map is stored and cached.
- Data checksums (cost on a 68020) or metadata only. Probably metadata only.
- Snapshots / copy-on-write of data blocks. Probably later.
- How a ZenFS volume is found on an RDB partition (a DosType of our own, e.g. 'ZEN\1').
