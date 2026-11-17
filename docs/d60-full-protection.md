# D60: full memory protection on CPUs with an MMU

Revision 2, 2026-11-18: revision 1 after its two independent design reviews, every finding folded in or answered
(section 17); proposed, nothing built, for the partner to see before any code (it changes base decisions). The
partner's decision of 2026-11-17 (docs/decisions.md, "Full memory protection"): "the memory protection should be like
real full memory protection, like you expect from a modern operating system. We are trying to build a modern operating
system for an ancient computer." It builds on D41 (docs/d41-memory-protection.md, revision 3: on by default where the
CPU has an MMU) and closes the limits its section 2 names. It changes two base decisions in DESIGN.md: D1 (messages
pass pointers, not copies) gets a kernel copy of the message and exact windows onto the buffers instead of the whole
client, and D8 (servers are trusted code) loses its trusted servers. Sources: the MC68030, MC68040 and MC68060 user's
manuals (the knowledge base's `manuals/`, sections named where a fact is used), the Amiga Hardware Reference Manual
(the custom chips' registers), D41 and its measurements (sections 15 and 19), and Zenix's own code (named by file). No
other system's code was read.

## 1. What the partner gets, in plain words

On an Amiga with a working MMU (a 68030, 68040 or 68060 with its MMU: the Apollo 1260, the A3000, most 030 cards),
**every process sees only its own memory, plus what it is given on purpose**, and nothing of the kernel. That now
includes the processes D41 left trusted: the filesystem servers, the disk server, the network server and its drivers,
the sound server, the USB server, `init`, `boot` and everything the service manager starts.

- A server gets, for each request it holds, a **copy of the message** in its own memory and a **window onto exactly the
  bytes of the client's buffers** the request is about, readable only or also writable as the request needs, for as
  long as it holds the request. The rest of the client's memory stays out of its reach, and so does the window once it
  has answered.
- **Who decides which bytes**: for the kernel's own requests (every file and socket call) the kernel, which knows each
  buffer. For a request a program sends itself, the client declares its buffers (a new form of the call); a server of
  the system (a service the service manager starts) or one that could debug the client anyway may still ask for a range
  of its client, as the display server does today (section 5.5). So a server any user runs sees only what its callers
  hand it.
- A driver gets **its own device's registers and its DMA memory**, nothing else. The sound server and the plipbox
  driver get Paula and the parallel port through the kernel, checked.
- A bug in the ext2 server can then damage only the files it serves and the buffers lent to it; it can no longer
  write into the shell, another server or the kernel.
- **What stays trusted, honestly** (section 6): the display server holds the custom chips until a later stage gives
  the blitter and the Copper a checked door (stage 7): through them it can read and write any chip RAM (not fast RAM)
  and stop the machine. ZINE is a take-over of the machine, on purpose, by root at the Amiga's console. Root keeps Unix
  root's powers through checked doors (section 8), not raw memory.
- **Speed**: D41's measured cost of a call between two protected programs (x3 to x7, section 19.4) is replaced by a
  switch that rewrites one register (each process has its own translation tree) and windows that stay mapped while a
  request is held. The budget (section 9): a filesystem request at most 10% slower than with protection off, a bare
  message round trip at most 1.5 times (a target the reviews call at risk: the ATC refills); measured in two emulators
  for the ratios and on the 1260, at the end of stage 1 already, for the truth.
- **Machines without an MMU** (the stock A1200's 68EC020, EC parts, the PiStorm's Emu68) run exactly as today and pay
  nothing but the floppy bytes of the new code and a few resident gates (section 11).

## 2. Where D41 leaves the doors open: every exemption today

Read from the code of main at 90332d7 (2026-11-17). These are the cases this note must close or name.

| What | Where | Why it needs to see more than its own memory today |
|---|---|---|
| Every service manager server (`fsd`, `zfsd`, `diskd`, `netd`, `displayd`, `sndd`, `usbd`, `msgpingd` as `ping`, the built-in images) | `svc_born` (svc.c) marks it system | it reads and writes its clients' buffers in place (D1); the kernel's requests lie on a kernel stack |
| A process creating a reserved port (`fs`, `fs.*`, `disk`, `net`, `audio`; the tests' `socktestd`) | `port_create` (ipc.c) | the kernel sends it requests whose message (and for `fs.*` its group list, a path, a `ustat`) lies on the caller's kernel stack (`fs_call_k` in vfs.c, `net_call` in sock.c with its `struct netreq`, `drv_block` in drv.c) |
| A mount's server (`fatd`, `ffsd`, `pfs3d`, `iso9660d`, `exfatd`, `nfscd`, a second `fsd`/`zfsd`) | `SYS_mount` (vfs.c) | the same |
| A network driver after its claim (`a2065`, `ne2000`, `plipbox`; `genet` on the Pi) | `SYS_drv` (drv.c) | it drives its board's registers and its DMA memory in user mode; **and the frames cross between netd and the drivers by address**: netd allocates the receive buffers in its heap and sends their addresses (`net/netd/iface.c`, `main.c`'s `a->buf[k]`), the drivers write into them (`ne2000.c`, `plipbox.c`), transmit pieces point into lwIP's buffers in netd's heap, and netd reads a DMA driver's claim memory directly |
| `diskd` | `SYS_blk_ram` (syscall.c) | it copies the boot RAM disk's sectors with `memcpy` from the RAM disk's own memory (`user/diskd.c`, `ram_base`) |
| `sndd` | `SYS_drv` `DRV_CLAIM` of `DRV_PAULA` | it writes Paula's `AUDxLC/LEN/PER/VOL`, `DMACON` and `INTREQ`, reads `INTREQR` (also in a short wait loop) and `VPOSR` itself (user/sndd.c) |
| `usbd` | the same, `xhci` | the xHCI's registers and DMA memory (only on the Pi: no MMU there) |
| A display server (`displayd`; the test `gcontest`) | `CON_GRAB` (gcon.c) | it drives the blitter (`display/server/draw.c`: `BLTCONx`, the pointers, `BLTSIZE`, `DMACONR`, the beam), writes its Copper lists in its chip RAM and draws there; **and between requests it reads each client's event ring's `tail` word in the client's memory** (`displayd.c`'s `flush`) |
| A protected server holding a request (`msgpingd` from a shell, the tests' servers, any user's server) | `mmu_lend` (ipc.c, mmu.c) | the client's whole view is lent: D1's model has no notion of which bytes the request is about |
| `zup` | `view` (mmu.c): every uid 0 process | it reads the update bundle in place (a kernel block, not on page bounds: its two end pages show a few bytes of kernel blocks) |
| `priotest` | runs with protection off | its children read the parent's block by its address |
| `pure-test` | runs with protection off | it expects D13's checksum to find a write into shared code |
| ZINE | `mmu_hand(0)` | an AmigaOS program gets the whole machine with translation off |
| The kernel's own reads of low chip RAM at run time | `hw()` in drv.c reads the hardware table at 0x1100; `mmu_fault_` the bootinfo at 0x1000 | harmless while nothing user-driven can write chip RAM; not once a DMA engine is in a confined process's hands (section 6.4) |
| The kernel's own fields of a message, in the client's memory | `call()`, `ipc_recv`, `ipc_client`, `ipc_best_client`, `ipc_proc_exit` (ipc.c); `NETSRV_REPLY` (sock.c) sets `UMSG_NETREPLY` there | the queue link, the state, the client, the stamped `uid`/`gid`/`kflags` live in the client's `struct umsg` and are read back: memory others could write once shared memory or lends exist (section 5.1) |

Also part of the picture: which servers ask the kernel before they touch a client pointer. The display server asks
(`SYS_peer` `PEER_RD`/`PEER_WR` before every read or write, D38 step 3), `sndd` and `usbd` do for some requests; the
others do not: `diskd` (`DISK_READ`/`DISK_WRITE`'s buffer, `DISK_INFO`'s struct), `sndd`'s other requests, `usbd`'s
others, `msgpingd`, `svctest`, `socktestd`, and every filesystem server for the kernel's requests (paths, `ustat`,
`udirent`, the cookie, the data).

## 3. The model

### 3.1 Who sees what

With full protection on, a process's **view** (what its user-mode accesses may reach) is exactly:

