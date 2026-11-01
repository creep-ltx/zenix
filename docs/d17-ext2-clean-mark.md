# D17: the ext2 clean mark and the check at boot (design note, revision 2)

Status: **revision 2, 2026-10-26, after two independent reviews (section 13); built on branch `fsck` (section 14,
not yet merged).**
Closes the rest of found items F5 and F6 of [audit-2026-10-26.md](audit-2026-10-26.md) (a fix, not a feature: no new
system call, message type or error number). The short version is in [DESIGN.md](DESIGN.md) D17. Facts about today's
code are *read* from main (`bba5232`); numbers marked *measured* come from host builds of the real code on image files.

## 1. The problem

ext2 has no journal. Since F5 every request writes its blocks in an order that leaves **at worst leaks** when a power
cut stops it half way (a count one too high, an unnamed inode, blocks or inodes marked in use that nothing holds, free
counts off): measured over all 2212 cut points of an update, 0 dangerous left. Two things are still missing:

1. **F6: the mark.** The superblock's state word (`s_state`, byte 58; bit 0 "cleanly unmounted", bit 1 "has errors")
   tells e2fsck, Linux and Zenix whether a volume needs a check. Zenix clears bit 0 only after a server crash in a
   request (D11) and while zup installs (`FS_MARK`, D15). A cut during ordinary use (`cp`, an editor saving) leaves its
   leaks on a volume that says clean; `e2fsck -p` on a PC skips it.
2. **F5: the repair.** Nothing repairs a marked root. Today only `fsck.ext2 -y` from the RAM-disk Zenix or e2fsck on a
   PC does. Leaks cost only space, but they pile up, and a count one too high keeps a deleted file's blocks for ever.

Constraints (read): the root server `fsd` (kernel/ext2.c in user mode) is built into the kernel image and holds the
device (`DISK_MOUNT`: diskd then refuses `DISK_RAW`, which fsck.ext2 needs); fsck.ext2 lives on the root itself
(`/bin/fsck.ext2`, 37 KB on disk, about 104 KB in memory with its buffers); the kernel image is 171660 bytes of the
179200 the RAM floppy allows (7540 left); the root is never unmounted; an A1200 may have 2 MB of chip RAM only and a
14 MHz 68EC020; Amiga owners switch the machine off rather than shut it down.

## 2. What others do

- **Linux (ext2):** a read-write mount clears bit 0 at once, a clean unmount (or remount read-only) sets it after the
  last write. At boot the root is mounted read-only (or checked from the initramfs before it is mounted), `fsck -p`
  (e2fsck "preen": fixes what is safe without asking) runs on every volume that is not clean, then the root is
  remounted read-write; if fsck changed a mounted root, the machine reboots so no stale cache survives. Every power-off
  without a shutdown means a full check at the next boot.
- **The BSDs (UFS/FFS):** a `fs_clean` flag, cleared at a read-write mount and set at unmount; `fsck -p` at boot for
  every dirty volume, the root first and read-only; FreeBSD with soft updates can check in the background later.
- **AmigaOS (FFS)** (AmigaOS 3.1 study, aos31-study/dos-and-filesystems.md): the first change after a quiet period
  writes the root block with the bitmap marked invalid; a timer fires every second, and when nothing is pending the
  buffers, the bitmap and the root block are flushed and the bitmap is marked valid again. A volume mounted with an
  invalid bitmap starts the validator, which rebuilds the bitmap from the directory tree. So a crash only costs a
  validation if it came within about a second of writing; switching off an idle Amiga costs nothing.
- (Linux, BSD: from their documented behaviour, not re-read for this note.)

## 3. F6: when the volume is marked

Options:
- **a. Linux's: not clean from the read-write mount to the unmount.** The root is never unmounted, so only `reboot` and
  the reset warning (both sync, `vfs_sync`) could mark it clean again; every switch-off would mean a check at the
  next start.
- **b. Around every request:** two more block writes and flushes per request; doubles the cost of small writes.
- **c. FFS's: not clean from the first change after a quiet moment until one second after the last change.** Chosen.

