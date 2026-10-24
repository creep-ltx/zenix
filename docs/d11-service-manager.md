# D11: the service manager (design note)

**Status: design, not built; awaiting the owner's approval.** Nothing in this note exists in the code yet. Revision 2
(2026-10-24): the first review's findings are worked in (section 14 lists them). Facts about today's Zenix were read
from branch `d11` (= main after D10); the file and function names below say where. The decision itself is DESIGN.md
D11; this note is its full design.

## 1. The goal, in plain words

Zenix's disk and filesystem programs ("servers") are started once at boot and never again. If one crashes, every
program that needs it gets "I/O error" until the next reboot. D11 makes **one small mechanism** that

- starts a server **the first time someone needs it** (Exec's "load on first use", launchd's "the port exists before
  the server"),
- **starts it again when it dies**, with limits, and writes what happened into the kernel log (Minix 3's
  reincarnation server),
- and takes the list of servers out of the kernel's boot code (`kmain`) into a table.

Principle "one idea per area": these three are one mechanism, not three. A server is named by its **port name**
(`disk`, `fs`, `fs.ide0p3`, `ping`); a call to that name is what starts it, and a call to that name is also what
starts it again after a crash.

**Not in D11** (each is its own later decision, or never):
- Port rights (Darwin): who may send to a port. D11 keeps today's rules (servers answer only the kernel and root) and
  makes them stronger for registered names (5.2).
- Stopping idle servers when memory is low (section 7 says why not now and how it would fit).
- Servers that must run with nobody calling them ("keep alive"), dependency lists, start-at-boot flags, per-service
  users (a server runs as root, as today), per-service log files.
- A status or control command (8.4: not needed).
- The servers that do not exist yet (display, index, network). D11 only makes room for them.
- Restarting `boot` itself or `init`'s logins: `boot` already restarts `init`; `init` restarts the logins.

## 2. What the code does today

**Boot order** (`kernel/main.c`, `kmain`): after the drivers, `kmain` creates four processes from images linked into
the kernel (`proc_spawn_image`, `kernel/proc.c`): `diskd` (pid 1, privileged: raw disk calls), `fsd` (pid 2), `boot`
(pid 3, privileged) and `zfsd` (pid 4), and renices diskd, fsd and zfsd to `NICE_SERVER` (-10, D10). If diskd, fsd or
boot cannot be created, it panics ("cannot start the filesystem server").
- `diskd` (`user/diskd.c`) reads the partition tables (`scan`) and creates port `disk`. A handle is a slot index
  (`handle`: `h = m->a[0]`, only `used[h]` is checked); slots are filled in discovery order at the first scan, and
  later rescans (`DISK_RESCAN` after `fdisk`, a floppy drive probed on demand: `fd_probe`) keep a device's slot and
  put new devices into free ones, so a long-running diskd's numbering is not what a fresh scan would give. Which
  server has which device mounted (`dev[].mounted`, a pid) lives only in diskd's memory.
- `fsd` (`user/fsd.c`) and `zfsd` (`user/zenixfsd.c`, `mount.zenixfs` without arguments) both wait for `disk` by
  polling `sys_port_find` (5 ticks, 600 tries) and both call `rootpick` (`user/rootpick.h`: a RAM disk, else the
  bootable ext2/ZenixFS partition, else the first, else the bare disk). zfsd follows rootpick's choice; **fsd only uses
  it to step aside for a ZenixFS root** and then takes the first writable ext2 partition (`find_root`), ignoring the
  bootable flag that rootpick and architecture.md section 9 promise. The server whose type it is creates port `fs`;
  the other returns 0 at once.
- `boot` (`user/boot.c`) polls for `fs` (30 s), then spawns `/bin/init` and restarts it if it ends.
- `init` (`user/init.c`) runs `mount -a`; `mount` (`user/mount.c`, `mount_one`) spawns `/bin/mount.TYPE DEVICE
  fs.DEVICE` with `sys_spawn` (so the server inherits mount's nice, 0), polls for the port (15 s) and calls
  `SYS_mount`. `/bin/mount.ext2` is a copy of the embedded fsd and `/bin/mount.zenixfs` the embedded zfsd (Makefile).

**Ports** (`kernel/ipc.c`): `NPORT` = 16 slots; `port_create` takes the first free slot and refuses a taken name (-1);
the names `fs`, `fs.*` and `disk` need root (`reserved`). A port number is just the slot index. Callers cache it:
the mount table (`kernel/vfs.c`, `struct mount.port`), fsd (`disk_port`, looked up once in `main`), ZenixFS's disk
layer (`user/zenixfs_disk.c`, `dport`).

**When a server dies** (`proc_exit` -> `ipc_proc_exit`): its ports are closed; every message queued on them and every
request it was serving finishes with -5 (EIO); its memory is freed. Its parent is 0, so its zombie is freed by
`proc_reap_orphans` and its exit status is seen by nobody. The mount table forgets the cached port number only when a
call returns -5 (`fs_call_on`). Nobody starts the server again.

**Kills** (`kernel/syscall.c` `may_signal`, `kernel/proc.c` `proc_kill`, `schedule`): only root may kill a server (a
process with parent 0 or `priv`). `proc_kill` sets `killed` and readies a process that sleeps, waits for I/O, a
child or a message as a server; a client in a call is not readied, but when its reply readies it, the tail of
`schedule()` sees `killed` and runs `proc_exit` right there, inside whatever kernel code was waiting. So a server
killed mid-request dies when its next call to diskd returns, inside a multi-block change.

**Exit** (`proc_exit`): masks interrupts (`ints_off`), then closes the files; `file_close` sends `FS_CLOSE` to the
filesystem server (`kernel/vfs.c`) - so a dying process makes message calls.

**Sync at the reset warning and at reboot** (`kernel/kbd.c` `kbd_reset_flush`, `SYS_reboot`): `vfs_sync` sends
`FS_SYNC` to every mounted filesystem's port, then `disk_frozen` stops all disk writes.

## 3. The design in one picture

```
 kernel (kernel/svc.c, new; small hooks in ipc.c, proc.c, vfs.c, syscall.c)
   service table: port name -> built-in image (@diskd, @fsd, @zfsd) or a file (/bin/pingd), arguments, nice
   held ports:    a registered name always has its port, in a slot range of its own
   first message on a held port: the CALLER starts the server, in its own context, as root
   server death:  its port goes back to held; queued messages wait for the next server; the one in service fails
   limits:        5 ends in 60 s and the service is given up (diskd and the root filesystem: a panic)
   kept for a restart: diskd's small saved table; every server can ask for the inodes the kernel holds open on it
 boot (user/boot.c, grows)
   registers disk -> @diskd, picks the root (rootpick, plus a superblock look on a bare disk),
   registers fs -> @fsd or @zfsd with the device and its volume id, reads /etc/svctab, starts init (as today)
 servers: diskd restores its slots; fsd and zfsd rebuild their open-file counts and check the volume id
```

### 3.1 Why the mechanism is in the kernel and the table comes from user space

The obvious design is a manager *process* (grown from `boot`) that owns the ports, waits for messages and deaths,
and spawns servers. It deadlocks in chains, which is the lesson of AmigaOS's library loader (AmigaOS 3.1 study,
aos31-study/small-hardware-modules.md, "ramlib"). ramlib **is** a loader process: an open that exec cannot satisfy is
sent to it, because a plain task cannot use DOS; only when the caller already *is* ramlib's own process does it load
directly, so as not to message itself into a deadlock. A Zenix manager process would have the same weakness without
that escape: it must read programs through `fs`, `fs` needs `disk`, and if `disk` dies while the manager waits on
`fs`, nobody is left to restart `disk`.

**Our extension (not ramlib's design):** in Zenix *every* client can load, because the kernel loads a program in the
context of whatever process asks (`proc_spawn` already does that for `spawn`). So the **client that needs a stopped
server starts it**, in its own context, and there is no loader process to deadlock with. A chain always ends: diskd
and the filesystem servers are images inside the kernel, loaded without any disk (`proc_spawn_image`, `fill_mem`),
and they depend only on each other (a filesystem server -> diskd; diskd calls nobody). The kernel part is small
(section 8); the *policy* (which servers exist) stays in user space (`boot`, `mount`, `/etc/svctab`).

## 4. Starting at boot

### 4.1 The new boot order
1. `kmain` starts **only `boot`** (from its image, privileged, as today). The four `proc_spawn_image` calls and the
   three `proc_renice` calls go away. pid 1 is now boot.
2. `boot` registers `disk` -> `@diskd`, nice -10. Nothing is started yet.
3. `boot` runs `rootpick` (the header moves into boot; fsd and zfsd no longer pick). Its first `DISK_LIST` call is
   the first message on `disk`: boot's own context loads `@diskd` from the kernel image (pid 2), diskd scans and
   creates `disk` (that hands the held port over, 5.2), and answers.
4. On a bare disk (no partition of either type) rootpick cannot tell the type; boot reads the device's superblock
   itself (`DISK_READ`; boot is root) and looks for the ext2 or ZenixFS magic, so no server is started on trial.
   boot also reads the **volume id** of the chosen device: the 16-byte uuid of the superblock (ext2 at byte 104,
   ZenixFS at byte 68, zenixfs-format.md), together with the device's start and size. Zenix's own `mkfs.ext2` writes
   no uuid today; it gets one (from the E-clock) in build step 3, and an all-zero uuid falls back to start, size and
   the filesystem's block count.
5. `boot` registers `fs` -> `@zfsd` or `@fsd` with arguments `DEVICE fs -root -id ID`, nice -10. `-root` means:
   must be writable (fsd's `EXT2_RW`, as `find_root` asks today), print as the root server, not as `mount.ext2`.
   `-id` means: serve only the volume with this id (4.3). So the very first start already serves rootpick's device
   (the bootable flag is honoured), and a restart can never mount another volume under the kernel's inode numbers.
6. `boot` calls `SYS_mountinfo` 0 once the root answers; that makes the kernel learn the root's inode (`mount_root`),
   after which `SYS_mountinfo` never calls a server again.
7. `boot` reads `/etc/svctab` (4.4), registers its lines, then spawns `/bin/init` and waits for it, as today.

**When nothing can serve.** diskd and `fs` are **essential** (set by the kernel for these two names, never by a
caller). An essential service that ends before it ever opened its port is a panic at once, with its reason: "svc:
diskd ended before it served (status 1): no disk", "svc: no root filesystem: fsd ended before it served". No five
retries, no 30 s wait: today's kmain panics in the same place ("no disk and no ramdisk").

**The chicken and egg** stays solved the way it is solved today: what is needed to read the disk is inside the kernel
image and never comes from the disk. What changes is only *who* starts the servers and *when*: boot's first calls, in
order, instead of kmain all at once. The polling loops in fsd, zfsd, boot and mount go away: a registered name has
its port from the start, and calling it starts the server. Only one root server is started (today zfsd is created and
exits again on every ext2 boot).

### 4.2 The service table (kernel)
`NSVC` = 12 entries (diskd, the root, up to seven mounts, three more). An entry:

| Field | What it holds |
|---|---|
| port | its held port, one of the `NSVC` slots reserved for held ports (5.2); the port holds the name |
| program | a built-in image `@NAME` (diskd, fsd, zfsd: the kernel's list of images, each with its `priv` bit) or an absolute path, plus at most five arguments, 80 bytes of text in all |
| nice | the server's base priority (-20 .. 19) |
| essential | for `disk` and `fs` only (4.1, 5.3) |
| state | stopped, loading (with the loader's pid), starting (with the new pid and a deadline), running (with its pid), given up |
| counters | starts, ends in the current 60 s window and when that window began; reset when the program changes |
| saved table | at most 512 bytes a server may leave for its next start (5.4, diskd only today), with a checksum |

Registering a name that exists replaces program, arguments and nice; it keeps the state and, if the program text is
unchanged, the counters. Registering a name that an ordinary (unregistered) port holds is -EBUSY. Only root may
register or remove, and only root may name a built-in image; an image keeps its own `priv` bit, registration cannot
give `priv` to anything else.

### 4.3 The volume id rule (filesystem servers)
fsd and zfsd, given `-id ID`, open the device named in their arguments and compare its volume id. On a mismatch
(partitions were renamed: `scan`/`same` keep a partition's slot but give it its new name, so `ide0p2` may now be
another partition) they look through `DISK_LIST` for the device that carries the id and use that one; if none does,
they say so ("fsd: the root volume ID is on no device") and end, and the start counts as failed. The same rule serves
`mount`'s registrations (6.4).

### 4.4 The service file: `/etc/svctab`
For servers that live on the disk (none today besides the `pingd` test server; later the display, index and network
servers). Like `/etc/fstab`: one line per service, `#` comments, blank lines ignored.

```
# PORT     NICE  PROGRAM [ARGUMENTS]
ping       0     /bin/pingd
```

There is deliberately **no** start-at-boot field, no restart field and no dependency list:
- *Dependencies* resolve themselves (launchd's insight): a server that needs another one calls its port, and that call
  starts it.
- *Restart* is the same thing as *on demand*: a server that died is started again by the next message. A server that
  ends with status 0 while it is not serving anyone has **stopped** (pingd after `ping`'s quit message): not counted
  as a crash, and the next message starts it again. A stateless server may end itself when idle and cost nothing.
- *Start at boot* is only needed by a server nobody calls, and none exists (1, "Not in D11").

The format is documented in `docs/man/boot.txt` (boot reads it). A bad line is reported in the kernel log and
skipped; it never stops the boot. A path in `/etc/svctab` is read from the root filesystem, never from a mount, so
starting a file service needs at most the root server (5.1, nesting).

## 5. How it works

### 5.1 A call to a held port (on-demand start)
A client calls a port (a program's `SYS_call`, or the kernel's `ipc_kcall` for a file operation). When the port is
**held** (registered, no server running):
1. **Given up:** the call fails at once with -ECONNRESET (8.2).
2. **Loading or starting** (someone else is at it): the message is queued and the client blocks, as for a busy
   server. It is served when the server opens its port, or fails with the start.
3. **Stopped:** this client becomes the **loader**. Its process is marked *loading* (5.6: a kill waits until the end
   of its system call). If the service ended recently it first sleeps the back-off (5.3). Then it starts the program:
   a built-in image through `fill_mem` (no disk); a file through `proc_spawn`, the program read **as root** (the
   kernel's reads carry uid 0: root chose the path when it registered it, so whether a service starts never depends
   on which user happened to need it first). The new process has no parent: uid 0, working directory `/`, empty
   environment, `ppid` 0 (the kill rule protects it as it does kmain's servers today), the entry's nice, output to
   console 1 (to the kernel log after build step 8).
4. With the process created the entry becomes *starting* and its **deadline** begins: 30 s from now, so a slow
   floppy read of the program or a back-off never eats into it. The loader then waits **in its own context**, a timed
   wait like `ipc_recv`'s (a sleep with a deadline that the hand-over or the new process's death also ends): never in
   the tick, which may only count and wake (CLAUDE.md's interrupt rule).
5. The server initialises and calls `port_create` with its name. The kernel finds the held port of its entry and,
   **only if the caller is the process it started for that entry**, hands the port over and wakes the loader. The
   queued messages are now the server's; the loader puts its own message at the front of the queue and waits for
   the reply as any client. The entry becomes *running*.
6. **Failure**, all handled by the loader in its own context: the program cannot be loaded (no such file, not a Zenix
   program, out of memory, process table full: logged, not counted); the process ends before it opened its port
   (counted, 5.3); the deadline passes (the loader logs "svc: NAME: PROGRAM did not open its port in 30 s", kills
   the process and counts it). In each case every queued message fails with -ECONNRESET and the entry is stopped
   (or given up).

**The loader's own message.** While the loader reads the program it is itself a client of the filesystem; the reply
lookup (`ipc_reply` matches `wait_msg`) and priority inheritance (`prio_set` follows `wait_msg`) must see only that
inner call. So the loader keeps its outer message out of the queue until step 5 and restores `wait_msg`/`wait_port`
there. (Otherwise the new server could answer the loader's message while the loader is still inside its nested call:
the reply would be refused as a stray pointer and the loader would wait for ever.)

**Calls that never start anything.** A kernel call can carry a *no-start* option (an argument of the kernel's own
`call()`, not a bit in the message: servers never see it). With it, a held port without a running server answers at
once (-ECONNRESET, which these callers ignore). Used by: `FS_CLOSE` from `file_close` and `fs_exec_close` (a new
server has no opens to release, and `proc_exit` must never become a loader with interrupts masked: a 28-36 KB image
copy would take milliseconds on a 14 MHz 68020 and lose serial input); `FS_SYNC` and `FS_QUIT` (6.3, 6.4). **Every
call made from `proc_exit` is a no-start call.**

**The deadlock rules** (the ramlib lesson, written down so nobody breaks them later):
- Nobody loads on behalf of someone else: the loader is always a client that needed the service.
- diskd calls nobody: no filesystem, no other server, not the mount table.
- A filesystem server (the root or a mount) calls only diskd.
- A server's start-up may call other services, never its own port (`call()` already refuses that) and never in a
  cycle (the deadline breaks one and logs it).
- **Loads nest at most two deep**, and every case fits: a file service read from the root while the root server is
  stopped (1: the file service, 2: `@fsd`/`@zfsd` from the image); a mount whose server is stopped (1: the image, no
  disk read at all, because mounts are registered as images, 6.4). A third level fails with -ECONNRESET and is logged.
  The kernel stack (4 KB) must hold two levels of `call()` + a spawn + filesystem calls: measured with
  `-fstack-usage` in build step 1.

### 5.2 Ports: held, handed over, given back
- `NPORT` grows from 16 to 28: **12 slots for held ports** (one per entry, the range used by nothing else) and 16
  ordinary ones as today. So a user who creates ports until the ordinary slots run out can never stop root from
  registering a mount.
- A held port keeps its slot for as long as the service is registered: every cached port number (the mount table,
  fsd's `disk_port`, ZenixFS's `dport`) stays right across restarts, and a held slot is never given to another port.
- `port_create` of a registered name succeeds only for the process the kernel started for it; anyone else gets
  -EBUSY, **root included**. `reserved()` stays only for the names nobody registered yet (an `fs.*` name before
  `mount` registers it), so a user still cannot put a fake server there.
- When the server dies its service port goes back to held instead of being closed. Its other ports, if it made any,
  are closed as today.
- Removing an entry (umount, 6.4) turns its port into an ordinary port of the running server, closed when it exits.

### 5.3 Crash handling
**Noticing.** `proc_exit` already calls `ipc_proc_exit(pid)`. If the dying process is the running server of an entry,
the kernel has its exit status (`p->exit_code`) and knows whether a request was in service and of which type.

**Messages in flight.**
- The request being **served** fails with -ECONNRESET (the server may have done half of it, and may have written part
  of the client's buffer: zero copy means there is nothing to undo). The kernel's file calls turn that into -EIO for
  programs.
- Messages **queued** but not yet received stay queued, in their clients' memory (the clients are blocked, so the
  memory stays valid). The first of those clients is woken to be the loader, and the new server serves them in
  order. **They see no error.**
- A server that ends **before it opened its port** has failed to start: counted, and the messages queued for it fail
  with -ECONNRESET instead of waiting (a start that failed once is likely to fail again).

**The policy** (the same for every service; no per-service options):
- An end with status 0 while nothing is in service is a **stop**: not counted.
- Anything else is an **end**: logged, counted. The start after the first end in a window waits 0 s, after the
  second 1 s, the third 2 s, the fourth 4 s (the loader sleeps; once the reset warning or `reboot` has begun there is
  no wait, and sleeping loaders are woken: 6.3).
- **5 ends within 60 s: given up.** The port stays held and every call fails at once with -ECONNRESET. For an
  essential service giving up is a **panic** with the reason ("svc: diskd ended 5 times in 60 s"), so it is kept in
  the previous-boot log (A12) and shown at the next boot (open question 2). An essential service that ends before it
  ever served is a panic at once (4.1).
- A given-up mount is cleared by `umount` (it removes the entry); a given-up `/etc/svctab` service by root registering
  it again, or a reboot.

**The kernel log** (`kprintf`: the serial port, `dmesg`, the previous-boot log). One line per event, for example:
`svc: disk: started @diskd (pid 2, for pid 1 boot)`, `svc: fs: fsd (pid 3) ended, status 139, during FS_RENAME for
pid 17; start 2 when next used`, `svc: fs.ide0p3: given up after 5 ends in 60 s`.

### 5.4 What a restarted server must recover
Every server here keeps its real data on the disk; what is lost is what it held in memory.

**diskd: the saved table** (the idea of Minix 3's data store, where a driver leaves state for its next incarnation).
A diskd started for the first time has nothing to restore. Whenever its table changes - a `DISK_MOUNT` or
`DISK_UNMOUNT`, a rescan, a floppy drive probed - diskd hands the kernel a small record (`SVC_SAVE`, 8.1): for every
used slot its medium, start, size, name and the pid that has it mounted, plus `fd_probe`. That is rare (never on a read
or write) and at most about 450 bytes for 20 slots. A restarted diskd reads it back (`SVC_LOAD`), checks it like
anything read from a disk (the checksum, every slot number, medium, start and size in range), puts each device back
**into its old slot**, keeps each claim whose pid is still alive (`alive`), probes the same floppy drives, and only then
runs `scan()`, which merges the partition tables as at any rescan (`same` keeps the old slots). So:
- old handles keep meaning the same device: requests that were queued for the dead diskd, and raw tools holding a
  handle (`fsck.ext2`, `fdisk`, `dd`), carry on correctly; a filesystem server that was idle when diskd died never
  notices;
- the claims survive, so `blocked()` still refuses writes and raw opens on mounted devices from the first request on;
- only the request in service fails (-ECONNRESET); the shared disk-client code of fsd and zfsd sends that one request
  again once (repeating a sector read or write is safe); raw tools report the error.
If the record is damaged (a wild write in diskd can damage anything, there is no MMU), the restarted diskd **does not
guess**: it ends with "diskd: the saved device table is damaged: stopping so no disk gets another's writes", which for
an essential service is a panic. Renumbering devices under running servers is worse than stopping.

**fsd (ext2).** Its block cache and its table of open inodes (`opens`) are lost; everything it wrote is on the disk
(write-through).
- *Open files.* Before its first receive, a restarted fsd asks the kernel for the inodes the kernel holds open on its
  mount (`SVC_OPENS`, 8.1: the `FT_INODE` files with that mount, plus the program files being loaded at that moment,
  which the kernel keeps on a short list while their load is open) and rebuilds `opens` with those counts. The list is
  exact: the kernel is never preempted, and it is taken after the port was handed over but before the server receives
  anything, so every `FS_OPEN` and `FS_CLOSE` made since is still queued and applies on top. Deleting and freeing then
  work as before; no space is held back.
- *A change that was half done.* If fsd died during a change (the log names the request), the volume may be as after
  a power cut during that change. ext2 has no journal. The restarted fsd **clears the superblock's "cleanly unmounted"
  state bit** (s_state bit 0, `EXT2_VALID_FS`), not its "has errors" bit (bit 1, which `ext2_mount_as` turns into a
  read-only mount, `kernel/ext2.c`, and which would make the next boot refuse it as the root). Linux and the host's
  `e2fsck` then know to check it; Zenix's own root pick is unaffected. Whether it then keeps writing or turns read-only
  is the owner's choice (open question 1); the default is to keep writing, with "fsd: DEV was being changed when its
  server ended: check it with fsck.ext2" in the log.

**zfsd / mount.zenixfs (ZenixFS).** Copy-on-write with atomic commits (zenixfs.md, Z1): the new server mounts the last
commit and the volume is consistent. Lost: changes not yet committed, up to 3 seconds of them, which the clients were
already told had succeeded, exactly as after a power cut. Two things need care:
- *Orphans.* `zenixfs_mount` frees every orphan on a read-write mount (`orphans_free`, zenixfs/zenixfs.c), which would
  destroy files deleted while open. A restart mounts with a new flag that keeps them, takes `SVC_OPENS`, rebuilds its
  `opens` (marking the open orphans so that their last close frees them) and frees only the orphans nobody holds.
  (A small library change in `zenixfs/`.)
- *Inode numbers of the lost seconds.* `next_ino` comes from the last commit, so files created after it would get
  their numbers again while the kernel still holds them (an open file, a program being loaded): an old handle - whose
  `FS_READ`/`FS_WRITE` carry the kernel's mark - could reach a new file, perhaps another user's. A restart therefore
  sets `next_ino` to the larger of (the committed value + 65536) and (the highest inode in `SVC_OPENS` + 1), and
  **commits that before the first receive**. A lost-window file still held by the kernel is simply not found (-EIO to
  the program), and no number is ever handed out twice. (Inode numbers are 32 bits; 65536 per restart is cheap.)

**Mounted filesystems** are the same two images with the same rules (6.4).

**What the kernel keeps for them.** The mount table (the port, the root inode, the device), open files (inode numbers)
and working directories (`cwd_ino`) are on-disk numbers: valid for the same volume, which the volume id rule (4.3)
guarantees.

### 5.5 What clients see
| Situation | A program's file call | A program's own `SYS_call` |
|---|---|---|
| the server is stopped | a slower first call (the load) | the same |
| the server dies while serving this request | -EIO (as today) | -ECONNRESET |
| the server dies while this request waits in its queue | nothing: served by the next server | the same |
| diskd dies under a filesystem server | nothing: a pause (5.4) | a raw tool: -ECONNRESET for the request in service |
| the program cannot be loaded, or did not open its port in 30 s | -EIO | -ECONNRESET |
| given up | -EIO at once | -ECONNRESET at once |

### 5.6 Kills, Ctrl-C and the loader
- **A loader is killed** (Ctrl-C on a program that happened to need a stopped server, or `kill`): its process is
  *loading*, and a kill of a loading process takes effect only at the end of its system call - `schedule()`'s tail
  does not run `proc_exit` for it. A back-off sleep is woken by the kill (`proc_kill` readies sleepers): the loader
  sees it, gives the loader role to the first queued client (woken to load instead) and leaves without starting
  anything. A load already under way finishes or fails normally, so `proc_load` always reaches its own clean-up (no
  memory owned by a pid that was never born, no 4 KB kernel stack lost) and `fs_exec_close` is always sent. Then the
  loader does not queue its own message, hands on the role if others are queued, and dies at the end of the call.
- **Any other exit of a loader** (a fault cannot happen in kernel code without a panic, but the rule is general):
  `proc_exit` checks whether the process is an entry's loader; if so the entry goes back to stopped and the first
  queued client is woken as the new loader.
- **A service process is killed** (`kill` by root, `killed` = 1): the kill takes effect only *between requests* - in
  `ipc_recv` (which already returns -1 to a killed server, and every server's loop then ends), or at the end of a system
  call while it serves nothing (no message with `server` = its pid in `MSG_SERVING`). So `kill` is a safe way to
  restart a server by hand (8.4), and a kill can no longer cut an ext2 change in half (section 13, finding 3). Faults
  and stack overflows (`killed` = 2) still end the process at once: they are crashes, and 5.3 handles them.

## 6. How it fits the rest of Zenix

### 6.1 Priorities (D10) and priority inheritance
- A started server gets its entry's nice: -10 for diskd, the root and (new) mounted filesystems. Today a mounted
  filesystem server runs at the nice of whoever ran `mount` (0); a mounted `/home` is just as much "a filesystem
  everyone waits for" (D10's reason), so `mount` registers it at -10.
- Inheritance still means "whoever does the work for you runs at least at your level": a client queued on a held port
  raises the **loader** instead of the missing owner; at the hand-over the server is raised to its best queued client
  (`ipc_best_client` already counts the queued messages of the ports it owns); after its load the loader drops back
  (`prio_recalc`).

### 6.2 Port security
Servers keep answering only the kernel (`UMSG_KERNEL`) and root (diskd: root). Registration is root-only, held ports
have their own slots, and a registered name can only be opened by the process the kernel started for it (5.2): a user
cannot put a fake server behind `fs`, `disk` or a mount, not even after a crash, and neither can a stray root program.
(Today a dead server's slot and name can be taken; section 13 finding 1.) A user *can* start a stopped system service
by using it (opening a file starts the root filesystem server, `ping` starts `pingd`); the service runs as root and its
program is read as root, as it would be at boot. When port rights come (Darwin, a later decision), a held port is where
the kernel would hand out the send right.

### 6.3 The reset warning and `reboot`
`vfs_sync` must still reach every filesystem that has something to write, within the 8 s of the reset warning:
- `FS_SYNC` is a no-start call (5.1): a stopped, loading or starting server is skipped at once - a dead ext2 server
  had nothing waiting (write-through), a dead ZenixFS server's uncommitted changes are gone with it, and a starting one
  has written nothing yet.
- A live filesystem server may still need diskd started again to flush; that start is allowed and has no back-off.
- When the reset warning or `reboot` begins, loaders sleeping in a back-off are woken and start at once.
- Once `disk_frozen` is set, nothing is started any more (writes are refused anyway).

### 6.4 `mount` and `umount`
`mount` stops spawning the server. It reads the device's volume id (as boot does, 4.1), registers `fs.DEVICE` ->
`@fsd DEVICE fs.DEVICE -id ID` or `@zfsd ...` (the **built-in images**, the same as `/bin/mount.ext2` and
`/bin/mount.zenixfs` are today, so a mount's restart never needs the root filesystem and there is only one way of
starting a filesystem server), nice -10, and calls `SYS_mount`; the kernel's first call to the new port (`FS_ROOT` in
`vfs_mount`) starts the server in `mount`'s context. If that fails, `mount` removes the entry and says why ("see
dmesg" for the server's own words once they go to the kernel log). `umount` (`vfs_umount`) removes the entry
**before** it sends `FS_QUIT` (a no-start call), so the server's end is final. A mounted filesystem is not essential:
given up, it stays mounted and answers -EIO until `umount`. `/bin/mount.ext2` and `/bin/mount.zenixfs` are then no
longer used and are removed (with their pages) in build step 6.

### 6.5 Zero copy
Nothing is copied by D11. A message still lives in its client's memory; a queued message survives a restart only
because its client is blocked. A message the dead server was serving may be half done in the client's buffers; the
client gets an error and must not trust the buffer (today's rule). The saved table and the open-inode list are copied
through the kernel once per change or restart, never per request.

## 7. Low memory: not in D11
AmigaOS's low-memory handler chain (AmigaOS 3.1 study, aos31-study/exec-kernel.md, item 9; and ramlib expunging one
unused library per call, aos31-study/small-hardware-modules.md) is the right shape: when an allocation fails, ask a
list of handlers in order to free a little, try again, repeat. It does not fit D11 yet because **no service could be
stopped**: diskd and every filesystem server hold a mount, and `pingd` is a test. D11 already gives the important
piece: a server that ends with status 0 while idle is "stopped", costs nothing and is started again when used. The
later step is small: a "stop if you are idle" message type and one low-memory handler that sends it to the least
recently used stoppable service. That belongs with the first real stateless service (the index server, DESIGN.md
"Planned servers") and with a kernel handler chain for its own caches, as one decision.

## 8. Interfaces (proposals; the main session claims numbers in CLAUDE.md)

### 8.1 One new system call
`SYS_svc(op, arg, buffer)`:
- `SVC_REGISTER` (root; arg: port name, program text, nice) and `SVC_REMOVE` (root; arg: port name).
- `SVC_SELF` (any process): how many times its entry has been started (1 = the first start, 0 = not a service).
- `SVC_SAVE` / `SVC_LOAD` (the running server of an entry only): leave or read back the saved table (at most 512
  bytes; 5.4).
- `SVC_OPENS` (the running server of an entry only): the inode numbers, with counts, that the kernel holds open on
  the mount this server serves (5.4).

No status operation: the kernel log already says everything (8.4).

### 8.2 One new error number
`ECONNRESET` (104, Linux's number, "the server is gone"; it collides with nothing, `kernel/errno.h` ends at 95):
returned by `call()` when the server died while serving the request, could not be started, missed its deadline or was
given up. Needed because today's -5 cannot be told apart from a real disk error. The kernel's file calls map it to
-EIO, so programs see no new number; its text for `zstrerror` goes into libz (a main-session file).

### 8.3 No new message types
`FS_*` 1-24 and `DISK_*` 1-8 stay as they are; the restart needs only the operations in 8.1. New port names are
claimed in CLAUDE.md as the rule says: `disk`, `fs` and `fs.*` become registered names; `ping` is the test entry.

### 8.4 No new command
- Status: `dmesg` shows every start, stop, end and give-up; `ps` shows the server processes (PPID 0, NI -10).
- Restart by hand: `kill` (root) between requests (5.6), then the next call starts the server again.
- Clearing a given-up service: `umount` for a mount, a reboot otherwise. A `svc` command can come later if real use
  asks for it.

### 8.5 Also changed
- `NPORT` 16 -> 28 (12 for held ports), `NSVC` 12, new (limits.md).
- Build step 8 (optional, can be left out on its own): the servers' standard output and error go to the kernel log
  instead of console 1 (a new kind of open file that `kprintf`s what is written to it). Then their messages are in
  `dmesg` and the previous-boot log next to the restart lines, and during the boot they still appear on the screen
  (the boot mirror). After the boot **no server message appears on any console any more**: `mount`'s failures then
  say "see dmesg" (open question 3).
- `kmain` loses its server list; `boot`, `mount`, `fsd`, `zfsd`, `diskd` and `mkfs.ext2` (the uuid) change as
  described. Process numbers change (boot 1, diskd 2, the root 3).

## 9. Failure cases and their tests

A new `tools/svc-test.sh` in `make check` (FS-UAE headless, its own serial port and build directory) with
`tools/tests/svc-test.txt`, a test-only helper like `priotest` (`svctest`: hold a file open, delete it, read it
again; make calls at once; time them), and a test-only `pingd` request "wait a second, then crash" (an `illegal`
instruction), as `crash` and `STACK_TEST` work. Existing tests touched by the pid change: `tools/prio-test.sh` (the
diskd and fsd lines) and `tools/users-d-checks.sh` (`kill 1` now names boot).

| Case | What must happen | How the test checks it |
|---|---|---|
| normal boot, ext2 root | boot 1, diskd 2, fsd 3; no zfsd; every existing test unchanged | `ps`; all of `make check` |
| ZenixFS root; bootable flag | zfsd serves `fs`; with two ext2 partitions, the bootable one is the root | `tools/zroot-test.sh`, a two-partition image |
| bare disk; no root at all | the superblock look picks the right server; with no filesystem: the panic text, at once | output, `tools/diag-test.sh` style |
| on-demand start | `ping` with `ping` in `/etc/svctab`: no pingd before, one after (nice from the table); pingd's quit is a stop, the next `ping` starts it again | output, `dmesg` |
| program missing | `/etc/svctab` names a missing file: `ping` fails at once, the log says why; nothing counted | `dmesg`, exit status |
| crash during a request | the client in service gets -ECONNRESET; a second client queued behind it is answered by the new pingd without an error | `svctest` |
| restart limit, back-off | five crashes in 60 s: "given up", the sixth call fails at once; the starts after the second, third and fourth end wait 1, 2 and 4 s | `dmesg`, E-clock times |
| root server killed while idle | the next `ls` works; `dmesg` shows the end and the start; the restart used `-root -id` | output, `dmesg`, `ps` |
| kill during a request | `kill` of fsd during a long `cp`: the kill waits for the end of the request, the restart follows; `e2fsck -fn` clean | `cmp`, `e2fsck` |
| diskd killed during a long `cp` | `cp` finishes, `cmp` equal; `e2fsck -fn` clean | `cmp`, `e2fsck` |
| diskd restart after a rescan, after `df1` | slots and handles unchanged (`DISK_LIST` before and after equal, a mounted server and an open `dd` carry on) | output compared |
| diskd killed idle, then `fdisk`/`mount` of the root's device | refused as mounted (the claims survived) | output |
| a raw tool across a diskd restart | `fsck.ext2` reading while diskd is killed: finishes or reports the one failed request, never reads another device | output |
| damaged saved table | a test-only build damages diskd's record before a restart: the stop message and the panic | `tools/diag-test.sh` style |
| fsd restart after `fdisk` renamed partitions | the root server finds its volume by id; a mount too | output, `dmesg` |
| deleted open file across a restart | `svctest` reads the same bytes from the deleted file after fsd (and zfsd) restarted; after it closes, `df` shows the space back | output, `df` |
| free space after deletes after a restart | `rm` of a big file after an fsd restart gives its space back | `df` |
| ZenixFS lost-window file | a file created and kept open, zfsd crashed within 3 s: the old fd gets -EIO, a newly created file never reads its data | `svctest` |
| ZenixFS open orphan | a deleted, open file survives a zfsd restart and is freed at its last close | `svctest`, `fsck.zenixfs` |
| loader killed | Ctrl-C of a loader during its back-off and during a disk load: no leak (`free` before and after equal), the entry not stuck, the next call works | `free`, output |
| deadlock attempts | a cycle of two test services: both fail after 30 s, logged; a start-up that calls its own name: an error at once; the root server killed while a client loads `pingd` from disk: never hangs | output within a timeout |
| mounted filesystem | kill the server of a mounted partition: files work after the restart; `umount` ends it for good; a failed `mount` leaves no entry | `tools/mounttest.sh` extended |
| ports used up | a user creates ports until refused; root's `mount` still works | output |
| reset warning, reboot | with a given-up mount and with a loader asleep in a back-off: the others sync in time and the machine resets | `tools/diag-test.sh reset` extended |
| port name taken | root and a user creating `fs` or a registered name: -EBUSY | output |
| priorities | restarted servers at nice -10; `tools/prio-test.sh` (inheritance) otherwise unchanged | `ps` NI |
| kernel stack | the deepest nesting (a file service loaded while the root restarts) under the stack guard | the guard does not fire; `-fstack-usage` sums |

**On the real A1200** (after FS-UAE passes; scratch partitions only, never the owner's AmigaOS partitions): the boot
still reaches the login; `bench` message round trip and task switch before and after; kill diskd during a copy on the
SD card (through `kernel/emmc.c`) and check the result with `e2fsck` on the PC; kill the root server while idle and
keep working; the time a restart of diskd and of the root server takes on a real 68020, written down with the date.

## 10. Cost

**Size** (estimates from comparable code; measured in step 1 and after every step). **Measured on d11 today: the
kernel is 174100 bytes and, with the 704 KB RAM disk, may grow to 179200: about 5.1 KB left** on the RAM floppy, and
everything below is on it (boot, diskd, fsd and zfsd are inside the kernel image).
- kernel: the table, held ports, start, hand-over, death accounting, limits, the loader and kill rules, the saved
  table, `SVC_OPENS`, the log lines: about 1.4-1.8 KB (a third of it log text), minus about 0.15 KB of kmain's spawns.
- boot: rootpick, the superblock look, the registrations, the `/etc/svctab` reader: about 0.8-1.0 KB.
- diskd: save and restore of its table about 0.3 KB. fsd and zfsd: the volume id rule, `SVC_OPENS`, the retry and the
  restart rules about 0.3-0.4 KB each (one shared disk-client copy).
- Saved: rootpick in fsd and zfsd (about 0.25 KB each) and four polling loops (about 0.05 KB each).
- Step 8's kernel-log file: about 0.1 KB.
- **About 2.6-3.4 KB in all: half to two thirds of what is left.** If it does not fit, the lever is the RAM disk's size
  (704 KB in the Makefile, a main-session and owner decision), not a second build of boot.
- RAM: the table about 1.3 KB (with the saved-table buffer allocated only by diskd), twelve more ports about 0.5 KB.
  Saved: zfsd's process on every ext2 boot.

**Speed.** `call()`: one test on the hot path (has the port an owner?); everything else happens only at a start, a
death or a change of diskd's table. `proc_exit`: the entry lookup only for a process that owns a service port or is a
loader. The tick: nothing new (deadlines are waited for by the loader). A first call to a stopped server pays its load
(a built-in image: a copy in memory; a disk program: its read). Measured with `bench` before and after, in FS-UAE and
on the A1200.

## 11. Build order (each step builds, passes `make check` and is committed on its own)

0. **Prerequisites in main** (section 13): the nested `proc_exit` file-slot leak and the kill-while-spawning leak, both
   found in today's code, because D11 adds calls on the same paths.
1. **The mechanism, for the built-in servers.** `kernel/svc.c`: the table, held ports and their slot range, the
   hand-over, the loader (images only) with its deadline, back-off, kill rules and no-start calls, death (the served
   request fails, queued ones wait), -ECONNRESET and the -EIO mapping, `SYS_svc` REGISTER, REMOVE and SELF; `kmain`
   starts only boot; boot registers `disk` and `fs` (rootpick, the superblock look, `-root -id`); essential panics.
   Measure the kernel stack depth and the size. Tests: all of `make check` (pids updated), kill fsd/zfsd idle, no root.
2. **diskd's saved table** (`SVC_SAVE`/`SVC_LOAD`) and the shared disk client's one retry. Tests: diskd killed during
   `cp`, after a rescan, after `df1`, idle then `fdisk`; a raw tool; a damaged record.
3. **The filesystem servers' restart rules**: `SVC_OPENS`, the volume id rule and the uuid in `mkfs.ext2`, the ext2
   state bit, ZenixFS's orphan flag and `next_ino` jump. Tests: deleted open files, free space, lost-window file,
   renamed partitions.
4. **Limits**: the back-off, the 5-in-60 s give-up, the deadline. Tests: limit, back-off, cycle, own port, loader
   killed.
5. **Sync paths**: no-start `FS_SYNC`/`FS_QUIT`/`FS_CLOSE`, waking loaders, nothing after `disk_frozen`. Tests: reset
   warning and reboot with a dead server and a sleeping loader.
6. **mount and umount as services** (images, `-id`, nice -10, `SVC_REMOVE`; `/bin/mount.ext2` and
   `/bin/mount.zenixfs` removed). Tests: `tools/mounttest.sh` extended, ports used up.
7. **`/etc/svctab` and file services on demand** (read as root, the nesting limit). Tests: ping on demand, missing
   program, crash during a request with a queued client, the ramlib case.
8. **Servers' output to the kernel log** (optional). Test: `dmesg` holds diskd's and fsd's start-up lines.
9. **Documents**, in each step's own commit as CLAUDE.md asks: architecture.md sections 3, 8 and 9; how-it-works.md
   (the boot walk-through and "what happens when a program crashes"); limits.md (NPORT, NSVC, the 30 s and 5-in-60 s
   numbers); features.md; the pages of boot (with the svctab format), diskd, mount, umount, mount.ext2 and
   mount.zenixfs (removed in step 6), kill (a server is killed between requests), ps (PPID 0, the pids), dmesg (the
   svc lines), badptr (if its examples name pids); CLAUDE.md: the port-name claims and D11 in the approved exceptions
   (once the owner approves); DESIGN.md "Where the decisions stand".

Steps 1-5 together are the useful core (servers come back); 6-8 can follow one at a time. `kernel/kernel.h`,
`kernel/main.c`, the Makefile, libz and `docs/` counts are the main session's files (CLAUDE.md): whoever builds D11 does
it there, or asks for those lines first.

## 12. Open questions for the owner

1. **After a filesystem server crashes during a change and comes back, should Zenix keep writing to that disk, or
   make it read-only until it is checked?** The disk is marked "needs a check" either way (5.4). My choice: keep
   writing, as after a power cut, with a line in the log. Read-only is safer for the files, but a read-only root makes
   the system hard to use.
2. **When the disk server or the root filesystem keeps crashing (5 times in a minute), should Zenix stop with a clear
   message, or stay up without disks?** My choice: stop, so the reason is shown at the next boot.
3. **Should server messages appear on the screen?** Build step 8 moves *all* of them (start-up notes, a failed mount's
   reason, restarts) off the screen into the kernel log (`dmesg`), so nothing writes over what you are doing; `mount`
   then says "see dmesg". My choice: yes, the log only.
4. **For later, not blocking D11:** should ordinary users one day run servers of their own (a user's own FTP site or
   archive shown as a folder, the Hurd idea in future-ideas.md)? D11 keeps every server as root.

## 13. Found in today's code (for the audit list; not fixed here)

| # | Finding (read from the code) | D11 |
|---|---|---|
| 1 | **Stale port numbers.** A port number is only the slot, reused first-free. The mount table forgets its cached number only on a -5 reply; a server that dies idle leaves the number, the next call on the closed slot returns -1 ("operation not permitted"), and whoever creates a port next - any user, any non-reserved name - gets the slot and then receives the kernel's `UMSG_KERNEL` filesystem requests. It can answer `FS_OPEN` for a program with a mode carrying set-user-id and uid 0, which `proc_spawn` trusts: any user becomes root. fsd's `disk_port` and ZenixFS's `dport` keep a dead diskd's slot the same way. | closes it for registered names (held slots are never reused); ordinary ports still need a generation count in the port number, separately |
| 2 | A dead server's request fails with the same -5 as a disk error. | fixed (ECONNRESET) |
| 3 | A kill of fsd mid-request takes effect in `schedule()`'s tail when diskd's reply readies it, inside a multi-block ext2 change. | fixed for kills (5.6); a crash can still do it (5.4) |
| 4 | Mounted filesystem servers run at nice 0 (`proc_load` copies the parent's nice; `mount` uses `sys_spawn`). | fixed (6.1) |
| 5 | **A Ctrl-C can corrupt a file slot.** `proc_exit` closes a file, `file_close` sends `FS_CLOSE`, the reply readies the killed process, `schedule()`'s tail runs `proc_exit` again, nested; it closes the same file again before the first loop cleared `p->fd[i]`, and the 16-bit `refs` wraps from 0 to 65535 (the slot is never freed). | **prerequisite** (step 0): D11's no-start `FS_CLOSE` narrows it but does not remove it |
| 6 | **Killing a process while it spawns** dies in the middle of `proc_load`: the child's memory (owned by a pid that was never born) and the program's `FS_CLOSE` are lost. | **prerequisite** (step 0); the same "kill at the end of the system call" rule as a loader (5.6) is the natural fix |
| 7 | fsd's `find_root` ignores the bootable flag (takes the first writable ext2 partition), unlike rootpick and architecture.md section 9. | fixed (4.1: boot passes the device); a one-line fix in main is possible before |
| 8 | `proc_exit` makes message calls (`FS_CLOSE`) with interrupts masked. | made safe for D11 (no starts from there); the masking itself is future-ideas item 5 |

Not a bug today, to be checked with D11: the kernel stack depth of nested calls (step 1).

## 14. What the first review changed (2026-10-24)

The review's findings and where each is answered: 1 stale diskd handles and lost claims -> 5.4 (the saved table; a
damaged one stops rather than guesses); 2 ZenixFS orphans and inode numbers -> 5.4; 3 names that move -> 4.1, 4.3,
6.4 (the volume id); 4 a killed loader -> 5.1, 5.6; 5 ext2 space held back -> 5.4 (`SVC_OPENS`); 6 loads from
`proc_exit` -> 5.1 (no-start calls); 7 the nesting depth -> 5.1, 6.4 (mounts as images); 8 the program read as the
first user -> 5.1 (as root); 9 the deadline in the tick -> 5.1 (the loader's own timed wait); 10 kill as a restart
-> 5.6; 11 the root pick and boot failures -> 4.1; 12 ports used up -> 5.2; 13 sync paths -> 6.3; 14 one idea per
area -> 5.1, 5.2; 15 size -> 10; 16 documents and tests -> 9, 11; 17 the owner's questions -> 12; the ramlib
paraphrase -> 3.1; today's bugs -> 13.

*Written 2026-10-24 by reading the code on branch d11 (= main after D10) and the AmigaOS 3.1 study notes; revised the
same day after the first review. A design: nothing built or measured, except the kernel size quoted in section 10
(measured by the reviewer).*
