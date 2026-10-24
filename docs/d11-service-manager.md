# D11: the service manager (design note)

**Status: design, not built; awaiting review and the owner's approval.** Nothing in this note exists in the code yet.
Facts about today's Zenix were read from branch `d11` (= main after D10) on 2026-10-24; the file and function names
below say where. The decision itself is DESIGN.md D11; this note is its full design.

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
- Port rights (Darwin): who may send to a port. D11 keeps today's rules (reserved names, servers answer only the
  kernel and root) and makes them a little stronger (5.2).
- Stopping idle servers when memory is low (section 7 says why not now and how it would fit).
- Servers that must run with nobody calling them ("keep alive"), dependency lists, start-at-boot flags, per-service
  users (a server runs as root, as today), per-service log files.
- A status or control command (section 8.4: not needed).
- The servers that do not exist yet (display, index, network). D11 only makes room for them.
- Restarting `boot` itself or `init`'s logins: `boot` already restarts `init`; `init` restarts the logins.

## 2. What the code does today

**Boot order** (`kernel/main.c`, `kmain`): after the drivers, `kmain` creates four processes from images linked into
the kernel (`proc_spawn_image`, `kernel/proc.c`): `diskd` (pid 1, privileged: raw disk calls), `fsd` (pid 2), `boot`
(pid 3, privileged) and `zfsd` (pid 4), and renices diskd, fsd and zfsd to `NICE_SERVER` (-10, D10). If diskd, fsd or
boot cannot be created, it panics ("cannot start the filesystem server").
- `diskd` (`user/diskd.c`) reads the partition tables (`scan`) and creates port `disk`.
- `fsd` (`user/fsd.c`) and `zfsd` (`user/zenixfsd.c`, `mount.zenixfs` without arguments) both wait for `disk` by
  polling `sys_port_find` (5 ticks, 600 tries), both apply the same rule (`user/rootpick.h`, `rootpick`): a RAM disk,
  else the bootable ext2/ZenixFS partition, else the first, else the bare disk. The one whose type it is mounts the
  device and creates port `fs`; the other returns 0 at once.
- `boot` (`user/boot.c`) polls for `fs` (30 s), then spawns `/bin/init` and restarts it if it ends.
- `init` (`user/init.c`) runs `mount -a`; `mount` (`user/mount.c`, `mount_one`) spawns `/bin/mount.TYPE DEVICE
  fs.DEVICE` itself, polls for the port (15 s) and calls `SYS_mount`.

**Ports** (`kernel/ipc.c`): `NPORT` = 16 slots; `port_create` takes the first free slot and refuses a taken name (-1);
the names `fs`, `fs.*` and `disk` need root (`reserved`). A port number is just the slot index. Callers cache it:
the mount table (`kernel/vfs.c`, `struct mount.port`), fsd (`disk_port`, looked up once in `main`), ZenixFS's disk
layer (`user/zenixfs_disk.c`, `dport`).

**When a server dies** (`proc_exit` -> `ipc_proc_exit`): its ports are closed; every message queued on them and every
request it was serving finishes with -5 (EIO); the dying server's memory is freed. Its parent is 0, so its zombie is
freed by `proc_reap_orphans` and its exit status is seen by nobody. The kernel's mount table forgets the cached port
number only when a call returns -5 (`fs_call_on`); the next call looks the name up again (`port_find`) and finds
nothing: -EIO. Nobody starts the server again. diskd notices a dead filesystem server's claim on a device only when
the claim is in the way (`holder`, `alive`).

**Who may kill a server** (`kernel/syscall.c`, `may_signal`): root only (a process with parent 0 or `priv` is out of
reach of other users). A killed process dies at the end of its next system call.

**Sync at the reset warning and at reboot** (`kernel/kbd.c` `kbd_reset_flush`, `SYS_reboot` in `kernel/syscall.c`):
`vfs_sync` sends `FS_SYNC` to every mounted filesystem's port, then `disk_frozen` stops all disk writes.

## 3. The design in one picture

```
 kernel (kernel/svc.c, new; small hooks in ipc.c, proc.c, vfs.c, syscall.c)
   service table: port name -> program (a built-in image "@diskd" or a file "/bin/pingd"), arguments, nice
   held ports:    a registered name always has its port; with no server running, the port is "held" (no owner)
   first message on a held port: the CALLER starts the server, in its own context (the ramlib rule), then waits
   server death:  its port goes back to held; queued messages wait for the next server; the one in service fails
   limits:        5 ends in 60 s and the service is given up (diskd and the root filesystem: a panic)
 boot (user/boot.c, grows)
   registers disk -> @diskd, picks the root server with rootpick, registers fs -> @fsd or @zfsd,
   reads /etc/svctab, registers its lines, starts init (as today)
 servers (diskd, fsd, zfsd, mount.*): unchanged, except two small things:
   a filesystem server reconnects to a restarted diskd and repeats the request (section 5.4)
   a restarted filesystem server never frees a deleted file early (section 5.4)
```