Why (c) is enough: fsd is one process that serves one request at a time and writes through (kernel/ext2.c has no
write-back cache). **Between two requests everything a request wrote has been sent to the device, and since F5 a
request that completes, or is cut by power, leaves at worst leaks**, except for one thing a finished request leaves on
purpose: a file whose last name went while it was open keeps its inode and blocks until the last close (an "orphan";
a cut leaves it as a leak). A request whose own block write or read failed may stop half way (ext2.c's give-back paths
can themselves fail): that is not a quiet moment, it is a need for a check (below).

fsd (`user/fsd.c`) keeps, for a writable ext2 volume:
- `marked`: bit 0 is clear on the disk.
- `need` (sticky; nothing but a check clears it): the volume needs a check. Set when it was already not clean when this
  fsd mounted it (a check failed or was not run, or a server before ended with it marked), at a restart after a crash
  in a request (D11, as today), when a block write fails, and when ext2.c met damage (its `damaged()` count).
- `hold`: zup's `FS_MARK 1` (section 5).
- the orphans: fsd's `open_busy` is called by ext2.c exactly when an open inode loses its last name (it is then kept),
  and the open entry is flagged (a spare bit of its count); the flag goes at the last close (which frees the inode). A
  restarted fsd checks the link count of each inode the kernel hands it (`SVC_OPENS`) and flags those with none.

