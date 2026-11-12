# D42: the faster IDE

Roadmap block 6, "faster IDE" (docs/ROADMAP.md "The order of work"). Built on branch `ide`, 2026-11-12, in FS-UAE.
**Not yet run on a real A1200**: what to measure on the Apollo 1260 is section 6. No system call, message, error
number or file format changes.

## 1. What the owner gets, in plain words

Reading files from the CF card (or any IDE disk on the A600/A1200's Gayle port) asks the disk fewer times and moves
each sector with half the bus requests:

- the driver reads a sector with 128 long-word moves instead of 256 word moves, once it has checked at boot that the
  machine gives the same data that way (the boot line says "long reads"); writes stay word by word;
- it waits for the drive with one status read per look instead of seven clock reads as well;
- the ZenixFS server reads runs of whole blocks straight into the program's buffer (up to 64 KB per request) and keeps
  the last data block it read, so a program reading 1 KB at a time asks the disk once per 4 KB block, not four times;
- the ext2 server reads ahead: a file read in order is fetched up to a quarter of its cache (12 blocks) per request;
  and a whole block it writes is no longer read from the disk first.

In FS-UAE (emulator figures only, section 5) reading a 1 MB file got 2.2 to 5 times faster on a ZenixFS root and
1.5 to 1.8 times on ext2; writing is about the same (2-6% faster). On the 1260 the gain depends on the Gayle's real speed, which only the
machine can show.

## 2. What was slow (measured first, 2026-11-12)

What the driver did per sector (kernel/ide.c, before): a wait for DRQ that read the E-clock (`hr_time`, seven CIA
reads, about 10 us on any CPU) before the first status read and again at every look; 256 `move.w` in a C loop (a
`cmp`/`bne` per word); two CIA reads of settle time (ATA's 400 ns); levels 1-2 masked around the data. Per command:
the task file (six byte writes in Gayle's slow window), a settle, the acknowledge. Commands already covered up to 256
sectors (hand-off note I10), but nobody asked for more than one filesystem block:

| Layer | Before |
|---|---|
| `diskd` | passes a request's sector count to the kernel unchanged (no limit of its own) |
| ext2 (`fsd`, kernel/ext2.c) | one block per disk request, on every cache miss; no read-ahead; a whole block written was read from the disk first |
| ZenixFS (`zfsd`, zenixfs/zenixfs.c) | one block per disk request, data blocks never cached: `cat` reading 1 KB at a time read each 4 KB block four times |

A probe in FS-UAE (a test-only kernel timing `ide_read` at boot, not committed), 512 KB from the emulated drive:

| | 68020, 4 MB fast | 68060 |
|---|---|---|
| word loop, 64 KB commands | 336 ms | 159 ms |
| `move.l` loop, 64 KB commands | 237 ms | 92 ms |
| `move.l`, 1 KB commands (each one more command) | 384 ms | 290 ms |

So in the emulator the long moves are 30-40% less time per sector, and a command costs about 0.3-0.4 ms beyond its
data. FS-UAE's IDE has no real PIO timing, so neither number is the A1200's (section 6).

## 3. The driver (kernel/ide.c)

- **Long moves.** The drive's DA0-DA2 are on A2-A4 (Gayle spec p.8): A1 selects nothing, so a `move.l` from the data
  port at `$DA2000`, which the bus splits into two word cycles (A1 = 0 first, landing first in memory), reads the
  data register twice in order. `sec_xfer` reads a sector as 16 `move.l` per round, 8 rounds. The check at boot: on
  the Gayle port, after IDENTIFY has been read word by word as always (the drive is found and sized from that read
  alone), IDENTIFY is sent once more and read with `move.l`; the words must hash the same (IDENTIFY's data is never
  all one value, as a blank sector could be). Whatever a failed long read left of the sector is read out word by word
  so the drive ends the command; any failure keeps `move.w`, and the drive stays found. **Writes stay `move.w`**: a
  boot check can only prove reads, and an accelerator's bus logic could treat long writes differently (the
  independent review's point); writes are bound by the filesystems' own work anyway (section 5). The A4000 port
  always uses words (out of scope, untested). 68020-safe, no `movem` (it would step through the register
  addresses).
- **Fewer clock reads.** `ide_wait` looks at the status first; only after 16 looks that were not ready does it read
  the clock, and then every 16th look: the time-out is still the clock's (CLAUDE.md: every wait timed on the CIA),
  the drive is seen ready within a status read, and a sector already waiting costs one register read.
- **FLUSH CACHE** goes through the same command code (`ide_cmd` with no sectors): 150 bytes less, which paid for the
  rest. The kernel's ide part went from 3,328 to 3,404 bytes; kernel.bin from 190,012 to 190,684 (with fsd's growth,
  section 4), leaving 804 bytes on the mount tests' floppy, where 800 must stay free (tools/ksize-budget).
- **Not done: READ MULTIPLE / SET MULTIPLE MODE.** With polling it saves one status look and one settle per sector
  after the first of a block (about 4 us of a sector's time); many CF cards announce one sector per block anyway
  (IDENTIFY word 47), and the kernel had no bytes to spare (the mount tests' floppy, tools/ksize-budget). If the
  1260's figures show the per-sector waits matter, it is the next step: SET MULTIPLE MODE with the largest power of
  two up to word 47's count and 16, falling back to one sector when the drive answers ERR.

Unchanged: every wait has its time-out and says what it waited for; errors end the command with the sector it failed
at; the drive's interrupt stays off (polling); levels 1-2 are masked only around a sector's data.

## 4. The filesystems

- **ZenixFS** (`zenixfs_read`): a read from a block's start that wants two or more whole blocks takes the run of the
  file's extent from there (one extent lookup instead of one per block) and reads it straight into the caller's
  buffer with one request (`zenixfs_set_readn`, `zdisk_readn` in zfsd: up to 64 KB). A block held in the library's
  cache may be newer than the disk (written in this transaction): the run ends before it, and a cached block is
  copied from the cache as before. Run bounds are checked against the volume. Smaller reads keep using the scratch
  block `sc_r`, which now remembers which data block it holds; any write or new allocation of that block, a cached
  block copied through `sc_r`, and the repair walk that borrows `sc_r`, forget it. No new memory.
- **ext2** (`ext2_read`, `bfill`): a read that starts the file or goes on where the last read of the same file ended
  is sequential; when such a read meets a block that is not cached, that block and the ones after it that are its
  neighbours on the disk (and not cached) are read with one request into neighbouring cache slots, the ones whose
  newest use is the oldest; at most a quarter of the cache (12 of 48 blocks). A cached block ends the run, so no block
  is ever in two slots. Whole blocks written (a new block's zeros, a full data block) no longer read the old contents
  first. No new memory; `fsd` (built into the kernel) grew 504 bytes.
- `diskd` needed no change: it already passed any sector count through, range-checked against the partition.

What the emulator showed is mostly not in the IDE: on FS-UAE's 68020, 1 MB read through ext2 takes about 3 s, of
which the drive's data (PIO) is about 0.5 s and the commands about 0.3 s before and 0.03 s after; the rest is the
server's own work per block (the block map, the cache, the messages) and the copies. On the 1260's 68060 that work is
far smaller, so there the disk's share is larger; only the machine can say how large.

## 5. Figures (FS-UAE, emulated seconds; `tools/ide-speed-test.sh`, 1 MB file)

| 1 MB file | ext2, 68020 before | after | ZenixFS 4 KB, 68020 before | after | ZenixFS 4 KB, 68060 before | after |
|---|---|---|---|---|---|---|
| `cat` (1 KB reads) | 3.94 | 2.69 | 7.63 | 3.44 | 4.56 | 2.09 |
| `dd bs=65536` read | 3.03 | 1.64 | 1.91 | 0.62 | 1.24 | 0.25 |
| `cp` (1 KB reads and writes) | 67.3 | 64.9 | 15.9 | 11.8 | 9.76 | 6.94 |
| `dd bs=65536` write of zeros | 57.1 | 55.1 | 4.06 | 3.97 | 2.41 | 2.26 |
| `ls -lR /bin /etc` | 1.86 | 1.83 | 3.04 | 2.98 | 1.72 | 1.70 |

Seconds inside Zenix (`time`), one run each, FS-UAE 3.2 in warp mode (the emulated clock is unaffected), the 68020
cycle-exact with 4 MB fast, the 68060 approximate with 64 MB of 32-bit RAM. **ext2 writes stay slow** (about 20 KB/s
here): each 1 KB block appended costs five or six disk writes - the bitmap, the zeroed new block, the group's and the
superblock's free counts, the data, and the inode per request - because ext2's cache writes through. That is ext2's
design in Zenix, not the IDE's; batching those writes changes what a power cut leaves behind, so it is a question for
the owner, not part of this item. ZenixFS writes in commits and is about fourteen times faster at it.

## 6. On the Apollo 1260 (to measure)

The boot log first: the IDE line must end "long reads" (else the 1260 gives different data for long moves and
the driver uses words: say so). Then, as root on the CF card's ZenixFS root, five runs each:

    dd if=/dev/urandom of=/r bs=4096 count=256 ; sync
    time cat /r > /dev/null
    time dd if=/r of=/dev/null bs=65536
    time cp /r /r2 ; sync
    time dd if=/dev/zero of=/z bs=65536 count=16 ; sync
    sha256sum /r /r2 ; rm /r /r2 /z

The same on the release before this branch gives the before. The two SHA-256 sums must match. With the 64 KB reads
mostly the Gayle's PIO speed shows (1 MB / the time); with `cat` the commands per 4 KB block add to it.

## 7. Tests

- `tools/ide-speed-test.sh` (new, FS-UAE): three machines (ext2 on a 68020 with 4 MB fast; ZenixFS with 4 KB blocks
  on a 68020 and a 68060), 1 MB of random data written, then read with 1 KB and 64 KB reads, copied, 1 MB of zeros
  written; checks: the five timed commands ran, the copy's SHA-256 is the original's, the zeros read back right, the
  host reads the copy off the disk image with the same SHA-256, and `e2fsck -fn` / `zenixfs-tool fsck` are clean. The
  boot line "long reads" is checked too. The times are printed, never compared to a limit.
- `zenixfs/tests/ztest.c`: `basic` reads a 1 MB file over data, a hole and its last byte in one call and at odd and
  block offsets, also while its blocks are still in the cache unsynced, with the multi-block reads on, and a block read
  again after a cached block went through the read scratch (the review's finding: before the fix it returned the
  cached block's data); `random` runs half of each stretch with them on and checks they were used.
- One independent review of the code (2026-11-12): it found that bug in the read scratch (fixed, tested), advised
  word writes (done) and a boot check that cannot cost a drive found by the word read (done: the word read comes
  first, a long read's leftovers are read out).
- The existing suites: ext2-host-test, disk-host-test, zenixfs host tests, mounttest, zenixfstest, zroot-test, the
  fat/ffs/pfs3 tests, disktools, update-test, the matrix (results in the branch's report).
- Not tested: a drive whose long reads differ (FS-UAE gives the same data both ways; the check's other branch is
  only read); a real drive's timing.