1. **Its own memory**: its block (image, BSS, stack, arguments), its `SYS_memalloc` regions, read and write; its code
   (a ZEX2 program's shared segment or private copy), read only. As D41 section 6.
2. **Shared memory it accepted** (section 7.4, `SYS_shm`): a region another process offered it and it took, read only
   or read and write as offered; a claim's DMA memory or netd's frame pool given to a driver the same way.
3. **The windows of the requests it holds** (section 5): exactly the bytes of the client's buffers that the kernel
   declares, the client declares, or (for a trusted asker, 5.5) the server asks for, in the request's direction, until
   it answers. A window may cover whole pages of a block the kernel made for another process (the loader's `FS_READ`
   into a new program's block): only those pages, only for that request.
4. **Its claim's device** (section 6): the register window of the one device it claimed (or the part of it it needs),
   cache-inhibited, and the claim's DMA memory.
5. **Its inbox** (section 5.1): one or more pages of its own memory where the kernel writes the copies of the messages
   it receives (counted as its memory; never freeable, never lent or offered).
6. For `diskd` (the `priv` process the kernel starts for the disk service): the boot RAM disk, read and write (6.2).
7. For the process that asked for it as root (`zup`), the update bundle, read only (section 7.2).

Nothing else: not the kernel (its image, heap, stacks, tables, the message records, other processes' inboxes), not
another process, not free memory, not the chips, the CIAs, the ROM or the boards. **No process is a system process any
more**: D41's system mode, its mask `mmu_sys` and its five marking rules (D41 section 7) go.

### 3.2 What it defends against, and what not

- **In**: a wild pointer in any process (D41 already); a hostile program run by any user; a hostile or broken server
  run by any user (it sees only what its callers declared, only while it serves them); a broken system server (an ext2
  bug damages the files and buffers it was given, never the kernel or another process); a server answering a request
  after it gave the answer (the window is gone); a client or a co-holder of shared memory rewriting a message in flight
  (the kernel no longer reads its own fields from the client's memory, 5.1).
- **Not in**: a DMA engine in a process's hands (no IOMMU on any Amiga: section 6.3); ZINE's take-over (section 7.3);
  a service of the system asking for more of its client than the request needs (it may, 5.5: it is the system's own
  code, as a Unix kernel driver is; a bug of that kind is contained to its clients' memory, never the kernel or other
  processes); root replacing the system's files on disk (a signed system is D15's and D22's business); using up CPU
  time or memory; timing side channels; the hardware itself (a Zorro card can write anything).
- The documents keep D8's honesty: until stage 7 is built, "the display server is trusted with chip RAM" is written
  where D8 says what is protected (section 15).

### 3.3 The kernel

The kernel never uses the processes' trees: its accesses are supervisor accesses and the transparent translation
registers match **supervisor accesses only** (D41 section 4's protected-mode values: 040/060 `DTT0` 0x0000A040,
`DTT1` D37's fast block with `S` = 01, `ITT0` 0x00FFA000; 030 `TT0` 0x00FF8143, function codes 4-7). Today these are
the values only while a protected process runs; with D60 they are written once at boot and never change, because no
process runs in D41's system mode. A user access never matches a window, so every user access goes through its own
tree, where kernel memory is never valid. So the kernel's memory is invisible to every user-mode process on all three
CPUs (both reviews checked it; `MOVES`, `PTEST` and `PLPA` are privileged).

## 4. The shape: a tree per process

- **Identity mapping stays** for every process's own memory: an address is the same byte in every process and in the
  kernel. Pointers in messages stay valid; the loader, spawn, shared code (D13) and ZINE are unchanged; the kernel
  keeps touching user memory by plain pointers after `user_ok_*`.
- **One translation tree per process** (D41 section 9's first later step, taken further: whole roots, not page tables
  at the pointer level). A process's tree has valid descriptors only for its view (section 3.1). The **only**
  translation that is not identity: the part pages of a window (section 5.3) map the client's address onto a
  kernel-made copy page (a "bounce page"), so the server sees exactly the buffer's bytes at the buffer's own address.
- **The switch** (`mmu_switch`), interrupts masked: the descriptor writes of that tree already pushed (they are pushed
  when written, never later) and a `nop` (the 060's store buffer, as D41's code does), then the next process's root
  into `URP` (040/060, `MOVEC`) and a `PFLUSHAN` (MC68040UM 3.7.2, MC68060UM 4.7.2: flushes every entry whose G bit is
  clear), or into `CRP` on the 030 (`PMOVE` with FD = 0, which flushes the ATC itself: MC68030UM 9.7.5.1). No page is
  flipped: the cost no longer grows with the processes' sizes. The kernel keeps which root is loaded; idle leaves it as
  it is; a tree freed while loaded is replaced by the kernel's empty tree at once.
- **`SRP`** (040/060) is the kernel's own tree, with nothing valid but the claimed boards' windows above 16 MB (for the
  quiesce programs, section 6.2): a stray supervisor access outside the windows still stops the kernel with a panic.
  The 030 needs none (its `TT0` covers every supervisor access).
- **Global pages** (040/060 only; on the 030 bit 10 of a page descriptor is part of the address and is never set): the
  pages of a D13 shared segment whose program file is readable by everyone (mode `o+r`: its bytes are no secret) carry
  G (bit 10), so `PFLUSHAN` keeps their ATC entries across a switch: `libz`'s shared code and the shell's stay cached.
  The cost: a process not running that segment may read it through a kept entry until the next full flush; acceptable
  only because such a file is readable by everyone anyway. A segment of a file that is not readable by everyone never
  carries G. A `PFLUSHA` (everything) at every segment's end and at every change of a segment's pages. The non-global
  flush variants are used only at the switch; every window and page change uses the plain `PFLUSH`, so no G entry
  survives a change that matters. A window's part page that lies in such a G page is mapped identity (its bytes are
  world-readable and read-only), never bounced.
- **Tables**, per process, the D41 geometry (4 KB pages; root 128 entries, pointer tables 128, page tables 64; the
  030's `TC` 0x80C07760): a root (512 bytes), a pointer table (512) for each 32 MB its view touches, a page table (256)
  for each 256 KB, aligned as the manuals ask (512 and 256 bytes; 16 on the 030). They come from a **table pool**
  (256- and 512-byte units cut from 4 KB kernel pages, fast RAM first), are made when a block is added to the view and
  given back when their last valid entry goes. D41's one shared tree over all the pools goes. 8 KB pages (one `TC`
  bit on 040/060, `PS` 13 on the 030; page tables of 32 entries) are a variant measured in stage 1 (section 9).
- **Descriptor rules** stay D41's (section 5): U and M preset, valid by bit 0, W for read only, every table write
  pushed with `cpushl` on a 040/060 with the data cache on (the 060's table search does not look in the cache,
  MC68060UM 4.2.5) before the ATC is flushed. **Cache modes**: a page of memory has the mode of the kernel's window
  at the address of the memory it maps (for a bounce page: the copy page's address, not the client's); a page of
  registers (the chips, the CIAs, the PC Card slot, a board's registers) is `CM` 10 on 040/060 (inhibited,
  serialized; on the 060 "precise"), never 11 (which lets a read pass a pending write), and has CI (bit 6) set on the
  030.
- **ATC rules**: on 040/060 `DFC` is set to 1 (user data) before a `PFLUSH (An)`; on the 030 `PFLUSH #0,#4,(An)`
  (function codes 0-3). A page made valid in the running process's tree gets a `PFLUSH` of that page (an instruction
  prefetch may have left an invalid entry); a page made invalid gets a `PFLUSH` before the kernel returns to user mode
  (one per page up to 8 pages, a `PFLUSHA` above that); a change in a tree that is not loaded needs no flush (the
  switch flushes).
- **The 030's caches are logical** (MC68030UM 6.1 and 9.2.1: they are searched before the translation, and a read
  hit is used without any check; their tags are the logical address and the function code, which no two user
  processes differ in). D37 leaves the 030's data cache off, and **with full protection it must stay off** (or be
  cleared, `CACR` CD, at every switch: not done, the off state is simpler and is D37's today). The instruction cache
  (256 bytes) stays on: a process could run up to 256 bytes of another program's code still in it, never read or
  write its data; a leftover named in limits.md. The 040/060 caches are physical: a bounce page and the client's page
  never alias.

## 5. Messages: a kernel record, a copy for the server, windows onto the buffers

The message protocol keeps its frozen meaning for every client: a client still builds a `struct umsg` in its memory,
passes its address, and gets `result` and `a[]` back in it; its buffers are still filled in place (no copy for whole
pages). What changes is what the kernel trusts and what the **server** is shown.

### 5.1 The message: a record in the kernel, a copy in the server's inbox

- **The kernel's record.** At `call()` the kernel copies the client's message (the `struct umsg`, and for its own
  requests the `struct umsg_groups` after it) once into a record of its own, one per process slot (it has at most one
  call in flight), stamps it there (`from`, `client`, `uid`, `gid`, `umask`, `kflags`), and queues the record. The
  queue, `ipc_recv`, `ipc_client`, priority inheritance (`ipc_best_client`), `ipc_proc_exit`, `ipc_cancel_mark`
  (`UMSG_CANCEL`), `NETSRV_REPLY` (`UMSG_NETREPLY`) and the kernel's own readers after a reply (sock.c's and drv.c's
  test of `UMSG_NETREPLY`) all use the record only. The client's message is written once more, at the end
  (`ipc_finish`: `result` and `a[0]`..`a[3]`), and never read again. So nothing another process can write (a lent
  window, shared memory) can forge a kernel request, a uid, a queue link or a state. The records (32 x about 76
  bytes, 2.4 KB) are allocated by `mmu_setup` only when full protection is on; without it the client's message is used
  as today.
- **The server's copy.** At its first `recv` a server gets an **inbox**: a page of its own memory (a `MEM_USER` block
  owned by it, valid in its tree, counted in `ps`, refused by `SYS_memfree`, never lent or offered), cut into 1 KB
  slots; another page when it holds more requests at once, up to `NPROC` slots. `ipc_recv` copies the record's public
  fields (`type`, `a[]`, `uid`, `gid`, `umask`, `kflags`, `from`, the groups; `next`, `client` and `server` written 0)
  into a free slot and **returns the slot's address**. The slot's link to the request is kept in the kernel, with a
  generation: a slot is handed out again only after the others (round robin), and a second reply to an answered slot
  gets -EINVAL as a stray pointer does today.