The rule:
- **Mark (before the change).** Before a request that may write, if bit 0 is set on the disk: clear it (one superblock
  write through ext2.c's cache), `DISK_FLUSH`, then serve the request. If the mark or its flush fails, the request
  fails with -EIO without changing anything. Requests that may write: `FS_WRITE CHMOD CHOWN UTIMES LINK SYMLINK MKDIR
  UNLINK RMDIR RENAME`, `FS_OPEN` with `O_CREAT` or `O_TRUNC`, `FS_CLOSE` while an orphan is open. A read-only mount
  never marks or writes (its writing requests are refused with -EROFS first, as today).
- **Clean (one second after the last change).** When `marked` and neither `need`, `hold` nor an orphan: one second
  after the last writing request (requests that only read do not delay it, as zfsd's commit timer counts from the first
  change, not the last request), between two requests: `DISK_FLUSH`, set bit 0, `DISK_FLUSH`. fsd waits for its next
  request with a timeout (`SYS_recv` with N > 1, as zfsd does) so the clean comes on time when nothing arrives. The same
  at `FS_SYNC` (the `sync` command, `reboot`, the reset warning) and at `FS_QUIT` (umount, after the orphans are freed).
- The `-fsck` server (section 4) is read-only and never marks.

What is on the disk, at each moment (bit 0 of `s_state`):

| moment | bit 0 | the volume |
|---|---|---|
| mounted, nothing written since the last clean point | 1 | consistent |
| a writing request arrives: the mark written and flushed | 0 | consistent |
| serving it, between two of its block writes (a cut here: leaks) | 0 | leaks possible |
| between requests, < 1 s after the last change, or an orphan open | 0 | consistent, or an orphan |
| 1 s after the last change: flush, bit set, flush | 1 | consistent (also on the medium) |
| `reboot` (each server has 5 s, A7), the reset warning (`KBD_RESET_MAX` for all): `FS_SYNC` | 1 (unless `need`, `hold` or an orphan) | consistent |
| `need` or `hold` | 0 | whatever it is |

**The invariant: bit 0 is never 1 on the medium while the volume is inconsistent there.** Every write is preceded by a
durable mark; the bit is set only after a flush of everything before it, only between requests, only without orphans,
and never after a failed write or met damage. One limit: a disk that cannot flush (`DISK_FLUSH` answers -95, no
`DISKCAP_FLUSH`) and reorders writes in its own cache gives no ordering guarantee; fsd treats -95 as "everything was
sent", as it does today (zfsd says so at mount; the A1200's IDE and SD paths write in order).

Cost: per burst of writes one superblock write and a flush at the start, one write and two flushes at the end. The
superblock is already written by every allocation (its free counts), so this is no new kind of wear on an SD card. A
power cut within a second of writing (or during a long copy) means a check at the next start; an idle switch-off does
not, exactly as on AmigaOS. A file kept open after its deletion (a daemon's log, say) keeps the volume marked until it
is closed.

Other volumes: the same code serves every ext2 mount (`fs.DEVICE`), so `/home` gets the same mark. The RAM-disk root
gets the marks too (harmless: it is rebuilt from the floppy at every start; boot never checks it).

## 4. F5: who repairs, and when

Options:
- **a. fsd repairs before it serves** (the check inside the root server): fsck.ext2's code is 37 KB; the kernel image has
  7.5 KB left. No.
- **b. A second fsck built into the kernel image:** the same size problem. No.
- **c. fsck run on the bare device before any root server:** needs the program from somewhere other than the root: no
  room in the kernel image, Zenix reads no FFS (the boot partition), and zenixboot runs in AmigaOS (68k code that would
  have to carry its own ext2 checker). No.
- **d. Linux's classic order: the root served read-only, fsck from it, then the writable server.** Chosen. Linux reboots
  after repairing a mounted root because its caches could be stale; Zenix does not need that: every cache of the
  volume lives in the server process, a new server starts with none, and a new server's hand-over gives the mount a
  new generation, so no program code loaded through the check server is reused (D13; read in svc.c and vfs.c). A
  reboot on an Amiga goes through Kickstart and the boot menu, much slower than a server start.
- **e. A targeted repair by zup** (recount the files its journal names): rejected in F5 (fixes about 60 of the 1830
  cuts; leaked blocks have no name).

The boot order (`user/boot.c`, built into the kernel image, pid 1), only when the root is ext2, not the RAM disk, and
its bit 0 is clear (boot already reads the superblock's first sector to choose the server, `user/volid.h`; the state
is in that sector at byte 58 for every block size):
1. boot registers `fs` as `@fsd DEV fs -fsck -id ID` instead of `-root` (the same length and word count: six words,
   the `SVC_ARGS` limit; at most 77 characters). With `-fsck` fsd applies the root's rules (a volume Zenix could not
   write, or one marked "has errors", is refused and the boot stops with D11's panic, as today) but serves it
   **read-only and opens the device without `DISK_MOUNT`**, so diskd allows a raw open. boot's first call (the root
   inode) starts it.
2. boot prints `boot: the root DEV was not shut down cleanly: fsck.ext2 -y DEV` and runs `/bin/fsck.ext2 -y DEV`,
   loaded (in boot's own context, D11) from the read-only root; it runs from memory. Nothing else runs yet (init, zup
   and the svctab services start later), so nothing reads the root meanwhile. fsck.ext2 prints its own findings and
   result. boot waits as for zup: a word after 1 minute, the check stopped after 20 (a cut check is safe, section 8).
3. boot sends the check server `FS_QUIT` (raw, root may; the server lets the device go and ends with status 0, which
   D11 counts as a stop), then registers `fs` again with the `-root` text. A changed text starts the service's counters
   again, so the next server is a first start; **the kernel now also forgets that the service had served** (svc.c), so
   a writable root server that fails to start gives D11's clear panic "an essential server ended before it served",
   as at any boot. The next call to `fs` (boot's own `sys_spawn` of `/sbin/zup`) starts the writable server, which
   reads everything fresh from the disk.
4. boot ends with one line if the check did not end well: `boot: fsck.ext2 ended with status N: the root is used as it
   is and stays marked "needs a check"` (4 errors it does not repair, 8 out of memory or a read error, stopped), or
   `boot: cannot start /bin/fsck.ext2`. In every case boot goes on with the writable root, as today (Zenix has no
   maintenance mode; a read-only root would leave the owner without logins). A root that stays marked is checked again
   at the next start.

fsck.ext2 (`user/fsck_ext2.c`, on the disk, not in the kernel image) changes so that one run settles everything and
only a proven clean volume is marked clean:
- **With `-y`, it marks the volume clean only after a final run that writes nothing finds nothing** (a repair is a
  check, a repair run, and that verifying run; a volume clean from the start needs only the first): then a flush, bit
  0 set, a flush, as its very last writes. Without `-y` it never writes. Any repair write that fails ends it with
  status 8 (and the volume stays marked).
- **Orphans are attached after pass 5** (the bitmaps and counts repaired first), so `/lost+found` and its entries are
  allocated from correct bitmaps; then it runs again by itself (today it stops and says "run fsck.ext2 -y again"), at
  most 3 runs. Each run starts from a full reset of its state.
- **An unattached empty regular file is cleared**, not attached as `/lost+found/#N` (e2fsck -p does the same:
  "Unattached zero-length inode. Clear"): a cut between creating a file and naming it leaves one. The inode is
  written free first, its bitmap bit cleared in pass 5 after (the F5 order).
- **A `..` repair writes in the safe order**: the new parent's count up, then the `..` entry, the old parent's count
  down last (in pass 4), so a cut never leaves a count below its names.

`mount -a` (from `/etc/fstab`, run by init before the logins): a writable ext2 volume whose bit 0 is clear is checked
the same way before it is mounted (`mount: DEV was not unmounted cleanly: fsck.ext2 -y DEV`); a failed check is said
and the volume mounted anyway, still marked. Without it a marked `/home` would stay marked for ever (`need`). A plain
`mount DEV DIR` only warns. (Linux's fsck pass for fstab; `/etc/fstab`'s format does not change.)

## 5. FS_MARK and the mark together

`FS_MARK` (D15, frozen: a0 1 = mark, 0 = clean again; answers the state before, 1 clean, 0 not) keeps its meaning for
its only caller, zup: "clean" is "needs no check". The raw bit would now answer "not clean" to every zup (its own
journal write marks the volume), zup would never clear it, and the root would be checked after every update. So:
- `FS_MARK 1`: answers 1 if neither `need` nor `hold` was set, else 0; sets `hold`; makes sure bit 0 is clear on the
  disk (written and flushed).
- `FS_MARK 0`: answers the same way, clears `hold` only (never `need`: a crash in a request during an install must
  not be cleaned away by zup's final `FS_MARK 0`), and cleans at once if nothing else keeps the volume marked.
This is not a change of meaning (not a second audit3 B7): the answer still says whether the volume needed a check
before; only fsd's own short in-use mark is no longer reported as one. `kernel/fsproto.h` says it that way.

zup's flows, read from `user/zup.c`, stay correct: `wasclean = fs_mark(1) == 1` before the journal is written ... the
install ... `if (wasclean) fs_mark(0)`; stage 1's transaction ends before stage 2 starts, so the marks never nest. A
cut anywhere in it: bit 0 is clear, the next start checks the root first (section 4) and then zup's recovery finishes
or rolls back the install on a repaired root. Recovery calls no `FS_MARK`; its writes are covered by fsd's own mark,
and a cut in recovery is checked at the following start. zup's line "(the root stays marked "needs a check")" is no
longer true after the check and goes. ZenixFS answers `FS_MARK` with -ENOSYS, unchanged.

## 6. Cost

- **Memory (fsck.ext2), read from its allocations:** 2 bits per block and 11.6 bytes per inode, plus about 119 KB for
  the program and its buffers (section 14). The owner's dual-boot card (tools/mkdualcard.sh): root 1 GB, 1 KB blocks,
  16384 inodes: about 560 KB; `/home` 3 GB, 65536 inodes: about 1.6 MB, which does not fit a 2 MB chip-only machine (fsck says out of
  memory, status 8: the volume is mounted unchecked and stays marked). At a cold start nothing else runs, so the root
  check fits wherever Zenix fits with that root; on the start that installs an update the bundle is still in memory.
- **Time, measured as I/O on the host** (section 14 has the exact figures): a check of the 1 GB root reads about 2.7 MB
  (the inode tables are 2 MB of it), in one-block requests; a repair reads that three times. `/home` 3 GB: about 9.4 MB
  a pass. No speed is claimed from an emulator; on a 14 MHz 68020 with IDE this is expected to be seconds for the root
  and some tens of seconds for a repaired `/home` (to be measured on the A1200). Only after a cut within a second of
  writing, never after a clean shutdown or an idle switch-off.
- **Kernel image:** +1712 bytes (section 14).
- **Normal use:** two superblock writes and three flushes per burst of writes (section 3).

## 7. What the owner sees

Nothing new after `reboot`, the reset warning, or switching off an idle machine. After a cut during writes, at the next
start, before the login: boot's line, fsck.ext2's pass lines (three runs when it repairs) and the problems it found,
its result. A file that was being written may come back in `/lost+found/#N` (non-empty) as with e2fsck. The same for a
marked `/home` from `mount -a`.

## 8. A cut during the check itself

fsck.ext2 sets bit 0 only at the very end, after a verifying run found nothing and a flush. A cut before that leaves the
root marked, and the next start checks again before anything writes to it. Each repair writes the value fsck computed
from the whole volume (link counts, bitmaps, free counts, entries naming free inodes cleared, `..` corrected in the
safe order), so a partial repair leaves a state its next run finishes (a host test cuts fsck.ext2 after each of its
writes). The only way the root is used before a complete check is boot's fallback when fsck.ext2 cannot run or gives
up (said on the console), the same as today.

## 9. ZenixFS

Nothing applies. ZenixFS is copy-on-write with commit records (Z1): a cut leaves the last commit, whose block map does
not hold the blocks of later, uncommitted changes, so there are no leaks to repair and no clean mark to keep. Orphans
are listed in the volume and freed at the next mount. zfsd keeps answering `FS_MARK` with -ENOSYS; boot never checks
a ZenixFS root (`fsck.zenixfs` stays a tool for damage; a root that can only be read gets boot's choices, DESIGN D34).

## 10. Interfaces (checked against CLAUDE.md "Frozen interfaces")

- No new system call, message type, error number or struct field.
- `FS_MARK`'s answer: the same meaning for the caller (section 5); `kernel/fsproto.h` reworded.
- `@fsd` takes `-fsck` as its fourth word instead of `-root` (the built-in program names are frozen; its arguments
  only grow).
- `s_state` is used as ext2 defines it (bit 0 only; bit 1 "has errors" untouched).
- The kernel's service manager forgets "served" when a service gets a new program text (a new program has not served).
- Changed program behaviour (manual pages): fsck.ext2, mount, sync (marks clean), boot's lines, zup (one message).

## 11. Tests (each fails on today's code)

- **Host, every cut is repaired:** zup-host-test section 3's cut points, each followed by `fsck.ext2 -y` (host build):
  e2fsck -fn then clean and the state clean.
- **Host, the mark:** ext2-host-test with fsd-host: a write marks the image, `sync` cleans it, an open orphan keeps it
  marked through `sync` until its close, `FS_MARK` answers and `need` survives `FS_MARK 0`, a failed write sets
  `need`; every block write fsd makes is inside the mark (a host-build count in `fs_blk_write`); a workload cut after
  each of its block writes: wherever e2fsck finds anything, the image is marked.
- **Host, a cut during fsck.ext2's repair:** cut after each of its writes, run it again: clean.
- **Emulator** (`tools/rootcheck-test.sh`): leaks put in by debugfs are repaired at boot; the emulator killed while
  files are written leaves the root "not clean", the next start repairs it; killed 4 s after the last write leaves it
  clean; `reboot` right after a write leaves it clean; a marked `/home` is checked by `mount -a`. update-test
  `powercut`: the second start checks the root, which is then e2fsck clean.

## 12. Open points for the owner (not blocking)

- A root fsck.ext2 cannot repair (duplicate blocks, a damaged directory) was used read-write after a message; decided
  by the owner (2026-10-26, the section at the end): read-only and four choices, built.
- The check's time on the real machine, especially a repaired 3 GB `/home` after a cut (section 6), to be measured.
- fsck.ext2 now clears empty unattached files instead of putting them in `/lost+found`, as e2fsck -p does.

## 13. Reviews

Two independent reviews of revision 1 (2026-10-26): one on correctness and crash safety, one on architecture fit,
frozen interfaces, size and the boot path. Both found the design sound in its shape (no deadlock under D11's rules,
the same mechanisms D11 and D15 already use, the old and new programs meeting safely); revision 2 takes in:
- **Failed writes and damage** (both): ext2.c's give-back paths can fail half way, so the invariant held only for power
  cuts. A failed block write or met damage now sets the sticky `need`.
- **`FS_MARK 0` could clean away a crash's mark** (correctness): one flag became two, `need` (sticky) and `hold`.
- **fsck.ext2 marked clean without proof** (correctness): repair writes' results were ignored and the counters could not
  show "nothing left"; now a final run that writes nothing must find nothing, and a failed write ends the run.
- **Orphans attached from unrepaired bitmaps** (correctness): now after pass 5.
- **The quiet timer** (correctness): it counted from the last request, so steady reads (a prompt, `top`) kept the volume
  marked; now from the last change.
- **boot waited for fsck.ext2 for ever** (correctness): now the zup pattern, 1 and 20 minutes.
- **The essential-server panic was lost after the swap** (both): the kernel forgets "served" with a new program text
  (the architecture reviewer's one-line fix, chosen over a check call in boot: it keeps D11's one clear message).
- **The order of `FS_QUIT` and the new registration** (both): `FS_QUIT` first.
- **Size** (architecture): "well under 1 KB" was wrong (measured prototypes about 1.2 KB); boot prints one line,
  fsck.ext2 the rest; the orphan flag lives in a spare bit, not a new field.
- **Memory** (architecture): fsck.ext2's fixed part is about 104 KB, not 53; figures corrected.
- **The RAM-disk root** is never checked; "has errors" still stops the boot as today (said); `..` repair order,
  regular files only for the clearing, the reset warning's time limit, wording, and the docs to update (sync's page,
  D11's and D15's notes, features.md) (both).
- **Points for the owner** (architecture): the `/home` time and the clearing of empty files are listed in section 12 as
  information, not as blocking decisions: the check only follows a cut while writing, and both follow e2fsck -p.
- Disagreement: none that changes the design. The correctness reviewer proposed a check call in boot after the swap, the
  architecture reviewer the kernel's "served" reset; the reset was chosen (smaller, and the panic message is D11's).

## 14. As built (2026-10-26, branch `fsck`)

- **Code:** `user/fsd.c` (the mark, `need`, `hold`, orphans as a bit of the open count, the timed wait from the last
  change, `-fsck`), `kernel/ext2.c` (`ext2_clean`, `ext2_ndamage` exported), `user/boot.c` and `user/volid.h` (the
  check), `kernel/svc.c` (a new program text forgets "served"), `user/fsck_ext2.c`, `user/mount.c`, `user/zup.c` (one
  message), `kernel/fsproto.h` (the comment).
- **One more fsck.ext2 rule, found by the tests:** 1371 of the update's 1971 leaky cuts (zup-host-test) were a file
  whose size stops before its last block (a write cut after its blocks were added, before the inode's size), which
  fsck.ext2 did not check and e2fsck does; it now sets the size to reach the last block, as e2fsck.
- **Sizes, measured:** the kernel image 171660 -> 173372 bytes (5828 left on the RAM floppy): `boot` 3728 -> 4572,
  `fsd` 28892 -> 29760 bytes. fsck.ext2 on the disk 37184 -> 39852; its fixed memory 34.7 KB code and 84 KB of
  buffers.
- **I/O, measured on the host** (hostdisk counts): the owner's 1 GB root (16384 inodes): a clean check reads 5624
  sectors and writes 2 (the clean mark); a repair of one link count reads 16864 and writes 1030. A 3 GB `/home` (65536
  inodes): 19296 sectors a pass.
- **An emulator trap (for the tests):** FS-UAE keeps the last sectors written in a host buffer until the next disk
  access, and the tests' kill loses them, as a disk cache without a flush would; a test that kills it after a quiet
  moment first reads from the disk (`tools/rootcheck-test.sh` part i).
- **Tests and results:** ext2-host-test 74 of 74 (11 new: the mark, the workload cut after each of its 590 sectors
  written, 297 cuts, 266 leaving something: never something on a volume that says clean; fsck.ext2 repairing each leaky cut in one run, and
  cut after each of its own repair writes; a mutant that never writes the mark fails 10 of them, one whose fsck.ext2
  never marks clean fails 2); zup-host-test 48 of 48 (each of the update's leaky cuts repaired by one fsck.ext2 run);
  rootcheck-test 20 of 20 in FS-UAE (on main's code parts r and b fail 10 of 12); update-test powercut 7 of 7.

Last checked against the code: 2026-10-26.

## Owner decision (2026-10-26): a repair that fails

Replaces "used read-write after a message": the root is mounted read-only and boot offers four choices: continue
read-only (the default after 30 s, so an unattended machine still comes up), continue read-write anyway (risky), a
rescue shell as root with the repair commands listed, or reboot; a manual page `rescue` explains each step. As AmigaOS
does with a disk that cannot be validated. To be built after D16.

**As built (2026-10-26, branch `diet`):**
- **When:** fsck.ext2 ends with a status above 1 (4: damage it does not repair; 8: out of memory or a read error; a
  check stopped after 20 minutes) or cannot be started. Section 4 step 4's "goes on with the writable root" is gone.
- **Read-only is the check server itself:** `@fsd ... -fsck` keeps serving (read-only, the device not claimed), so
  nothing new was needed and fsck.ext2 may open the device from the rescue shell. The writable server is started only
  for choice 2 (or a root the rescue shell left clean): `FS_QUIT` and `-root`, as after a successful check.
- **The question** (`user/boot.c`, `rescue()`): four lines on console 1, one key in raw mode (no Return), waited for
  with `SYS_evwait` on standard input and a 30 s deadline; any other key is ignored. 3 spawns `/bin/sh` as root with
  `PATH`, `HOME`, `USER`, `LOGNAME`, `SHELL` set for it (boot's own environment stays empty) and lists the commands
  with the root's device name; when the shell ends, boot reads the superblock again (`vol_id`): marked clean, it goes
  on read-write without asking, else the four lines again. 4 is `SYS_reboot` (the next start checks and asks again).
- **On a read-only root boot installs no update:** zup is not started; with a bundle waiting, boot says so and
  marks the update note failed for it, so zenixboot keeps the bundle for the next start.
- **Size:** `boot` 4572 -> 5332 bytes; the kernel image 3056 bytes short of the RAM floppy's limit afterwards.
- **Tests** (`tools/rootcheck-test.sh` part u, FS-UAE, a root whose `/etc/motd` and `/etc/passwd` share a block,
  marked not clean): nobody answers, read-only after 30 s (a write refused, reading works, the image byte for byte
  unchanged); 2: read-write (a file written and read back, the root still marked); 3: a root shell, the commands
  listed, fsck.ext2 checks the read-only root and says it cannot repair it (status 4), a write refused, exit brings
  the question back, 1 goes on; 4: reboot, the next start asks again. Not tested: the rescue shell leaving the root
  clean (Zenix has no tool that repairs what fsck.ext2 refuses; the path is a superblock read and the same writable
  start as choice 2).
- **The manual page** `rescue(8)`: the choices, copying files to safety first, checking again, the thorough way
  (e2fsck on a PC, or the RAM-disk Zenix's fsck.ext2), `/lost+found`.
