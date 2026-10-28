# D23: the NFS client (design note, revision 2)

Status: **design, revision 2 after two independent reviews (section 15); steps 0-4 built (the core, the cancel mark, nfscd reading, writing, users and nosuid: the as-built sections at the end).** Roadmap block 2, "Zenix
reaches the world": "an NFS client". It mounts a directory the owner's NAS (or any Linux PC) exports, so `ls /nas`,
`cp`, `less` and running a program from there work as on a local disk. It is a filesystem server like `zfsd` (D11 6.4's
"file service" rule), the first one that talks to the network instead of `diskd`, so it answers D19 6.5's open point
("a later NFS client is a filesystem server that calls `net` ... decided with NFS"). It needs two small kernel changes
(section 5: a killed program's request is marked and its server's poll sees it, so `kill` and Ctrl-C work while the NAS
does not answer; section 5.4: network mounts are synced last and briefly at a reboot). The short version is DESIGN.md
D23.

Facts about today's code are *read* from main at 22fd4ba (2026-10-27). The owner's AmigaOS NFS client, AmiNFSv3
(its own repository; MIT licence, so its code may be reused here with its notice; its lab notebook and documentation
were read, its code was not copied), is the prior art: NFSv3 over TCP, verified against the owner's Synology and a
Linux PC (*read* from its notes). Every size and time below is an *estimate* unless it says *measured*.

## 1. What the owner gets, in plain words

- `mount -t nfs nas:/volume1/zenix /nas` (or a line in `/etc/fstab`) and the NAS's folder is a directory of Zenix:
  `ls`, `cp`, `less`, running a program or a script from it, `df`. Names with å, ä, ö work (both sides use UTF-8).
- **On the Synology:** a shared folder with an NFS permission for the Amiga's address (section 8 says which settings).
  NFSv3 over TCP, as AmiNFSv3 proved on the same NAS.
- **When the NAS is off or the cable is out,** a program that uses `/nas` waits and goes on when the NAS is back;
  **Ctrl-C stops it** (unless the program catches Ctrl-C itself, as the editor does: then `kill -9` always works,
  section 5). `mount`, `df` and `sync` never hang on a dead NAS, and a boot never waits for one.
- **Files are safe across a NAS restart:** what was written but not yet on the NAS's disk is sent again (section 7).
- **It costs nothing when not used:** the server program is loaded only for a mount; on the stock A1200 one mount
  takes about 70-110 KB while mounted, besides the network's own ~115 KB (section 9).
- **Honest limits:** files of 4 GB or more cannot be opened, and seeking past 2 GB does not work (Zenix's file
  offsets are 32 bits, `lseek` signed); over the serial cable NFS runs at SLIP's ~10 KB/s; users and permissions are as
  safe as NFS's own "trust the client" model (section 10); a failed save on the NAS after a file was closed is told in
  the kernel log, not to the program (7.3).

## 2. Today (read)

- **A mount is a service** (D11 6.4): `mount` registers `fs.DEVICE` with the server's program (`@fsd ...` built in, or
  the file `/sbin/zfsd DEVICE -id ID` since D19 step 1) at nice -10, and calls `SYS_mount`; the kernel's first call
  (`FS_ROOT`) starts the server in `mount`'s context. A server that dies is started again by the next call. The program
  text is at most 79 characters and six words (`SVC_TEXT` 80 with its NUL, `SVC_ARGS` 6 in `kernel/svc.c`: frozen). A
  server may leave 512 bytes for its next start (`SVC_SAVE`/`SVC_LOAD`, kept while the entry exists). `NMOUNT` is 8
  (the root and 7 mounts) and `NSVC` 12 (disk, fs, 7 mounts, net, ping: one spare, D19 13).
- **The protocol to a filesystem server** is `kernel/fsproto.h` (frozen, append-only): paths relative to the mount's
  root (the kernel normalises the text, `..` included, and passes the root's inode as the working directory), 32-bit
  inode numbers as handles, 32-bit offsets, `struct ustat` (28 bytes), `struct udirent` (one entry per `FS_READDIR`, a
  32-bit cookie, names up to 63 bytes), `struct ustatfs` (block size and 32-bit counts), `struct fsinfo` (device 16
  bytes, type 8). The kernel stamps the caller's uid, gid and umask into every message; servers answer only kernel
  messages (`UMSG_KERNEL`) and root (fsd.c, zenixfsd.c) and check permissions with `kernel/fsperm.h`'s rules.
- **A client in a server call cannot be interrupted** (G4, D16 4: `kill_now` waits while `wait_msg` is set). For a
  disk that is fine: a request takes milliseconds. For a server that waits for a NAS it is not (section 5). D19 kept
  socket waits in the kernel for exactly this reason (2.1, "not taken": a `/net` filesystem); file calls have no such
  way out, because the kernel must hand the request to the filesystem's server.
- **A server may hold many requests at once and answer them in any order** (`ipc_recv` marks each `MSG_SERVING`;
  `ipc_reply` finds any of them, `kernel/ipc.c`), and may wait for its port and its sockets in one `evwait` (D16,
  `POLLPORT`), as netd does. `port_poll` reports `POLLIN` only while a message is queued.
- **Sockets** (D19): BSD calls in libz, netd on demand, ports below 1024 for root only, `getaddrinfo` through netd,
  `SO_REUSEADDR`. netd's small profile has a TCP window of 2 x 966 bytes and a 4 KB send buffer (`net/netd/core.c`).
- **D13 shared code** keys a program's copy by (mount, generation, inode, size, mtime); a restarted server gives the
  mount a new generation. Servers stay plain ZEXE (D13 stage 1: programs that deal in raw messages are not pure).
- **Syncing** (`vfs_sync`): every mount in turn; at `reboot` each has 5 s and the tick resets the machine the moment
  one misses it; the reset warning has 8 s for all (`kbd.c`). Only the root's result reaches `sync`'s caller.
- **AmiNFSv3's findings** (*read* from its notebook): v3 is the floor (modern Linux serves no v2); TCP because
  nfs-utils turns UDP off by default; mountd's port is dynamic (portmapper `GETPORT`; the Synology answered 892, a
  Linux PC 20048), nfsd's is 2049; the root handle is opaque, up to 64 bytes (28 seen); a Linux export needs
  `insecure` for a client on an unprivileged port, while the owner's Synology accepted AmiNFSv3's unprivileged ports
  "with no DSM changes"; a default-deny firewall on the server looks exactly like a client bug; every wait needs a
  timeout; UNSTABLE writes with COMMIT and the write verifier keep data safe across a server restart.

## 3. The protocol choices

### 3.1 NFSv3 over TCP, first and only for now

| | **NFSv3 over TCP** | NFSv3 over UDP | NFSv4.x |
|---|---|---|---|
| The Synology | yes (AmiNFSv3, *read*) | not known; Linux's nfs-utils turns it off by default | yes |
| A Linux PC | yes | off by default | yes |
| Big reads over SLIP (MTU 1006) | TCP segments; a lost segment costs one segment | an 8 KB datagram is 9 IP fragments; one lost fragment loses all 8 KB, and netd's small profile reassembles little | as TCP |
| Retransmission | TCP's (lwIP's, tested) | our own timers per request | TCP's |
| Code | RPC record marking | retransmission and duplicate handling | sessions, state, leases, callbacks, compounds: several times the code |

**Decision: NFSv3 (RFC 1813) over TCP**, with MOUNT v3 (RFC 1813 appendix I) and portmapper v2 `GETPORT` (RFC 1833) to
find mountd. UDP and NFSv4 are not built. nfsd's port 2049 is taken without asking the portmapper (one round trip
less), mountd's always through it. Records may come in several fragments (RFC 5531 record marking): nfscd joins them.

### 3.2 The source port

nfscd runs as root (it is a service, D11 5.1) and binds a **privileged source port** (a random free one from 665-1023,
D19 9.1), so a Linux export without `insecure` accepts it. **Over SLIP or in the emulator** the NAS sees the PC's
NAT port, which is not privileged: a Linux export then needs `insecure`, a Synology its "allow connections from
non-privileged ports" box (or nothing, as AmiNFSv3 found on the owner's NAS: checked in step 6). Said in the how-to.

### 3.3 Credentials: AUTH_SYS