### 3.1 Why the mechanism is in the kernel and the table comes from user space

The obvious design is a manager *process* (grown from `boot`) that owns the ports, waits for messages and deaths,
and spawns servers. It does not work here without new deadlocks, which is exactly the lesson of AmigaOS's library
loader (AmigaOS 3.1 study, aos31-study/small-hardware-modules.md, "ramlib"): a loader process that needs the disk to
load something must never wait on a service that needs the loader. A manager process must read programs through
`fs`, and `fs` needs `disk`. If `disk` dies while the manager waits on `fs`, and `fs` waits on `disk`, nobody is left
to restart `disk`. Making the manager smart enough to avoid that needs new waiting calls (it would wait on many
ports and on child deaths at once: future-ideas section 1, not built) and still deadlocks in chains.

ramlib's own escape is the answer: **when the opener can load the thing itself, it does.** In Zenix every client can:
the kernel loads a program in the context of whatever process asks for it (`proc_spawn` already does that for
`spawn`). So the **client that needs a stopped server starts it**, in its own context, and there is no loader process
to deadlock with. A chain always ends: diskd and the root filesystem server are images inside the kernel, loaded
without any disk (`proc_spawn_image`, `fill_mem`), and they only depend on each other (fs -> disk; diskd calls
nobody). What the kernel needs for this is small (section 8); the *policy* (which servers exist) stays in user space
(`boot` and `/etc/svctab`), where reading a file is easy.

## 4. Starting at boot