- Every call that names a request by its message (`replyrecv`, `SYS_reply`, `SYS_peer`, `NETSRV_REPLY`) takes the
  slot's address; `ipc_client` looks the slot up among the slots of requests really in flight to the caller.
- At the reply the kernel copies `a[0]`..`a[3]` from the slot into the record (servers answer there: netd's `a[2]` and
  `a[3]`, msgpingd's, sigtest's), except an argument the kernel rewrote (5.2), which gets its old value back, and then
  into the client's message with `result`. Nothing else is taken from the slot.
- `UMSG_CANCEL` set while the request is held is set in the record and in the slot.
- So **a message on a kernel stack is never shown**: `fs_call_k`'s, `net_call`'s and `drv_block`'s messages reach the
  server as copies. This closes D41's rules 2, 3 (its kernel messages) and 5.

### 5.2 The buffers: copied while small, lent when large

- **Copied into the slot** (small ones the kernel itself declares, 5.4): after the message, while the slot has room
  (about 900 bytes): a path (two for `FS_RENAME`, `FS_LINK`, `FS_SYMLINK`, also when `UMSG_LINKS` lets the server
  write `FS_PATHMAX` bytes into them), a `ustat`, a `udirent`, a cookie, a `netreq`, a `diskinfo`. The argument that
  pointed at it is **rewritten** to the copy's address in the slot; at the reply a buffer the server may write is
  copied back to the client and the argument restored. The cheapest way for the common small request (`stat`: the
  path in, the `ustat` out), and the kernel stack's bytes are never mapped.
- **Lent as a window** (5.3): every other buffer, and every buffer a client declares or a trusted server asks for (5.5).
  Its address does not change.

The threshold is therefore a page, not a byte count: what lies on whole pages of the client is mapped (never copied:
mapping a page is a descriptor, copying it 1,024 long moves), what lies on part pages is copied, and the kernel's
small declared buffers are copied whole into the slot.

### 5.3 A window