Every request carries AUTH_SYS (RFC 5531): the machine name `zenix` (servers only log it), the uid and gid **of the
program the request is for** (the kernel's stamp in the message) and its supplementary groups (from `/etc/group`, read
at nfscd's start: a `gpasswd` change counts from the next mount; at most 16). Credentials are per request, not per
open: a program that drops its rights after opening a file reads it with the new ones (as Linux). Root's own requests
go as uid 0; the NAS decides what root may do ("squash"). No Kerberos (`RPCSEC_GSS`): it needs a key server, and on a
machine without an MMU a ticket in memory is readable by any program anyway (section 10).

### 3.4 One connection a mount, many requests on it

Each mount's server keeps one TCP connection to the NAS and sends requests on it without waiting for the answers of
earlier ones: every request has its RPC transaction id (xid, starting at a random value at each nfscd start), answers
are matched by xid. Writes go out as they come; reads are sent ahead only on the large profile (depth 4): on the small
one netd's TCP window (1,932 bytes) is the limit, and read-ahead gains nothing.

**Reconnecting:** a connection that breaks (a reset, or no answer within `timeo`) is closed by abort (no TIME_WAIT);
nfscd connects again **from the same source port** (`SO_REUSEADDR`; another one only if that port is really taken)
after 1, 2, 4 ... up to 30 s, and sends every unanswered request again with its old xid. A Linux server's duplicate
request cache matches **the client's address, port and xid**, so the same port is what lets a resent REMOVE or RENAME be
answered from the cache instead of done twice. If it still comes back with "no such file" or "exists" (the cache lost
it), the error is passed on, as Linux does, except for CREATE in exclusive mode, whose verifier proves the file is ours
(4.5). A request whose answer was a record too big for the buffer, or that broke the connection twice, fails with -EIO
(logged) instead of being retried for ever. No TCP keepalive (lwIP's idle time is 2 hours and netd has no option for it):
a NAS that vanished while idle is found at the next request, by `timeo`.

## 4. nfscd, the server process

### 4.1 Where it lives, how it starts

- **`/sbin/nfscd`** (not a command, no manual page of its own; `docs/man/mount.txt` describes NFS mounts, as it does
  `zfsd`), a plain ZEXE program like `zfsd` and `netd` (making a server pure is a D13 decision for the main session; until
  then a second mount loads the code again, 9.1).
- **`mount -t nfs HOST:/EXPORT DIR [-o OPTIONS]`**, or the type `nfs` in `/etc/fstab` (its frozen format "device dir type
  options" fits as it is; the type field grows by one word). `mount` picks the first free name `nfs0`..`nfs7` and
  registers `fs.nfs0` -> `/sbin/nfscd nfs0 HOST:/EXPORT [CODES]` at nice -10, the D11 6.4 way: **four words at most**.
  **HOST is registered as typed** (a name or a dotted address) and nfscd resolves it at each connect, so an fstab line
  with a name works at boot (the lookup happens at the first use, after the network is up) and a NAS whose DHCP address
  changed is still found. The options go as **one word of short codes** (`h`/`s` hard or soft, `tN` timeo in seconds,
  `rN` retrans, `o` read-only, `y` sync, `aN` actimeo, `l` lazy; defaults left out), so the text is `/sbin/nfscd nfs0 `
  (17 characters) plus HOST:/EXPORT and the codes: **HOST:/EXPORT may be 62 characters with no codes, about 55 with a
  few** (`nas:/volume1/zenix` is 18; `192.168.100.200:/volume1/zenix` 30). `mount` computes the room left and refuses a
  longer one with "nas:/...: too long for the service table: at most N characters", never cut.
- **The same export after a restart** (D11 4.3's idea, through D11's own saved table): at its first contact with the NAS
  nfscd keeps the export's **root handle and the root's fileid** in its saved table (`SVC_SAVE`: at most 64 + 8 bytes of
  the 512). A server started again (`SVC_LOAD`) that finds a saved root asks mountd and refuses a different one ("nfs0:
  the NAS's export is not the one that was mounted: mount it again"), so a restart can never put another export's files
  under the old inode numbers. A lazy mount whose first server never reached the NAS gave out no inode number but the
  root's, so it needs no check.
- **Lazy, or checked:** a hand `mount` lets nfscd reach the NAS (up to 10 s) before it answers `FS_ROOT`, so a typo or
  a firewall is said at once; `mount` gets only an error number back, prints its text ("mount: nfs0: the NAS did not
  answer (see dmesg)") and the details are nfscd's log lines ("nfs0: no answer from nas (192.168.1.20) on port 111: is
  NFS on, is a firewall in the way?"). **`mount -a` adds the code `l` (lazy)**: nfscd answers `FS_ROOT` without the
  network and contacts the NAS at the first use (AmiNFSv3 connects lazily too). So init's `mount -a`, which runs before
  `ifup -a`, never waits for a NAS, a cable or a DHCP lease.
- **The device name** in `mount`'s list and `SYS_mountinfo` is `nfs0` (`struct fsinfo`'s 16-byte device field), the
  type `nfs`; the source `HOST:/EXPORT` is in nfscd's first log line (`dmesg`).
- **The start order** (binding for the build): read `/etc/group`, `port_create`, `SVC_LOAD`, `SVC_OPENS`, and only then
  any socket call. nfscd's first socket call starts netd if it is not running: a load in nfscd's own process (D11 5.1),
  during which nfscd's loop waits (the one exception to "never blocks", bounded by D11's 30 s deadline for netd);
  requests queue meanwhile and are served after.

### 4.2 Who calls whom (D11's deadlock rules, one rule changed)

D11 5.1: "A filesystem server (the root or a mount) calls only diskd." **New: an NFS mount's server calls `net` (its
socket, through the kernel) and, at its start only, the root's filesystem server (`/sbin/nfscd` by the loader,
`/etc/group`); never another mount, never itself.** No cycle is possible as long as netd's and nfscd's own files are
never on NFS: **`mount -t nfs` refuses the mount points `/`, `/bin`, `/sbin`, `/etc` and anything below the last three**
(netd reads `/etc` and starts `/sbin/net/*`; the kernel routes by path, so an NFS mount there would turn netd's "calls
only `fs`" into a call to the NFS mount: netd -> nfscd -> netd). nfscd refuses to serve a root (`-root`, D19 6.5). The
nesting stays within D11's two levels: nfscd's start reads `/sbin/nfscd` from the root (1: nfscd, 2: `@fsd` if the root's
server is stopped too); netd is started by nfscd's first socket call, a load with its own depth. A program run from an
NFS mount is loaded through nfscd as from any mount.

### 4.3 The loop

One process, one loop: the port and the socket are polled in one `evwait` (`POLLPORT | POLLIN | POLLPRI` on the port,
5.2; `POLLIN`, and `POLLOUT` while something waits to be sent, on the socket, which is `O_NONBLOCK`), with the next timer
as the timeout (a request's `timeo`, the commit timer of 7.2). Each turn: read what the NAS sent (whole records only; a
record over the receive buffer is the case of 3.4; every length checked before use), answer the Zenix requests whose
RPCs are done, take new requests from the port (at most 8 a turn, as netd), start their RPCs or answer them from the
caches, act on cancel marks (5.2). **No request waits inside nfscd for another one:** a request that needs several RPCs
(a path walk, an open) is a small state machine, and the table entries it depends on (its parents in a walk) are pinned
until it ends.

**Requests and messages:** each Zenix request nfscd holds is an internal request with its own id; its RPCs refer to that
id, never to the message. **The link to the message is cut when nfscd replies**, and nothing reads or writes a message
after its reply: an RPC answer that arrives later for a cancelled request finds no message and is dropped. (A client's
next message is often at the same address as its last one; a stale pointer would otherwise copy old data into a new
request's buffer.)

**Only the kernel and root** may send requests (`UMSG_KERNEL`, or uid 0, as fsd and zfsd): anything else is answered
-EACCES. Otherwise any user could send `FS_CLOSE` or `FS_QUIT` for the mount.

**When nfscd itself is killed** (by root, or D11's second kill): it takes no new request, answers every request it holds
-EIO, logs how many unsaved bytes it drops ("nfs0: killed: 12 KB not saved on the NAS"), and ends. It never spins on a
poll that returns at once.

### 4.4 File handles and inode numbers

The kernel names files by 32-bit inode numbers; NFS by opaque handles of up to 64 bytes plus a 64-bit `fileid`. nfscd
keeps a **handle table**: inode number -> (handle bytes, fileid, attributes, when they were fetched, unsaved data,
pins). Rules:

1. **A file is its (fileid, handle bytes)**: a Linux server reuses inode numbers after a delete, so the same fileid with
   another handle is another file.
2. **Its number is the fileid's low 32 bits** when that number is free in the table or already belongs to the same
   file; else (a clash, or the old file of a reused fileid still held) a number from a probe that starts at a hash of
   the fileid with the top bit set. So numbers are the same across nfscd's restarts in the normal case (`ls -i`,
   `find`'s loop check, D13's key), and two files are never given one number at the same time.
3. **The export's root** is always number 2 (the kernel keeps it across restarts, D11 5.4); a fileid whose low half is 2
   takes the probe's number instead. Numbers 0 and 1 are never given.
4. **An entry is kept while the kernel holds the inode** (an open file, a program being loaded: nfscd counts `FS_OPEN`
   against `FS_CLOSE` as fsd counts `opens`), while it has unsaved data, and while a request is pinned on it; otherwise
   it is a cache entry, dropped oldest first when the table is full (256 entries on the small profile, 2,048 on the
   large). A request for a number nfscd does not know is answered **-ESTALE** (section 12: Linux's 116, to be claimed;
   -EIO until then).
5. **After a restart** (nfscd killed or crashed) the table is gone. `SVC_OPENS` tells nfscd which numbers the kernel
   holds; NFSv3 cannot turn them back into handles, so **each of them is reserved as stale**: answered -ESTALE, never
   given to another file, until its `FS_CLOSE` (answered 0, as an `FS_CLOSE` of any unknown number). The log says "nfs0:
   started again: N open files on the mount can no longer be used (reopen them)". New opens work at once. (Saving the
   handles of open files in the saved table would cover a few; not in revision 2: a crash of nfscd is a bug, and the
   first goal is that nothing wedges.)

### 4.5 The requests, one by one

| `FS_*` | NFS calls | Notes |
|---|---|---|
| `ROOT` | `MNT` (through `GETPORT`), `FSINFO`, `GETATTR` | lazy: none until the first use; fills `struct fsinfo` with `nfs0` and `nfs` |
| `OPEN` | the path walk (`LOOKUP` per name not in the name cache), `GETATTR` (always: close-to-open, 6.1), `ACCESS` for the mode asked; `CREATE` `UNCHECKED`, or `EXCLUSIVE` with a verifier unique to that create for `O_EXCL`, then `SETATTR` of the mode (fsperm rule 7) and the times (RFC 1813: the verifier is kept in the times); `SETATTR` size 0 for `O_TRUNC` | symbolic links followed by nfscd (`READLINK`), at most 8 deep (fsd's limit), absolute targets from the export's root (as fsd resolves them from its volume's root: a NAS link to `/volume1/...` does not resolve, said in `mount`'s page); `..` never above the export's root; a name with a NUL or `/` from the NAS refused; a link target plus the rest of the path at most `FS_PATHMAX`. **D26 (built 2026-10-28):** a request with `UMSG_LINKS` and an absolute path follows no link: the first `READLINK`'s target gives the `FS_LINKED` answer (docs/d26-links-across-mounts.md 4: absolute targets from Zenix's `/`, as a Linux client resolves them), its TEXT written into the client's buffer only at the answer, never after a cancel or a kill; on a read-only mount (`o`) such a changing request is refused -EROFS only after its walk met no link. Without the bit, as above |
| `READ` | `READ` (rsize pieces) | from the data cache when valid (6.2) |
| `WRITE` | `WRITE` `UNSTABLE` (or `FILE_SYNC` with `y`) | section 7 |
| `STAT`, `LSTAT`, `FSTAT` | the walk, `GETATTR` (from the attribute cache while fresh) | `ustat` from `fattr3`, section 8; a size of 4 GB or more: -EFBIG |
| `READDIR` | `READDIRPLUS` (names and attributes in one call), `READDIR` if the server refuses PLUS | 6.3 |
| `CHDIR` | the walk, `ACCESS` (x) | |
| `MKDIR`, `UNLINK`, `RMDIR`, `RENAME`, `LINK`, `SYMLINK`, `READLINK` | the same NFS calls | the name cache and the parent's attributes dropped |
| `CHMOD`, `CHOWN`, `UTIMES` | `SETATTR` | set-user-id and set-group-id refused (-EPERM: `nosuid`, 8.3) |
| `STATFS` | `FSSTAT` | counts capped below 4 TB in 1 KB units (`df` and `mount` multiply blocks by KB in 32 bits), said in `df`'s page |
| `SYNC` | `COMMIT` of every file with unsaved data | 7.2, 5.4 |
| `CLOSE` | none at once | answered at once (7.3) |
| `QUIT` | `COMMIT`s (at most 10 s), `UMNT` (at most 2 s), close | 7.4 |
| `MARK` | none | -ENOSYS (as ZenixFS) |

About 19 NFS calls and NULL. NFS errors map one by one (`NFS3ERR_PERM` -EPERM, `NOENT`, `IO`, `ACCES`, `EXIST`,
`XDEV`, `NOTDIR`, `ISDIR`, `INVAL`, `FBIG`, `NOSPC`, `ROFS`, `MLINK`, `NAMETOOLONG`, `NOTEMPTY` the same names, `DQUOT`
-ENOSPC, `STALE` and `BADHANDLE` -ESTALE, `NOTSUPP` -EOPNOTSUPP, `SERVERFAULT` and anything unknown -EIO with a log line
naming it); `NFS3ERR_JUKEBOX` ("try later") is retried after 1, 2, 4 s, never passed on.

## 5. When the NAS goes away: hard, soft, `kill` and Ctrl-C

**The problem.** A program in a file call waits in the kernel for nfscd's reply, and nothing can interrupt that wait
(G4). With a disk it is milliseconds; with a NAS that was switched off it is for ever. Linux's answer since 2.6.25 is a
"hard" mount that a **fatal** signal can still interrupt (`TASK_KILLABLE`). Zenix needs the same from the first NFS
step, or one unplugged cable makes every shell that touches `/nas` unkillable.

### 5.1 The cancel mark (kernel change 1, section 12)

- **When:** a process that waits in a server call is **killed**: `kill -9`, or a signal whose action is to end it (a
  default-terminate signal it does not catch or block: Ctrl-C's SIGINT for `cat`, `cp`, `ls`), i.e. whenever
  `proc_kill` marks it. **Not** for a signal it catches: D16 4 decided that a caught signal waits for the server call to
  return ("otherwise a handler would make `open` or `spawn` fail"; `sh`'s SIGCHLD handler would cut its reads of a
  script on `/nas`), and that decision stands. A program that catches Ctrl-C (the editor, `less`) waits until the NAS
  answers or `kill -9`.
- **What:** the kernel sets **`UMSG_CANCEL`** in the waiting message's `kflags` (the message is in the client's memory,
  which stays valid while it waits) and raises the port's cancel count (5.2). Nothing else changes: the client still
  waits for the reply (G4 holds: the server may still be using its buffers).
- **Calls after the kill:** a killed process still in the kernel may make more server calls in the same system call
  (an `O_TRUNC` open's `FS_STAT` then `FS_OPEN`, an `unlink`'s `FS_LSTAT` then `FS_UNLINK`, `sync` over several mounts,
  `mount`'s `FS_QUIT` after a failed `FS_ROOT`). **`call()` sets `UMSG_CANCEL` on every new message of a process whose
  `killed` is set**, so each of them is cancelled at once too.
- **The server's choice:** a server that sees the bit may answer **-EINTR** once it no longer uses the client's buffers;
  one that ignores it (fsd, zfsd, diskd, netd: their requests end in milliseconds) answers as before. The kernel never
  restarts such a call (`sig_intr` is not set; the process is dying anyway).
- Cost: a few lines in `proc_kill` and `call()`, about 150 bytes (*estimate*). Tests: step 1.

### 5.2 The server sees it: `POLLPRI` on a port (kernel change 1, part 2)

A request nfscd already holds is off the port's queue, so a plain wake-up of the port's pollers would be swallowed
(`port_poll` reports `POLLIN` only for a queued message; `ev_wait_items` sleeps again when no item is ready). So **each
port counts the cancels of messages sent to it**, and `port_poll` reports **`POLLPRI`** (Linux's value 2) while that
count is above what the port's owner has seen (the owner's next `evwait` that reports it, or its next `ipc_recv`, takes
it as seen). It is growth of D16's frozen `POLLPORT` answer (a bit nobody got before), claimed in section 12. nfscd polls
its port with `POLLPRI`, and on it walks the requests it holds:
- a request whose RPC is still out is answered -EINTR at once; its RPC answer is dropped when it comes (4.3);
- **a WRITE whose bytes nfscd has already copied** (7.1) is answered with that count (the bytes will reach the NAS
  anyway: telling the program "nothing was written" would be false); one still waiting for room in the unsaved buffer
  answers the count copied so far, or -EINTR for none;
- a cancelled message still queued is received and answered -EINTR at once.

So **a killed program waiting on a dead NAS ends within one turn of nfscd's loop**.

### 5.3 hard and soft

- **`hard` (the default, as Linux):** a request is tried until the NAS answers; after `timeo` (default 30 s, code `tN`
  in seconds) without an answer nfscd marks the NAS "not answering", logs once "nfs0: nas is not answering, still
  trying" and once "nfs0: nas answers again" (D11 step 8: the servers' output is in the kernel log, `dmesg`). Programs
  wait, killable (5.1).
- **`soft`** (code `s`): after `retrans` (default 3) timeouts a request fails with -EIO. Programs that do not expect
  errors on files may then lose data; said in `mount`'s page (Linux says the same).
- **While the NAS is "not answering"**, whatever the mode: `STATFS` and `STAT` of the export's root are answered from
  the cache (so `mount`'s list and `df`, which stat every mount, never hang), and `FS_SYNC` answers -EIO at once with a
  log line, keeping the unsaved data (so `sync`, `zup`'s sync in an update and `reboot` never hang on it).
- **Not answering is never a crash:** nfscd stays up, its D11 entry stays running, nothing counts toward D11's five ends.

### 5.4 Reboot and the reset warning (kernel change 2, section 12)

`vfs_sync` gives each mount 5 s at `reboot` and resets the machine the moment one misses it, and the reset warning has
8 s for everything: a dead NAS mount early in the table would leave the local mounts after it unsynced (an ext2 `/home`
"not clean", ZenixFS's last seconds lost). So: **`vfs_sync` syncs the network mounts last** (a mount whose `fsinfo.type`
is `nfs`, read at `FS_ROOT`; later network types join by the same rule), and **`FS_SYNC` gets an argument: a0 1 means
"bounded"** (older callers pass 0, so the frozen meaning grows where every caller passed 0): nfscd then answers within
2 s with what it could commit and logs what it could not ("nfs0: 12 KB of /nas/x not saved: the NAS did not answer
before the reset"). `reboot` and the reset warning pass 1.

## 6. Caching and consistency

### 6.1 Attributes and names: close-to-open

As every NFS client: **attributes are cached** for a while (files 3 s rising to 60 s while they do not change,
directories 30-60 s: Linux's `acregmin`/`acregmax`/`acdirmin`/`acdirmax`), **names are cached** (name -> handle, per
directory, dropped when the directory's change time moves; sized with the handle table, 6.3), and **every `FS_OPEN` asks
`GETATTR` again** (close-to-open consistency: a file the PC changed and closed is seen whole by the next open on Zenix).
The code `a0` turns the attribute cache off (slower, for files two machines write at once).

D13's key takes `size` and `mtime` from that fresh `GETATTR`, so a program changed on the NAS is loaded afresh at its
next start. **A limit, said in `mount`'s page:** a change on the NAS that keeps the size and falls in the same second as
the copy in memory is not noticed (`ustat.mtime` is whole seconds, frozen); the next change is.

### 6.2 File data

A small **data cache** of whole rsize pieces (small profile: 4 pieces of 8 KB; large: a share of free memory, 6.3/9.2),
valid while the file's change time (`ctime`) and size are what they were when the piece was read, checked at every open
and whenever the attributes are fetched. Zenix's own writes update the pieces they touch. No cache survives a server
restart.

### 6.3 Directories

`FS_READDIR` asks for one entry at a time with a 32-bit cookie; NFS lists in chunks with 64-bit cookies and a cookie
verifier. nfscd gives the kernel **the entry's position** as its cookie and keeps, per recently listed directory (8
small, 64 large), a table of (position -> NFS cookie) at each chunk's start and the last chunk, so any position can be
asked for again with one call. If the directory's change time moved between two calls, or the NAS refuses the cookie
verifier (`NFS3ERR_BAD_COOKIE`), the listing restarts from the start and skips to the position: an entry may be seen
twice or missed, as on every NFS client; said. `READDIRPLUS`'s attributes and handles fill the handle table and the name
cache (sized together, so `ls -l` of a directory of up to 256 entries on the small profile needs no `LOOKUP` or
`GETATTR` per file). **A name longer than 63 bytes** cannot be shown (`udirent.name[64]`, frozen): it is left out and the
log says so once per directory.

## 7. Writes and their safety

### 7.1 UNSTABLE writes, kept until committed

`FS_WRITE` copies the program's bytes into nfscd's **unsaved buffer**, split into wsize `WRITE`s with `UNSTABLE` (the NAS
may answer from its memory: fast). When the buffer has no room for all of it, nfscd copies what fits, sends a `COMMIT`
for the file, and copies the rest as room comes (the client's buffer stays valid while it waits); the answer is the full
count when every piece's `WRITE` was answered, never short except on a real error or a cancel (5.2). Each answer carries
the NAS's **write verifier** (it changes when the NAS restarts). The bytes stay until a `COMMIT` that covers them returns
**the same verifier** as their writes; then they are dropped. A verifier that changed means the NAS restarted and may
have lost them: nfscd writes them again and commits again. Each unsaved range keeps **the credentials of the program
that wrote it** (uid, gid, groups), used for its `COMMIT` and any resend (a server checks permission on both; root's
would be squashed).

**The cap:** 32 KB on the small profile, a share of free memory on the large (9.2).

### 7.2 When nfscd commits

At `FS_SYNC`, when the cap is reached, 5 s after a file's last write (as ZenixFS's 3 s commit: the window in which a
power cut of the NAS may lose data), after a file's last close (7.3), and in `FS_QUIT`. The code `y` writes `FILE_SYNC`
instead (each `write` is on the NAS's disk when it returns; slower).

### 7.3 Close never waits for the network, and what a program learns

`FS_CLOSE` is sent from `proc_exit` (D11 5.1: every call made there is a no-start call, and it must not wait: an exiting
process with an open file on a dead NAS would hang inside the kernel). So **nfscd answers `FS_CLOSE` at once** and
commits that file in the background. The bytes are already on the NAS (each `write` returned only after its `WRITE` was
answered), so another machine that opens the file sees them (close-to-open holds); only their safety on the NAS's disk
waits for the commit. **A commit that fails is told only in the kernel log** ("nfs0: /nas/x: written data may not be
saved: the NAS restarted and the data could not be sent again"): `file_close` gives programs no result, and `sync`
reports only the root's. Said in `mount`'s page. (A later `fsync` would be `FS_SYNC` for one file with a result; not in
Zenix yet.)

### 7.4 umount

D11 6.4's `umount` removes the service, then sends `FS_QUIT`, and unmounts whatever its answer. So `FS_QUIT` does the
work: nfscd commits what is unsaved (at most 10 s, killable like any request: a cancelled `FS_QUIT` drops the rest),
sends `UMNT` (at most 2 s), closes the connection, answers, and ends; what it could not save is logged. No `umount -f` and
no new interface in revision 2.

## 8. Users, groups and permissions

### 8.1 Whose numbers, and the Synology's settings

NFS sends numbers, not names. A Synology numbers its users from 1026 and its "users" group is 100 (AmiNFSv3's note,
DSM's default); Zenix's shipped user is 1000 (group 100), `useradd` numbers from 1000. **The recommended set-up**
(owner question 2) needs no mapping on Zenix: a shared folder for Zenix, with an NFS rule for the Amiga's address only,
and DSM's squash **"map all users to admin"** or to the owner's NAS user, so every Zenix user and root act on the NAS as
that one user (the same as AmiNFSv3's Linux advice, `all_squash`). Files then show the NAS user's number on Zenix, and
writing works. A user map on Zenix (Linux's NFSv3 has none) is not built; if two-way user numbers are ever wanted, it is
its own small step.

### 8.2 Who checks

**The NAS decides** (it has the files and its export rules: squash, read-only). nfscd asks `ACCESS` at each open for the
mode wanted (cached with the attributes), so a refusal comes at `open`, not at the first `read`, and checks the execute
bit itself for `O_EXEC` (`fsperm.h` rule 1). New files and directories get the mode `fsperm.h` rule 7 gives in
`CREATE`/`MKDIR`; their owner is whatever the NAS makes it. A remote owner above 65535 (Zenix's numbers are 16 bits)
shows as 65534 ("nobody").

### 8.3 Always `nosuid`

Set-user-id and set-group-id bits from the NAS are **never honoured**: nfscd clears them in every `ustat` it answers
(`fsperm.h` rule 8: the kernel runs a set-user-id program as its owner from the mode `FS_OPEN` returns), and refuses to
set them. Otherwise anyone who can write to the share from another machine could make a program that runs as root on
Zenix. Device, fifo and socket files on the NAS are shown with their type and cannot be opened (-ENXIO).

## 9. Size, memory and speed

### 9.1 Code (*estimates*, -Os; measured at their steps)

| Part | Size |
|---|---|
| XDR, RPC calls and replies, record marking with fragments, AUTH_SYS | 3-4 KB |
| Portmapper `GETPORT`, MOUNT v3 | 1.5 KB |
| NFSv3: the ~19 calls used, their XDR | 8-10 KB |
| The handle table, caches, the path walk, readdir positions | 5-6 KB |
| The `FS_*` side, the loop, the unsaved buffer and commits, the codes | 6-8 KB |
| libz's part (sockets, formatting, files) | ~6 KB |
| **nfscd** | **30-36 KB**, for each mount (a plain ZEXE, 4.1) |
| `mount`'s NFS part (the source, the codes, the refused mount points) | ~1 KB on `mount` |
| The kernel's two changes | ~0.3 KB, every machine |

### 9.2 Memory per mount

| | Small profile (netd's rule: chip RAM only, or less than 2 MB of fast RAM free) | Large profile |
|---|---|---|
| rsize/wsize | 4 KB (amended at step 3: 8 KB cost too much, below) | 32 KB (the NAS's preference is 1 MB; Zenix's buffers decide) |
| receive buffer (one record) + send buffer | 5 + 5 KB | 33 + 33 KB |
| data cache | 16 KB (4 pieces) | together with the unsaved cap at most an eighth of the fast RAM free at the start, and at most 1 MB |
| unsaved buffer (cap) | 32 KB | (above) |
| handle table and name cache, readdir tables | 256 entries ~20 KB, 8 tables ~4 KB | 2,048 entries ~160 KB, 64 tables |
| stack | 8 KB | 8 KB |
| **Total, one mount** (with the code) | **~70 KB idle, ~150 KB at most** (the caches and buffers grow as used); *measured* at step 3: 106 KB idle, 122 KB after reading, 122 KB after writing (below) | up to ~1.3 MB on a machine with 8 MB free, less on smaller ones |

**Amended at step 3 (the main session, 2026-10-28; *measured in FS-UAE*, the stock A1200, `ps`):** step 2's 8 KB
transfers held 112 KB idle and 144 KB after reading; with 4 KB transfers (the buffers 5 + 5 KB, the data cache 16 KB)
and libz's heap regions sized for four 4 KB pieces (16,416 bytes: at 16,400 the fourth piece took a region of its own,
16 KB for 4), nfscd holds 106 KB mounted and idle, 122 KB after reading (the data cache full) and 122 KB after
writing 108 KB. The rest is the code (54,120 bytes of text, 9.4 KB of data and bss: estimated 30-36 KB in 9.1) and its
8 KB stack (about 72 KB), the tables (about 25 KB) and the buffers. Free chip RAM with one login, the A2065's driver
and netd: 996 KB with the mount idle, 820 KB after writing (the emulator's figures).

On the stock A1200 with SLIP up (1,159 KB free with one login, netd 116 KB: D19, *measured in FS-UAE*) one NFS mount
leaves about 1,010-1,090 KB free; a second mount pays its code again (~35 KB more). The 1 MB chip-only machine (232 KB
free with SLIP up) can mount one at the small profile with no second session; the matrix measures it (step 5), and if it
does not fit, the main session decides a smaller setting from the measurement.

### 9.3 Speed (*estimates*; measured on the real machines in step 6)

- **Over SLIP** (every Amiga): netd's small window (1,932 bytes) and SLIP's 11.5 KB/s: about 8-10 KB/s each way; `ls` of
  a directory of 50 files about 1 s; opening a file one or two round trips (~50-100 ms).
- **Over Ethernet on the PiStorm** (GENET, large profile): netd's copies and Emu68's speed decide; AmiNFSv3 measured
  13-14 MB/s with AmigaOS's stack (*read*); Zenix's figure is measured in step 6, never guessed.
- **In FS-UAE over the emulated A2065** (the stock A1200 has no Zorro slot: this path exists only in the emulator):
  emulator figures, labelled as such.

## 10. Security, said plainly

- **NFSv3 with AUTH_SYS trusts the client:** the NAS believes the uid Zenix sends. Anyone who can run a program on the
  Zenix machine can, without an MMU, act as any user the export lets in (D8, D19 9.2: users are a convenience, not
  protection). The NAS's export rules are the real protection: **export to the Amiga's address only, a share of its own,
  squash all users to one NAS user**; said in `mount`'s page and the owner's how-to.
- **Nothing on the wire is encrypted** (NFSv3 has no encryption without Kerberos): the home network only. A share that
  holds secrets is not mounted on Zenix.
- **Everything from the NAS is untrusted** (CLAUDE.md): every XDR length, count and string is checked against the record
  before use; a handle over 64 bytes, a name over 255 or with a NUL or `/`, a record over the buffer, a count past the end
  fail that reply (-EIO, logged) or the connection (3.4), never nfscd. The XDR decoder is fuzzed (11.1).
- **`nosuid`** always (8.3). **A NAS that lies** (wrong attributes, a handle of another file) can make Zenix show wrong
  data, never write outside nfscd's buffers.
- **Only the kernel and root** reach nfscd (4.3).

## 11. Tests

### 11.1 On the PC (host tests, `tools/host/nfs-test.sh`, in `check.sh host`)

- **nfscd's core on the PC** (as netd's core and `fsd-host`): the system-call-free part (`net/nfs/`: XDR, RPC, the NFS
  calls, the table, the caches, the request state machines) built for Linux against a fake of the kernel's side. Its OS
  layer (binding once approved): `nfs_now()` (milliseconds), `nfs_send(buf, n)` (what the socket took), `nfs_log(text)`,
  `nfs_alloc`/`nfs_free` (a counted heap with the profile's cap), and the core's entry points `nfs_request(req)` (a
  Zenix request in: its type, arguments, credentials, an id), `nfs_input(buf, n)` (bytes from the socket),
  `nfs_tick()`, `nfs_cancel(id)`, with answers handed back through `nfs_answer(id, result)`.
- **A test server of our own, `tools/host/nfs-sim.py`** (as `bbs-sim.py`): its own portmapper, MOUNT v3 and NFSv3 over TCP
  on an in-memory tree, with **faults on command**: answers dropped, delayed, out of order, duplicated; the connection
  cut mid-record; records in several fragments; a changed write verifier (a "restart"), `JUKEBOX`, `BAD_COOKIE`, stale
  handles, a reused fileid with a new handle, a different root handle, oversize records and lengths; a duplicate request
  cache keyed by address, port and xid. Checks: every request's result against the tree, data compared byte by byte
  after every fault, nothing waits for ever, a cancelled request answered at once, a late RPC answer after a cancel
  never reaches the next request (4.3), a reconnect from the same port answered from the cache.
- **A real NFS server without root:** **unfs3** (a user-space NFSv3 server, BSD licence; its source fetched at a pinned
  version at test time and built in the test's cache, never committed) with `-p` (no portmapper registration) and
  nfs-sim's portmapper, in `unshare -rnm`. `rpcbind` does not run there (*measured* by the review: it exits on
  `setgid`), and the kernel's own nfsd needs real root, so neither is used. If unfs3 will not run either, nfs-sim alone
  carries the tests and the real-server check is the owner's Synology (step 6). libnfs's `nfs-ls`/`nfs-cp` (installed on
  this PC) check nfs-sim against a real client.
- **The XDR decoder fuzzed** (libFuzzer, one run at a time with a memory limit: this PC's rule).

### 11.2 In FS-UAE (`tools/nfs-test.sh`)

The emulated A2065 with FS-UAE's slirp reaches the PC's 127.0.0.1 as 10.0.2.2 (D19 step 8); FS-UAE and the PC's test
server run in a user and network namespace, `tools/eth-test.sh`'s way. Parts:
- **e, the stock A1200 over the A2065:** `mount -t nfs 10.0.2.2:/export /nas`, `ls -l`, `cat`, `cp` a 100 KB file in and
  out (checksums compared on the PC), `mkdir`, `mv`, `rm`, symbolic links, a program run from `/nas` (twice: once with
  a change on the PC between the runs: the new one runs), `df`, `umount`; the memory (`free`, `ps`); refused mount
  points (`/etc/x`, `/sbin`).
- **h, hard:** the server stopped while `cat` reads: the log line, Ctrl-C ends `cat` within a second, `kill -9` ends a
  program that catches SIGINT, `mount` and `df` answer, `sync` answers -EIO; an `O_TRUNC` open and an `rm` killed
  mid-way (the calls after the kill cancelled too); the server started again: the next `cat` works; a write during the
  stop: the data whole after the restart (verifier changed in between).
- **s, soft:** `-o soft,timeo=5`: -EIO after the retries.
- **b, boot:** an `/etc/fstab` NFS line with a host name and no server: the boot reaches the login prompt with no wait;
  the first `ls /nas` waits, the server is started, it answers.
- **r, reboot:** a dead NFS mount listed before an ext2 mount: `reboot`; the ext2 mount is clean at the next boot (the
  network mount synced last, bounded).
- **k, nfscd killed:** while idle (the next use starts it again, the root handle checked from the saved table); with
  an open file (-ESTALE for that file until it is closed, its number never given to another; a new open works); a
  different export at the same address (refused, logged); a user's raw message to `fs.nfs0` (-EACCES).
- **u, users:** two Zenix users and squash on the test server: files created by each; a set-user-id program on the
  share runs as the caller (nosuid).
- **l, the serial cable:** the same mount over SLIP through `zenix-slirp` (part e's file copy only: slow), on the 1 MB
  machine too.
- Every part ends with `e2fsck` of the root image (nothing local damaged) and the PC-side tree compared.

### 11.3 On the owner's machines (step 6)

The Synology: mount, the owner's files with accents, a copy both ways compared, the NAS restarted during a copy, the
cable pulled (Ctrl-C), whether the unprivileged NAT port of SLIP is accepted (3.2), the speed table (SLIP on the stock
A1200; GENET on the PiStorm; the 060 if it has a network), labelled by machine.

## 12. For the main session (interfaces to claim; not owner questions)

- **`UMSG_CANCEL`** (a bit of `struct umsg`'s `kflags`, which servers read; proposed value 4: 1 is `UMSG_KERNEL`, 2 the
  internal `UMSG_NETREPLY`): set by the kernel only, for a killed client (5.1), on the message in flight and every new
  one; never cleared; a server may answer -EINTR, the kernel does not restart the call. Frozen once built.
- **`POLLPRI` (2) on a port** (5.2): "a message sent to this port was cancelled since you last looked"; growth of D16's
  frozen `POLLPORT` answer. Frozen once built.
- **`FS_SYNC`'s a0** (5.4): 1 = bounded (answer within 2 s), 0 as today; and `vfs_sync`'s order (network mounts last,
  by `fsinfo.type`). Growth of a frozen message's meaning where every caller passed 0.
- **`ESTALE` 116** (Linux's), "stale file handle", with its text in libz. Until claimed, nfscd answers -EIO.
- **D11 5.1's deadlock rules** get the NFS line of 4.2, with the refused mount points; D19 6.5's open point closes with
  it. DESIGN.md's D1 status line ("a hung server blocks its clients") gets "a killed client of a hung server is
  cancelled (D23)" when step 1 lands.
- **`mount`**: the type `nfs`, `HOST:/EXPORT`, `-o` with `hard`, `soft`, `timeo=`, `retrans=`, `ro`, `sync`,
  `actimeo=` (written as the codes of 4.1), the refused mount points; `/etc/fstab`'s type field gains `nfs` (growth of a
  frozen format: older programs never meet it); `mount -a` adds `l`. Its page, `commands.md`, `features.md`; `df`'s page
  (the 4 TB cap).
- **New files:** `/sbin/nfscd` (in `SBINAPPS`; `tools/mkroot.sh` names the `/sbin` programs one by one; every full root
  image; the base package or a network family package, D22's call).
- **Sizes:** the kernel's ~0.3 KB against the mount tests' floppy (2,152 bytes of headroom after D19 step 8: still fits;
  counted in step 1).
- **The feature halt:** the roadmap's approved order names "an NFS client" (block 2); the owner's yes to this note is the
  yes to `nfscd`, `mount -t nfs` and the two kernel changes.
- **Review depth:** two reviews done (kernel changes, a new kind of filesystem server); a second look at the cancel
  mark's code before step 1's merge.

## 13. Build order (each step small, merged with its tests; the full suite before each merge)

Steps that touch `kernel/`, `Makefile`, libz, `user/mount.c`, the images or the counts in `docs/` are the main session's;
a side session may take the parts marked *side* (CLAUDE.md "Sessions"), with this note's names and the core's interface
of 11.1.

0. *side:* **nfscd's core on the PC** (`net/nfs/`, system-call free, the interface of 11.1), `tools/host/nfs-sim.py`, the
   unfs3 check, the fuzzer, `tools/host/nfs-test.sh`.
1. **The two kernel changes** (5.1-5.2, 5.4) with their tests: a test server (`svctest` gains "hold the request until
   cancelled") shows a killed client marked and a caught signal not, `POLLPRI` reported and taken as seen, the calls
   after a kill marked, -EINTR passed on, no restart; `FS_SYNC` a0 and the mount order; fsd and zfsd unchanged (the full
   suite).
2. **nfscd read-only on Zenix:** `ROOT`, `OPEN` without create, `READ`, the stats, `READDIR`, `READLINK`, `CHDIR`,
   `STATFS`, `CLOSE`, `QUIT`; hard, soft, lazy, the saved root; `mount -t nfs`, `/etc/fstab`; FS-UAE parts e (reading),
   h (reading), s, b, k. **The first thing the owner can use:** reading the NAS's files.
3. **Writes** (section 7) and the rest of the calls; parts e (writing), h with writes, r.
4. **Users and nosuid:** part u.
5. **Over SLIP and on 1 MB:** part l; the profiles in the matrix.
6. **The owner's Synology and the speed table** (11.3), then a focused review of nfscd (the network's security audit at
   block 2's end covers it again).

## 14. Open questions for the owner (one line each, the recommendation in brackets)

1. When the NAS is off, should a program using it wait until it is back (Ctrl-C or `kill -9` stops it; what it was writing may be cut off there) rather than fail after half a minute? [Wait, as Linux does]
2. On your Synology: a shared folder of its own for Zenix, NFS allowed only from the Amiga's address, and every Zenix user mapped to your NAS user? [Yes; tell me the folder's name, or I suggest "zenix"]

## 15. Reviews

Two independent read-only reviews of revision 1 (2026-10-27); every finding is worked in above.

**Correctness, robustness, security** (read against ipc.c, svc.c, vfs.c, signal.c, proc.c, event.c, kbd.c, fsd.c,
zenixfsd.c, mount.c, cp.c, libz.c, netd): *blockers* - the cancel only woke the port's pollers, which `port_poll` never
reports for a request already received, so nfscd would notice only at its next timer (5.2: `POLLPRI` and a cancel count);
only the call in flight was marked, while the kernel makes more calls after a kill (`O_TRUNC`, `unlink`, `rename`,
`sync`, `mount`'s `FS_QUIT`: 5.1, `call()` marks them); the reboot sync claims were false and a dead NAS early in the
table would leave local mounts unsynced (5.4: network mounts last, `FS_SYNC` a0 bounded). *Should fix* - cancelling on
caught signals reversed D16 and would cut `sh`'s script reads (5.1: kills only); a stale message pointer after a cancel
(4.3: request ids, the link cut at the reply); a cancelled WRITE already copied must answer its count, and big writes
must never come back short (5.2, 7.1); the duplicate cache matches address and port, so reconnect from the same port,
random xids (3.4); EXCLUSIVE create needs a SETATTR (4.5); commits with the writer's credentials, pinned entries (7.1,
4.3); stale numbers reserved after a restart, identity by (fileid, handle) (4.4); the kernel-and-root-only rule (4.3);
close and sync errors cannot reach programs (7.3 says so); umount's real order and no `-f`/`-v` interfaces (7.4); the
volume id `mount` could not compute (4.1: the saved table); a host name in fstab at boot (4.1); the start order and
netd's load (4.1); a killed nfscd spinning (4.3); `mount` and `df` hanging (5.3); an oversize record retried for ever
(3.4). *Minor* - the small TCP window (3.4, 9.3), keepalive (3.4), the text budget, fsd's 8 links, ~19 calls, the
Synology's ports, the memory figures, readdir positions, unit and range details, path checks, per-call credentials;
facts checked correct: SVC_TEXT, the structs, G4, several requests in flight, POLLPORT, root's ports, the D13 key, paths
relative to the mount, BLAKE2s in random.c, useradd, init's order, AmiNFSv3's figures.

**Architecture fit, frozen interfaces, size** (the text budget computed, rpcbind run in `unshare -rnm`): *blockers* - the
volume id cannot exist for lazy fstab mounts and `mount` has no MOUNT code (4.1: `SVC_SAVE` of the root handle, no `-id`,
no hash); the host name resolved by `mount -a` before the network (4.1: registered as typed, resolved by nfscd); the
cancel wake-up swallowed (the same as above); the 80-byte budget wrong and seven words with `lazy` (4.1: four words,
short codes, the real numbers). *Should fix* - the caught-signal reversal and the calls after a kill (the same as above);
the deadlock rule incomplete: an NFS mount on `/etc` or `/sbin` makes a cycle with netd (4.2: refused mount points); the
duplicate cache and the source port (the same); the privileged-port claim unverified for SLIP and the Synology (3.2);
`umount -f`, `mount -v` and a killable umount need unnamed interfaces (7.4: dropped); `zup`'s sync hanging an update
(5.3); ZEX2 for a server breaks D13 stage 1 (4.1: plain ZEXE); a second hash copy (gone with the saved table); memory:
the large profile's size, netd not counted, the small window, an invented third profile, two different figures (9.2);
`/etc/nfsmap` a new frozen format for a corner case (8.1: DSM's squash, no map); STATFS above 4 TB (4.5); owner questions
that were technical calls (14: two left, both about the owner's NAS and what Zenix should do). *Minor* - `fsinfo.type`
is 8 bytes; `lseek` is signed (1); `mount` gets only an error number (4.1); netd's load as the exception to "never
blocks" (4.1); rpcbind in a namespace (11.1, *measured*: it exits); the namespace pattern (11.2); `mkroot.sh`'s `/sbin`
list, the mount floppy's headroom, ESTALE's text, D1's status line (12); the name cache sized with READDIRPLUS (6.3);
groups read at the start (3.3); the side session's core interface written down here (11.1); the mount slots (2).

Revision 2 keeps the shape of revision 1 (NFSv3 over TCP, one server process per mount as a D11 file service, a loop
that never blocks, UNSTABLE writes with COMMIT and the verifier, `nosuid`, the D11 rule change, one kernel mark instead of
breaking G4); both reviewers judged that shape sound. It adds a second small kernel change (the reboot sync order) and
removes `-id`, the hash, the user map and `umount -f`.

Last checked against the code: 2026-10-27 (main at 22fd4ba).

## As built: step 0, the NFS client core (side helper, merged 2026-10-28; reviewed by the main session)

Built: `net/nfs/` without system calls (`nfs.h` the interface, `rpc.c` XDR/RPC/AUTH_SYS/record marking, one connection,
reconnects with old xids, hard and soft timeouts, JUKEBOX; `proto.c` GETPORT, MNT/UMNT, NFSv3's 19 calls and NULL with
every length checked; `table.c` handles, inode numbers, caches, readdir positions; `ops.c` every FS_* request as a small
state machine; `write.c` UNSTABLE, COMMIT, the verifier and resend; `core.c` setup, options, the mount sequence, the
saved-root check). m68k -Os: about 37.2 KB of code, no 64-bit multiply or divide. Tests: `tools/host/nfs-test.sh` (in
`check.sh host`): `nfs-sim.py` (portmapper, MOUNT, NFSv3 with every fault of 11.1; 16 scenarios, 185 checks), libnfs as a
check on the simulator, a real unfs3 0.10.0 run rootless (fetched with a pinned sha256, never committed), and a fuzzer
(300 s, 850,173 inputs, nothing found; each request answered exactly once, no leaks).

The step-0 helper's interface additions and smaller choices (its report, 2026-10-28: `nfs_connect`/`nfs_connected`/
`nfs_disconnect`, `nfs_save`, `nfs_poll`, `nfs_kill`/`nfs_end`/`nfs_init`/`nfs_codes_parse`, the "NFS1" saved-table
format of at most 77 bytes, "broke twice" counting only closes/resets while a call is out, STATFS and the root's stat
bounded at 2 s from the cache, a handle without attributes refused, a changed handle for the same fileid kept on one
inode number, reads pipelined only within a request, JUKEBOX retries 1, 2 then every 4 s, 8/32 requests per mount, a short
WRITE count as -EIO, a failed COMMIT dropped with a log line, ACCESS cached per credential, ext2 file-type codes) are
binding unless changed here.

**Decided by the main session (2026-10-28), for step 1:** (a) **memory:** the idle tables were about 87 KB against the
note's ~70 KB with code; the node table (256 entries of 158 bytes, each with a 64-byte handle) shrinks: handles move
to a separate pool sized to their real length (Linux and Synology handles are 28-36 bytes), and the small profile keeps
64 nodes (the large one 256); target under 40 KB idle on the stock A1200, measured; (b) **an unlinked open file** gets
Linux's "silly rename" (`.nfsXXXX`, removed at the last close), because Unix programs rely on reading an unlinked
temporary file; (c) **CHOWN** is checked locally as fsperm rule 5 (root only), and the NAS decides too; (d) `ESTALE`
(116) is claimed in step 1 and replaces -EIO for stale handles; (e) messages stay queued in nfscd while `NFS_P_TAKE` is
clear, as the host client does; (f) `mount.c` turns a checked mount's error numbers into messages.

## As built: step 1, the two kernel changes (branch nfs12, 2026-10-28)

Built as 5.1, 5.2 and 5.4 say (`kernel/ipc.c` `ipc_cancel_mark`, `call()`, `port_poll`; `proc_kill`; `vfs_sync`),
tested in FS-UAE by `tools/svc-test.sh c` with `svctest serve`, a server that holds every request until it is cancelled.
What the text did not settle, decided conservatively:

- **A plain `recv` does not take a cancel as seen;** only an `evwait` that asked for `POLLPRI` and reported it does
  (5.2 said "or its next `ipc_recv`"). Otherwise a kill between nfscd's poll and its receive would be swallowed for a
  request nfscd already holds. A server that never polls (fsd) only lets the port's count grow.
- **`proc_exit`'s calls are never marked** (the closes of a killed program): they must reach the server as they are,
  and nfscd answers `FS_CLOSE` at once anyway (7.3).
- **A service port's count is taken as seen when a new server takes the port over** (its queued cancelled messages
  come to it marked; nothing it holds yet).
- **The network flag** is the type the server gives in `FS_ROOT`'s `fsinfo` (`nfs`), at the mount and at a later
  first `FS_ROOT`; not the type `mount` passes.
- **`ESTALE` 116** is claimed (errno.h, libz's text "stale file handle"); the core's `NFS_ESTALE` now uses it.
- **`FS_CLOSE` is never marked** (any close, not only `proc_exit`'s: the kernel has already dropped its file, so a
  close answered -EINTR would leak the open in the server); **`FS_QUIT` is carried out marked or not**, dropping what
  it could not save; **a bounded `FS_SYNC` ignores the mark** (a killed `reboot` must not lose the 2 s of commits);
  diskd ignores it (a killed server's own calls may carry it). All four are in the frozen text (`fsproto.h`).
- **A loader killed while it reads the program** (its reads are now cancelled) passes the load to the next client,
  as one killed in its back-off does, instead of "cannot start" failing the whole queue.
- **Size:** the kernel grew by 556 bytes (`kernel.bin` 160,332 -> 160,888 on the disk; section 9.1 estimated ~300):
  1,928 bytes left on the mount tests' floppy, 18,312 on the RAM floppy.

One independent read-only review (2026-10-28): no blocker. Its should-fix findings are fixed: `FS_CLOSE` marked by
`call()` (now never), a killed `reboot` cutting the bounded sync short (the sync ignores the mark), a killed loader
failing the queue (it passes the load on), and test gaps (kill -9's mark, a cancelled read whose exit close must come
unmarked: `svc-test.sh c`). Minor ones fixed: comments (POLLPRI also for queued messages; loads and watches),
`FS_SYNC` a0 "any other value as 0", `mt->net` set only after a root, architecture.md's wording. Not tested in FS-UAE,
said: the handover's reset of the seen count and "a plain receive does not take it as seen" (read in the code).

## As built: step 2, nfscd reading (branch nfs12, 2026-10-28)

Built: `/sbin/nfscd` (`user/nfscd.c`, the OS layer over `net/nfs/`), `mount -t nfs` and fstab's `nfs` lines
(`user/mount.c`), the base package and every full root image carry it. Tested in FS-UAE by `tools/nfs-emu-test.sh`
(not `tools/nfs-test.sh` as 11.2 says: `make check` already has a test called nfs-test, the host one), parts e
(reading), h (reading), s, b and k of 11.2 in two boots, with `tools/nfs-host.py` (nfs-sim's server with the test tree,
steered by the Amiga's lookups of `.ctl-*` names) as the PC's server. Decided where the note was silent:

- **Step 2 serves reading only:** nfscd adds the code `o` itself, so every change is -EROFS until step 3 brings the
  writes' tests (the core has writes; they are not let loose on a NAS before FS-UAE has checked them).
- **Memory (decision (a)):** a handle of up to 36 bytes lives in its node (Linux's and the Synology's fit), a longer
  one on the heap; 64 nodes and 64 cached names on the small profile, 256 on the large; all tables and the two buffers
  are one heap block. The heap is libz's region heap (moved there from wasabid: one copy, `rheap_alloc`), its region
  16,400 bytes so two 8 KB cache pieces share one. **Measured (FS-UAE, stock A1200, `ps`): 112 KB mounted and idle,
  144 KB after reading (the data cache full)**, against the note's ~70 / ~150 KB: the code is 51.6 KB (9.1 estimated
  30-36 KB), the tables 25 KB, the receive and send buffers 9 KB each. With SLIP's netd and one login the stock A1200
  keeps about 790-990 KB free in these runs (the emulator's figure).
- **The source port:** netd has no abortive close (no `SO_LINGER`), so the port of the connection to the portmapper
  or mountd is still closing when the next connection is made: nfscd then takes the next port (bind or connect said
  "address in use"). For the duplicate request cache this matters only for changing requests (step 3); *open for the
  main session:* netd's `SO_LINGER` {1, 0} (an abort) would keep one port, as 3.4 wants.
- **A host name** is looked up by `SOCK_RESOLVE` on a socket of its own, inside the loop (the loop never waits for
  DNS); a number needs no lookup.
- **Messages held by nfscd** (decision (e)): up to 8 more than the core's slots; `FS_CLOSE` and a cancelled message go
  to the core at once; on `POLLPRI` each held message newly marked is cancelled once.
- **The groups** come from `/etc/passwd` and `/etc/group` at the start: 32 users with groups, 16 groups each.
- **mount's messages** (decision (f)): -ETIMEDOUT "the NAS did not answer", -EACCES "refused the mount (its export
  rules)", -ENOENT "no such export", -ECONNREFUSED, -EHOSTUNREACH, -ENETDOWN, each "(see dmesg)". A missing or
  non-directory mount point is the kernel's own refusal.
- **wasabid still refuses files on an NFS mount:** D24 5.3 serves them through a helper (`wasabid --file`, killed after
  10 s without progress) that needs the cancel mark; the mark exists now, the helper is D24 work not yet built, so the
  refusal stays (its words now name the helper). *Open for the main session.*
- **Not checked in FS-UAE in this step:** reading over SLIP (part l, step 5), the large profile (the emulated A1200
  with fast RAM and the A2065: the matrix, step 5); the real Synology (step 6).

Sizes: `/sbin/nfscd` 51,616 bytes of image (58,460 on the disk with its relocations), 60.9 KB loaded with its 8 KB
stack; `mount` 15.5 KB; the kernel unchanged by step 2 (160,888 bytes on the disk: 18,312 left on the RAM floppy,
1,928 on the mount tests' floppy); nfscd is on neither floppy.

## Owner decisions (2026-10-28)

The owner answered "Yes" to every recommendation of this note's questions.

## Main session, 2026-10-28: steps 1-2 merged

- **Memory:** nfscd at 112 KB idle on the stock A1200 is too much for the golden path: step 3 makes the small
  profile's rsize/wsize 4 KB (about 25 KB less) and re-measures; 9.2 is amended then.
- **`SO_LINGER {1,0}`** (an abortive close, D19 interface growth) is added to netd in step 3, before writes rely on
  the duplicate request cache's "same port on reconnect".
- **wasabid's refusal of nfs files** stays until D24's `--file` helper is built (D24 step 3 or later).

## As built: steps 3 and 4, writing, users and nosuid (branch nfs34, 2026-10-28)

Built: **nfscd no longer adds `o`**, so the core's writes (section 7, `net/nfs/write.c` since step 0) reach the NAS;
`-o ro` is the only read-only mount. The main session's two decisions: **the small profile's 4 KB transfers** (9.2,
amended and measured above) and **netd's `SO_LINGER`** (Linux's 13 and `struct linger`, D19 growth: `{1, 0}` closes by
an abort, a reset with no FIN_WAIT or TIME_WAIT, so the port is free at once); nfscd sets `{1, 0}` on every connection,
so a reconnect comes from the same source port and the NAS's duplicate request cache can answer a resent REMOVE or
RENAME (3.4). Two decisions of 2026-10-28 that waited for writes: **(b) the silly rename** and **(c) CHOWN for root
only** (fsperm rule 5; the NAS decides too). `nosuid` (8.3) was in the core since step 0 (no set-user-id or
set-group-id bit in any `ustat`, `CHMOD` with them -EPERM); step 4 tests it on Zenix. Tests: `tools/nfs-emu-test.sh`
parts **w** (e's writes and h with writes), **r** (reboot) and **u** (users), each its own boot; the host's
`nfs-sim-test` scenario `silly` and more checks in `users` and `cache`, unfs3's run (an open file unlinked on a real
server), `netd-host`'s `linger` checks (`tools/host/net-test.sh`).

Decided where the note was silent:

- **A longer linger** (`{1, N}`, N > 0) is kept (`getsockopt` answers it) but not waited for: a close never blocks in
  netd (D19 rule 11); the data goes out after the close as without the option. Linux would block `close`.
- **The silly rename** (Linux's): an `UNLINK`, or a `RENAME` over a name, of a regular file with one link that the
  kernel holds open (`FS_OPEN` counted against `FS_CLOSE`) renames it first to `.nfs` + 8 hex digits of its fileid + 4
  of a counter in the same directory, after a `LOOKUP` shows that name free (8 tries, then -EBUSY), so it never
  replaces a file. A name not in the name cache is looked up first (one more call), so an open file is never missed.
  The stand-in is removed after the file's last close, once its unsaved data is committed; `FS_QUIT` removes the closed
  ones before `UMNT` (within its 10 s). At most 8 at once (`NSILLY`): more are removed at once, logged once (their
  readers then get -ESTALE, step 2's behaviour). Unlinking a stand-in while it is open is -EBUSY (Linux's). A stand-in
  stays on the NAS when nfscd is killed or crashes, as on Linux after a crash; a later `rm` removes it. A cancelled
  `UNLINK` whose `RENAME` was already sent still gets its stand-in (the entry is the rename's, not the request's); a
  connection that breaks while such a `RENAME` is out with nobody waiting takes the stand-in as made (its names lived in
  the request), and the later `REMOVE` finds out (a missing name is not logged). The stand-in name is not frozen.
- **CHOWN**: the kernel already refuses a non-root `chown` (-EACCES, `SYS_chown`); nfscd's -EPERM for a request stamped
  with another uid is the server's own check, as fsd's.
- **The same source port** cannot be seen across slirp's NAT in FS-UAE (each connection is a new port on the PC), so
  `netd-host` checks `SO_LINGER` itself (a plain close leaves the same connection -EADDRINUSE while the peer is open;
  `{1, 0}` resets it and the port is bound and connected again at once) and FS-UAE checks that no connection fails with
  "address in use".
- `tools/host/net-test.sh` with an absolute `NET_TEST_OUT` built zenix-slirp into the source tree (`$PWD/$OUT`): fixed
  (found while testing).
- **Part r** uses an ext2 floppy (`df1`) as the local mount after the NFS one; part **u** the shipped users `user`
  (1000) and `guest` (1001) and the test server's squash (`.ctl-squash-1026-100`, DSM's "map all users"); the
  set-user-id program is `id` (mode 4755, root's) on the share, and the same file copied to the root runs as root.

Sizes: nfscd 54,120 bytes of text (+2,526 for the silly rename and CHOWN's check), 9,456 of bss; netd +226 bytes;
libz's region heap unchanged in code; the kernel unchanged (160,888 bytes on the disk).