### 4.1 The new boot order
1. `kmain` starts **only `boot`** (from its image, privileged, as today). The four `proc_spawn_image` calls and the
   three `proc_renice` calls go away (they become boot's registrations); the panic "cannot start the filesystem
   server" becomes "cannot start boot". pid 1 is now boot.
2. `boot` registers `disk` -> `@diskd`, nice -10, essential (5.3). Nothing is started yet.
3. `boot` runs `rootpick` (the header moves from the servers into boot as well). Its first `DISK_LIST` call is the
   first message on `disk`: boot's own context loads `@diskd` from the kernel image (pid 2), diskd scans and creates
   `disk` (that hands the held port over to it, 5.2), and answers.
4. `boot` registers `fs` -> `@zfsd` if rootpick found a ZenixFS root (and no RAM disk), else `@fsd`; nice -10,
   essential. In the bare-disk case (no partition of either type) rootpick cannot tell; boot tries `@fsd` and, if
   that start fails, registers `@zfsd` and tries again. Only one root server is started: today zfsd is created and
   exits again on every ext2 boot.
5. `boot` reads `/etc/svctab` (4.3). That read is the first message on `fs`: the root server starts in boot's context.
   Once it answers, boot asks the mount table which device the root is on (`SYS_mountinfo` 0, already there) and
   registers `fs` again with that device as an argument (`@fsd ide0p2 fs`), so a restart never picks another device
   (`rootpick` could choose differently after `fdisk boot` changed the flags; the kernel's open files hold inode
   numbers of *this* volume). fsd and zfsd already take `DEVICE PORTNAME` arguments (that is how `mount` starts them).
6. `boot` registers each line of `/etc/svctab`, then spawns `/bin/init` and waits for it, as today.

**The chicken and egg** stays solved the way it is solved today: what is needed to read the disk is inside the kernel
image (diskd, fsd, zfsd, boot) and never comes from the disk. What changes is only *who* starts them and *when*:
boot's first calls, in order, instead of kmain all at once. The polling loops in fsd, zfsd and boot ("wait for the
port") are no longer needed: a registered name has its port from the start, and calling it starts the server.

### 4.2 The service table (kernel)
The kernel keeps a fixed table, `NSVC` = 12 entries (diskd, the root, up to seven mounts, three more). An entry:

| Field | What it holds |
|---|---|
| port | the slot of its held port; the port holds the name (16 bytes, as today) |
| program | a built-in image `@NAME` (diskd, fsd, zfsd: the kernel's list of images, with their `priv` bit) or an absolute path, plus at most three arguments, 64 bytes of text in all |
| nice | the server's base priority (-20 .. 19) |
| essential | given up means a panic (diskd, the root): set by the kernel for `disk` and `fs`, never by a caller |
| state | stopped, starting (with the loader's pid and a deadline), running (with the server's pid), given up |
| counters | starts, ends in the current 60 s window and when that window began |

Registering a name that exists replaces its program, arguments and nice, and keeps its state and counters (boot's step
5). Only root may register or remove; only root may name a built-in image. A built-in image keeps its own `priv` bit;
registration cannot give `priv` to anything else.

### 4.3 The service file: `/etc/svctab`
For servers that live on the disk (none today besides the `pingd` test server; later the display, index and network
servers). Like `/etc/fstab`: one line per service, `#` comments, blank lines ignored.

```
# PORT     NICE  PROGRAM [ARGUMENTS]
ping       0     /bin/pingd
```

There is deliberately **no** start-at-boot field, no restart field and no dependency list:
- *Dependencies* resolve themselves (launchd's insight): a server that needs another one calls its port, and that call
  starts it. Ordering by hand is how init systems get boot-order bugs.
- *Restart* is the same thing as *on-demand*: a server that died is started again by the next message. A server that
  ends with status 0 while it is not serving anyone has simply **stopped** (pingd after `ping`'s quit message); that is
  not counted as a crash, and the next message starts it again. So a stateless server may end itself when idle and
  cost nothing until used.
- *Start at boot* is only needed by a server nobody calls, and no such server exists (1, "Not in D11").

The format is documented in `docs/man/boot.txt` (boot reads it). A bad line is reported on the console and in the
kernel log and skipped; it never stops the boot.

## 5. How it works

### 5.1 A call to a held port (on-demand start)
A client calls a port (a program's `SYS_call`, or the kernel's `ipc_kcall` for a file operation). Today `call()`
queues the message, wakes the server and blocks. New, when the port is **held** (a service with no server):
1. **Given up:** the call fails at once with -ECONNRESET (8.2).
2. **Starting** (someone else is loading it): the message is queued and the client blocks, as for a busy server. It
   is served when the server opens its port.
3. **Stopped:** this client becomes the **loader**. It marks the entry "starting" (loader = itself, deadline = now +
   30 s), and starts the program: a built-in image through `fill_mem` (no disk), a file through `proc_spawn` with no
   parent: uid 0, working directory `/`, empty environment, output to console 1 (to the kernel log after build step
   8), `ppid` 0, as kmain's servers today, so the kill rule protects it. The new process gets the entry's nice. Then the client queues its own
   message and blocks as usual.
4. The server initialises and calls `port_create` with its name. The kernel finds the held port of its entry and,
   **only if the caller is the process it started for that entry**, hands the port over: the queued messages are now
   the server's, and it receives them in order. The entry becomes "running".
5. If the program cannot be loaded (no such file, not a Zenix program, out of memory, process table full), every
   queued message fails with -ECONNRESET and the kernel log says why ("svc: ping: cannot start /bin/pingd: no such
   file"). A load failure is not counted as a crash (5.3).

**The loader's own message.** While the loader reads the program it is itself a client of the filesystem; the reply
lookup (`ipc_reply` matches `wait_msg`) and priority inheritance (`prio_set` follows `wait_msg`) must see only that
inner call. So the loader keeps its outer message **out** of the queue while it loads and puts it at the front
afterwards, and restores `wait_msg`/`wait_port` before it blocks again. (Without this, the new server could answer
the loader's message while the loader is still inside its nested call: the reply would be refused as a stray pointer
and the loader would wait for ever. A trap for whoever builds it.)

**Waiting for the port.** A server that never opens its port (stuck in its start-up) would hold its clients for ever,
as a hung server does today. The deadline (30 s, longer than a big ZenixFS mount: `mount` allows 15 s today) is
checked by the tick only while some entry is starting (one compare otherwise). When it passes, the kernel logs "svc:
NAME: PROGRAM did not open its port in 30 s", kills the process and fails the queued messages with -ECONNRESET. This
also breaks a cycle (A's start-up calls B, B's calls A), which is a mistake in the table, not something to support.

**The deadlock rules** (the ramlib lesson, written down so nobody breaks them later):
- Nobody loads on behalf of someone else: the loader is always a client that needed the service.
- diskd calls nobody: no filesystem, no other server, not even the mount table (`SYS_mountinfo` would call the root
  filesystem server, which may be the very client that is waiting for diskd).
- The root filesystem server calls only diskd. A mounted filesystem server calls only diskd.
- A server's start-up may call other services, never its own port (already refused: `call()` returns an error for a
  call to one's own port) and never in a cycle (the deadline breaks it).
- Loads nest at most two deep (a loader whose program lives on a mounted filesystem whose server must start first);
  a third level fails with -ECONNRESET. Each level puts `call()` + a spawn + filesystem calls on the 4 KB kernel stack:
  the depth must be measured (`-fstack-usage`) in step 1 of the build order.

### 5.2 Ports: held, handed over, given back
- A held port is an ordinary slot with no owner (`op` = 0). It keeps its slot number for as long as the service is
  registered, so every cached port number (the mount table, fsd's `disk_port`, ZenixFS's `dport`) stays right across
  restarts. This removes, for the system servers, the stale-number problem in section 12.
- `port_create` of a registered name succeeds only for the process the kernel started for it; anyone else gets
  -EBUSY, **root included**. That is stronger than today's rule (root may create a reserved name once the real server
  is gone). The reserved-name rule stays for names that are not registered.
- When the server dies, its service port goes back to held instead of being closed. Its other ports (if it made any)
  are closed as today.
- Removing an entry (umount, 6.4) turns its port into an ordinary port of the running server, closed when it exits.
- `NPORT` grows from 16 to 24: held ports are permanent (diskd, the root, up to seven mounts, svctab lines), and the
  test servers and programs' own ports need room too. About 40 bytes of BSS each.

### 5.3 Crash handling
**Noticing.** `proc_exit` already calls `ipc_proc_exit(pid)`. If the dying process is the running server of an entry,
the kernel has the exit status there (`p->exit_code`) and knows whether a request was in service and of which type.

**Messages in flight.**
- The request being **served** fails with -ECONNRESET (the server may have done half of it, and may have written part
  of the client's buffer: zero copy means there is nothing to undo). The kernel's file calls turn that into -EIO for
  programs, so programs see what they see today.
- Messages **queued** but not yet received stay queued, in their clients' memory (the clients are blocked, so the
  memory stays valid). The first of those clients is woken to be the loader (5.1, step 3), after the back-off below,
  and the new server serves them in order. **They see no error at all.**
- A server that ends **before it opened its port** has failed to start: it is counted as an end, but the messages
  queued for it fail with -ECONNRESET instead of waiting (a start that failed once is likely to fail again, and the
  root pick in 4.1 must see the failure at once to try the other server).

**The policy** (decided here, the same for every service; no per-service options):
- An end with status 0 while nothing is in service is a **stop**: not counted, no log line beyond "stopped".
- Anything else is an **end**: logged, counted. The next start waits 0 s after the first end in a window, then 1 s,
  2 s, 4 s (the loader sleeps before it loads; once the reset warning or `reboot` has begun there is no wait).
- **5 ends within 60 s: given up.** The port stays held and every call fails at once with -ECONNRESET (no more
  loads, no more log lines). For an essential service (diskd, the root filesystem) giving up is a **panic** with the
  reason ("svc: diskd ended 5 times in 60 s"), so it is kept in the previous-boot log (A12) and shown at the next boot:
  without its disk or root Zenix can do nothing useful, and a clear stop is better than a machine that answers
  every command with "I/O error". (The owner may decide otherwise: open question 2.)
- A given-up mount is cleared by `umount` (it removes the entry); a given-up `/etc/svctab` service by root
  registering it again (no command for it: a reboot does it too).

**The kernel log** (`kprintf`: the serial port, `dmesg`, the previous-boot log). One line per event, for example:
`svc: disk: started @diskd (pid 2, for pid 1 boot)`, `svc: fs: fsd (pid 3) ended, status 139, during FS_RENAME for
pid 17; start 2 when next used`, `svc: fs.ide0p3: given up after 5 ends in 60 s`. The servers' own messages
(`printf` to their console 1 today) go there too (8.5), so `dmesg` holds the whole story.

### 5.4 What a restarted server must recover
Every server here keeps its real state on the disk; what is lost is what it held in memory.

**diskd.** Its device slots are rebuilt by `scan` from the partition tables, exactly as at boot. Lost: which
filesystem server has which device mounted (`dev[].mounted`), and which floppy drives were probed (`fd_probe`).
The filesystem servers get those back themselves:
- fsd's and ZenixFS's disk calls (`disk_rw` in `user/fsd.c`, the calls in `user/zenixfs_disk.c`) treat -ECONNRESET
  as "diskd died during my request": they open their device again **by name** with `DISK_MOUNT` (that call starts the
  new diskd, in the filesystem server's context), check with `DISK_INFO` that it is the same place on the same medium
  (start and size equal to what they mounted; otherwise they stay disconnected and answer -EIO, never write to another
  partition: `fdisk` may have renamed `ide0p2` to `ide0p1` since), and send the request again **once**. A sector read
  or write may be repeated safely. The new handle replaces the old one. The program that asked sees nothing but a
  pause. One copy of this code serves both servers (one small shared disk-client file, as CLAUDE.md asks for shared
  helpers).
- A floppy reopened by name (`df1`) is probed again by `DISK_OPEN` (it already does that). A RAM-disk root (`ram0`)
  survives: its memory belongs to the kernel (diskd only learns its address, `sys_blk_ram`), not to diskd.
- Not covered: in the moment between diskd's restart and the filesystem servers' reconnection, a root `mount` or
  `fdisk` of the same device would not see it as mounted. That needs root and a few milliseconds' timing; diskd cannot
  ask the mount table instead (the deadlock rule in 5.1).

**fsd (ext2).** Its block cache and the table of open inodes (`opens`) are lost. Everything it wrote went straight to
the disk (write-through). Two dangers and their answers:
- *A deleted file that is still open.* fsd keeps such an inode until its last `FS_CLOSE` (`open_ref`, `open_unref`,
  `ext2_release`). A new fsd does not know which inodes are open, so it could free one at once and give the number to
  a new file, which an old handle would then read. Answer: a restarted fsd (it asks the kernel whether this is a
  restart: `SYS_svc`, 8.1) **never frees a deleted inode while it runs**; such inodes keep their blocks until the next
  `fsck.ext2`, exactly what happens after a crash today (architecture.md section 9). The kernel does not re-send its
  open files: that would race with other clients' requests and costs more than it saves.
- *A change that was half done.* If fsd died during a change (the log names the request), the volume may be as after
  a power cut during that change (a block marked used but not linked, a directory entry without its inode). ext2 has
  no journal to repair it. The new fsd mounts it read-write, as a power cut would leave it, and the kernel log says
  `fsd: restarted after ending during a change: check DEV with fsck.ext2`. (Open question 1.)

**zfsd / mount.zenixfs (ZenixFS).** Copy-on-write with atomic commits (zenixfs.md, Z1): the new server mounts the last
commit and the volume is consistent. Lost: changes not yet committed - up to 3 seconds of them, which the clients
were already told had succeeded, exactly as after a power cut. Open deleted files: ZenixFS never reuses an inode
number and its orphan markers survive a crash (zenixfs.md), so a restarted server only needs the same rule as fsd
(make every deleted, possibly open file an orphan instead of freeing it).

**Mounted filesystems** are the same two programs with the same rules (6.4).

**What the kernel keeps for them.** The mount table (`struct mount`: the port, the root inode, the device), open files
(inode numbers) and working directories (`cwd_ino`) are on-disk numbers: they stay valid for the same volume. Step 5
in 4.1 guarantees it is the same volume.

### 5.5 What clients see
| Situation | A program's file call | A program's own `SYS_call` |
|---|---|---|
| the server is stopped | a slower first call (the load) | the same |
| the server dies while serving this request | -EIO (as today) | -ECONNRESET |
| the server dies while this request waits in its queue | nothing: served by the next server | the same |
| diskd dies under a filesystem server | nothing: a pause (5.4) | - |
| the program cannot be loaded, or did not open its port in 30 s | -EIO | -ECONNRESET |
| given up | -EIO at once | -ECONNRESET at once |

## 6. How it fits the rest of Zenix

### 6.1 Priorities (D10) and priority inheritance
- A started server gets its entry's nice: -10 for diskd, the root and (new) mounted filesystems. Today a mounted
  filesystem server runs at the nice of whoever ran `mount` (0), below the root's; a mounted `/home` is just as much
  "a filesystem everyone waits for" (D10's reason), so `mount` registers it at -10.
- Inheritance continues to mean "whoever does the work for you runs at least at your level": a client queued on a
  held port raises the **loader** (it is doing the work) instead of the missing owner; when the port is handed over,
  the server is raised to its best queued client (`ipc_best_client` already counts the queued messages of the ports
  it owns); after its load the loader drops back (`prio_recalc`).
- The loader loads at its own (possibly raised) level. A nice 19 program that happens to be first only delays the
  start until a better client queues and raises it.

### 6.2 Port security
Servers keep answering only the kernel (`UMSG_KERNEL`) and root (diskd: root). Registration is root-only, and a
registered name can only be opened by the process the kernel started for it (5.2), so a user cannot put a fake
server behind `fs` or `disk`, and neither can a stray root program by accident. A user *can* start a stopped system
service just by using it (opening a file starts the root filesystem server, `ping` starts `pingd`); the service runs as
root, as it would at boot. The kernel reads the program file with the *client's* identity (`fs_exec_open` stamps the
caller), so service programs must be world-executable, as everything in `/bin` is. When port rights come (Darwin, a
later decision), a held port is where the kernel would hand out the send right; nothing in D11 is in the way.

### 6.3 The reset warning and `reboot`
`vfs_sync` must still reach every filesystem that has something to write, within the 8 s of the reset warning:
- `FS_SYNC` (and `umount`'s `FS_QUIT`) are sent **without starting** a stopped server (a kernel-only flag on the
  call): a dead ext2 server had nothing waiting (write-through), a dead ZenixFS server's uncommitted changes are gone
  with it. Such a mount is skipped at once.
- A live filesystem server may still need diskd started again to flush; that start is allowed and skips the back-off.
- Once `disk_frozen` is set, nothing is started any more (writes are refused anyway).

### 6.4 `mount` and `umount`
`mount` stops spawning the server itself. It registers `fs.DEVICE` -> `/bin/mount.TYPE DEVICE fs.DEVICE`, nice -10,
and calls `SYS_mount`; the kernel's first call to the new port (`FS_ROOT` in `vfs_mount`) starts the server in
`mount`'s context, so the 15 s polling loop in `mount_one` goes. If the start fails, `mount` removes the entry and
says why ("see dmesg" for the server's own words, which now go to the kernel log). `umount` (`vfs_umount`) removes
the entry **before** it sends `FS_QUIT`, so the server's end is final and never restarted. A mounted filesystem is
not essential: given up, it stays mounted and answers -EIO until `umount`.

### 6.5 Zero copy
Nothing is copied by D11. A message still lives in its client's memory; a queued message survives a restart only
because its client is blocked and its memory therefore valid. A message the dead server was serving may be half
done in the client's buffers; the client gets an error and must not trust the buffer (today's rule).

### 6.6 Kill, stop, Ctrl-C
- Root may still `kill` a server; it then counts as an end (status 130) and restarts on the next call. Section 12
  (finding 3) explains why killing the ext2 server in the middle of a request can damage the volume today; the tests
  only kill it while it is idle.
- A loader is a client in a call: as today it cannot be stopped or killed until its call is answered; the 30 s
  deadline bounds that wait.

## 7. Low memory: not in D11
AmigaOS's low-memory handler chain (AmigaOS 3.1 study, aos31-study/exec-kernel.md, item 9; and ramlib expunging one
unused library per call, aos31-study/small-hardware-modules.md) is the right shape: when an allocation fails, ask a
list of handlers in order to free a little, try again, repeat. It does not fit D11 yet because **no service could be
stopped**: diskd and every filesystem server hold a mount, and `pingd` is a test. D11 already gives the important
piece: a server that ends with status 0 while idle is "stopped", costs nothing and is started again when used. So the
later step is small: a "stop if you are idle" message type and one low-memory handler that sends it to the least
recently used stoppable service. That belongs with the first real stateless service (the index server, DESIGN.md
"Planned servers") and with a kernel handler chain for its own caches, as one decision.

## 8. Interfaces (proposals; the main session claims numbers in CLAUDE.md)

### 8.1 One new system call
`SYS_svc(op, arg, buffer)`, root only except `SVC_SELF`:
- `SVC_REGISTER` (arg: a struct with port name, program text, nice): add or replace an entry.
- `SVC_REMOVE` (arg: port name): remove an entry (mount on failure; the kernel's own `umount` does it internally).
- `SVC_SELF`: for the calling process, how many times its entry has been started (1 = the first start, 0 = not a
  service). fsd and zfsd use it for the restart rules in 5.4.

No status call: the kernel log already says everything (8.4).

### 8.2 One new error number
`ECONNRESET` (104, Linux's number, "the server is gone"): returned by `call()` when the server died while serving the
request, could not be started, missed its deadline or was given up. Needed because today's -5 (EIO) cannot be told
apart from a real disk error, and fsd must know whether to reconnect (5.4). The kernel's file calls map it to -EIO,
so programs see no new number; `zstrerror` gets its text for the programs that call servers directly. It goes into
`kernel/errno.h` with the others.

### 8.3 No new message types
The restart rules in 5.4 need none: the kernel does not re-send open files, and diskd's claims come back through the
existing `DISK_OPEN`/`DISK_INFO`. `FS_*` 1-24 and `DISK_*` 1-8 stay as they are.

### 8.4 No new command
- Status: `dmesg` shows every start, stop, end and give-up; `ps` shows the server processes (PPID 0, NI -10).
- Restart by hand: `kill` (root) is a restart, as the next call starts the server again.
- Clearing a given-up service: `umount` for a mount, a reboot otherwise. Not worth a command until a service exists
  that one would want to clear by hand. A `svc` command can come later if real use asks for it.

### 8.5 Also changed
- `NPORT` 16 -> 24 (5.2); `NSVC` 12, new (limits.md).
- The servers' standard output and error go to the kernel log instead of console 1 (a new kind of open file that
  `kprintf`s what is written to it): their messages are then in `dmesg` and in the previous-boot log next to the
  restart lines, and during the boot they still appear on the screen (the boot mirror). After the boot they no
  longer appear on whichever console is shown, which today interrupts the user there.
- `kmain` loses its server list; `boot`, `mount`, `fsd`, `zfsd` and `diskd` change as described. Process numbers
  change (boot 1, diskd 2, the root 3): tests that match pids (`tools/prio-test.sh`) are updated with it.

## 9. Failure cases and their tests

A new `tools/svc-test.sh` in `make check` (FS-UAE headless, its own serial port and build directory), with
`tools/tests/svc-test.txt`; a test-only helper program like `priotest` (`svctest`: hold a file open, delete it, read it
again; make two calls at once). `pingd` gets a test request "wait a second, then crash" (an `illegal` instruction) in
a test-only build, the way `crash` and `STACK_TEST` work.

| Case | What must happen | How the test checks it |
|---|---|---|
| normal boot, ext2 root | boot 1, diskd 2, fsd 3; no zfsd process; every existing test unchanged | `ps`; the whole of `make check` |
| normal boot, ZenixFS root | zfsd serves `fs`, no fsd | `tools/zroot-test.sh` plus `ps` |
| on-demand start | `ping` with `ping` in `/etc/svctab`: no pingd before, one after (nice from the table); pingd's quit (status 0) is a stop, the next `ping` starts it again, `dmesg` says "stopped", not "ended" | output and `dmesg` lines compared |
| program missing | `/etc/svctab` names a missing file: `ping` fails at once with the error, the log says why; nothing counted | `dmesg`, exit status |
| crash during a request | the client in service gets -ECONNRESET; a second client queued behind it is answered by the new pingd without an error | `svctest` prints both results |
| restart limit | five crashes in 60 s: "given up", the sixth call fails at once, no further load | `dmesg`, timing of the call |
| back-off | the starts after the second, third and fourth end wait 1, 2 and 4 s (to the tick) | E-clock times in `svctest` |
| root server killed while idle | the next `ls` works; `dmesg` shows the end and the start; the restart used the root's device (argument) | output, `dmesg`, `ps` arguments |
| diskd killed during a long `cp` | `cp` finishes without an error, `cmp` equal; `dmesg` shows diskd's restart and the reconnect; on the disk image the host's `e2fsck -fn` is clean | `cmp`, `e2fsck` |
| deleted open file across a restart | `svctest` reads the same bytes from the deleted file after fsd (and zfsd) was killed and restarted; after the next boot `fsck.ext2` finds the inode and frees it | output |
| root server given up | a test-only build with a low limit: the panic text, and at the next boot the previous-boot lines show it | `tools/diag-test.sh` style |
| deadlock attempt: a cycle | two test services whose start-ups call each other: both fail after 30 s with "did not open its port", the system goes on | `dmesg`, a command after it |
| deadlock attempt: own port | a service whose start-up calls its own name gets an error at once (no hang) | output |
| ramlib case | the root server is killed while a client is loading `pingd` from the disk: the load fails or succeeds, but never hangs; the next `ping` works | output within a timeout |
| mounted filesystem | kill `mount.zenixfs` of a mounted partition: files there work after the restart; `umount` then ends it for good (no restart line); `mount` of a non-ZenixFS device fails cleanly and leaves no entry | `tools/mounttest.sh` extended |
| reset warning with a dead server | a mount whose server is given up: the reset warning still syncs the others in time and resets | `tools/diag-test.sh reset` extended |
| port name taken | root (and a user) creating `fs` or a registered name while it is held or running: -EBUSY | output |
| priorities | restarted servers at nice -10; `tools/prio-test.sh` (inheritance) unchanged | `ps` NI column |
| kernel stack | the deepest nesting (a load from a mounted filesystem whose server is stopped, while the root restarts) under the stack guard | the guard does not fire; `-fstack-usage` sums recorded |

**On the real A1200** (after FS-UAE passes; scratch partitions only, never the owner's AmigaOS partitions): the boot
still reaches the login; `bench` message round trip and task switch before and after (one more test per call);
kill diskd during a copy on the SD card (Emu68's card, through `kernel/emmc.c`) and check the result with `e2fsck`
on the PC; kill the root server while idle and keep working; the time a restart of diskd and of the root server takes
on a real 68020 (the RDB read and the mount), written down with the date.

## 10. Cost

**Size** (estimates from comparable code; measured in step 1 and in every step after). The RAM floppy has about 5.6 KB
left, and everything below is on it: the kernel, and boot, fsd, zfsd and diskd, which are inside the kernel image.
- kernel: the table, start, hand-over, death accounting, limits and log lines about 1.0-1.3 KB (a third of it the log
  texts), minus about 0.15 KB of kmain's spawns.
- boot: rootpick, the registrations and the `/etc/svctab` reader about 0.6-0.8 KB.
- fsd and zfsd: the reconnect and the restart rule about 0.2-0.3 KB each (one shared copy of the reconnect code).
- About 2.0-2.7 KB in all, half of what is left. If that is too much, the `/etc/svctab` reader (step 7) is the part to
  leave out of the RAM floppy's boot first; nothing else depends on it. `mount` is not on the RAM floppy.
- RAM: the table about 1.2 KB, eight more ports about 0.3 KB. Saved: zfsd's process on every ext2 boot.

**Speed.** `call()`: one test on the hot path (has the port an owner?); everything else happens only at a start or a
death. `proc_exit`: the entry lookup only for a process that owns a service port. The tick: one compare while a start
is pending. A first call to a stopped server pays its load (a built-in image: a copy in memory; a disk program: its
read). Measured with `bench` (message round trip) before and after, in FS-UAE and on the A1200.

## 11. Build order (each step builds, passes `make check` and is committed on its own)

1. **The mechanism, for the built-in servers only.** `kernel/svc.c`: the table, held ports, the hand-over in
   `port_create`, the start in the caller's context (images only), death: the served request fails, queued ones wait,
   the next caller starts it again; -ECONNRESET and the -EIO mapping in `fs_call_on`; `SYS_svc` REGISTER and SELF;
   `kmain` starts only boot; boot registers `disk` and `fs` (rootpick moves into boot) and passes the root device on.
   Measure the kernel stack depth and the size. Tests: all of `make check` (pids updated), kill fsd/zfsd while idle.
2. **diskd restarts under a working filesystem.** The shared reconnect-and-retry code in fsd and zfsd. Test: kill
   diskd during `cp`, `cmp` and `e2fsck -fn` clean.
3. **The restart rules of the filesystem servers** (no early free of deleted inodes; the "check DEV" line). Test:
   `svctest` deleted-open-file.
4. **Limits**: the back-off, the 5-in-60 s give-up, the essential panic, the 30 s deadline with its kill. Tests:
   limit, back-off, cycle, own port, panic.
5. **Sync paths**: no start for `FS_SYNC`/`FS_QUIT`, no back-off once the reset or reboot has begun, nothing after
   `disk_frozen`. Test: reset warning with a dead server.
6. **mount and umount as services** (nice -10 for mounts, `SVC_REMOVE`). Tests: `tools/mounttest.sh` extended.
7. **`/etc/svctab` and disk programs on demand** (the loader through `proc_spawn`, the nesting limit). Tests: ping on
   demand, missing program, crash during a request with a queued second client, the ramlib case.
8. **Servers' output to the kernel log.** Test: `dmesg` holds diskd's and fsd's start-up lines.
9. **Documents**, in each step's own commit as CLAUDE.md asks: architecture.md sections 3, 8 and 9, limits.md
   (NPORT, NSVC, the 30 s and 5-in-60 s numbers), features.md, the pages of boot (with the svctab format), diskd,
   mount, mount.ext2 and mount.zenixfs; DESIGN.md "Where the decisions stand".

Steps 1-5 together are the useful core (servers come back); 6-8 can follow one at a time. `kernel/kernel.h`,
`kernel/main.c` and `docs/` counts are the main session's files (CLAUDE.md): whoever builds D11 there, or asks for
those lines first.

## 12. Found while reading the code (for the audit list; not fixed here)

1. **Stale port numbers.** `port_create` reuses the first free slot, and a port number is only that slot. The mount
   table forgets its cached number only when a call returns -5 (`fs_call_on`); a server that dies while no request
   is in flight returns no -5, so the next call to the closed slot returns -1 (programs see "operation not
   permitted") and the stale number stays. When a later `port_create` (another mount, `pingd`, any user's port: only
   `fs`, `fs.*` and `disk` are reserved) takes that slot, the old mount's requests - inode numbers of the old volume -
   go to that other server. fsd's `disk_port` and ZenixFS's `dport` have the same problem with diskd: after diskd
   dies they keep sending to its slot for good. D11's held ports end this for the system servers; a port number that
   includes a generation count would end it for all ports.
2. **A dead server's request fails with the same -5 as a disk error**, so no caller can tell that it should
   reconnect (8.2).
3. **Killing fsd in the middle of a request can damage the ext2 volume.** A killed process dies at the end of its
   next system call (`syscall.c`, `if (current->killed) proc_exit(130)`); for fsd that is the next disk call inside a
   multi-block change (a bitmap written, the inode not yet). Root only. ZenixFS is not affected (copy-on-write).
4. **Mounted filesystem servers run at nice 0** (they inherit `mount`'s), not at the -10 D10 gives the root's
   servers (6.1).

## 13. Open questions for the owner

1. **After a filesystem server crashes and comes back, should Zenix keep writing to that disk?** My choice: yes, as
   after a power cut, with a line in the log asking for a disk check at the next boot. The alternative: that disk
   becomes read-only until it is checked - safer for the files, but a read-only root makes the system hard to use.
2. **When the disk server or the root filesystem keeps crashing (5 times in a minute), should Zenix stop with a clear
   message, or stay up without disks?** My choice: stop, so the reason is shown at the next boot. Staying up would
   let you still read the consoles, but almost every command would fail.
3. **Should restarts be shown on the screen, or only in the kernel log (`dmesg`)?** My choice: only the log (and the
   serial port), so a crash in the background does not write over what you are doing; a later desktop could show a
   small notice.
4. **For later, not D11: should ordinary users one day run servers of their own** (a user's own FTP site or archive
   shown as a folder, the Hurd idea in future-ideas.md)? D11 keeps every server as root; the answer decides whether
   the service table later gets a "user" field.

*Written 2026-10-24 by reading the code on branch d11 (= main after D10) and the AmigaOS 3.1 study notes; a design,
nothing built or measured.*