A window is a range `[a, a + n)` of a client's memory lent to a server for one request, read only (`R`) or read and
write (`RW`; the client's bytes are copied in too, so a short answer leaves the rest of the buffer as it was). In the
server's tree:

- every **whole page** of the range maps the client's own page (identity), W set for `R`: zero copies, the server reads
  and writes the client's memory itself, as D1 meant;
- each **part page** at the range's ends (at most two; one when the range lies inside a page) maps a **bounce page**: a
  page-aligned page of fast RAM made by the kernel, holding exactly the range's bytes of that page at their offsets and
  zero elsewhere. A part page that two windows of the server share (the client's path and its `ustat` on one stack
  page; two kernel stacks in one page) has one bounce page with the bytes of both; RW if either window is.
- **A bounce page has one owner, the outermost window** (the first lend from the client's own memory). A window lent on
  (5.6) maps the same bounce page and counts as a holder; its end only invalidates its own descriptors and flushes.
  Only the owner's end copies the `RW` bytes back to the client and clears them in the bounce page (so a reused bounce
  page shows nothing of an earlier client).
- At the end of the request (the reply, the server's end, `-ECONNRESET`): that copy-back, the descriptors made invalid
  and flushed, emptied tables given back.
- **No DMA on a part page**: a device never sees a bounce page (the blitter, the floppy's and the A3000's DMA use the
  physical address, which is the client's page). So the kernel refuses to give a DMA engine an address on a window's
  part page (`DRV_AUDIO`, stage 7's `CON_BLIT`: only whole pages or shared memory), and the kernel's own DMA drivers
  never DMA into a user buffer (they copy, as today).
- **Limits** (charged to the server): at most 8 windows a request, 32 bounce pages a server (128 KB), and a pool of 4
  zeroed pages kept; past a limit the lend fails with -ENOMEM (the request is answered as the server chooses). nfscd
  can hold 40 requests: its reads are into the kernel's declared buffers, whose whole pages cost no bounce page.
- The window stays mapped for as long as the request is held, across every switch: D41 section 9's second later step,
  "keep the client flipped in", now exact.

### 5.4 Buffers the kernel declares: its own requests

The kernel builds three kinds of request itself and knows each buffer: `fs_call_k` (vfs.c: every `FS_*`), `net_call`
(sock.c: every `NET_*`, the program's buffers inside `struct netreq`) and `drv_block` (drv.c: `NETDRV_ATTACH`,
`NETDRV_XFER`). A static table per message type (in the span, 11) says them; `ipc_recv` copies or lends them before
the server runs. From `kernel/fsproto.h`'s own comments: `FS_OPEN`/`STAT`/`LSTAT`/`CHDIR`/`MKDIR`/`UNLINK`/`RMDIR`/
`CHMOD`/`CHOWN`/`UTIMES`/`READLINK` the path (`R`, up to its 0, at most `FS_PATHMAX`; with `UMSG_LINKS` the arguments
`fsl_ok` allows, `a0`, and `a1` for `FS_RENAME`/`FS_LINK`/`FS_SYMLINK`, are `RW` for the whole `FS_PATHMAX`, as
D26's rewriting needs), `FS_RENAME`/`LINK`/`SYMLINK` two paths, `FS_OPEN`/`STAT`/`LSTAT`/`FSTAT` the `ustat` (`RW`),
`FS_READ`/`READLINK` the buffer (`RW`, `a[3]` bytes), `FS_WRITE` the buffer (`R`), `FS_READDIR` the `udirent` and the
cookie (`RW`), `FS_READDIRS` the buffer (its size from `fsrdx`) and the `fsrdx`, `FS_STATFS` the `ustatfs`, `FS_ROOT`
the `fsinfo`, `FS_BMAP` its runs (8 x `a3` bytes). For `NET_*`: the `netreq` (copied), `r.buf` for `netreq.len`
bytes (`R` for a send, `RW` for a receive), `r.addr` for `alen` (`R` for bind/connect/send, `RW` for
accept/recvfrom/getsockname), `r.alen` and `r.vlen` (4 bytes, `RW`), `r.val` for `vlen`. For `NETDRV_*`: the block.
The servers need no change for these: fsd, zfsd, fatd and the rest read and write exactly where they did. A server that
reaches beyond a declared buffer faults with D41's line naming the client's memory; every server runs with full
protection in the tests (section 13).

### 5.5 Buffers of program requests: declared by the client, or asked by a trusted server

Requests a program sends itself (`SYS_call`: the display, disk, audio, USB, ping protocols, anyone's own) carried no
declaration until now. Two ways, both kept:

- **The client declares** (new, a growth): `SYS_callv` (102: port, message, `struct ucallbuf` list, count): up to four
  `{address, length, direction}` after the message, checked against the client's own memory (not its inbox), lent at
  `recv` as the kernel's own. `libz`'s protocol helpers (libdisplay, sndlib, the disk and USB calls in the tools) use
  it; it costs nothing more than `SYS_call` without protection (the list is not read).
- **The server asks**, with the call the display server already makes before every access: `SYS_peer(msg, PEER_RD |
  PEER_WR, {addr, len})`. With full protection on it lends the range until the reply (the answer as before: 0 or
  -EFAULT), but only for a server **trusted to ask**: a service the service manager started (displayd, diskd, sndd,
  usbd, netd: the system's own programs), or a server that could debug the client anyway (exactly `SYS_debug`'s rule,
  dbg.c: root, or the same real and effective user as a client that is not set-user-id and not `NODEBUG`). For any
  other server `PEER_RD`/`PEER_WR` answer 0 only for a range the client declared (and lend nothing new). So the old
  clients of the system's servers keep working, and a server any user runs sees only what its callers declared.
- Never lent: the client's inbox, the client's message (the server has its copy). A range must lie in the client's
  own memory or a window the client holds (5.6).
- The in-tree servers that today touch a client pointer without asking (`diskd`, `sndd`'s and `usbd`'s other requests,
  `msgpingd`, `svctest`, `socktestd`) ask (a `libz` helper, `peer_rd`/`peer_wr`, which makes no system call when
  protection is not full: section 11), and their clients declare.
- `PEER_EXACT` (new op 5, on a port, by its owner): "this port's server asks for what it touches" (5.8). `libz`'s port
  creation calls it when protection is full.
- **The display server's rings**: it reads each client's `tail` word between requests (`displayd.c`'s `flush`). `SYS_post`
  grows `POST_READ` (3): the poster a window was opened to (`POST_BUF`) may also read up to 16 bytes of it; displayd
  reads the tail that way. So the display server changes too; the old displayd keeps working through 5.8.
- A later growth, if the measurements ask for it (stage 6): `PEER_DECLARE` (6), a table a server registers once for its
  port so the kernel lends at `recv` for clients that use plain `SYS_call`.

### 5.6 Forwarding: a server passing a client's buffer on

`fsd` reads a file into its client's buffer by asking `diskd` to put the sectors straight there (D42: ZenixFS reads
runs of blocks into the caller's buffer). With windows:

- **A window can be lent on**, and only by the server that holds it, to the server it calls: when `diskd` asks
  `PEER_WR` for a range of its client `fsd` (or `fsd` declares it), the kernel accepts it if the range lies in `fsd`'s
  own memory or in a window `fsd` holds (`RW` for `PEER_WR`; an `R` window is lent on only as `R`), and maps into
  `diskd`'s tree the same pages `fsd`'s tree has there: the client's own pages, and `fsd`'s bounce pages themselves (a
  holder, 5.3; `fsd` waits in its call, so it cannot touch them meanwhile).
- `fsd` cannot answer its client while it waits for `diskd` (calls are synchronous), so the inner window always ends
  before the outer one. The chain is at most as deep as the calls (8, as the priority boost's chain).
- A window is never offered by `SYS_shm` (only a process's own regions and claim memory are), never posted from (a
  post's source is checked as the poster's own memory), never kept past the request.

### 5.7 System calls on a window

A server may hand the kernel a pointer into a window (`diskd`'s `blk_read` into a lent buffer, `nfscd`'s socket
receive into one). So the kernel's pointer checks split in two:

- **`user_ok_*`** keep today's meaning, the caller's own memory (and no read-only shared region for a write), where
  the kernel keeps the pointer or checks another process: `POST_BUF` and a post's source, `CON_GRAB`'s Copper list,
  sprite and Topaz, `DRV_AUDIO`'s channels, `DRV_CACHE`, `SHM_OFFER`, `ipc_peer`'s check of the client, `SYS_post`'s
  check of the owner, `SYS_callv`'s list.
- **`user_ok_call_*`** (new) accept the caller's own memory and its windows by direction, for a pointer used only
  during the call (`read`, `write`, `blk_*`, `send`, `recv`, `stat`'s buffer...).

The kernel works on the identity address, which is right for whole pages (they are the client's) and wrong for a
bounce page (the server's bytes are in the copy). So when a checked range touches a bounce page, the kernel first
copies that window's bytes of the page from the bounce page to the client's page, does the call, and copies them back
before it returns to user mode (syscall.c's exit path, a flag on the process). Nothing else can change those bytes
meanwhile: the client waits in its call, and only the servers of this one chain hold the window. Cost only where it
happens, at most two part pages a buffer. This rests on D41's rule that every user pointer the kernel follows passes a
check first; stage 0 adds a build check for it (section 13).

### 5.8 Old servers

A server built before D60 does not know it must ask. A server whose port is not marked `PEER_EXACT` (5.5: every
server built with the new `libz` marks its ports by itself) is an **old server**. It is given at `recv`, besides the
declared buffers, **D41's old lend of the client's whole view**, but only when `SYS_debug`'s rule would let it debug
the client (root, or the same user with a client that is not set-user-id and not `NODEBUG`); for any other client the
request is answered -EACCES at once and the boot log says so once. This is what an update's first boot needs (D15: the
new kernel starts with the old `diskd`, `sndd`, `usbd`, `displayd`). An old `sndd` claiming Paula and an old
`displayd` grabbing the console get D41's custom chips' page; an old network driver its window and, from netd, the old
frame lending (the frames' pages lent whole, 6.2). `MMU=STRICT` (section 12) refuses all of that. A rebuilt program
is a new one: rebuilding an old server's source with the new `libz` marks it, and it must then ask (the tests find
any that does not). The ZEXE header has no free field (its `+28` is `stage0`, `+32` already code), so no header bit
can carry this; the mark is made at run time.

## 6. Drivers and the chips

### 6.1 What the MMU can split, on each CPU

| | 68030 | 68040 | 68060 |
|---|---|---|---|
| Page sizes | 256 bytes to 32 KB, one for the whole tree (MC68030UM 9.7.2, `TC`'s PS) | 4 or 8 KB (MC68040UM 3.1.2) | 4 or 8 KB (MC68060UM 4.1.2) |
| ATC | 22 entries, fully associative, tagged by function code (MC68030UM 9.4) | 64 data + 64 instruction, 4-way | 64 + 64, 4-way (MC68060UM 1.4.2.5) |
| Global entries | none | G bit, `PFLUSHAN` | G bit, `PFLUSHAN` |
| Caches | logical, 256 + 256 bytes (the data cache off: 4) | physical | physical |
| Translation registers | 2, function-code matched | 2 data + 2 instruction, `S` field | the same |

All the custom chips' registers lie in 512 bytes at $DFF000 (AHRM appendix B): the blitter at $040-$074, the Copper
at $080-$08A, `DMACON` $096, `INTENA` $09A, `INTREQ` $09C, `ADKCON` $09E, Paula's channels at $0A0-$0DF, the
bitplanes, sprites and colours from $0E0. So on a 040/060 they are one page, and even the 030's smallest page (256
bytes, at 16 times the tables) puts the blitter, the Copper, Paula and the shared registers in one; and `DMACON`,
`INTENA`, `INTREQ` and `ADKCON` mix every device's bits in one register anyway. **The chips cannot be split between
drivers by the MMU on any of the three CPUs**; only the kernel can split them, by doing the writes itself. The two
CIAs are one 4 KB page each (CIA-A at $BFE001, CIA-B at $BFD000, a register every 256 bytes); their registers mix
devices too: CIA-B's port A is the parallel and the serial port's lines, its port B the floppy's select, motor, side
and step lines, and reading CIA-A's `ICR` clears the keyboard's, the timers' and `FLAG`'s bits at once. The Gayle's
registers (the IDE's and the PC Card's status) are the kernel's.

### 6.2 Each device

| Device | Holder | What its tree gets (D60) |
|---|---|---|
| A Zorro network board (`a2065`: the LANCE and its buffer RAM on the board) | the driver | the board's window from the hardware table (whole 64 KB units), `CM` 10; its DMA stays on the board: **fully confined** |
| The PC Card slot (`ne2000`, the FA411) | the driver | the parts of the slot it uses (the attribute and I/O spaces at 0xA00000 and 0xA20000, not the 4 MB of common memory), `CM` 10; programmed I/O: **fully confined** |
| The parallel port (`plipbox`) | the driver | **nothing mapped**: a new op `DRV_PARXFER` (10) runs one frame's handshake (send or receive, up to 1,536 bytes, the deadline the driver gives) in the kernel on the claimed lines only, into or out of the driver's own buffer (whole pages or its own memory, not a part page of a window); the CIAs stay the kernel's: **fully confined** |
| Paula (`sndd`) | the sound server | **nothing mapped**: a new op `DRV_AUDIO` (9): a batch of up to 16 writes to `AUDxLC` (as one long), `AUDxLEN`, `PER`, `VOL`, `DAT`, the audio bits of `DMACON` (never `DMAEN` or another device's bit), `INTENA` and `INTREQ` (the audio bits, never the master bit), `ADKCON` bits 0-7; then reads of `INTREQR`'s and `DMACONR`'s audio bits and `VPOSR`; and a wait (until given `INTREQR` bits are set, at most a given number of E-clock ticks), so sndd's short wait loop is one call. The kernel keeps each channel's `LC` and `LEN` and checks the pair, after the batch, against the caller's own chip RAM (`LEN` 0 counted as 65,536 words); a region freed while a channel plays it stops that channel first. sndd calls it a few times a buffer half, or once a tick for modules: **fully confined**, Paula's DMA reads only sndd's memory |
| The custom chips for the desktop (`displayd`, `CON_GRAB`) | the display server | the custom chips' page, `CM` 10, and its own chip RAM: **trusted for chip RAM** until stage 7 (6.3) |
| xHCI, GENET (the Pi) | `usbd`, `genet` | unchanged (Emu68 has no MMU); on a future MMU machine a bus-master device's driver is trusted for memory (6.3) |
| IDE, floppy, SD card, the A3000's SCSI | the kernel | unchanged: kernel drivers; `diskd` reaches them through `blk_*` and `SYS_atapi` (its `priv` flag) on its view (5.7) |
| The boot RAM disk | `diskd` | the RAM disk's pages, read and write, from `SYS_blk_ram` on (3.1 item 6), as a claim |

**The frames between netd and its drivers** (section 2): netd offers its drivers one region, its frame pool, by
`SYS_shm` (read and write), and copies transmit frames into it (as it already does for a DMA driver); a DMA driver
offers netd its claim's DMA memory the same way (`SYS_shm` accepts claim memory from its holder). The pointers in the
blocks then lie in memory both have accepted.

`DRV_CLAIM` makes the window valid in the holder's tree and, for a board above 16 MB, in the kernel's `SRP` tree (for
the quiesce program the kernel runs at the driver's end); `DRV_DMAMEM` gives page-aligned whole pages and makes them
valid; both go at the claim's end. A board above 16 MB outside the fast RAM's block, which D41 refuses (its section 3),
can then be mapped for its driver and reached by the kernel through `SRP`; with it D37's reason for keeping a 68040's
data cache off with such a board goes too (stage 4 detail, tested with FS-UAE's Zorro III boards).

### 6.3 What must stay trusted: DMA

The MMU checks the CPU's accesses only. Every Amiga DMA engine reads or writes memory by physical address, and no
Amiga has an IOMMU:

- **The blitter** writes any chip RAM its pointers name.
- **The Copper** cannot write memory, but without `CDANG` it writes every register from $080 up (AHRM, "Copper control
  register"): `DMACON` and `INTENA` (it can stop every interrupt), Paula's and the bitplanes' and sprites' pointers (it
  can make DMA read any chip RAM onto the screen or the speaker); with `CDANG` (set only by the CPU, `COPCON` $02E) it
  also programs the blitter.
- **Paula's audio DMA** reads any chip RAM (`DRV_AUDIO` checks it); **disk DMA** writes chip RAM (the kernel's floppy
  driver only).
- **A bus-master card** (none in use on an MMU machine today) reaches all memory.

So the display server, while it holds the chips' page, can read and write all chip RAM and stop the machine, but not
touch fast RAM, where on an MMU machine every program, server, the kernel's image and its heap live. To keep that
true, D60 adds: the kernel copies the bootinfo (0x1000) and the hardware table (0x1100) into its own fast-RAM memory at
boot and never reads low chip RAM after that; on a machine with fast RAM the kernel's own blocks come from chip RAM
only when they must be chip RAM (the console's planes and Copper list, the floppy's track buffer, which it treats as
untrusted input already). `CON_GRAB` is allowed only for the process the service manager started for the service
`display` (today: any root process), so being root is no longer enough to get the chips; `gcon-test`'s own grabber
runs as that service in its test image.

Stage 7 (decided after measuring) closes this as Linux's DRM closed it for X: the blits go through a kernel call
(`CON_BLIT`, a batch: the kernel checks every channel's first and last address, ascending or descending, against the
caller's chip RAM, then writes the registers), the Copper list is checked when it is shown (only `MOVE`s to an
allowed set of registers: the bitplane and sprite pointers inside the caller's chip RAM for the screen's size, the
display's own registers; `CDANG` never set) and write-protected in the server's tree while the Copper runs it. Then
the display server is fully confined too, and only ZINE and bus masters stay.

## 7. The special cases

### 7.1 Kernel messages on the caller's kernel stack

Recorded in the kernel and copied into the inbox (5.1); their small buffers copied into the slot, their large ones
lent (5.2-5.4). No server ever sees a kernel stack. Kernel stacks stay 4 KB blocks wherever they lie (a part page
shared by two kernel stacks is the bounce page's business, 5.3).

### 7.2 zup and the update bundle

`zup` (root; stage 1 of an update may be an older `zup` from the root, so no new call may replace this, D41 section 6)
reads the bundle in place. D60 shows it only to a root process that asked `SYS_update` `UPD_INFO` (every `zup` does,
to learn the address), read only, until `UPD_RELEASE` or that process's end; its two part pages through bounce pages,
so no kernel byte around it shows (D41 showed those to root).

### 7.3 ZINE

ZINE stays a deliberate take-over (D29): root at the Amiga's own console (`SYS_zine` already demands both), and the
kernel log says "memory protection paused for ZINE" and, after, "back on". During the session the AmigaOS program can
write anything, as it can today. After it the kernel checks, before translation goes back on, its own code and
read-only data against a checksum taken at boot, and the records it rebuilds from (the process slots, the memory
manager's block headers and magic words, the claims, the held windows) for consistency; a mismatch ends the session as
`KLOG_END_ZINE_DAMAGED` (as D29's other damage) and the machine resets. Then it rebuilds every process's tree from
those records instead of reusing the tables in RAM. This finds damage; it cannot promise there is none (ZINE could
have written a record into another consistent state): ZINE is the one way to run code unprotected, said so in its
manual page. A later option (stage 7): a **guarded ZINE**, the session run with translation on and a tree that maps all
memory but Zenix's own blocks, which stops a program's stray writes into Zenix; it is no containment (supervisor code
can rewrite the MMU's registers, and demos take the CPU over), only a safety net, and demos that use the MMU or depend
on its absence would need the plain mode. D57's guest mode (user mode) is the real containment: its programs are
ordinary processes.

### 7.4 Shared memory: `SYS_shm`

Memory shared by passing an address stops working (D41 section 2); D60 gives the honest way. Needed by netd and its
drivers (6.2), `priotest`, D57's guest windows (a canvas the display server shows), and the C library's ports that use
shared memory (POSIX `shm_open`/`mmap(MAP_SHARED)` on anonymous memory). The call, `SYS_shm` (101):

- `SHM_OFFER` (1: region, pid, flags `SHMF_RO`): the owner of a `SYS_memalloc` region (or a claim's holder, its DMA
  memory) offers it to a living process, read only or read and write. Nothing changes for the receiver yet.
- `SHM_TAKE` (2: region): the receiver accepts an offer made to it; the region becomes part of its view at the same
  address (identity: pointers into it are the same in both). Shared regions sit in a table of their own (`NSHM` 8 a
  process), not in `NREG`, so offers cannot use up a process's regions.
- `SHM_DROP` (3: region): a holder lets go (an offer not taken is withdrawn the same way by its owner). The owner's
  `SYS_memfree` of a region others hold is a drop: the memory stays until the last holder drops it or ends, then is
  freed (it is counted to the owner while it lives, then to the last holder). A process's end drops all it holds.
- `SHM_INFO` (4: region, `struct ushm *`): its size, owner, holders, flags.
- Without full protection: the same calls keep the count only (the memory is visible anyway), so a program written for
  it works everywhere.

`priotest` changes to offer its block's region to its children; `prio-test` and `pure-test` run with protection on
(`pure-test` expecting D41's protection line where it now expects the checksum's "damaged").

### 7.5 The debugger, signals, the service manager

- `zdb` reads and writes another process through `SYS_debug` (root or the same user, as `ptrace`): a kernel copy,
  unchanged. D54's rule "a system process has no debugger" goes (there are none): servers can be debugged (their
  `priv` and service rules stay as dbg.c has them).
- A signal handler runs in the process's own view; a server's windows stay valid during it.
- The service manager's loads (`svc_role`) run in the kernel; `svc_born` marks nothing.

## 8. Root

Root keeps what Unix root has, through checked doors, and no raw memory or hardware for being root alone:

| Power | Door, and its check |
|---|---|
| The raw disk (`fdisk`, `mkfs`, `fsck`, `dd`) | `diskd`'s `DISK_RAW` (root's messages only), refused while an overlapping device is mounted (architecture.md section 9); `blk_*`, `fd_rw`, `SYS_atapi` only for the `priv` process (diskd) |
| Another process's memory (`zdb`) | `SYS_debug` (root or the same user) |
| The chips for a desktop | `CON_GRAB`, only the `display` service's process (6.3) |
| The screen and the keyboard (`wasabid`) | `CON_READ`, `CON_LINE`, `CON_INPUT` (root): kernel copies of the console's cells and lines, input records |
| A device | `SYS_drv`, only the network server's children, the `audio` and `usb` services |
| Starting services, mounting | the service manager and `SYS_mount`, as today; the servers they start are confined |
| The kernel log | `SYS_klog` and `dmesg`: kernel copies |
| The update bundle | `UPD_INFO`, read only (7.2) |
| The whole machine | ZINE (7.3), `SYS_reboot` |

Gone: D41's "every uid 0 process sees the bundle", "a reserved port makes you a system process", "a mount makes the
port's owner a system process", "a `CON_GRAB` by any root process".

## 9. Speed

### 9.1 Where D41's cost is

D41 section 19.4 (emulator figures, only ratios mean anything): a call to a system server +1-4%; a call to a protected
server x2.3-4.6; a round trip between two protected programs x3.1-7.2. Its causes: every switch between two protected
programs flips every page of both views (linear in their size) and flushes the whole ATC; a lend flips the client's
whole view; the refills after every flush.

### 9.2 What D60 does about it

1. **A root per process**: a switch is one register and one flush (section 4), whatever the sizes.
2. **Windows kept while a request is held** (5.3): no lend or unlend at a switch; a window is built once and taken
   down once.
3. **Exact windows are small**: a `stat` lends nothing (the path and the `ustat` are copied into the slot, about 300
   bytes); a 4 KB read lends one or two pages; D41 lent the client's whole view (about 40 pages for a shell).
4. **Global shared code** on 040/060 (section 4): the ATC entries of `libz` and the shell survive `PFLUSHAN`. Better
   still (stage 6, measured): the readable-by-all segments placed together in one 256 KB-aligned area, its one page
   table shared by every tree (less memory, faster tree building, and those pages never miss after the first use).
5. **Single-page flushes** (`PFLUSH`) for windows instead of `PFLUSHA`, so a server keeps its own ATC entries.
6. **Page-aligned big buffers**: `libz`'s allocator gives a request of 4 KB or more whole pages, and stdio's buffers
   are page-aligned, so the common read and write lend whole pages and copy nothing.
7. **8 KB pages**, measured in stage 1 against 4 KB: half the ATC misses and the tables, twice the rounding (D41
   section 6).
8. Measured, and only if the budget is missed: `PEER_DECLARE` (5.5); a direct hand-off from a client to its server in
   `call()`.

### 9.3 Estimates, to be measured

A round trip between two programs costs two switches. Each: a `MOVEC` and a `PFLUSHAN` (on the order of 20 clocks on
a 68060), then the refills. The 040/060 have separate instruction and data ATCs, so code and data pages miss apart:
each side of a round trip touches some 10-15 data pages and 5-10 code pages, some 40-50 table searches a round trip,
each three descriptor reads from memory (the 060 searches outside its cache) at some 60-120 clocks; G pages win back
much of the code side. So **some 2,000-5,000 clocks a round trip on a 50 MHz 68060, 40-100 us** (review A's estimate;
revision 1 said 20 us, too optimistic), against D41's flips of 40-80 us a switch plus the same refills. On a 25 MHz
68030 (22 ATC entries for code and data together, a working set of about 25 pages thrashing even inside one request,
some 35-45 clocks a search, MC68030UM 11.7) **over 100 us a round trip**. A window costs a few descriptor writes,
their `cpushl`s and `PFLUSH`es, and the part pages' copies (at most two pages, usually much less).

### 9.4 The budget

Measured with protection off (`MMU=OFF`) and full on the same machine, the same build:

| | Budget | D41 today (emulators) |
|---|---|---|
| A filesystem request (`stat`; a cached 4 KB `read`; `ls -laR /`) | at most +10% | +1-4% (the server unprotected) |
| A disk read through `fsd` and `diskd` (`dd`, `cat` of a big file) | at most +10% | +1-4% |
| A call to a server and back (`msgping`) and a round trip between two programs (`bench`) | target x1.5, at most x2 | x2.3-7.2 |
| A pipe's throughput (`cat big | wc`) | at most +15% | not measured |
| The desktop: a full redraw, a window drag (`dpytest`'s counters) | at most +10% | not measured (displayd unprotected) |
| Sound: `play` and `modplay` keep their timing; `sndd`'s CPU time | no underrun; at most +10% | not measured |
| A system call that reaches no other process | +0 | +0 |

A filesystem request takes about a millisecond (D41 section 19.4: 906-5,001 us in the emulators), so 40-100 us of
refills fit its +10%; a bare round trip (66-243 us there) is where the budget is tight, hence a target and a limit.
The budget is judged on the Apollo 1260 (the real 68060), **first at the end of stage 1** (the switch's real cost, 4 KB
against 8 KB pages), before stage 2 starts; the emulators give the ratios (FS-UAE and Amiberry, the 030, 040 and 060
models, as D41 section 19), and neither models the ATC's real cost, so a pass there is not a pass. No 68030 is among
the test machines: its figures stay estimates and the docs say so. If stage 1 misses the limits on the 1260, stage 2
waits for a new plan and the partner hears it.

## 10. Size

- **The kernel**: about +7 to +10 KB of code (estimated: the per-process trees and the table pool 2.5 KB, which
  replace D41's flips and system mode, about -1.5 KB; the records, the inbox, windows, bounce pages and the sync 3.5
  KB; the static declaration tables for `FS_*`, `NET_*`, `NETDRV_*` 0.8 KB; `SYS_callv`, `SYS_peer`'s lend,
  `POST_READ`, `SYS_shm` 1.5 KB; `DRV_AUDIO` and `DRV_PARXFER` 1.2 KB; ZINE's checks and rebuild 0.6 KB). The
  protection-only part (trees, windows, records, tables, ZINE's rebuild) lies in D45's span `.zhwmmu`, given back after
  the boot when protection is off, behind the `mmu_ops` table. **Resident on every machine** (review A): `SYS_callv`
  (it is `SYS_call` without protection), `SYS_shm`'s counting, `DRV_AUDIO` and `DRV_PARXFER` (sndd and the plipbox use
  them on every machine, 11), `POST_READ`, and the gates in ipc.c, syscall.c and proc.c's checks: about 2.5-3 KB. The
  mount tests' floppy is the binding limit (tools/ksize-budget: the TOTAL 206,200, the room 800 bytes kept, about 870
  bytes to spare): its RAM disk goes from 676 KB to about 666 KB, as D41 and D43 did, measured at every stage. If that
  RAM disk can give no more, the question for the main session is to pack that floppy's kernel as the demo floppy's
  is.
- **RAM with protection on only**: each process's tree, 1.5 to 3 KB for an ordinary program (a root, one or two pointer
  tables, a page table per 256 KB it touches), up to about 7 KB for one with 16 regions spread over the pools; a
  driver's device windows (the PC Card's two spaces 2 page tables; a 16 MB Zorro III window would be 16 KB, so a
  driver maps only the part of a big board it uses); each server's inbox, 4 KB (8-10 servers: 32-40 KB); the records,
  2.4 KB; the `SRP` tree, about 1.5 KB; bounce pages only while requests are held (4 kept, 16 KB). For 15-30 processes
  with 8-10 servers: **about 70-130 KB** (review A's recount; revision 1 said 40-90), instead of D41's one tree (8-14 KB
  on a 68030 with 4-8 MB, about 77 KB on the 1260's 72 MB). So the 1260 pays some 0.1% of its RAM, a 68030 with 4 MB
  about 2-3%; measured, and the shared segment page table (9.2 item 4) and 8 KB pages would cut it.
- **Programs**: `libz` +300 bytes (the helpers, `callv`, the port mark); `sndd` smaller (the register code becomes
  batches); `diskd`, `usbd`, `msgpingd`, netd and the drivers, and the tests' servers +50-200 bytes each.

## 11. Machines without an MMU

Exactly as today, but for the resident bytes of section 10. Every protection path is behind D41's gate (`mmu_on`, the
table `mmu_ops`), in the span given back after the boot: `ipc_recv` returns the client's own message, nothing is
recorded, copied or lent, the checks of 5.7 are today's, `SYS_callv` is `SYS_call`, `SYS_shm` only counts. `sndd`
keeps its direct register writes when protection is not full (it holds the claim, as today), so the stock A1200 pays
no system call per buffer; with full protection it uses `DRV_AUDIO`. The plipbox driver uses `DRV_PARXFER` on every
machine (one code path; the kernel's loop is the driver's loop moved, measured on a 68020 against today's). The `libz`
helpers read once whether protection is full (`SYS_meminfo`'s new fifth word, section 12, zeroed first so an older
kernel's 16 bytes read as "none") and make no system call when it is not, so `diskd` costs nothing more on the stock
A1200. Measured with `bench` and `ls -laR /` on the 68020 matrix machines, the stock A1200 first.

## 12. Interfaces (every addition; frozen unless said)

- **`SYS_recv` and `SYS_replyrecv`**: the message they return may be a copy in the server's memory (always with full
  protection), not the client's; every call that names a request (`replyrecv`, `SYS_peer`, `NETSRV_REPLY`) takes that
  address; a second reply to it is -EINVAL. The reply carries back `a[0]`..`a[3]` (an argument the kernel rewrote gets
  its old value back) and the result, nothing else. With full protection the kernel never reads the client's message
  after `call()`. A growth of D1's meaning: no server may rely on the message being the client's memory (none does).
- **`SYS_callv` 102** (port, message, `struct ucallbuf *`, count 0-4; `struct ucallbuf` {addr, len, flags: `CALLB_RD`
  1, `CALLB_WR` 2}, 12 bytes): `SYS_call` with declared buffers.
- **`SYS_peer`**: `PEER_RD` 2 and `PEER_WR` 3 unchanged in their answers (0, -EFAULT), also 0 for a range of a window
  the client holds (5.6); with full protection on they also lend the range until the reply, for a server trusted to
  ask, and for others answer 0 only within the client's declared buffers (5.5); `PEER_EXACT` 5 (arg a port the caller
  owns: its server asks for what it touches; 5.8).
- **`SYS_post`**: `POST_READ` 3 (`struct upost` as `POST_WRITE`, the bytes copied out of the window, at most 16).
- **`SYS_shm` 101** (`SHM_OFFER` 1, `SHM_TAKE` 2, `SHM_DROP` 3, `SHM_INFO` 4; `SHMF_RO` 1 in the flags argument;
  `struct ushm`, first size 16: size, owner, holders, flags; `NSHM` 8 a process).
- **`SYS_drv` ops 9 `DRV_AUDIO`** (the `DRV_PAULA` claim's holder only; `struct udrvaudio`: a count, up to 16
  `{register, value}` pairs, a wait mask and its limit; first size 76; the answer `INTREQR`'s and `DMACONR`'s audio
  bits and `VPOSR`; -EINVAL for a register or bit outside the audio set, -EFAULT for a channel outside the caller's
  chip RAM) and **10 `DRV_PARXFER`** (the `DRV_PARPORT` claim's holder; `struct udrvparx`: direction, buffer, length up
  to 1,536, deadline in ticks; the bytes moved or -ETIMEDOUT).
- **`SYS_meminfo`'s fifth word** (`UMEMINFO_FIRST` stays 16; the size 20 gives it): 0 no protection, 1 D41's, 2 full.
- **`kopts` bit 32 `KOPT_MMU_STRICT`** and zenixboot's `MMU=STRICT` (old servers refused, 5.8); **bit 64
  `KOPT_MMU_D41`** and `MMU=D41` (D41's model as it is today, system processes and all: the way back on the first
  hardware runs, removed after 1.0 if never needed).
- **`CON_GRAB`**: with full protection on, only for the `display` service's process (-EPERM for any other process,
  root too). A change of meaning of a frozen call, accepted once if the partner agrees (section 16), like the earlier
  ones in frozen-interfaces.md.
- **Other changes of meaning, written there**: `user_ok_*` split (5.7: internal, but what a server may pass a system
  call grows); D54's "a system process has no debugger" gone; the reply's copy-back rule.
- **The boot line**: `mmu: memory protection on: 68060, full, 4 KB pages, N KB of tables` (the start the tests read
  is unchanged).
- Later, only if built: `PEER_DECLARE` 6 (5.5), `CON_BLIT` and `CON_COPPER` (stage 7).
- **Internal, not frozen**: the records, the inbox's slot layout, the table pool, the bounce pages and their limits,
  the declaration tables.

`docs/claims.md` gets `SYS_shm` 101 and `SYS_callv` 102 (next free: 103), `DRV_AUDIO` 9, `DRV_PARXFER` 10, `PEER_EXACT`
5, `POST_READ` 3, the `SHM_*` ops and `SHMF_RO`, `CALLB_*`, the `kopts` bits 32 and 64, and the `SYS_meminfo` word, in
the commit that first uses each.

## 13. Tests

- `tools/mmu-test.sh` grows: a server (the test's `msgpingd`) that reads one byte past its client's declared buffer
  is killed naming the client's memory, and so is one that touches its client's buffer after its reply; a user's server
  asking `PEER_RD` for its client's stack gets -EFAULT; a server that reaches the kernel's stack, another process or the
  chips is killed; a client rewriting its own message in flight (through a shared region) changes nothing the kernel
  does; `fsd` serving a buffer that straddles pages (the bytes come back, the bytes around the buffer are untouched: a
  pattern checked); a forwarded window into an unaligned buffer (`cat` of a file read by `fsd` through `diskd`: the
  ends' bytes are the file's, not zeros); `sndd` pointing a channel outside its chip RAM gets -EFAULT, and freeing a
  playing region stops it; `SYS_shm` offers, takes, drops and the owner's early free; ZINE's checks
  (`zine-emu-test` with a stray write after it).
- Every suite with full protection on (`tools/mmu-suite.sh` on the 030, 040 and 060 models in both emulators), now
  including `prio-test`, `pure-test`, `sound-test`, `dpy-test`, `zdesk-test`, `gcon-test`, `svc-test`, the RAM-disk
  floppies' boots and the network tests (`a2065`, `ne2000` and `plipbox` with their models): a server that reaches
  beyond what it was given shows at once (the D41 runs found `sed`'s bug that way).
- **The update path**: an update test whose first boot runs the new kernel with the previous release's servers (5.8),
  both with the default and with `MMU=STRICT`.
- `user_ok` coverage: a check (tools/, part of the build) that every system call path that follows a user pointer
  calls one of the checks first, the kind (own or call) named: the assumption section 5.7 rests on.
- The speed table of section 9.4, every stage, in both emulators; on the 1260 at the end of stages 1 and 6.
- The matrix unchanged on every machine without an MMU; `bootmem-test` for the span; `sound-test` and the plipbox's
  tests on the 68020 (11).

## 14. The build, in stages

Effort in my own helper hours, each stage merged and tested before the next (one helper at a time); a code review
after stages 1, 2, 3 and 4 (kernel core), the two design reviews done (section 17).

| Stage | What | Hours |
|---|---|---|
| 0 | Baseline: the benches of 9.4 written as tests, run with D41 on and off in both emulators; the kernel's pointer-check check (13); the boot tables copied to fast RAM (6.3) | 5-7 |
| 1 | A tree per process, the table pool, the switch by root, G pages and `PFLUSHAN`, 4 KB and 8 KB pages; D41's model otherwise unchanged (system processes still). Useful alone: D41's x3-7 gone. Then **the 1260** (the partner's time): the switch's real cost | 12-16 |
| 2 | The kernel's records; the inbox and the message copy; windows (whole pages, bounce pages, owners, directions, limits); the declaration tables for every `FS_*`, `NET_*` and `NETDRV_*`; the split checks and the sync; forwarding | 22-30 |
| 3 | Every server confined: `svc_born`, reserved ports and mounts mark nothing; `SYS_callv`, `SYS_peer`'s lend and its trust rule, `PEER_EXACT`, old servers; `POST_READ` and displayd on it; `diskd`'s RAM disk; `SYS_shm` with netd's frame pool and the drivers on it; `diskd`, `usbd`, `msgpingd`, `svctest`, `socktestd` ask, their clients declare; every suite with full protection | 18-26 |
| 4 | Drivers: claims' windows and DMA pages, `SRP` for quiesce; `DRV_AUDIO` and `sndd` on it; `DRV_PARXFER` and the plipbox on it; `CON_GRAB` for the display service only, its chips' page, `gcon-test` as that service | 12-16 |
| 5 | The special cases: zup's bundle, `priotest` and `pure-test` on `SYS_shm`, ZINE's checks and rebuild, `MMU=STRICT` and `MMU=D41`, the update-path test | 8-12 |
| 6 | The speed pass against 9.4 (and, only if needed, its item 8); the shared segment page table; the documents (architecture.md, limits.md, features.md, DESIGN D1 and D8, frozen-interfaces.md, claims.md); the 1260's full run, prepared as D41 section 18 | 10-14 |
| | **Full protection as section 1 describes it** | **87-121** |
| 7 | Options, each only on the partner's word after stage 6's figures: the checked blitter and Copper (`CON_BLIT`, `CON_COPPER`; displayd moved onto them) 24-34; the guarded ZINE (a safety net, 7.3) 8-12 | 32-46 |

## 15. What changes in the other documents (when built)

DESIGN D1 ("pointers, no copies": the message is copied with full protection, buffers on whole pages are still not),
D8 (no trusted servers on an MMU machine; the display server's chip RAM until stage 7), D41 (sections 2, 7 and 8 point
here), architecture.md sections 5, 6 and 8 and the servers' entries, limits.md ("the no-MMU limit"; the 030's
instruction cache leftover), features.md, frozen-interfaces.md and claims.md (section 12), the man pages of
`zenixboot` (`MMU=STRICT`, `MMU=D41`), `zine` (the one unprotected way), `priotest`, and `libz`'s documentation of
`callv`, `shm` and the peer helpers.

## 16. Questions for the partner

1. **The display server**: is "trusted with chip RAM, like X before DRM" acceptable for 1.0, with the checked blitter
   and Copper (stage 7, about 24-34 hours, and some cost on every blit) after 1.0? Or is it part of "full" for you?
2. **The system's own servers may ask for any range of their client** (5.5: displayd, diskd, sndd, netd...), while a
   server a user runs gets only what its callers declare. Fine, or should every server, the system's too, get only
   declared bytes (more client changes, the old clients of displayd would then need 5.8's rule)?
3. **Old servers** (5.8): give a server built before D60 its old whole-client lend when it could debug the client (so
   an update's first boot works with the old servers), or refuse them always (`MMU=STRICT` as the default)?
4. **`CON_GRAB` only for the display service**: a frozen call's meaning narrowed (root alone no longer gets the chips).
   Agreed?
5. **ZINE**: keep it as a plain take-over with the checks and rebuild after it (7.3), and the guarded mode (a safety
   net only) as a later option?
6. **The way back** `MMU=D41` for the first runs on the 1260, removed after 1.0: wanted?
7. **The budget** of section 9.4 (+10% for a file request; a bare call x1.5 as the target, x2 the limit): right? And a
   run on the 1260 at the end of stage 1, before the big stages, can you give it?

## 17. Reviews

Two independent reviews of revision 1 (2026-11-18), each a fresh read of the note against D41, DESIGN.md,
architecture.md, frozen-interfaces.md, claims.md and the code; their findings and what was done.

**Review A** (the 68k MMU mechanics on the 030, 040 and 060):
1. *Must*: a forwarded bounce page was cleared by the inner window's end, so the client's buffer ends came back as
   zeros: a bounce page now has one owner, the outermost window; an inner window only drops its descriptors (5.3, 5.6);
   a test (13).
2. *Must*: the guarded ZINE cannot keep out supervisor code (it can rewrite the MMU): now called a safety net only;
   D57's guest mode named as the containment (7.3).
3. The 030's caches are logical and a read hit skips the MMU: the data cache must stay off with full protection; the
   instruction cache's 256-byte leftover named (4).
4. The cost estimate was too optimistic (split ATCs, the 030's thrashing): re-estimated (9.3); 8 KB pages measured in
   stage 1; the 1260 run at the end of stage 1 is the plan; the bare-call budget a target and a limit (9.4).
5. `PFLUSH` needs `DFC` 1 on 040/060, never a non-global variant for changes, a `nop` and pushes at write time (4).
6. Register pages' cache modes: `CM` 10, CI on the 030, never G on the 030 (4, 6.2).
7. The plipbox with the CIA pages could disturb the floppy and steal the kernel's interrupts: `DRV_PARXFER` moved into
   the plan (stage 4), the CIAs never mapped (6.1, 6.2).
8. `DRV_AUDIO`'s checks: `LC` as one long, the pair checked after the batch, `LEN` 0 as 65,536 words (to verify), a
   freed region stops its channel (6.2).
9. The span cannot hold what runs without an MMU: the resident part counted (10, 11).
10. The RAM estimate was low (inboxes, `SRP`, device windows): recounted, 70-130 KB; a driver maps only what it uses
    (10).
11. DMA never sees a bounce page: no DMA address on a part page; bounce pages page-aligned fast RAM (5.3).
12. Notes taken: ZINE's rebuild checks its records too and promises less (7.3); the loaded root tracked and a freed one
    replaced (4); a part page in a G page mapped identity (4); kernel pointers zeroed in the slot (5.1); the shared
    segment page table (9.2), 8 KB pages (4, 9.2), the `SRP` tree lifting D37's board rule (6.2). Not taken: `CM` 11
    for user chip RAM on the 060 (unverified, and the rule "the kernel's mode" is simpler).

**Review B** (the interface and the security model):
1. *Must*: the kernel trusted fields of the message in the client's memory (the link, the state, the stamped uid and
   `kflags`), which shared memory or a lend could rewrite: a kernel record per call, the client's message written only
   at the end (5.1).
2. *Must*: `SYS_peer` would let any server lend itself any range of its caller: the trust rule (services, or a server
   that could debug the client) and the client's declaration `SYS_callv` (5.5); the inbox never lent; limits (5.3).
3. *Must*: netd and its drivers exchange frames by address: netd's frame pool and claim memory through `SYS_shm` (6.2).
4. *Must*: diskd reads the RAM disk in place: mapped for diskd (3.1, 6.2).
5. *Must*: displayd reads each client's ring tail between requests: `POST_READ`; "binary for binary" dropped (5.5).
6. *Must*: D26's `UMSG_LINKS` lets the server write paths: `RW` for `FS_PATHMAX`; 1 KB slots fit two (5.2, 5.4).
7. *Must*: `UMSG_NETREPLY` was set in the slot, not the real message: the record (5.1).
8. *Must*: the ZEXE header has no reserved long: the mark is a run-time `PEER_EXACT` on the port (5.8).
9. The pointer checks split into "own" and "for this call" (5.7); read-only shared regions refused for writes.
10. Reused slots: a generation and round robin; a second reply -EINVAL (5.1).
11. The old-server rule is exactly `SYS_debug`'s; rebuilding marks a program; the mark comes with stage 3 (5.8, 14).
12. Limits on windows and bounce pages, charged to the server (5.3).
13. `DRV_AUDIO`: `ADKCON` masked, no master bits, `VPOSR` and the wait in the call; `sndd`'s direct writes kept without
    full protection, so the stock A1200 pays nothing (6.2, 11).
14. `SYS_shm`: offer and take, a table of its own, the owner's early free (7.4).
15. `gcon-test` runs as the display service; the root table completed (6.3, 8).
16. Notes taken: windows over another process's block named (3.1); `SYS_meminfo`'s fifth word zeroed first (11);
    the other changes of meaning listed (12); stages 3's dependencies and the extra 15-25 hours (14: now 87-121).
    Open, for stage 3: whether `SYS_memfree` refuses a D55 screen's planes while they are shown (as `CON_GRAB`'s
    regions are).
