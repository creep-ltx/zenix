# D44: CD-ROMs - an ATAPI CD drive on the IDE port, ISO 9660 discs

Roadmap block 6, "CD" (docs/ROADMAP.md "The order of work", approved by the owner 2026-10-25; the details delegated,
decided by the builder, as D40-D43's). Revision 1, 2026-11-12, branch `cd`. Built and checked in FS-UAE and on the PC;
**nothing here has run with a real CD drive**: section 9 says what only a real drive can show. No change of meaning to
any interface: one new system call (98 `atapi`, the disk server only), one new capability bit (`DISKCAP_RO`), one new
file service (`/sbin/iso9660d`), all at the end of their tables.

## 1. In plain words

A CD-ROM drive on the A600/A1200's IDE port (beside the CF card or hard disk as the slave, or alone as the master) is
found at boot and appears as the disk device `cd0`. `mount cd0 /mnt` (or `mount -t iso9660 cd0 /mnt`) reads the
disc: an ISO 9660 CD as made by every PC and Amiga mastering tool, with the long Unix names, modes and symbolic links of
Rock Ridge when the disc has them, else Joliet's long names (the Windows discs), else the plain ISO names. Read-only,
as the medium. A disc taken out or changed while mounted answers "stale file handle" until it is unmounted and mounted
again. Any ISO 9660 volume on another device (an image written to a partition or to a whole CF card) mounts the same way.

The three parts are the three every disk already has in Zenix (architecture.md section 9): the kernel moves the bytes,
the disk server `diskd` owns the medium and serves it as a named device, a file service per mount reads the format.

| Part | What it does | Where |
|---|---|---|
| kernel | finds an ATAPI CD drive at boot; sends one packet command for the disk server (`SYS_atapi`), moves the data | `kernel/ide.c` |
| diskd | knows what the packets mean: the disc's size, a disc put in, taken out or changed, a drive spinning up; serves `cd0` in 512-byte sectors over the disc's 2048-byte blocks; read-only | `user/diskd.c` (built into the kernel image) |
| iso9660d | reads ISO 9660, Rock Ridge and Joliet; a file service per mount, as `fatd`, `ffsd` and `pfs3d` | `user/iso9660d.c`, `/sbin/iso9660d` |
| mount | the type `iso9660`, found by itself (CD001 at the volume's byte 32769); refuses any other type on a read-only medium | `user/mount.c` |

## 2. Decisions (made here, the owner delegated the details)

1. **No new path to the hardware.** The file service never touches the drive: it reads `cd0` through diskd's
   `DISK_READ` like any server reads any partition. diskd is still the only process that may use the raw calls
   (`current->priv`, the built-in images); `SYS_atapi` is one more of them.
2. **The kernel's part is a packet mover, nothing more.** It finds the drive, sends a 12-byte packet, takes the data
   (never sends any: nothing here writes to a disc) and answers the byte count or the drive's error register; a drive that
   stops answering or sends nonsense gets DEVICE RESET (08h), so the next command does not wait out the same state. Which
   packets to send and what their answers mean is diskd's (cold code, user space, -Os). The kernel does not know what a
   READ (10) or a sense key is.
3. **`cd0` counts 512-byte sectors**, as every device does (`struct diskinfo`'s `sectors`, frozen). The drive's blocks
   are 2048 bytes; a read of any sectors is one READ (10) of the blocks around them, and the kernel keeps exactly the
   bytes asked for (`skip`, `len`: the rest of the transfer is read off the port and dropped, no bounce buffer). So
   `dd`, `vol_id`, mount's type probe and the servers work on `cd0` unchanged.
4. **Read-only throughout.** `DISKCAP_RO` (8) in `cd0`'s capabilities; `DISK_WRITE` to it answers `EROFS`; `mount`
   refuses any type but `iso9660` on it and `-o rw`; `iso9660d` has no write code and answers `EROFS` to every change
   after the checks a writable volume would make (a link leading out of the volume still bounces, `ENOENT`, `EEXIST`,
   `EACCES` come first), as `pfs3d` does.
5. **Changed discs.** Every UNIT ATTENTION (a disc put in, a reset) and every look that finds no medium (ASC 3Ah)
   raises diskd's disc generation (nothing else does: a failed READ CAPACITY or a drive still starting is not a change);
   the server that mounted `cd0` before it gets `ESTALE` from then on ("stale file handle": `umount` and mount again),
   and `iso9660d` stops reading. Any other reader reads whatever disc is in. The tray is never locked (no PREVENT
   MEDIUM REMOVAL): the eject button works at any time, and the mount then goes stale. No `eject` command (not asked).
6. **Waiting for a disc.** A drive "becoming ready" (sense 2, ASC 04h ASCQ 01h: a disc just put in, spinning up) is
   waited for at an open of `cd0`, up to 10 s (one that asks for it, ASCQ 02h, is sent START UNIT first), by diskd with `sleep` (TEST UNIT READY every 0.1 s): no process waits in the kernel for
   it. A READ that finds the disc spun down waits in the kernel, as an IDE disk's read from standby does (up to 30 s;
   polling, the machine pauses meanwhile): a limit of the polled driver, said in limits.md.
7. **Which names.** Rock Ridge when the root's first record carries SUSP's `SP` mark (and not `-o norock`): `NM` names
   (bytes, as Linux; UTF-8 from today's tools), `PX` permission bits, `SL` symbolic links, `CE` continuation areas,
   `CL`/`PL`/`RE` (a tree deeper than 8 that the mastering tool moved under `rr_moved`: found where it was, the moved
   copy hidden; `rr_moved` itself is listed, as on Linux). Else Joliet when a supplementary descriptor has a Joliet
   escape (`%/@`, `%/C`, `%/E`) and not `-o nojoliet`: UCS-2 names as UTF-8. Else the primary names: `;1` cut, one
   trailing dot cut (`README.` is `README`), Latin-1 as UTF-8. Rock Ridge names are exact; Joliet and plain names are
   found in any case (ASCII). `/` and control characters in a name are shown as `_`. A name over 63 bytes of UTF-8
   does not fit `udirent`: listed as its start, `~` and its inode number in hex, which opens it (as ffsd and pfs3d).
8. **Modes and owners.** With Rock Ridge, `PX`'s permission bits, without set-user/set-group-id (a disc is not to be
   trusted with them); owner and group always from `-o uid=,gid=` (default 0): a disc's numbers are another machine's
   users. Without Rock Ridge, `-o fmode=` and `-o dmode=` (default 555 both, as Linux's isofs: programs on a disc run).
9. **Inode numbers** are half the byte position of the object's directory record on the volume; a directory's is its
   `.` record, the first record of its extent. They are small positive numbers below 2^31 for volumes up to 4 GB, never 0,
   and name the record again after a restart of the server: no table of open files, no stale files after a restart
   (unlike ffsd and pfs3d). A volume over 4 GB (a DVD) is refused, said.
10. **Not read, said:** files in several extents (only made for files of 4 GB or more) and interleaved files are listed
    and answer `EOPNOTSUPP` at open; logical blocks other than 2048 bytes are refused at the mount; a multi-session
    disc shows its first session (the volume descriptors at block 16; the last session's would need the drive's table
    of contents and an option); associated files are not listed; El Torito boot records, the path tables, extended
    attribute records (skipped for a file's data, refused on a directory), Rock Ridge's `TF` times (the record's date
    is used), `PN` device numbers and the Amiga's own `AS` protection bits are not read.
11. **Untrusted disc.** Every number is checked before use: a record's length (even, at least 34, inside its block, its
    name inside it), every extent and size inside the volume, a directory at most 16 MB (about 100,000 names; a
    listing's place is 31 bits) (an empty file's extent is not looked at: tools write 0
    there), a directory's first record its own `.` naming its own extent, every SUSP entry's length inside its area, at
    most 4 continuation areas per record, a link's text without NUL and at most 255 bytes, at most 40 links followed per
    request. A damaged record ends its block for that listing, which goes on and answers `EIO` at its end. Damage
    fails a request, never crashes or hangs the server (the host test's fuzz, ASAN and UBSAN).
12. **The hardware it looks for:** only the A600/A1200's Gayle port (where the owner's machines have IDE). The A4000's
    port and the A3000's SCSI are not probed for CD drives (no machine to test them; the A4000 port keeps its one-sector
    ATA commands, d42). The CD as the slave beside an ATA master, or as the master with no ATA drive. A second ATA drive
    as the slave is left alone, as before.

## 3. The kernel: ATAPI on the Gayle port (`kernel/ide.c`)

**Finding the drive** (`cd_probe`, at boot, after the ATA master is set up, or when no ATA drive answered): select the
unit, send IDENTIFY DEVICE (ECh). A packet device aborts it and puts its signature, 14h EBh, in the cylinder registers
(ATA/ATAPI); an ATA drive shows DRQ; a unit that is neither busy
nor answering 50 ms after the command is no unit (an absent slave reads 0, or the master's status); busy is given 10 s,
before the command and after it (a drive spinning up at power-on); status FFh or busy with ready (a floating bus) is no
unit. An ATA drive's answer (DRQ) is read out, so nothing is left half done (a CF card may answer for an absent slave). Then IDENTIFY PACKET DEVICE (A1h): word 0 must say ATAPI, a CD-ROM (type 5), 12-byte
packets. The boot log says `ide: ATAPI slave CD drive: MODEL` (or master, or "not a CD drive: not used"). Every register
write that selects the CD selects the master again before the driver returns, so the ATA commands find the master
selected, as before. nIEN (the drive's interrupt off) was already written to the device control register, which both
units share; Gayle's IDE interrupt stays off: polling only, as for the ATA drive.

**`SYS_atapi` (98)**, for the disk server only (`-EPERM` otherwise; `struct uatapi` checked as the caller's, the data
buffer is a client's, as `blk_read`'s): `struct uatapi` (`kernel/diskproto.h`, frozen, first size 28) is the packet
(12 bytes), the buffer, `len`, `skip` and `err` (back). The PACKET command (A0h) with the byte count limit FFFEh, PIO,
then the 6 words of the packet when the drive asks for them (DRQ). The phases are told by the interrupt reason register
(ATAPI): data in (DRQ, IO 1, CoD 0): the drive's byte count from the cylinder registers is read off the data port,
bytes `[skip, skip + len)` of the whole transfer kept (the buffer may be at an odd address: a 68020 moves words anywhere,
and a program's read at an odd file offset lands there) in the buffer, whole 512-byte pieces inside that window with the
long moves of d42 (`sec_xfer`), everything else word by word, kept or dropped; the status phase (no DRQ, IO 1, CoD 1)
ends it. A drive that does not set the reason register ends after 50 ms in a row with neither busy nor DRQ (counted
from the first such look, not from the command: a long busy spell does not end it early). Anything else is
waited past: FS-UAE shows neither busy nor DRQ for a moment after the packet before it has taken it (found in the
emulator test: without this, a READ CAPACITY was taken as finished with no data). Answers the bytes the drive sent;
`-EIO` with the error register in `err` (the sense key in bits 7-4) when ERR is set at the end; `-EIO` with `err` 0
when a wait ran out (said on the log, as every IDE wait); `-ENODEV` without a CD drive; `-EINVAL` for an odd `len` or
`skip`. More than 64 MB in one command, or a phase of 0 bytes, is a drive gone wrong: `-EIO`. After a wait ran out or
such nonsense the drive gets DEVICE RESET (08h): the next command finds it ready (and a UNIT ATTENTION, which diskd
counts as a changed disc). Levels 1-2 are
masked only around 512 bytes at a time, as ide_cmd's sectors.

**Bytes (the binding limit is the mount tests' floppy, tools/ksize-budget):** `ide.o` 3,404 -> 4,671 (+1,267: the
probe about 450, the packet command about 700, the model text shared with the ATA line), `syscall.o` 10,050 -> 10,108
(+58), the built-in `diskd.zx` image 8,064 -> 8,876 (+812, section 4); `kernel.bin` 190,684 -> 193,072 (+2,388). No other part of the
kernel had room left at the floppy's 800-byte margin (the cold parts are -Os already), so the mount tests' RAM disk
gave 3 KB (692 -> 689 KB: 110 of its blocks stay free, mounttest needs 16; 1,488 bytes of the floppy's margin left). For
a running system: 2.4 KB more of kernel in
RAM on every machine, a CD drive or not; nothing else until `cd0` is mounted.

## 4. diskd: `cd0`

- At its start diskd asks for TEST UNIT READY: `-ENODEV` means no CD drive, and nothing else changes. With a drive,
  `cd0` is listed always, with the disc's size from READ CAPACITY ((last block + 1) x 4 sectors), or 0 sectors when no
  disc can be read (`mount` then says "no disc in cd0"). Capabilities `DISKCAP_RO`, at most 128 sectors (64 KB) per
  request. One slot for the drive, whatever disc is in it (the size changes with the disc).
- An open of `cd0` looks again (as an open of `df1` probes that floppy drive): UNIT ATTENTION is counted and asked
  past (up to 100 times), NOT READY with ASC 04h (becoming ready, from REQUEST SENSE) is waited for, 0.1 s at a time,
  at most 10 s (ASCQ 02h: START UNIT first); NOT READY with ASC 3Ah is no disc (the generation goes up); any other
  failure is 0 sectors without a change.
- `DISK_READ` of sectors `lba .. lba + n - 1`: one READ (10) of blocks `lba / 4 ..`, `(lba % 4 + n + 3) / 4` of them,
  keeping `n x 512` bytes from byte `(lba % 4) x 512`; anything but the whole transfer is `-EIO`; more than 128 sectors
  `-EINVAL`. A UNIT ATTENTION there raises the generation; the read is tried once more for any other reader, answers
  `-ESTALE` for the server that mounted `cd0`.
- The saved table (D11) knows the CD's medium number (6); a restarted diskd keeps the claim, and the mount reads on.
  The disc generation is not in the table: a disc changed and its UNIT ATTENTION used up just before diskd ended would
  be read by the old mount (a crash at that moment; said here, not guarded).
- What it costs the disk server: 812 bytes of its image; 28 bytes for the request, 80 for the slots' generations.

## 5. iso9660d (`user/iso9660d.c`)

`mount -t iso9660 DEV DIR` (or a device whose block 16 has CD001 at its bytes 1-5) registers `fs.DEV` ->
`/sbin/iso9660d DEV -id START,SIZE [-o OPTIONS]` at nice -10 (a file service, D11 6.4), in the `disktools` package
beside fatd, ffsd and pfs3d. One process per mount, the protocol of `kernel/fsproto.h`; it answers `FS_LINKED` for Rock
Ridge's symbolic links (D26) and follows them inside the volume for a request without `UMSG_LINKS` (an absolute target
from the volume's root). Only reads: nothing but `DISK_OPEN`, `DISK_INFOSZ`, `DISK_READ` and `DISK_UNMOUNT` goes to diskd.

**The mount:** the volume descriptors from block 16 (up to 64, to the terminator): the primary (type 1) and a Joliet
supplementary one (type 2 with its escape); logical block size 2048; the volume's size the smaller of its own and the
device's; the root record (primary byte 156) and its `.` record; then Rock Ridge, Joliet or plain (section 2, 7).
Refused with the reason in dmesg (`iso9660d: DEV: ...`): no disc, too small, not ISO 9660 (no CD001 at block 16), no
primary descriptor, other block sizes, over 4 GB, a damaged root, a damaged Rock Ridge mark.

**The format as read** (ECMA-119; all both-endian numbers read in their big-endian halves): a directory record is
length (byte 0), extended attribute length (1), extent (6, big-endian half), size (14), date (18: years since 1900,
month, day, hour, minute, second, offset from GMT in 15 minutes), flags (25: 1 hidden, 2 directory, 4 associated, 128
not the last extent), interleave unit and gap (26, 27), name length (32), name (33), a pad byte when the length is
even, then the system use area (Rock Ridge, after the `SP` entry's skip count). Records never cross a block: a length
byte 0 ends the block's records. The first two records of a directory are `.` and `..` (names 00h, 01h). SUSP entries:
two letters, a length, a version, data; `CE` gives block, offset and length of the next area (both-endian, big halves at
8, 16, 24); `NM` flags at 4 (2 current, 4 parent: ignored), name from 5; `PX` mode at 8; `SL` flags at 4, then components
(flags: 1 continues, 2 `.`, 4 `..`, 8 root; length; text) joined with `/`; `CL`, `PL` a block at 8; `RE` no data.

**Reading:** a cache of 6 directory blocks (12 KB) and a window of up to 8 of a file's blocks (16 KB) for small reads;
a read of whole blocks goes straight into the caller's buffer, 64 KB at a time. A file is never opened in a table: its
inode number finds its record again, a block the cache most likely holds. Size: `iso9660d.zx` 16.6 KB on the disk; a
mount takes about 28 KB of buffers, its 8 KB stack and the program.

**Options:** `uid=`, `gid=`, `fmode=`, `dmode=` (octal), `norock`, `nojoliet`, `ro`, `defaults`, `noauto`; `rw` refused.
They travel in the service's text (80 bytes, frozen), so at most about 35 characters of them (`mount` says "too many
options" beyond).

## 6. mount

The type `iso9660` (`-t`, `/etc/fstab`); without `-t`, after the partition types, a device whose descriptor at block 16
says CD001 is `iso9660` (so `mount cd0 /mnt` and an ISO image on a partition both mount by themselves). On a device with
`DISKCAP_RO` any other type is refused ("cd0 is read-only: only -t iso9660"), and with no disc "no disc in cd0". `-o rw`
is refused for `iso9660` as for `pfs3`.

## 7. FS-UAE: an ATAPI CD on the A1200's IDE port

FS-UAE (3.2) has an IDE CD-ROM on the A1200's Gayle with two lines in the config:

    cdrom_drive_0 = /path/to/image.iso
    cdrom_drive_0_controller = ide1

FS-UAE turns them into WinUAE's `cdimage0=IMAGE,image` and `uaehf1=cd0,ro,:,0,0,0,2048,0,,ide1` (its log says so): the
CD as unit 1, the slave of the A1200's port, beside `hard_drive_0` with `hard_drive_0_controller = ide0`, the master. The
drive answers as `UAE-ATAPI`. `ide0` instead would make it the master (with no hard drive on the port). Without the
controller line FS-UAE attaches no CD to an A1200 (the CD32's and CDTV's drives are other hardware). `tools/cd-test.sh`
writes these lines into a copy of `fsuae/a1200-020-fast.fs-uae`.

What the emulator showed: the probe and every command work as written; FS-UAE leaves a moment with neither busy nor
DRQ after the packet (section 3), which is why the phases follow the interrupt reason register.

## 8. Tests

- **`tools/iso9660-host-test.py`** (host, ASAN + UBSAN; SKIP without bsdtar): images made by the PC's `bsdtar`
  (libarchive; xorriso and genisoimage are not installed here): Rock Ridge (names, bsdtar's modes, strict modes and
  set-id dropped, symbolic links, a 12-deep tree moved under `rr_moved`, a 205-byte name in a continuation area),
  Joliet only (a Swedish name, a long one), plain ISO names; every file read in odd pieces and its size and CRC-32
  compared with the tree it was made from; a directory of 260 entries over many blocks; permissions; every change
  `EROFS`; D26's `FS_LINKED` answers; nothing written (the image opened read-only, the writes counted); six volumes
  refused at the mount; aimed damage (an extent and a size off the volume, a record odd, across its block, a name past
  its record, a directory onto a file, a continuation area that loops, a SUSP entry of length 0 and past its area, a link
  component past its entry and with a NUL), interleaved and multi-extent files; a fuzz run (600 volumes by default) over
  the descriptors, directories and Rock Ridge areas: no crash, no hang.
- **`tools/disk-host-test.sh`**, its D44 part: diskd against a model of the drive (`tools/host/diskd-host.c`, `HOSTCD`):
  `cd0` listed read-only with its size, sectors inside a block, 128 at once and the last ones are the image's, past
  the end refused, writes `EROFS`, no raw open while mounted, a changed disc `ESTALE` and staying so, the new disc read
  after a new mount, a drive becoming ready waited for, no disc 0 sectors, a disc put in under a mount `ESTALE`, a
  restarted diskd keeping the claim, no drive no `cd0`.
- **`tools/cd-test.sh`** (FS-UAE, an A1200 with 4 MB of fast RAM, the CD as the slave holding a bsdtar image): the boot
  line; raw reads of `cd0` (three sectors inside two blocks, and the whole disc with `dd`) with the PC's SHA-256 sums;
  `mount cd0 /mnt` by itself; a file, a Swedish name, a 300 KB file, a program and a 2049-byte file against the PC's
  SHA-256; links; the deep tree; 120 entries; `..` inside the disc; the program run from the disc; writes, `mkdir`, `rm`
  refused; the server killed and reading on; no raw open while mounted; `-o rw` and `-t ext2` refused; `-o norock`
  (Joliet's names, no links), plain names with the options' owner and modes, found in any case; the image unchanged.
- Run for what this touches: mounttest, ide-speed-test, zenixfstest, fat-test, pfs3-test, disk-host-test (results in
  the branch's report).

## 9. What only a real drive can show

FS-UAE's ATAPI is a model; real drives differ, and IDE CD drives on an A1200 need a 44-to-40-pin adapter with power
(most owners use a buffered "4-way" interface for a 3.5" drive beside a 2.5" disk or CF card). To check on the A1200
(the 1260 with its CF card is the natural machine; the stock A1200 the floor), with a CD drive as the slave:

1. The boot log has `ide: ATAPI slave CD drive: MODEL` and the CF card's line unchanged with "long reads"; the boot is
   not slower (the probe of an absent slave is at most 50 ms).
2. `fdisk list` (or `mount`) shows `cd0` with the disc's size; `dd if=cd0 of=/tmp/x bs=2048 count=64` reads.
3. `mount cd0 /mnt; ls -l /mnt; sha256sum` of files whose sums are known (a disc burnt on the PC with `bsdtar` or
   xorriso, the sums taken there).
4. The eject button while mounted: the next read says "stale file handle"; `umount /mnt`, a new disc, `mount` again.
5. A disc put in and `mount` at once: diskd waits for the drive to spin up (at most 10 s).
6. The CD drive alone as the master (no CF card; boot from the floppy): the boot line says master. Here the ATA probe
   before it may wait up to 10 s for a status that is 0 (ide_present's spin-up rule, I3), a cost only on such machines.
7. What is not known until then: whether old drives abort IDENTIFY DEVICE within 50 ms when idle; whether they set the
   interrupt reason register (else every command ends 50 ms after its data: slow but right); the byte counts a real
   drive gives per DRQ; the long moves of a CD's data through the Gayle on the 1260; a READ waking a spun-down disc
   pausing the system for its spin-up.
8. Speed: no figures from FS-UAE (CLAUDE.md). On the 1260, `time dd if=cd0 of=/dev/null bs=65536 count=64` is the
   first figure to take.

## 10. The review (one independent review, 2026-11-12)

It found, and these were fixed: an even-buffer rule in `SYS_atapi` that made a read at an odd file offset fail on `cd0`
(dropped: the buffer may be anywhere; the host test now reads into an odd buffer); the 50 ms quiet counted from the
command instead of from the first quiet look; the probe leaving an ATA unit's IDENTIFY data unread (a CF card answering
for an absent slave would have had a command left open), not waiting for a busy unit before the command, nor refusing
a floating bus; no reset of a drive that stopped answering (every later look would wait 30 s); a failed READ CAPACITY
counted as a changed disc; waiting on any "not ready" ASC 04h (now only ASCQ 01h, and START UNIT for 02h); the
128-sector limit not enforced; `iso9660d` reading a record's name length before knowing the record is inside its block;
directories over 2 GB colliding with the listing's damage bit (now at most 16 MB); `ESTALE` given to every reader of a
mounted `cd0` instead of only its server. Left as said limits: the disc generation not saved across a diskd restart
(section 4); a restarted `iso9660d` takes the disc in the drive as its own (its volume id is the place only, as fatd's,
ffsd's and pfs3d's: a disc swapped while the server was down is read with the old inode numbers, and answers errors or
another disc's files until it is mounted again); boot's root picker looks at 16 devices, and `cd0` now takes one of them
on a machine with a CD drive (a 15th partition as the root would no longer be found).

## 11. What is not done

Audio CDs (no audio playback, no CDDA reading); writing CDs; the A4000's port and SCSI CD drives; a drive on the PC
Card slot; multi-session's later sessions; Joliet and Rock Ridge together (Rock Ridge wins); UDF (DVDs); an `eject`
command and the tray lock; the drive's own interrupt (polling only, as the whole IDE driver).
