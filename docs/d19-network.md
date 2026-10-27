# D19: the network (design note, revision 2)

Status: **design, revision 2 after two independent reviews (section 13); step 1 (section 3, zfsd out of the kernel
image) built on branch zfout 2026-10-27; steps 0, 2 and 3 (msgping, random numbers, the kernel's socket layer with a
test server) built on branch n023 2026-10-27; steps 4 and 5 (netd over the loopback; SLIP, the interfaces, ping) on branch n45
2026-10-27; steps 6-8 (names, the driver protocol, the A2065) on branch n678 2026-10-27 (section 14, "As built");
steps 9-11 not built.** Roadmap block 2, "Zenix
reaches the world": the network, the CM4's Ethernet, TCP/IP as a server, DHCP, DNS, `ping`; telnet, the crypto
library, ssh, HTTPS, NFS, remote control and `zpkg` come after it. A deep core change (a kernel file kind, new system
calls, a message family, the way servers are loaded). The short version is DESIGN.md D19. Facts about today's code are
*read* from main at 7b5149d (2026-10-26); sizes marked *measured* were compiled here; everything else is an estimate
and says so. Section 3 (servers from the disk) is independent of the network and is built and merged first.

## 1. What the owner gets, in plain words

- Zenix talks TCP/IP: `ping`, then `telnet` to BBSes, later ssh, wget, NFS, a remote control, `zpkg`.
- **Every Amiga can join**, also the stock A1200 with no expansion: through the serial cable to a PC (SLIP), where a
  small program on the PC (no root needed) gives the Amiga the PC's network and the internet. The PiStorm A1200 uses
  the CM4's own Ethernet port.
- **It costs nothing when it is not used.** The network server is a program on the disk that starts the first time a
  program opens a socket, and the kernel image gets *smaller* (section 3: the ZenixFS server leaves it): about 30 KB
  more free memory on every machine that does not use the network.
- **Unix as people know it:** BSD sockets, `/etc/network/interfaces`, `/etc/hosts`, `/etc/resolv.conf`, `ifconfig`,
  `route`, `ping`, `netstat`, `host`.

## 2. Where the network lives

### 2.1 The options

| | A. In the kernel | B. One server, drivers inside it | **C. One server, small driver processes below it (SANA-II-like)** |
|---|---|---|---|
| Resident cost | 60-90 KB always in memory and on every boot floppy | 0 when unused | 0 when unused |
| A bug in the stack | crashes the kernel | crashes the server; D11 starts it again | the same; a driver bug ends only the driver |
| Drivers nobody has | always | every driver in every netd (15-25 KB, *estimate*) | only the machine's own |
| Speed | best | one message per socket call | the same, plus one call per batch of frames |
| ROADMAP principle 1 ("the network ... are servers behind named ports") | no | yes | yes |

**Decision: C.** One TCP/IP server, `netd` (port `net`, a D11 service, `/sbin/netd`), and one small user-mode driver
process per kind of hardware (`/sbin/net/genet`, later `a2065`, `ne2000`, ...), as AmigaOS has one stack and a SANA-II
device per card. **Two parts are not hardware drivers and live inside netd**, as in Linux and BSD where they are line
disciplines of the stack: **SLIP** (later PPP), a framing over the kernel's serial device `/dev/ttyS0` (D12), and
**loopback**. The rule: *code that touches device registers is a driver process; a link protocol over an existing
Zenix device is part of netd.* So a stock A1200 on SLIP runs one process, netd.

**User-mode drivers are a second driver model, on purpose:** the block drivers (IDE, SD, floppy, A3000 SCSI) stay in
the kernel because the root needs them before any program can run. Every driver *not* needed to reach the root (the
network now; USB and the sound server later) is a user-mode process; the kernel gives it only what user mode cannot do
(cache instructions, interrupts, a claim on the hardware, memory that outlives a crash: section 5.1). That rule goes
into DESIGN.md with D19.

**Not taken:** a `/net` filesystem served by netd over the `FS_*` protocol (Plan 9 style): a blocking read would wait
*inside* netd's message call, where ^C cannot reach it (G4, D16 section 4), and `poll` does not see readiness a file
server knows. A kernel file kind with one multiplexed call is the right shape (section 4).

### 2.2 Which TCP/IP code

| | Own stack | **lwIP 2.2** | uIP 1.0 | picoTCP |
|---|---|---|---|---|
| Licence | ours | BSD-3: GPL-2 compatible (*read*, COPYING) | BSD-3 | GPL-2.0 only: compatible, but archived, no security fixes |
| Code (m68k, -Os) | 15-25 KB, *estimate* | **48.8 KB** with Ethernet/ARP, DHCP, DNS (*measured*, below) | 5-10 KB | similar to lwIP |
| TCP quality | months of work, every bug remote | 20+ years in products: windows, retransmit timers, congestion control, keepalive, backlog, reassembly | one segment in flight per connection | full |
| Single-threaded server | yes | yes: `NO_SYS=1`, raw API | yes | yes |
| Maintained | by us | yes (2.2.1) | no | no |

**Decision: lwIP 2.2.x**, `NO_SYS=1`, raw (callback) API only (its `sockets`/`netconn` layers need threads: Zenix's
sockets are a kernel file kind, section 4), IPv4, IPv6 off (+33 KB; a later option). lwIP goes into `net/lwip/` with
its BSD-3 headers and a README naming the version and **the local patches**, each small and kept as a patch file:
pool limits that hold with heap allocation (9.4), random ephemeral ports (9.3). Our port layer (`net/port/`:
`arch/cc.h`, `lwipopts.h`, `sys_now`, the checksum and copy in assembler) beside it. Asserts keep every check but
report only the line number (`LWIP_PLATFORM_ASSERT`: "safety stays", without 7 KB of message strings).

*Measured 2026-10-26* (lwIP STABLE-2_2_1_RELEASE, `m68k-elf-gcc -m68020-60 -Os -ffunction-sections -msoft-float`,
`size` summed over the *unlinked* objects, so an upper bound before `--gc-sections`; re-measured linked at step 4): core
IPv4 + TCP + UDP + ICMP + raw + `slipif` 42,617 bytes with lwIP's assert strings; with `ethernet`, `etharp`, `dhcp`,
`dns` and no `acd` **55,964**, and **48,836 with line-number asserts** (43,976 with no asserts at all, not taken); IPv6
adds about 33 KB. Of the 48.8 KB, DHCP + ARP + Ethernet are 10.4 KB that a SLIP-only machine carries unused:
**accepted**, so one netd serves every machine and an update never needs to know which build a machine runs; a split
is possible later if a measurement on the stock A1200 asks for it. Reassembly stays (`IP_REASSEMBLY`), sending
fragments goes (`IP_FRAG 0`, 0.7 KB); `raw` stays (ping).

**Speed** (*estimates*, measured later on the stock A1200 and the 060, never Emu68): TCP input or output roughly
2,000-4,000 instructions a segment; on a 14 MHz 68EC020 from chip RAM with the checksum in an `addx.l` loop and one copy
each way, about 2 ms per 1,460-byte segment: a ceiling of 500-700 KB/s, far above SLIP (11.5 KB/s) or a PCMCIA NE2000.
A key typed in telnet costs a few hundred microseconds. Hot loops (checksum, copies, SLIP's byte loop) assembler or
`-O2`; the rest `-Os`.

### 2.3 netd and its drivers

Zenix's calls are synchronous; two processes calling each other deadlock. So **netd never calls a driver; a driver is
netd's client** (as fsd is diskd's), and **a driver reaches netd only through the kernel**:

- **`SYS_drv DRV_ATTACH` and `DRV_XFER`**: the kernel checks the driver's block (D13 section 5: pointers and sizes) and
  sends it to `net` as a kernel message (`UMSG_KERNEL`) that **never starts a server** (`SVC_NS_SKIP`): an orphaned
  driver whose netd is gone gets -ECONNRESET, it never starts a second netd that would start a second driver for the
  same card. `ATTACH` says who the driver is (`eth0`, the MAC, the MTU, the receive buffers it wants, DMA rules,
  checksum offload) and gets the interface number and its buffers (2.4). `XFER` carries, *in*, the receive buffers the
  driver filled (number, length, flags), the send buffers it finished, the link state, counters; *out* (netd writes
  into the block), frames to send (number, pieces, checksum request: a frame may be a chain of up to 4 pieces, else
  netd copies it into one buffer) and empty receive buffers. One call per batch, never per frame.
- **The doorbell:** netd starts each driver as its child (`SYS_spawn`) with a pipe as its standard input and writes one
  byte when it has work and has not rung since the driver's last `XFER` (the pipe never holds more than one byte, so
  the write never blocks). The driver waits with `poll` on the pipe and its hardware event (5.4) or a timer. **netd
  ignores SIGPIPE** (a dead driver's pipe must not end netd); a driver that sees POLLHUP on the pipe knows netd is gone,
  stops its hardware and exits. (Signals were the other doorbell: they need a handler and cut every wait with -EINTR.)
- **Supervision:** netd is the drivers' parent: on a driver's end (SIGCHLD, D16; the handler only sets a flag) it
  marks the interface down, logs the line in D11's format (`net: eth0: /sbin/net/genet (pid N) ended, status S`) and
  starts it again with D11's numbers (1, 2, 4 s back-off, given up after 5 ends in 60 s). This is not a second service
  manager: D11 starts a server by a *call to* it, and nobody calls a driver; netd keeps its helpers as `init` keeps a
  login on each console. DESIGN gets the rule "a server may supervise its own helper processes, with D11's numbers".
- **Who is a driver:** netd answers `NETDRV_*` only from a pid in its own spawn list (pids are never reused,
  `proc.c`), and checks every buffer number, piece, length and count (a driver is trusted code; a wrong number fails
  the frame, never netd). A driver is ZEXE v1 (D13's raw-message rule needs no exception: it sends no raw messages).

### 2.4 Buffers, and DMA that outlives its owner

netd lends every frame buffer to the driver: empty ones to fill, full ones to send. A copying driver (NE2000 and
CS8900A by PIO, the A2065 from its board memory, plipbox through the CIA) copies straight into netd's buffer; a DMA
driver lets the hardware write into it. No frame is copied twice. A frame lent for sending is held (`pbuf_ref`) until
the driver reports it done, so a TCP retransmit never rewrites a header still in the ring (lwIP's
`tcp_output_segment_busy` relies on that). A zero-copy receive uses lwIP's custom pbufs, whose free gives the buffer
back to the ring.

**DMA is the dangerous case** (GENET; any later bus-master card): without an MMU, hardware that keeps writing after
its driver or netd died writes into memory the kernel has given to someone else. So:
1. **A claim:** `DRV_CLAIM` (the hardware table's GENET base, or a board from the board list) makes the driver the
   only owner of that device; a second claim is -EBUSY. A claim ends only after its quiesce has run (3).
2. **DMA memory belongs to the claim, not to a process:** `DRV_DMAMEM` gives a region (fast RAM, 64-byte aligned,
   whole cache lines) owned by the claim (an owner value that is no pid, as D13's `SEG_OWNER`): netd's buffers for a
   DMA interface are made in it (netd may read and write it: one address space). Neither netd's nor the driver's end
   frees it.
3. **A quiesce program:** with the claim the driver registers a short list of register writes that stop the device
   (GENET: receive and transmit DMA off, UMAC receive and transmit off; the same small language as the interrupt check,
   5.4). The kernel runs it **when the driver ends, and when netd ends**, before anything of either is freed, then
   detaches the driver's interrupt program and frees the claim's region. A new driver's `DRV_CLAIM` resets the device
   anyway.
4. netd never reuses a buffer lent to a dead driver; it frees them with the claim.

**No network buffer needs chip RAM** (no Amiga network hardware uses the chipset's DMA); buffers come from fast RAM when
there is any, else chip RAM. On the PiStorm the Pi's DMA reaches only Emu68's RAM, which is all of its fast RAM
(knowledge base `amiga/emu68-network-usb-rtg-hardware.md` section 1).

## 3. The kernel's RAM-floppy limit: servers from the disk (independent; built and merged first)

### 3.1 Today (*measured*, `make BUILD=...` at 7b5149d)

The RAM floppy (`zenix-ram.adf`) holds the bootblock, the kernel and a 704 KB RAM disk in 880 KB, so the kernel may be
at most 179,200 bytes; it is 176,144 (3,056 left). 80,624 bytes of it are four server images (`kernel/embed.S`: fsd
28,676, zfsd 36,688, diskd 9,360, boot 5,900). They stay in memory with the kernel, and each running server is a second
copy (D18's "not taken" list asked for "its own design note and reviews": this section). D11 already starts a service
from a file (an absolute path, read as root). `root-ram.img` has 203 free 1 KB blocks, `root-mount.img` 37 (and 27 free
inodes).

### 3.2 What must stay built in

To read the root, Zenix needs the disk server and the root's filesystem server before any file can be read, and D11
starts them again after a crash from the image in the kernel: **the restart lifeline must not need the disk it
serves.** So `diskd`, `fsd` (ext2: the root of the RAM floppy, the SD card and every image the owner uses) and `boot`
(pid 1) stay built in. **zfsd leaves the kernel**: only a ZenixFS mount or a ZenixFS root needs it, never the owner's
card or the RAM floppy.

### 3.3 The change (step 1)

1. **D11 6.4's own rule** ("a filesystem type with no built-in image is a file service, registered by `mount` with its
   path"): `mount` registers `/sbin/zfsd DEVICE fs.DEVICE -id ID` (user/mount.c; as built without the port word, which
   does not fit the 80 bytes: section 14). The frozen built-in name `@zfsd` keeps
   working for an older `mount` through **one fixed alias** in svc.c's image table (`@zfsd` = the file `/sbin/zfsd`),
   not a general fallback (no `@NAME` to path rule, so nothing like `@../x` can arise). D11 5.1's "a mount's restart
   needs no disk read" stays true for ext2 mounts and becomes a read from the root for ZenixFS mounts; d11's note, the
   svctab comment and CLAUDE.md's frozen-list wording say so.
2. `kernel/embed.S` loses zfsd: **the kernel image shrinks by 36,688 bytes**, and every machine has 36 KB more free
   memory. `/sbin/zfsd` is installed on every root image (the base package, D15).
3. **The RAM disks are sized per image:** `zenix-mount.adf` (the mount tests) gets a larger RAM disk of its own to carry
   `/sbin/zfsd` (36 data blocks and an indirect block: exactly its 37 free blocks today, so it needs more), while
   `zenix-ram.adf` keeps 704 KB (a RAM disk stays resident on a RAM-floppy boot, which the demo floppy will be). The
   freed bytes are kernel headroom: about 39 KB, of which the network's kernel part takes 5-6.5 KB (7.1).
4. **A ZenixFS root** needs zfsd before any file can be read. Today only the FS-UAE tests boot one (`zroot`, svc-test's
   `z`/`zo` parts); no real machine does. The build makes `kernel-zfs.bin` (fsd, zfsd, diskd, boot: about 181 KB with
   the network's part, too big for a RAM floppy, so on a kernel-only `zenix-zfs.adf`) for those tests. **They then test
   a test-only kernel, not the shipped one** (said in their scripts). **The update refuses a bundle whose kernel has no
   server for the running root's type** (the kernel's image list in its version record; D15's formats grow at the end),
   so an update can never make a ZenixFS-root machine unbootable (built in zenixboot, not zup: section 14). When block 4 makes a ZenixFS root real, its own decision
   picks the variant in the bundle or a boot module (zenixboot handing the root's server to the kernel, as it hands the
   RAM disk). **Owner question 5.**
5. The network is never built in: `/sbin/netd` and `/sbin/net/*` are files from the start.

**Not taken:** running fsd, diskd and boot in place from the kernel image (ZEX2 execute-in-place, about 44 KB): the
pristine image is the restart lifeline, and a stray write would damage the only copy; freeing boot's 5.9 KB image after
its start (a hole in the kernel's reserved range, a small win for its own change); compressing the images (a
decompressor in the boot path, D13 section 10).

## 4. The programming interface: BSD sockets

### 4.1 A socket is a file descriptor

A new kernel file kind, **`FT_SOCK`** (internal, not frozen), next to pipes and ttys in `vfs.c`, and a mode type
**`S_IFSOCK` 0140000** for `fstat` (Linux's value; mode bits are frozen, so it is claimed). Each socket has a kernel
object, a **ksock** (`NSOCK` 32: its index and a generation name it to netd; netd's ready bits; the D16 waiter slot
mask; a reference count; the epoch, 4.3; the send and receive timeouts). `read`, `write`, `close`, `dup`, `dup2`,
spawn's standard files (an `accept`ed socket handed to a child, as inetd does) and `poll`/`select` work as on a pipe.
netd always names sockets by the kernel's (index, generation); for `accept` the kernel takes a free ksock (and checks a
free fd) first and passes it in the request. A few ksocks are kept for root (so two users cannot take every socket),
and `limits.md` gets `NSOCK`, `NFD` 16 per process and `NFILE` 64 shared with every file.

**`O_NONBLOCK`** does not exist in Zenix yet: it becomes a file-table flag (Linux's value 0x800, octal 04000, claimed: `O_*` are
frozen) set by `socket(..., SOCK_NONBLOCK)` or libz's `fcntl(F_SETFL)`, meaning the same for sockets now and pipes
later. `FIONREAD` and `FIONBIO` (ported code uses them) are `SYS_sock` ops.

### 4.2 System calls (three new numbers, 90-92 today; claimed in CLAUDE.md before building)

- **`SYS_sock` (op, fd, request)**: every BSD socket call, multiplexed like Linux's `socketcall`. Ops (frozen,
  append-only): `SOCKET`, `BIND`, `CONNECT`, `LISTEN`, `ACCEPT`, `SENDTO`, `RECVFROM`, `SHUTDOWN`, `GETSOCKOPT`,
  `SETSOCKOPT`, `GETSOCKNAME`, `GETPEERNAME`, `IOCTL` (`FIONREAD`, `FIONBIO`), `RESOLVE` (4.5), `IFLIST`, `IFSET`,
  `ROUTE`, `NETSTAT` (section 6). `request` points to a **`struct usockreq`** (first size `USOCKREQ_FIRST` 32,
  frozen; buffer, length, flags, address, address-length pointer, level, option, value, value-length pointer). **The
  kernel copies the request** (its first size, or the caller's if bigger) into the message, so netd never reads it
  again from user memory, and checks every pointer in it by a small per-op table of directions (`user_ok_rd` for what
  netd reads, `user_ok_wr` for what it writes, both for in/out lengths; D13 section 5): an address length is clamped to
  `min(*addrlen, 28)` for the check and the write and the true length is returned (programs pass `sizeof(struct
  sockaddr_storage)` = 128); a buffer over 64 KB is clamped (a blocking stream send loops, 4.3). netd reads and writes
  the program's buffers in place: **no copy in the kernel**; the kernel clamps every count netd returns to what it
  checked. (TCP keeps sent data until it is acknowledged, so netd copies once into its send buffer and once out of its
  receive buffer: the minimum.)
- **`SYS_netsrv` (op, a, b)**: only for the process the kernel started for `net`: `NETSRV_HELLO` (the current epoch,
  4.3), `NETSRV_READY` (a list of (index, generation, bits), `user_ok_rd`, at most `NSOCK` items) and `NETSRV_REPLY`
  (reply to a message *and* set its ksock's bits in one trap: 4.3 rule 1).
- **`SYS_drv` (op, a, b)**: for drivers: only a child of the running `net` server, and for claim-bound ops only the
  claim's holder (enforced by the kernel through `ppid` and svc's pid; not merely root, so no sudo session installs an
  interrupt program). Ops: `DRV_HWINFO` (a copy of the hardware table, `kernel/hwinfo.h`, with `HWINFO_FIRST` and the
  caller's size: user-visible from now on, so it grows at the end: new fields from `reserved[5]`, then only after
  `board[]`), `DRV_CLAIM`, `DRV_DMAMEM`, `DRV_QUIESCE`, `DRV_CACHE` (clean, or clean+invalidate, a range inside the
  claim's DMA region; an "invalidate" is done on whole 64-byte lines only, and partial lines at the edges are cleaned
  and invalidated, never just invalidated: CLAUDE.md's never-`cinva` rule; `cpushl` per line on 040/060 and Emu68, the
  Emu68 range opcode with `HW_RANGEOPS`; nothing on 020/030, whose data cache Zenix keeps off), `DRV_ATTACH`,
  `DRV_XFER` (2.3), later `DRV_IRQ` (5.4).

**Messages:** `NET_*` (kernel to netd, 1-63) and `NETDRV_*` (drivers' blocks the kernel forwards, from 64) in a new
`kernel/netproto.h`, frozen and append-only like `fsproto.h`, non-overlapping because both arrive on one port. Port
name `net` (claimed). **Error numbers** (Linux's values, claimed): `ENOTSOCK` 88, `EDESTADDRREQ` 89, `EMSGSIZE` 90,
`EPROTOTYPE` 91, `ENOPROTOOPT` 92, `EPROTONOSUPPORT` 93, `EAFNOSUPPORT` 97, `EADDRINUSE` 98, `EADDRNOTAVAIL` 99,
`ENETDOWN` 100, `ENETUNREACH` 101, `ECONNABORTED` 103, `ENOBUFS` 105, `EISCONN` 106, `ENOTCONN` 107, `ETIMEDOUT` 110,
`ECONNREFUSED` 111, `EHOSTUNREACH` 113, `EALREADY` 114, `EINPROGRESS` 115. `ECONNRESET` 104 now has its Unix meaning
too ("reset by the peer") besides D11's "the server is gone": on a socket the server's absence is reported as
`ENETDOWN` instead (4.3), so a program can tell them apart.

### 4.3 Blocking, readiness, netd's end: the rules (binding for the build, as D16 section 14)

1. **netd's bits are the only truth, and they change only inside netd's own system calls.** The kernel never clears or
   sets a ksock's bits by itself. Every reply that can change a socket's state is a `NETSRV_REPLY` (reply and new bits
   in one trap); changes that come from the network are pushed with `NETSRV_READY` once per turn of netd's loop.
   So a reply of -EAGAIN always arrives together with the bits that say "not ready".
2. **netd never keeps a caller waiting**: every `NET_*` request is answered at once, done or -EAGAIN. A client in a
   message call cannot be interrupted (G4), so waiting inside netd would make ^C useless.
3. **The kernel's blocking loop** (in `sock.c`, D16's primitives):
   ```
   for (;;) {
       ev_arm(ksock)                       register on the slot mask, clear EV_IO
       if (epoch dead) return -ENETDOWN (send: and SIGPIPE rules as below)
       if (bits & needed) or nonblocking:
           r = call netd                   an EV_IO posted meanwhile stays pending (ev_post does not ready P_MSG)
           if r != -EAGAIN or nonblocking: return r (after the partial-count rule)
           continue                        to the top, not straight to the wait
       ev_block(timeout)                   -EINTR on a caught signal; SO_RCVTIMEO/SO_SNDTIMEO: -EAGAIN at the deadline
   }
   ```
4. **Partial sends:** a blocking stream send queues the whole buffer in pieces (at most 64 KB a message); after progress
   a signal or timeout returns the count with no restart flag (`pipe_write`'s rule, vfs.c), so `SA_RESTART` never sends
   the start again. A send on a connection the other side closed gives -EPIPE and SIGPIPE (as a pipe), unless
   `MSG_NOSIGNAL`.
5. **`SA_RESTART`** as Linux (`man 7 signal`): `accept`, `recv`, `send`, `connect` restart; not when the socket has a
   timeout set (`SO_RCVTIMEO`/`SO_SNDTIMEO`), never `poll`/`select`.
6. **`connect`:** the first message is `CONNECT`; a blocking connect then waits for POLLOUT|POLLERR and asks the result
   with `GETSOCKOPT SO_ERROR`, never sends `CONNECT` again (a second `tcp_connect` would fail). An interrupted connect
   returns -EINTR and the connection goes on (POSIX); a restarted one only waits. Non-blocking: -EINPROGRESS.
7. **`accept`:** a free fd and a free ksock are reserved before `ACCEPT` is sent and given back on -EAGAIN, -EINTR or a
   kill.
8. **`poll`/`select`** read the bits like a pipe's: no message. netd reports POLLOUT only when at least one MSS (or a
   third of the send buffer) is free, not at one byte.
9. **The epoch:** a kernel counter `net_epoch` in `sock.c` (not the service's start count, which D11 resets when the
   program text changes), raised **when netd ends**. netd learns it with `NETSRV_HELLO` and returns it in every reply
   that creates a socket (`SOCKET`, `ACCEPT`, `RESOLVE`); the kernel keeps a new ksock only if that epoch is the live
   one, else it frees it and answers -ENETDOWN itself. netd answers any (index, generation) it does not know with
   -ENETDOWN: requests queued at netd's death go to the next netd (D11), and this is how they fail.
10. **netd's end** (`sock_server_gone()`, called from svc.c's end of the `net` entry's server, before its memory is
   freed, after the drivers' quiesce of 2.4): the epoch is raised; every ksock of the old epoch is marked dead
   (POLLERR|POLLHUP) and **its waiters are woken** (`ev_wake`), so a blocked `recv` or `accept` returns -ENETDOWN at
   once; any later call on it fails the same way; only `close` succeeds. A netd restart loses every connection, as a
   reboot of a router would.
11. **Close:** a ksock's last close sends `NET_CLOSE` **only if its epoch is live**, as a call that never starts or
   waits for a server (`SVC_NS_SKIP`, not `FS_CLOSE`'s `SVC_NS_CLOSE`, which would make a dying process wait up to
   30 s for a loading netd that never knew the socket); the ksock is freed after the reply.
12. **"Given up"** (5 ends of netd in 60 s, D11): socket calls get -ENETDOWN; `ifup` or `ifconfig ... up` (root)
   registers `net` again and says so. D11's limits are not frozen, so a later timed retry for `net` alone stays
   possible; a remote packet that crashes netd five times is otherwise a remote "network off" until root acts (9.2).

The host model of these rules (8.1) runs every interleaving of a reply, a push, a waiter and netd's end.

### 4.4 libz

`socket`, `bind`, `connect`, `listen`, `accept`, `send`, `sendto`, `recv`, `recvfrom`, `shutdown`, `getsockopt`,
`setsockopt`, `getsockname`, `getpeername`, `fcntl` (`F_GETFL`/`F_SETFL`), `ioctl` (the two above), `inet_pton`,
`inet_ntop`, `htons` and friends, `getaddrinfo`, `freeaddrinfo`, `gai_strerror`, with BSD/Linux structures and
constants (`AF_INET` 2, `SOCK_STREAM` 1, `SOCK_DGRAM` 2, Linux's `SOL_SOCKET`/`SO_*` values), so ported programs need no
changes. Each is a few lines over `SYS_sock`; `--gc-sections` keeps them out of programs that do not use them.

### 4.5 Names

Lookups happen in netd, not in each program (one resolver and one cache, golden path "no copies"): `getaddrinfo` parses
numbers itself, else `SOCK_RESOLVE` makes a ksock that becomes readable when the answer is in (the same interruptible
wait), reads the addresses and closes it. netd answers from `/etc/hosts`, its cache, then DNS (6.2).

## 5. Drivers

### 5.1 The interface that fits every card

| Shape | Cards | What the driver does with netd's buffers | Interrupt |
|---|---|---|---|
| DMA into host memory | CM4 GENET | posts them (from `DRV_DMAMEM`) in the ring, `DRV_CACHE` around each transfer | GIC via level 6, or polled |
| Buffer memory on the card | A2065 (LANCE, 32 KB on the board), Ariadne (PCnet-ISA) | copies from the card's memory into them | INT2 (shared) |
| Programmed I/O | NE2000 (PCMCIA), CS8900A (clockport), plipbox (parallel port) | reads/writes the data register into them | INT2/INT6; the CIA FLAG for plipbox |

A user process reaches device registers by plain pointers (no MMU); the kernel gives a driver only the claim, the DMA
memory, the quiesce, the cache instructions (privileged) and interrupts. The 68k data cache is off on every Zenix
machine (`start.S`), so register access needs no cache rules on a real Amiga; on Emu68 the device windows are mapped
uncached.

### 5.2 (a) The CM4's Ethernet under Emu68: GENET v5 (`/sbin/net/genet`)

*Read* from the knowledge base (`amiga/emu68-network-usb-rtg-hardware.md`, `emu68-driver-stack-why-fast.md`,
`zenix-pi-drivers-progress.md`: rondoval's `emu68-genet-driver`, Linux's and U-Boot's `bcmgenet`), not run:
- **Where it is:** stage0 reads Emu68's device tree while AmigaOS lives and fills the hardware table: `genet` (the base
  after Emu68 rewrote `/scb`'s ranges: 0xF7580000 on Emu68 1.1, 0xFD580000 seen on 1.0.7, *verified on the A1200
  2026-10-23*: never hard-coded), `genet_irq0/1`, `phy_addr`, `mac` (`local-mac-address`).
- **Bring-up** (U-Boot's and Linux's order): flush the receive buffer, UMAC soft reset, MIB reset, RBUF control with
  the 64-byte status block on and EEE off (EEE breaks reception), the MAC, the rings (256 descriptors in the register
  window, default queue 16), the PHY through GENET's MDIO (a BCM54xx, RGMII with receive delay), link, DMA on. Every
  stage logs a line, every wait has an E-clock timeout (CLAUDE.md).
- **Memory:** frames 2 KB in the claim's DMA region, 64-byte aligned (every received frame starts with the status
  block). The 68k address is the DMA address (the SD driver's mailbox works that way on the A1200, *verified
  2026-10-24*; GENET's path is *read*).
- **Caches:** the ARM's data cache is not coherent with DMA: receive buffers cleaned+invalidated before posting and
  invalidated (whole lines) after; frames to send cleaned before the descriptor. With `HW_RANGEOPS` the driver issues
  the range opcodes itself (AmigaOS drivers do it from task code, which runs in user mode, so they should work from a
  Zenix process): **to check first on the A1200 with a one-page test program, on the owner's weekend checklist of
  2026-10-30/31**, because its answer shapes `DRV_CACHE`; else `DRV_CACHE`.
- **Polled first:** the rings are checked at every wake-up (the doorbell; a 10 ms timer while up; no sleep while the
  last turn moved frames). A 256-entry ring holds 2.5 ms of a full gigabit line and Emu68 empties it in microseconds:
  enough for telnet, ssh, wget. The interrupt (5.4) after measurements; on Emu68 it is silenced at GENET's own
  interrupt mask, never at the GIC (Emu68 owns IAR/EOIR).
- **Offloads later** (transmit checksum, receive full-frame checksum).
- **Effort** (*proposed*): ~800-1,000 lines, plus a host model like `tools/emmc-fake.h` (GENET's registers and rings)
  so the bring-up order and the ring arithmetic are tested on the PC before the A1200.

### 5.3 (b) SLIP over the serial port, for every Amiga (inside netd)

- netd opens `/dev/ttyS0` (D12: root only, one opener) and runs lwIP's `slipif` (RFC 1055, MTU 1006, MSS 966), 115,200
  baud 8N1, no handshake. Reads with `poll` in netd's main loop. **While SLIP is up the port is the network's:** no `cu`,
  and the kernel log stays in `dmesg` but not on the wire (D12). **Owner question 3.**
- **netd never blocks on the port:** the kernel's serial write waits tick by tick when the 4 KB send ring is full
  (`serial.c`), and its `poll` always says writable. So the kernel gets a truthful POLLOUT for `/dev/ttyS0` (free room
  in the send ring; D16 rule 11 changes for this device only, a correction, not a new meaning for programs that wrote
  before) and netd writes at most that much, keeping the rest of a frame in its own queue. The receive ring is 1 KB
  (89 ms of SLIP), so netd's turn must stay short: no file reads in the request path (6.1).
- **The PC side** (`tools/host/zenix-slirp`, GPL-2.0-only): reads SLIP from a serial port or pty and gives each IP
  packet to libslirp (QEMU's user-mode network, BSD-3): NAT to the PC's network and the internet, DNS, port forwards
  (bound to 127.0.0.1 unless asked), **no root**; Zenix is 10.0.2.15, the gateway 10.0.2.2, DNS 10.0.2.3. It passes
  every byte that is not a SLIP frame to a log (the kernel's lines reach the wire again when netd closes the port).
  libslirp needs GLib, so the tool is not one standalone binary unless linked statically: **owner question 4.** A PC
  with `slattach` needs nothing Zenix-specific: plain SLIP.
- **PPP later:** lwIP's PPP (30-40 KB, *estimate*) has pppd-derived files whose licence notices must be checked
  against GPL-2 first. SLIP needs none of it.

### 5.4 Interrupts for drivers (with the first interrupt-driven driver)

Amiga interrupt lines are shared and level-triggered (INT2 with CIA-A, the keyboard, Gayle's IDE; INT6 with CIA-B and,
on Emu68, every ARM interrupt), and the line must be quiet before the interrupt returns. As Linux's UIO: **the kernel
silences the device, the driver handles it.** `DRV_IRQ` attaches a check-and-silence program: at most 8 steps of
read-a-register / test-bits / write-a-register, **no jumps backwards**, every address inside the claimed register
window or board (a bus error in supervisor mode would be a kernel crash), run after the kernel's own sources. When it
claims an interrupt, the kernel posts the driver's event (D16: interrupts only queue, `kwork` wakes). **Storm guard:**
too many level-2 or level-6 interrupts in a row that nobody claimed turns the attached programs off and logs it (Linux's
"nobody cared"). Detached at the driver's end, after the quiesce. A CIA source (plipbox's FLAG on CIA-A, read-to-clear
and shared with the keyboard) is a fixed source the kernel's own CIA handler passes on. Designed in detail with the
first interrupt-driven driver.

### 5.5 (c) Later: plipbox, NE2000, CS8900A, A2065, Ariadne

All fit 5.1. plipbox (knowledge base `zenix-hardware-driver-survey.md`) needs the CIA FLAG source and the kernel to own
the CIA register bits it shares with the serial lines; NE2000 on PCMCIA needs Gayle's PCMCIA I/O window; CS8900A is PIO
on the clockport; A2065 and Ariadne are Zorro II boards in the hardware table's board list (FS-UAE's A2065 appears there
as manufacturer 0x202, product 112, *verified 2026-10-26*, 8.2). A polled A2065 driver needs no interrupt design.

## 6. Configuration, DHCP, DNS, commands

### 6.1 Files (new `/etc` formats, frozen once shipped; unknown words reported and ignored)

- **`/etc/network/interfaces`**, a small subset of Debian's format: `auto IFACE`, `iface IFACE inet dhcp|static`, and
  in a stanza `driver NAME [ARGS]` (Zenix's word: `genet`, `slip /dev/ttyS0 115200`; NAME has no `/` and means
  `/sbin/net/NAME`), `address A/PREFIX`, `gateway G`, `pointopoint P`, `mtu N`, `hostname H`. netd uses the file only
  if it is owned by root and not writable by group or others (it starts programs as root from it). Example:
  ```
  auto eth0
  iface eth0 inet dhcp
      driver genet

  iface sl0 inet static
      driver slip /dev/ttyS0 115200
      address 10.0.2.15/24
      gateway 10.0.2.2
  ```
- **`/etc/resolv.conf`** (`nameserver`, `search`) and **`/etc/hosts`**, standard. DHCP's DNS servers stay in netd's
  memory and show in `ifconfig` (nothing written behind the owner's back).
- **Read at netd's start and on `ifup`/`ifconfig` (a reload), never per request** (a file read from a floppy takes
  hundreds of milliseconds, and netd's turn must stay short).
- **A static address:** the `inet static` stanza, or `ifconfig eth0 192.168.1.50/24` and `route add default
  192.168.1.1` (root; until netd's next start).

### 6.2 DHCP and DNS live in netd

lwIP's DHCP client and DNS client (cache, random query ids, `LWIP_DNS_SECURE`) run in netd: they need its timers, a
lease belongs to the interface, and a separate `dhcpcd` would be one more process on a 2 MB machine. The ARP probe of an
offered address (`acd`) is left out. When a lease is lost or the address changes, lwIP ends the connections bound to the
old address (`tcp_netif_ip_addr_changed`); programs see `ECONNABORTED`.

### 6.3 Bringing the network up

- **boot registers `net` itself** (`net` -> `/sbin/netd`, nice 0, when `/sbin/netd` exists), as it registers `disk` and
  `fs`: not a line in `/etc/svctab`, because D15 never makes a new feature depend on the owner merging a config
  `.new` (d15-self-updates.md, config files), and an owner who edited svctab would get no network. One held slot, no
  memory until used.
- **Nice 0, not the servers' -10:** clients lift netd to their own level anyway (priority inheritance, D10); at -10, a
  flood from the LAN or a netd bug would starve every shell on a 14 MHz machine, `kill` included. netd also yields
  after a fixed amount of work per turn.
- netd **opens its port before** it brings interfaces up (D11's 30 s start deadline), then reads
  `/etc/network/interfaces` and brings up the `auto` ones.
- At boot **`init` starts `ifup -a` without waiting for it** when the file has an `auto` line (that call starts netd;
  logins never wait for a DHCP lease or a missing cable). A machine without the file never starts netd.

### 6.4 Commands (each with its page, `commands.md`, `features.md`, in its build step)

- **`ifconfig`** (BSD/Roadshow style; `ifup` and `ifdown` are names of the same program, as `more` is of `less`): show
  interfaces (address, MAC, MTU, link, lease, DNS, counters), `up`/`down`, an address; root for changes; returns once
  the interface is configured, not when a lease arrives.
- **`route`**: show, `add default GW`, `del`.
- **`ping`**: ICMP echo over Linux-style unprivileged ICMP datagram sockets; netd sets each socket's echo id itself,
  sends only echo requests on it and gives replies only to the socket with that id. Today's `ping`/`pingd` (the message
  test programs, D11's test service) become `msgping`/`msgpingd` in step 0; the port name `ping` stays.
- **`netstat`** (`-t -u -l -a`): `SOCK_NETSTAT`'s `unetstat` rows (first size `UNETSTAT_FIRST` 40, frozen; the caller's
  buffer size).
- **`host NAME`**: a lookup through netd.

### 6.5 Who calls whom (a rule added to D11's list)

netd calls only the root's filesystem server `fs` (its config files, spawning drivers) and the kernel; never a `fs.*`
mount; drivers reach only `net` (through the kernel). So **no server that netd's files depend on may call `net`**: no
root filesystem over the network. A later NFS client is a filesystem server that calls `net`, which changes D11's
"filesystem servers call only diskd": allowed for mounts only, decided with NFS.

## 7. Size and memory budgets

### 7.1 The kernel (always resident; *estimates* at -Os unless measured)

| Part | Size |
|---|---|
| `FT_SOCK`, ksocks, `SYS_sock` with its pointer table and request copy, the blocking loop, `poll`'s socket case, SIGPIPE, the epoch and `sock_server_gone`, `NET_CLOSE` | 3-4 KB |
| `SYS_netsrv` | 0.3 KB |
| `SYS_drv`: hwinfo, claims, DMA regions, quiesce run, cache ranges (the interrupt programs later, ~0.5 KB) | 0.8 KB |
| The entropy pool (BLAKE2s ~1 KB, ChaCha20 616 bytes *measured* at -Os), `/dev/urandom`, `/dev/random` | 1.7-2 KB |
| The `@zfsd` alias, `O_NONBLOCK`, `S_IFSOCK`, `ttyS0`'s truthful POLLOUT | 0.2 KB |
| boot: registering `net`, the seed file (boot.zx is inside the kernel image) | 0.3 KB |
| **Total** | **6-7.5 KB**, against **36,688 bytes** zfsd frees |
| Data: 32 ksocks (~24 bytes each), the pool, claims | ~1 KB |

New kernel code is `-Os` in its own files (`sock.c`, `random.c`, `drv.c`); only the ready-bit test on the poll path
is inline.

### 7.2 A machine that does not use the network (the stock A1200 as the owner runs it today)

**About 28-30 KB more free memory than today**: the kernel grows by 6-7.5 KB of code and 1 KB of data, and the
36,688-byte zfsd image is no longer resident. netd is never loaded. The disk holds `/sbin/netd` and the pages.

### 7.3 A machine that does (*estimates*, measured at steps 5 and 8)

| | Stock A1200, SLIP | PiStorm or a 060, Ethernet |
|---|---|---|
| netd code: lwIP 48.8 KB (*measured*, unlinked), netd's own 10-15 KB (config, ops, hosts and cache, the driver protocol, SipHash ISN, ChaCha20, netstat, ICMP sockets), libz ~6 KB | ~65-70 KB | ~65-70 KB |
| netd data, sized by the machine as fsd's cache (D18): pool limits 9.4 | 16-24 KB: MSS 966, window 2 x MSS, 4 KB send buffer, 8 TCP connections, 16 buffers | 100-200 KB: MSS 1460, window 16-32 KB, 64-128 receive buffers |
| netd stack + kernel stack | 8 + 4 KB | 8 + 4 KB |
| Driver | none | GENET ~10 KB code, 4 + 4 KB stacks, its DMA region |
| **Total** | **~100 KB** of 1,332 KB free with one login (D18, *measured* in FS-UAE) | ~200-300 KB |

The 1 MB chip-only matrix machine (395 KB free, D18) can run netd on SLIP with a session less; the matrix says so.
netd's memory comes in a few large regions (`SYS_memalloc`, 8 per process).

### 7.4 The demo floppy (roadmap block 2's end)

One 880 KB RAM floppy with the network, telnet and a web fetcher: netd (~65-70 KB), the commands and their pages must
fit its RAM disk, which stays resident on a 2 MB machine. With zenix-ram.adf's RAM disk at 704 KB and 203 free blocks
today, it does not fit as plain files: the demo floppy needs the packed programs the roadmap plans for it (programs
unpacked when they start). Its own step after this note, measured.

## 8. Tests

### 8.1 On the PC

- **netd on the host** (as `zup-host`, D15): lwIP and netd built for Linux against a fake of the kernel's socket layer,
  SLIP over a socketpair to `zenix-slirp`: TCP to a host echo server, UDP, DNS from a test resolver, ICMP, netd's end
  and restart. No root.
- **The rules of 4.3 in a model** (as `tools/signal-host-test.c`): every interleaving of a reply, a push, a waiter, a
  signal and netd's end; no lost wake-up, no spin (a client better placed than netd with a stale bit must block).
- **lwIP's unit tests** (`test/unit`) with Zenix's `lwipopts.h` and the local patches.
- **Fuzzing** the input path (lwIP's `test/fuzz`), one run at a time with a memory limit (this machine's rule).
- **GENET's register and ring model** with the driver, including a quiesce while DMA runs.
- **zfsd's move:** none needed on the host; the size counts in every build.

### 8.2 In FS-UAE

- **Loopback first** (no hardware): sockets, `poll`, blocking and ^C, `SA_RESTART`, partial sends, `O_NONBLOCK`,
  SIGPIPE, an accepted socket given to a child, many connections, a SYN flood against the pool limits, netd killed
  (every blocked call returns -ENETDOWN at once, the next call starts a new netd), five kills (given up, `ifup` brings it
  back), bind below 1024 refused to a user, raw messages to `net` refused, a ZEX2 program's literal as a receive buffer
  (-EFAULT), `netstat`.
- **SLIP:** FS-UAE's serial port is a pty; `zenix-slirp` on it. The rig types into the same port, so a SLIP test runs
  its commands from a script started at boot, ends with `ifdown sl0` and prints its results; `zenix-slirp` passes the
  console's bytes to the tap. Checks: `ping 10.0.2.2`, TCP through a forward, DNS, a 100 KB transfer compared by
  checksum, noise on the line (bad frames dropped and counted).
- **Ethernet in the emulator:** FS-UAE 3.2.35 emulates an **A2065** (`network_card = a2065`, slirp inside; *verified
  2026-10-26*: on the A1200 config it appears in Zenix's board list as 0x202/112 at 0xE90000, 64 KB; its DHCP is not
  yet checked). With a polled A2065 driver, ARP, DHCP, the driver protocol, a driver crash and its restart run in
  `make check`. **Owner question 2.**
- **The driver protocol before any card:** a fake driver (test image only) that answers ARP and ICMP itself, killed
  mid-transfer; a second claim of one device refused.
- **zfsd from the disk** (step 1): the mount tests (a ZenixFS mount from `/sbin/zfsd`, a killed zfsd started again from
  the file), the ZenixFS-root tests on `zenix-zfs.adf`, `free` showing the 36 KB, zup refusing a bundle without the
  root's server.

### 8.3 Only on the real machines

GENET at all; the range opcodes from user mode; SLIP at 115,200 through Paula on a 14 MHz 68EC020 while the display and
the disk work (overruns); the speed table (ping round trip, TCP over SLIP and over loopback as the stack's CPU cost) on
the stock A1200, the 060 and the PiStorm, labelled by machine; the entropy sources' real behaviour.

## 9. Security

### 9.1 What the server and the kernel check

- netd answers `NET_*` only with `UMSG_KERNEL`, `NETDRV_*` only from its own children (pid list) through `SYS_drv`;
  anything else -EACCES.
- **Ports below 1024: root only** (the effective uid the kernel stamps, as Linux). Raw IP sockets: root only. ICMP
  datagram sockets: echo only (6.4). Interfaces, routes, addresses: root. `SO_REUSEADDR` never shares a port between
  different uids.
- No listening service by default; no firewall in revision 1 (lwIP's input hook is where one would go: knowledge base
  `amiga/lwip-amiga-firewall.md`).
- Everything from the network is untrusted: lwIP's parsing, fuzzed (8.1); netd's own parsing (config, driver blocks)
  range-checks every number.

### 9.2 What a system without an MMU can and cannot promise

Can: a program uses the network only through the kernel's checks (its own fds and buffers, root for low ports and
configuration); a crash of netd or a driver is contained (the quiesce, 2.4) and restarted. **Cannot:** any program can
read or write netd's memory (other users' connection data) and poke the card's registers: users are a convenience, not
protection (D8, limits.md). A remote bug in netd's parsing has the whole machine's rights (no MMU, no non-executable
memory), and five remote crashes in a minute switch the network off until root acts (4.3 rule 12). So: lwIP rather than
new code, the fuzzing, the pool limits, few listening services, and the security audit at block 2's end.

### 9.3 Random numbers

**Uses:** TCP's initial sequence numbers (RFC 6528: SipHash-2-4 keyed with 128 random bits over the connection's
addresses and ports, plus a 4 µs clock made of the E-clock and the tick; `LWIP_HOOK_TCP_ISN`; SipHash is smaller than
contrib `tcp_isn`'s MD5), ephemeral ports (lwIP picks them **sequentially** after one random start, `tcp_new_port` and
`udp_new_port`: a local patch makes each one random, RFC 6056), DNS query ids and ports, later the crypto library.

**The kernel's pool:** samples are accumulated with **BLAKE2s** and output comes from **ChaCha20 with fast key
erasure**, as Linux since 5.17; **`/dev/urandom`** never blocks; **`/dev/random`** blocks until the pool was first
seeded with an estimated 256 bits, then is the same, and polls readable from then on (Linux since 5.6). Samples are
mixed in batches (per tick, or when the ring fills), never one hash per event.

**netd's keys:** netd seeds its ChaCha20 (`LWIP_RAND`) and its ISN key from `/dev/urandom` *before* `lwip_init`, and
**polls `/dev/random`**: when it becomes readable (seeded) netd takes fresh keys; it reseeds every few minutes after.
So keys from an early boot are replaced as soon as the pool is real.

**Sources, and what each is credited:**
- **Credited** (a clock independent of the Amiga's crystal): key presses and releases (the keyboard's own controller,
  1 bit each), bytes from the far end of the serial line (the other machine's clock; sampled per SLIP frame, half a
  bit), the floppy's index pulse while the motor runs (motor speed jitter), and on Emu68 the Pi's hardware generator
  (BCM2711 RNG200, when stage0 finds it: a field of the hardware table's reserved words), fully.
- **Mixed but credited 0:** network frames (partly timed by an attacker, and sampled on a tick that comes from the same
  crystal as the beam under a polled driver), disk and SD completions (clocked by the chipset), mouse movement, a hash
  of not-yet-used chip RAM at a cold start.
- The sample is the **beam position** (`VHPOSR`, one chip-bus read): the CIA E-clock is the same crystal and a slow
  access. Interrupt handlers only store the 16-bit sample in a small ring.
- **The seed file** `/var/lib/random-seed` (root, mode 600): read at boot and at once rewritten and synced; **credited
  only after that rewrite reached a writable, persistent root** (never on a RAM-disk root, a read-only or rescue root,
  where the same seed would come back every boot) and only if the boot that wrote it had itself been seeded (a flag in
  the file). Written again at `reboot` and shutdown. **No image ever ships one:** `mkroot` and the bundle refuse a seed
  file (every copy of an image, a card or the demo floppy would share it).

### 9.4 lwIP's limits (a SYN flood must not eat netd)

lwIP's pools would hold their limits only as static arrays; with heap allocation (`MEMP_MEM_MALLOC`, needed to size
them by machine) `memp_malloc` ignores the counts. So a local patch counts each pool against the limits netd sets at
start (by machine memory): TCP connections, TIME_WAIT (killed oldest first when full), reassembly, out-of-order segments
(`TCP_OOSEQ_MAX_PBUFS`/`BYTES`, `IP_REASS_MAX_PBUFS`), a capped receive queue per UDP socket, and per listener a capped
backlog (`TCP_LISTEN_BACKLOG`, `tcp_backlog_delayed`).

### 9.5 lwIP's raw-API rules for the builder (`NO_SYS=1`)

`sys_check_timeouts()` every turn, `sys_timeouts_sleeptime()` as the `evwait` timeout; no lwIP call from a signal
handler (they only set flags); in a `tcp_err` callback the pcb is already gone; `tcp_close` may fail with ERR_MEM
(retry, or `tcp_abort`); `tcp_listen` returns a new pcb and frees the old; `tcp_recved` only after the program took the
data, and a receive callback that cannot queue returns ERR_MEM (lwIP keeps the data); `tcp_write` with COPY, ERR_MEM
becomes a partial count or -EAGAIN; `SO_REUSE` on, so a restarted server binds while TIME_WAITs exist.

## 10. Build order (each step small, merged with its tests; the full suite before each merge)

Almost every step touches the shared files (`kernel/kernel.h`, `Makefile`'s `UAPPS` and images, `user/usys.h`,
`user/libz.*`): **the steps are the main session's** (CLAUDE.md "Sessions"). A side session can take only the host-only
parts: the lwIP import with the host netd and its tests, `zenix-slirp`, the GENET model.

0. **`ping`/`pingd` become `msgping`/`msgpingd`** (`UAPPS`, `MOUNTAPPS`, tests, pages). Test: the D11 suites unchanged.
1. **zfsd from the disk** (section 3; its own DESIGN line, merged first): mount registers `/sbin/zfsd`, the `@zfsd`
   alias, zfsd out of `kernel.bin`, per-image RAM disks, `kernel-zfs.bin`/`zenix-zfs.adf`, zup's root-server check.
   Tests: 8.2 "zfsd from the disk", the RAM-floppy count.
2. **Random numbers** (9.3): pool, sources, devices, the seed file in boot and reboot, `mkroot`'s refusal. Tests: host
   (BLAKE2s and ChaCha20 against RFC 7693/8439 vectors, the crediting rules), FS-UAE (two boots differ; the seed file
   rewritten; `/dev/random` waits on a fresh image until keys are typed; a RAM-disk root never credits its seed).
3. **The kernel's socket layer** (4.1-4.3, `O_NONBLOCK`, `S_IFSOCK`, the error numbers) with a small test server in
   place of netd (`socktestd`: answers from a table, pushes bits, dies on command). Tests: the 4.3 model; FS-UAE:
   blocking, ^C, `SA_RESTART`, partial sends, `poll`, SIGPIPE, -ENETDOWN on its death with waiters woken, the
   no-start close, pointer checks, the clamps.
4. **netd with lwIP, loopback only**, libz's socket calls, `netstat`; sizes measured linked. Tests: host netd; 8.2
   "loopback".
5. **SLIP, `/etc/network/interfaces`, `ifconfig`/`ifup`/`ifdown`, `route`, `ping`, `zenix-slirp`, boot's `net`, init's
   `ifup -a`, ttyS0's POLLOUT.** Tests: 8.1, 8.2 "SLIP". **First thing the owner can use:** the stock A1200 on the cable.
6. **DNS, `host`, `getaddrinfo` by name**; DHCP code in. Tests: host, and FS-UAE through slirp's DNS.
7. **The driver protocol** (`SYS_drv`: claim, DMA memory, quiesce, cache, attach, xfer; doorbell, supervision) with the
   fake driver. Tests: 8.2 "driver protocol", a driver killed, a second claim refused, bad buffer numbers refused.
8. **(owner question 2)** a polled **A2065** driver: Ethernet, ARP and DHCP in `make check`.
9. **GENET** (polled): the host model, then the A1200 checklist (link, DHCP from the owner's router, ping, telnet to a
   BBS, the speed table). **The CM4's Ethernet in daily use.**
10. Interrupts (`DRV_IRQ`, 5.4) with whichever driver needs them first, measured.
11. A focused audit of the network (security first), its findings fixed; then telnet (its own note: pseudo-terminals,
   SIGHUP, the BBS terminal), the crypto library, ssh, wget, NFS, the remote control, `zpkg`, in the roadmap's order.

## 11. Open questions for the owner

1. **Order:** the roadmap names the CM4's Ethernet first; the note builds the stack and SLIP first (steps 3-6), because
   GENET can only be tested on the A1200 and needs the stack under it anyway, and SLIP works on every Amiga and in
   the emulator. The CM4's Ethernet comes at step 9 (8 with the A2065). Fine?
2. **An A2065 driver before GENET** (step 8), as the emulator's Ethernet, so the Ethernet half is checked by every
   `make check`; or later in the card order?
3. **SLIP takes the stock A1200's only serial port:** while the network runs over the cable there is no `cu` and no
   serial log. Acceptable (the network gives telnet and later ssh to the PC instead)?
4. **The PC-side helper** `zenix-slirp` (no root on the PC) needs the PC's libslirp and GLib: a normal Linux program, or
   one standalone binary (static linking, bigger)? Or no helper, and set up `slattach` + NAT as root on the PC?
5. **ZenixFS as the root becomes test-only until block 4** (the shipped kernel no longer carries zfsd; updates refuse to
   break a ZenixFS-root machine). CLAUDE.md lists "then ZenixFS as root" among the goals after the first boot: fine to
   wait for block 4?
6. **Licence notices:** lwIP (BSD-3) asks that its notice travels with binaries (miniz's MIT notice too, already
   shipped without it). Proposed: a `/usr/share/licenses/` folder on every image and in the bundle. Fine?
7. **lwIP itself** (BSD, 48.8 KB, loaded only while the network runs) instead of writing our own TCP/IP: the technical
   call is made; said here because it is the biggest outside code Zenix would take in.

## 12. Measurements behind this note

lwIP sizes: section 2.2 (*measured* 2026-10-26 by this note's author and again by the architecture review, the same
method). The kernel and image numbers: 3.1 (*measured* by the architecture review, `make` at 7b5149d). ChaCha20's block
at -Os: 616 bytes (*measured*, the architecture review). FS-UAE's A2065: 8.2 (*verified*). Everything else in sections 7 and 2.2 is an
estimate and is measured at its build step.

## 13. Reviews

Two independent reviews of revision 1 (2026-10-26), both read-only; every finding is worked in above.

**Protocol, robustness, security** (the stack's and the kernel's side, read against ipc.c, svc.c, event.c, signal.c,
vfs.c, proc.c, serial.c and lwIP 2.2.1): *blockers* - stale ready bits could spin a better-placed client or lose a
wake-up (4.3 rules 1-3); nothing woke socket waiters when netd died (rule 10); the epoch used D11's start count, which
resets, and requests queued at netd's death reach the next netd (rule 9); a driver's raw call to a stopped `net` would
*start* a new netd (2.3: drivers go through `SYS_drv`, no-start); DMA kept writing after its owner's death (2.4:
claims, DMA memory owned by the claim, quiesce). *Should fix* - partial sends and `SA_RESTART`, connect and accept loops,
`NET_CLOSE` as `SVC_NS_SKIP`, address-length clamping and the request copied by the kernel, netd at nice -10 starving
shells, `MEMP_MEM_MALLOC` losing lwIP's limits, lwIP's raw-API rules, netd blocking on the serial port and on files,
SIGPIPE ending netd, "given up" (-ENETDOWN, `ifup` re-registers) and the error numbers, entropy credit for
attacker-timed and replayable inputs (BLAKE2s mixer), netd's keys before the pool is seeded, sequential ephemeral
ports, bounds for interrupt programs, `SYS_drv` limited to netd's children. *Minor* - drivers by pid, ICMP ids, root's
share of ksocks, names with `/`, slirp forwards on 127.0.0.1, kernel lines on the wire after `ifdown`, `O_NONBLOCK` and
the ioctls, drivers stay ZEXE.

**Architecture, frozen interfaces, size, loading** (with measurements at 7b5149d): *blocker* - DMA into freed memory
(the same as above). *Should fix* - `net` in `/etc/svctab` breaks D15's config rule (boot registers it); a general
`@NAME` fallback is a second way next to D11 6.4's file-service rule (mount registers the path, one alias); a ZenixFS
root and updates (zup's check, `zenix-zfs.adf`, test-only kernel said, owner question 5); `root-mount.img` has exactly
zero blocks to spare for zfsd (RAM disks per image); the size table (acd counted, 7 KB of assert strings, 10.4 KB of
Ethernet code on SLIP machines accepted, netd's own 10-15 KB, unlinked figures); ready-bit ownership (the same as the
first review's blocker); the demo floppy (7.4); the Sessions rule (all steps main's); owner decisions not listed (serial
port, licence notices, GLib, ZenixFS root, the order). *Minor* - the frozen items named one by one (op numbers, first
sizes, message ranges, `S_IFSOCK`, the device names, the file formats, hwinfo now user-visible), D11's capacity
(NSVC 12: disk, fs, 7 mounts, net, ping: one spare), netd's call graph (6.5), `ifup -a` not waiting, the second driver
model and the supervision rule said in DESIGN, the `/net` filesystem in "not taken", stale wording.

Revision 2 changes no part of the overall design (one lwIP server on demand, user-mode driver processes as its
clients, SLIP inside netd, sockets as a kernel file kind blocking in the kernel, zfsd off the kernel image); both
reviewers judged that shape sound.

Last checked against the code: 2026-10-26.

## Owner decisions (2026-10-27)

The owner answered section 11 with "Yes" to every recommendation: (1) the stack and SLIP first, the CM4 Ethernet after;
(2) the A2065 driver early; (3) SLIP may take the stock A1200's only serial port; (4) `zenix-slirp` an ordinary Linux
program in the repository; (5) a ZenixFS root is test-only until block 4; (6) licence notices travel with every image
and bundle; (7) lwIP as the TCP/IP code.
## 14. As built

### Step 0: `msgping` and `msgpingd` (branch n023, 2026-10-27)

`ping` and `pingd` (the message test client and server) are now `msgping` and `msgpingd`: `user/msgping.c` and
`user/msgpingd.c`, `UAPPS`, their pages, `commands.md`, `source-tree.md`, `limits.md`'s raw-message list, the svctab
example and every test script (svc-test, cmds2-test, the mount test's file services). The port name `ping` and the
service name in D11's log lines (`svc: ping: ...`) stay; `msgping` prints `msgping: ...`. The older notes (D11, D13,
the audits) keep the old names as history. `MOUNTAPPS` never held them: the mount test only registers
`/bin/msgpingd` under ten names (the service table's capacity) and never starts it, so its list is unchanged. Tests:
svc-test and cmds2-test.

### Step 1: zfsd from the disk (branch zfout, 2026-10-27)

What section 3.3 asked for, with three local corrections where the note did not fit the code:

1. **`mount` registers `/sbin/zfsd DEVICE -id ID`, without the port word.** The note's `/sbin/zfsd DEVICE fs.DEVICE
   -id ID` is 82 bytes for `ide0p2` (the volume id alone is 50), over the frozen 80-byte program text (`SVC_TEXT`,
   at most 79 characters). zfsd now takes its port as optional (`zfsd DEVICE [PORT] [-root] [-id ID]`; left out, or a
   word starting with `-` in its place: `fs.DEVICE`, named as `mount` names it), so the old form still works.
2. **The alias** (`kernel/svc.c`): `@zfsd` with no built-in zfsd starts the file `/sbin/zfsd` with the same
   arguments; **never for an essential entry** (`disk`, `fs`): the root's file could only be read through the server
   being started (the loader would wait on its own port for ever). The shipped kernel on a ZenixFS root therefore
   says "svc: this kernel has no ZenixFS server built in: a ZenixFS root needs the test kernel kernel-zfs (DESIGN
   D19)" and stops with D11's panic "cannot start an essential server" (`svc-test` zs).
3. **The update refusal is zenixboot's, not zup's.** zenixboot installs the kernel before Zenix runs; zup runs
   *under* the new kernel, so a new kernel that cannot serve the root never reaches zup. So: the version record
   grows at its end (`ZVER`, number, string, then `ZSRV` and a word of the roots the kernel serves: 1 ext2, 2 ZenixFS;
   the shipped kernel says 1, kernel-zfs 3), and zenixboot refuses a bundle (renamed `.bad`, the reason said, the
   installed kernel starts) whose kernel says it has no ZenixFS server **when the installed kernel says it has one**.
   The installed kernel stands for the root's type, which zenixboot cannot read (an ext2 or ZenixFS partition behind
   AmigaOS's devices); a machine runs kernel-zfs only for a ZenixFS root. A record without `ZSRV` (every kernel before
   step 1) is no evidence and refuses nothing: no real machine runs a ZenixFS root (3.3 item 4), and on such a machine
   the kernel's own stop above plus D15's evidence rule ("crashed - start the previous version?", default previous,
   after the warm reset) is the way back. zenixboot 0.2b3.

The rest as the note says:
- `kernel/embed.S` carries zfsd only with `-DKERNEL_ZFSD`. **Kernel image 176,144 -> 139,644 bytes** (the RAM-floppy
  count: 3,056 -> 39,556 bytes left), *measured* 2026-10-27; zfsd.zx 36,688 -> 36,764 bytes (the optional port).
- **Free memory with one login** (`free`, FS-UAE, the matrix machines, *measured* 2026-10-27): 2 MB chip-only
  1329 -> 1365 KB, 1 MB chip-only 392 -> 428 KB (36 KB each, as the note estimated; the boot pool 1807 -> 1843 and
  783 -> 819 KB).
- `/sbin/zfsd` (`SBINAPPS` in the Makefile; not a command, no page) is on `zenix.hdf`, `root-card.img`,
  `zenix-zroot.hdf` and the mount tests' image, and in the base package (`tools/mkzup.py`), so an update brings it
  with the kernel that needs it. Not on the RAM floppy's image (no `mount` there). Between the boot half and the root
  half (a root half that failed: legal, D15) a ZenixFS mount does not start ("svc: fs.DEVICE: cannot start
  /sbin/zfsd (error -2)" in dmesg) until `/sbin/zfsd` is installed.
- **RAM disks per image:** `zenix-ram.adf` keeps 704 KB; `zenix-mount.adf` 728 KB (`RAMDISK_MOUNT_K`): 23 blocks and
  25 inodes free with `/sbin/zfsd` on it (37 blocks before, without it), and a kernel limit of 154,624 bytes
  (14,980 left), printed by every build.
- **`kernel-zfs.bin`** (176,420 bytes) on the kernel-only floppy `zenix-zfs.adf`: `embed.S`, `svc.c` and `syscall.c`
  built again with `-DKERNEL_ZFSD` (`zfs-*.o`). `fsuae/zroot.fs-uae` and svc-test's `zo` boot it; `zroot-test`,
  `svc-test` z and zo say in their scripts that they test that kernel, not the shipped one.
- **Tests:** `zenixfstest` (mount starts `/sbin/zfsd`; zfsd killed, started again from the file, the volume reads
  back; `@zfsd` registered the old way starts the file and serves), `mounttest` unchanged, `svc-test` z (kernel-zfs),
  zs (the shipped kernel on a ZenixFS root: the message and the panic), zo; `zroot-test` with kernel-zfs; `update-test`
  zfsroot (a machine with kernel-zfs: a bundle with the shipped kernel refused and renamed `.bad`, the installed kernel
  starts; a bundle with a kernel-zfs of the new version installed); `zup-host-test` (base now holds `/sbin/zfsd`);
  the matrix (free memory above); the full `make check`.
- **What changes for a ZenixFS root** (owner question 5 still open): it is test-only. The shipped kernel no longer
  boots one; only `kernel-zfs.bin` does. A ZenixFS **mount** on an ext2 root works as before, its server read from
  `/sbin/zfsd`. When block 4 makes a ZenixFS root real, its decision picks the bundle variant or a boot module (3.3
  item 4).
- **Not changed, said here:** CLAUDE.md's frozen list names "the built-in `@name` programs"; `@zfsd` stays a valid
  name (the alias), so the frozen meaning holds; the wording there ("`@zfsd` is an alias to `/sbin/zfsd` since D19")
  is for the main session to add, as that file is its own.
- **A limit found:** a file service that lives *on* a ZenixFS mount, started while both that mount's and the root's
  servers are stopped, needs three nested loads and fails once (D11's two-level limit, logged; the next call works).
  Noted in d11-service-manager.md 5.1.

### Step 2: random numbers (branch n023, 2026-10-27)

Section 9.3 as written: `kernel/random.c` (BLAKE2s pool, ChaCha20 with fast key erasure, `/dev/urandom` never waits,
`/dev/random` waits for 256 credited bits and then polls readable), the sources, the seed file in boot and `reboot`,
and the refusal in `mkroot.sh`, `mkroot-zenixfs.sh` and `mkzup.py`. Local choices the note left open:

1. **How a seed is credited:** root's write to `/dev/random` credits 8 bits a byte; anyone's write to either device
   mixes without credit (Linux's write never credits; its RNDADDENTROPY ioctl needs a call Zenix does not have, and a
   new system call for it was not worth a number). boot does the rule (`user/randseed.h`, host-tested).
2. **The seed file's format:** 40 bytes, `ZRS1`, a flags byte (bit 0: from a seeded pool), three zero bytes, 32 bytes
   of seed (256 bits, the seeding threshold). Root's, mode 600. Frozen once shipped, like the `/etc` formats.
3. **When the file is written:** at every start (the rule of 9.3), **and once more when the pool is first seeded**
   (boot waits for init with `evwait` on `/dev/random`, a minute at a time: a machine switched off without `reboot`
   otherwise kept an uncreditable seed for ever; meanwhile `ps` shows boot as "sleep", a timed event wait, where it
   showed "wait"; signal-test counted every line with "sleep" in it and now counts processes named sleep). `reboot`
   writes it before the reset, without a sync of its own (`SYS_reboot` syncs, each server at most 5 s; a plain sync
   waited for ever on a server that never answers: svc-test h). **Not at the reset warning:**
   the kernel syncs there inside init's wait and writes no files; the start and the first seeding cover it.
4. **The sources:** a key press or release 1 bit (`kbd_isr`, every raw code below 0x78); "bytes from the far end of
   the serial line, per SLIP frame": every received `0xC0` (SLIP's END) half a bit, in the receive interrupt, so it
   holds before netd exists and costs nothing per ordinary byte; the floppy's index pulse 1 bit, seen as CIA-B's FLG
   flag in the driver's polled wait for a track's DMA (the note's "while the motor runs": the driver polls, so only
   while it reads or writes a track; CIA-B's interrupts stay off); the RNG200 32 bits a word (stage0 finds
   `brcm,bcm2711-rng200` under `/soc`, `hwinfo.rng200` from the first reserved word; the kernel switches it on and takes
   up to 8 words a turn while unseeded, polled once a second from the tick; a repeated word ends its credit; *read*,
   not run). Mixed at 0: disk and SD completions (`blk_read`/`blk_write`), mouse counter changes (the tick), the boot's
   clocks and boot information, one word of every 4 KB of chip RAM below 2 MB. Network frames: with netd (step 4).
5. **Limits:** a read or write moves at most 4,096 bytes a call; the sample ring holds 32.
6. **`O_NONBLOCK`** (step 3) applies to `/dev/random` too (-EAGAIN while unseeded).

Sizes (*measured* 2026-10-27): `random.o` 3,655 bytes of code, 268 of data (the note's estimate 1.7-2 KB was for
the hashes alone: the devices, the ring, the RNG200 and the log lines are the rest); boot.zx 5,900 -> 6,600 bytes
(inside the kernel image). **Kernel image 139,644 -> 145,588 bytes** (RAM floppy: 33,612 left; the mount tests'
floppy 9,036). Tests: `tools/random-host-test.sh` (BLAKE2s: RFC 7693 appendix B and the appendix E self-test;
ChaCha20: RFC 8439 2.3.2 and A.1 #1, #2; the crediting and seed rules; mkroot and mkzup refuse a seed);
`tools/random-test.sh` in FS-UAE (f: a fresh image's seed file, mode 600, flag 0; `/dev/random` waits; keys typed on
the emulated keyboard seed it and the reader gets its bytes; boot rewrites the file with flag 1; `reboot` writes it.
s: the next boot credits it, `/dev/random` answers at once, the file is rewritten, `/dev/urandom` differs from the
first boot's. r: the RAM floppy with a seed (flag 1) on its RAM disk never credits it); `stage0-test` (the RNG200's
address from the CM4's device tree).

### Step 3: the kernel's socket layer (branch n023, 2026-10-27)

Sections 4.1-4.3 with the test server: `kernel/sock.c` (ksocks, `SYS_sock`, `SYS_netsrv`, the blocking loop, the
epoch and `sock_server_gone`, `NET_CLOSE`), `kernel/netproto.h` (the frozen protocol), `FT_SOCK`, `O_NONBLOCK` 0x800,
`S_IFSOCK` 0140000, the network's error numbers (`kernel/errno.h`, their texts in libz), `net` a reserved port name,
`user/socktestd.c` in netd's place. Local choices the note left open:

1. **`struct usockreq` is 32 bytes as the note says, with its nine fields:** `level` and `option` are 16 bits each
   (`buf len flags addr alen level:option val vlen`). Every length is behind a pointer (`alen`, `vlen`, BSD's
   `socklen_t *`): bind, connect and sendto read it, accept, recvfrom, getsockname and getsockopt also write it, so
   one field serves both and the kernel's direction table covers it. Which fields each op uses is listed in
   netproto.h (`SOCKET`: level the domain, flags the type and `SOCK_NONBLOCK`, option the protocol).
2. **"Its first size, or the caller's if bigger":** the caller's request size rides in `SYS_sock`'s op word
   (`op | size << 16`, 0 = 32, below 32 -EINVAL), as every other frozen struct passes its size.
3. **The messages:** `NET_*` are the `SOCK_*` op numbers (1-18), `NET_CLOSE` 19. A request carries the socket's
   name (index | generation << 8) in `a[0]` and a `struct netreq` (the copied request, the reserved new socket's name,
   the clamped lengths, the caller's pid, uid, gid) in `a[1]`; netd writes the socket's new bits into `a[2]` (ACCEPT:
   the new socket's in the upper 16) and its epoch into `a[3]`, then `NETSRV_REPLY`. The kernel applies the bits to
   the socket it recorded for the caller's call (a per-process note), never to a name read back from the message.
4. **"Answered" versus "the server is gone":** `NETSRV_REPLY` marks the message (`UMSG_NETREPLY`, internal); a call
   that comes back unmarked (the server died in it, could not start, was given up, or no `net` is registered) is
   -ENETDOWN, so a peer's -ECONNRESET passes through unchanged. netd must therefore answer every kernel request with
   `NETSRV_REPLY` (a plain `SYS_reply` reads as no answer); a non-kernel message it answers -EACCES with either.
5. **No reference count in the ksock:** a socket is one `struct file`, whose count already covers `dup`, `dup2` and
   spawn's standard files; its last close is `file_close`'s (`sock_release`).
6. **The kernel keeps `SO_RCVTIMEO`/`SO_SNDTIMEO`** (set and get, `struct timeval` in ticks) and `FIONBIO` (the file's
   `O_NONBLOCK`) to itself; `FIONREAD`, `SO_ERROR` and the rest go to netd. A connect whose `SO_SNDTIMEO` runs out
   returns -EINPROGRESS and goes on (Linux).
7. **SIGPIPE:** a send that netd answers -EPIPE, or on a dead socket (the pseudo-code's "send: and SIGPIPE rules"),
   sends SIGPIPE unless `MSG_NOSIGNAL`, then -EPIPE or -ENETDOWN; after progress the count comes back and no signal.
8. **Checks in Linux's order:** the descriptor (-EBADF, -ENOTSOCK) before the pointers (-EFAULT); a datagram send
   over 64 KB is -EMSGSIZE (only a stream send loops). `MSG_WAITALL` is netd's to honour, not the kernel's loop's.
9. **`O_NONBLOCK`** is a file flag set by `SOCK_NONBLOCK`, `FIONBIO`, accept's flags or `open` (it makes
   `/dev/random` answer -EAGAIN too); libz's `fcntl` comes with libz's socket calls (step 4), as do the BSD functions:
   the step-3 programs call `sys_sock` (usys.h) directly.
10. **`SYS_drv` 92** is claimed (CLAUDE.md) but not in kernel.h until step 7, so the call counts stay true.
11. **A test client:** besides `socktestd` (the note's test server), `socktest` holds the checks (as `sigtest` does
    D16's), both with pages. `socktestd`'s network is a loopback of stream sockets on port numbers (a 512-byte buffer
    a socket, so a big send waits: partial sends), slow connects on ports 8000 and up, and commands for its death, an
    inflated count and its request count.

Sizes (*measured* 2026-10-27): `sock.o` 3,987 bytes of code and 1,028 of data (the note's estimate 3-4 KB plus 0.3); the
kernel image 145,588 -> 150,244 bytes (RAM floppy: 28,956 left; the mount tests' floppy 4,380). **Free memory with
one login** after steps 2 and 3 (`free`, FS-UAE matrix, *measured* 2026-10-27): 2 MB chip-only 1365 -> 1351 KB, 1 MB
chip-only 428 -> 414 KB (the kernel's 10.6 KB of code, its 1.3 KB of new data and boot's 0.7 KB). The mount tests'
floppy has 4,380 bytes of kernel headroom left: step 4 must count on that (its RAM disk can shrink by a few KB). Tests: `tools/sock-host-test.sh` (the 4.3 model: a push before the call,
between the check and the masked test, during the server's turn and while asleep, never lost; a stale bit costs one
call; a handler's -EINTR and the restart rule; the deadline; the server's end waking a waiter, its death mid-request
-ENETDOWN, a stale epoch's socket dropped, NET_CLOSE as a no-start call, a partial send's count, accept's
reservations, a restarted connect sending no second CONNECT, poll without a message, SYS_netsrv's checks);
`tools/sock-test.sh` in FS-UAE (socktest's checks against socktestd, ^C on a blocked recv, the log of the server's
death and restart in epoch 2, e2fsck). **Checked against the host-only parts' `net/netd/netd.h` and their binding
choices** (merged from main): sockets named by (index, generation) as `a[0]`'s low 8 and next 16 bits; ACCEPT's
reserved socket is `netreq.newsock`; the timeouts are the kernel's (their choice 6: netd answers -ENOPROTOOPT if one
reaches it, and none does); ready bits only from socket state, truthful in every reply (their choice 10, rule 1);
`SOCK_RAW` -EPROTONOSUPPORT is netd's answer. Step 4's message loop adapts the wire fields to netd's calls (FIONREAD's
`int` to `netd_ioctl`'s `long`, RESOLVE's name in `buf`). The 4.3 model their section leaves to step 3 is
`tools/sock-host-test.sh`.

Last checked against the code: 2026-10-27.

### Step 4: netd with lwIP over the loopback (branch n45, 2026-10-27)

netd is `/sbin/netd`: lwIP and its port (`net/lwip`, `net/port`), the core of the host-only parts (`net/netd`, unchanged:
the same files the host tests build) and **`net/netd/main.c`**, netd's main program on Zenix (the OS
layer the core asks for: the clocks, the serial port, the log; and the message loop). boot registers `net` as
`/sbin/netd` at nice 0 when the file exists (before `/etc/svctab`, which may still name another program for it);
the kernel starts it at the first socket call. libz has the BSD calls (`user/socket.h`), `netstat` shows netd's table,
`nettest` holds the checks (as `socktest` does step 3's). Local choices:

1. **The message loop:** each turn runs `netd_turn` (lwIP's timers, the loopback, the lines), pushes the changed bits
   (`NETSRV_READY`), waits in one `evwait` on the port (`POLLPORT`), the serial lines and `/dev/random` (until the pool
   is seeded) for the core's next timer (milliseconds rounded up to ticks), then answers at most **8 requests** before
   the next turn (the note's "a fixed amount of work per turn"). Every kernel request is answered with `NETSRV_REPLY`
   (its result, the socket's bits in `a[2]`, ACCEPT's new socket's in the upper 16, the epoch in `a[3]`); a message
   without `UMSG_KERNEL` gets -EACCES. The wire fields are adapted to the core's calls: FIONREAD's `long` to the
   program's `int`; addresses and values in place, the true lengths written back through `alen`/`vlen`.
2. **`SOCK_RESOLVE` turns the program's socket into the lookup** (its name in `buf`, `len` bytes, no NUL needed): netd
   gives up the socket it knew under that (index, generation) and starts the lookup under the same name, which becomes
   readable when the answer is in. libz's `getaddrinfo` does that on a `SOCK_DGRAM` socket of its own, then `recv` (the
   interruptible wait), then `close`. Services are numbers or a built-in table of twelve names (no `/etc/services`).
3. **`SOCK_NETSTAT`** fills `struct unetstat` rows (first size `UNETSTAT_FIRST` 40, frozen: proto, TCP state with
   BSD's numbers, local and remote address and port, receive and send queues, the owner's uid; the row size the
   caller knows in `flags`, 0 = 40), on any socket of the caller's that netd knows. So `netstat` opens a socket, which
   starts netd if it was not running: said in its page.
4. **`fcntl`:** `F_SETFL` sets or clears `O_NONBLOCK` through `FIONBIO`; `F_GETFL` needs the flag back, which no call
   gave: **`FIONBIO` with no value pointer now answers the file's `O_NONBLOCK`** (1 or 0; before, -EFAULT: a new
   meaning only where every older call failed, the frozen rule). On other files `F_GETFL` says `O_RDWR` and `F_SETFL`
   refuses `O_NONBLOCK` (-EINVAL: pipes later, 4.1); `F_GETFD`/`F_SETFD` answer 0 (no exec).
5. **More Linux constants in `netproto.h`** (appended, frozen as the rest): `AF_UNSPEC`, `IPPROTO_IP/ICMP/TCP/UDP`,
   `SO_REUSEADDR/TYPE/BROADCAST/SNDBUF/RCVBUF/KEEPALIVE`, `TCP_NODELAY`, `SHUT_*`.
6. **The machine profiles** (binding choice 4 left the boundary to this step): **large when at least 2 MB of fast RAM
   is free at netd's start, else small** (chip RAM alone is always small). *Measured* in FS-UAE (`ps` after the tests):
   netd takes 105 KB with the small profile on the 2 MB chip-only machine and 169 KB with the large one on 4 MB of fast
   RAM (twelve connections open; its buffers grow on demand up to the profile's cap of 24 or 200 KB). At the boundary
   the large profile costs at most about 280 KB of 2 MB; the small one keeps 1,209 KB free with one login on the stock
   A1200 while netd serves. The small profile allows 8 TCP ends: 4 loopback connections (each takes two), the next
   socket refused at once with -ENOBUFS (`nettest many`).
7. **The assembler (2.2), measured:** `net/port/m68k.S` has the checksum (lwIP's `LWIP_CHKSUM`, 32 bytes a turn with
   `addx.l`) and the copy (lwIP's `MEMCPY`: the destination aligned, then 32 bytes a turn; the source read whole even
   when misaligned, which the 68020 does in hardware and libz's `memcpy` cannot assume: it copies such a buffer byte by
   byte). `nettest asm` checks both against C (1,206 checksum and 3,200 copy cases, every alignment) and times them:
   in FS-UAE's 68020 (an emulator's figure, not a real Amiga's speed), the checksum of 1,460-byte segments 7.7 times
   faster than a C loop at -Os (86,276 against 666,096 E-clock counts), the copy 2.9 times faster than libz's on a
   misaligned buffer (22,676 against 65,371) and 1.7 times on an aligned one (16,290 against 28,044). It pays, so both
   are in. SLIP's byte loop (`slipif.c`, `iface.c`) is built at -O2, the rest of netd at -Os. The host build keeps
   lwIP's C (`NETD_HOST`).
8. **The clocks:** `sys_now` is the tick (10 ms); RFC 6528's 4 µs clock is the E-clock shifted by 2 (5.6 µs PAL).
9. **Keys:** 48 bytes from `/dev/urandom` before `lwip_init`; `/dev/random` polled until readable, then fresh keys
   from it; every 5 minutes fresh keys from `/dev/urandom`.
10. **Configuration at the start:** `/etc/hosts` (now shipped: `127.0.0.1 localhost`, a base config file) and
   `/etc/resolv.conf`, at most 16 KB each.
11. **Licence notices** (owner decision 6): `/usr/share/licenses/lwIP` and `miniz` on every image that carries netd or
   gzip/zup (`tools/mkroot.sh`, `mkroot-zenixfs.sh`), and in the base package (`tools/mkzup.py`). The RAM floppy and
   the mount tests' floppy carry neither program, so neither notice (the mount floppy had 4 free blocks).
12. **`EAGAIN`'s text** in libz is Linux's, "resource temporarily unavailable" (it said "too many processes", spawn's
   old meaning; sockets made it common).

Sizes (*measured* 2026-10-27, linked): **netd 66,482 bytes of code, 24 of data, 13,056 of BSS**, the file 74,112 bytes
(the note's estimate 65-70 KB); of the code, lwIP with the port and netd's core are 59,498 linked on their own
(`tools/host/net-test.sh size`), libz and the main program the rest. Stack 8 KB (the deepest frames, `-fstack-usage`:
libz's formatting 560 bytes, the parsers 496, the main loop 304). The kernel grew by 112 bytes (boot's registration
and the `FIONBIO` question): **150,356 bytes, 28,844 left on the RAM floppy, 4,268 on the mount tests' floppy** (no
RAM-disk change needed). A machine that never opens a socket never loads netd: its free memory changes only by those
112 bytes. Tests: `tools/netd-test.sh` (FS-UAE; s: the stock A1200, the small profile: `nettest tcp udp poll nonblock
names many perm asm`, `netstat -a`/`-tl`/plain against a listener, a connection and a UDP socket, `nettest down`: netd
killed while a child waits in `recv`: `ENETDOWN` after 1 tick, the next socket starts netd in epoch 2, which serves; ^C
on a waiting `accept`; the log lines; e2fsck. l: 4 MB of fast RAM, the large profile, twelve connections);
`tools/host/net-test.sh` (unchanged, passing; its size part assembles `m68k.S`).

### Step 5: SLIP, the interfaces and their commands (branch n45, 2026-10-27)

SLIP on `/dev/ttyS0` through lwIP's `slipif` in netd (the host-only parts' core: `iface.c`), `/etc/network/interfaces`
read by netd, `ifconfig`/`ifup`/`ifdown` (one program, `user/ifconfig.c`; the other names are links, as `more` is
`less`), `route`, `ping`, `init`'s `ifup -a`, `/dev/ttyS0`'s truthful POLLOUT and writes that never wait, and the
FS-UAE test through `zenix-slirp`. The owner's how-to: `docs/network-serial.md`. Local choices:

1. **The wire formats** (frozen, `kernel/netproto.h`; each asked on any socket of the caller's that netd knows, as
   `SOCK_NETSTAT`): `SOCK_IFLIST` fills `struct unetif` rows (first size `UNETIF_FIRST` 64: name, address, netmask,
   gateway, MTU, flags `NETIF_UP/LINK/LOOPBACK/DEFAULT`, six counters, the two name servers; the row size in
   `flags`); `SOCK_IFSET` takes a `struct unetifreq` (first size 32: name, `NETIFREQ_UP` from the file, `DOWN`,
   `ADDR` with an address and netmask; the name "" means every `auto` interface for UP and every one but the loopback
   for DOWN); `SOCK_ROUTE` a `struct uroute` (first size 32: `ROUTE_GET` writes the default gateway and its
   interface back, `ROUTE_ADD`, `ROUTE_DEL`). IFSET and the route changes are root's (-EPERM, `nettest perm`).
2. **When interfaces come up:** netd brings up the `auto` ones at its start, after its port (6.3), so a netd started
   again after a crash brings SLIP back by itself; `init` starts `ifup -a` without waiting when the file has an `auto`
   line (it reads the file 128 bytes at a time: no BSS for init, which lives for ever), which starts netd at boot.
   `ifup` is the reload (6.1): netd reads `/etc/hosts`, `/etc/resolv.conf` and `/etc/network/interfaces` again; for
   `ifup -a` an interface already up is no error. The file is used only if root owns it and nobody else may write it
   (otherwise -EACCES, said by `ifup`). netd's core got two small functions for the commands: `netd_route_get` (the
   default route and its interface) and `netd_if_addr` (`ifconfig IFACE ADDRESS/PREFIX`).
3. **"Given up" (4.3 rule 12):** root's `ifup` or `ifconfig IFACE up` whose socket call gets -ENETDOWN registers `net`
   as `/sbin/netd` again (when the file exists) and says so, then tries once more.
4. **`ping`** times the round trip with the E-clock counted at the PAL rate (NTSC reads under 1% long: said in its
   page); `-i` below 0.2 s is root's; no TTL (an ICMP socket gives none). **`route`** shows the default route and one
   route per interface's net, and changes only the default.
5. **`/dev/ttyS0`** (kernel, +180 bytes): `POLLOUT` when 1 KB of the 4 KB send ring is free; a poll that found less
   leaves a flag, and the drain in the idle loop that makes the room asks kwork, whose `serial_work` wakes the waiter.
   With `O_NONBLOCK` a write takes what fits (-EAGAIN for nothing) and a read with nothing received answers -EAGAIN.
   Every older program keeps its meaning (they never set `O_NONBLOCK`; only `POLLOUT` became truthful, as 5.3 says).
   `tools/serial-test.sh` checks both on the host model of Paula. SLIP runs at 115,200 only: another speed in the
   `driver slip` line is -EINVAL (Zenix's port has one speed, D12).
6. **netd's heap on Zenix** (found measuring step 5): with SLIP up netd held 176 KB on the stock A1200, because
   libz's `malloc` rounds each block up to a power of two and grows in chunks of 4, 16 and 64 KB (84 KB for a 24 KB
   heap). netd now takes its buffers from regions of its own (first fit, neighbours joined on free; each a quarter of
   the profile's limit, at least 8 KB, taken when needed, at most 5), and reads its configuration files into regions
   given back at once: **115 KB with SLIP up** (99 KB on the loopback). `net/port/port.c`'s counted heap calls
   `netd_heap_alloc` on Zenix and `malloc` on the PC (a two-line change to the host-only parts' port).
7. **Choice 12 of the host-only parts, the FS-UAE SLIP test** (`tools/slip-test.sh`): `zenix-slirp --pty` owns
   FS-UAE's serial port (`tools/run-test.sh` with `ZENIX_NOTAP=1` starts no serial-tap); nothing is typed, so the
   image carries `/etc/network/interfaces`, `/etc/resolv.conf` and an `/etc/profile` that runs `/tests/slip.sh` once
   at the autologin; the results are files read from the image afterwards (debugfs). **The console's bytes reach the
   test through `zenix-slirp --log`**: after `ifdown sl0` the console's copy is on the wire again and the profile
   prints a marker, which the test waits for in that log before it stops the emulator. The PC's servers (TCP echo,
   UDP echo, a source of 100 KB, a DNS server for `amiga.zenix.test`) and zenix-slirp run in a user and network
   namespace (`tools/slip-host.py`, as `tools/host/net-test.sh` does): no root, nothing leaves the PC.
8. **zenix-slirp paces a pty to the line's speed** (`-b`, default 115,200): found by the test: a pty has no baud rate,
   FS-UAE took the PC's bytes far faster than a cable, and Zenix's 1 KB receive ring lost about a fifth of them (TCP
   at 0.7-1 KB/s, retransmitting). Paced, the test moves 100 KB at 7-8.4 KB/s (*in the emulator*). A real serial
   port paces itself; the host tests' socketpair (`--fd`) is not paced.
9. **A step 4 bug found here** (commit 4b5177b): netd polled its port with `POLLPORT` alone; `evwait` masks the
   answer with the events asked, so `POLLIN` never came back and every request waited for netd's timer (up to 1 s:
   ping 2 s on the loopback, the 100 KB loopback echo 100 s). Now `POLLPORT | POLLIN`: 12.5 ms on the loopback, the
   echo 5 s.
10. **DNS through 10.0.2.3 works already** (netd's core has lwIP's resolver; `getaddrinfo` asks netd): the test looks
   up a name from the PC's test DNS server through slirp's 10.0.2.3, and an unknown name gives `EAI_NONAME`. Step 6
   keeps `host`, DHCP and the DNS tests of its own.
11. **Bytes the receive ring still loses:** in one run 90 of 122,634 (0.07%), while `nettest` compared 100 KB byte by
   byte at netd's level (netd waited for its time slice of 50 ms, and its turn took the rest of the ring's 89 ms).
   TCP sends them again; the test allows at most 1% and reports the count. The real A1200's numbers are 8.3's (the
   ring is 1 KB: a larger one costs every machine its kernel memory, not decided here).

Sizes (*measured* 2026-10-27): the kernel 150,356 -> 150,536 bytes (28,664 left on the RAM floppy, 4,088 on the mount
tests' floppy: no RAM-disk change needed); netd 72,392 bytes of code (the files 80,900); `ifconfig` 13,196 bytes,
`route` 9,668, `ping` 14,160; `init` 9,092 -> 9,928 bytes (`ifup -a`). **Free memory with one login, the network
unused** (the matrix, `free`, main at ab941b6 against this branch): 2 MB chip-only 1,351 -> 1,350 KB, 1 MB chip-only
414 -> 413 KB (the kernel's 292 bytes and init's 836). **Free memory with one login and SLIP up** (`free` and `ps` in FS-UAE): 2 MB chip-only
1,161 KB free, netd 115 KB; 1 MB chip-only 232 KB free, netd 107 KB (the 1 MB machine runs netd on SLIP with room
to spare for one session). Tests: `tools/slip-test.sh` (s: the stock A1200: sl0 up at boot, `ifconfig`, `route`,
`ping -c 3 10.0.2.2`, `ping 127.0.0.1`, `nettest slip`: 10 KB TCP echo, 100 KB checked byte by byte, UDP echo, DNS
through 10.0.2.3, the counters, `ifdown sl0` giving the port back, the log, the ring's losses, e2fsck; m: 1 MB, ping
and the memory), `tools/serial-test.sh` (the port's POLLOUT and writes that never wait), `tools/netd-test.sh` (`nettest
perm`: a user may not change interfaces or routes), `tools/host/net-test.sh`, cu-test (the port's POLLOUT for `cu`).

### The tests steps 4 and 5 left open (branch n678, 2026-10-27)

8.2's loopback list had four items tested only against `socktestd` or not at all; now against the real netd:
- **`SA_RESTART`, partial sends, SIGPIPE** (`nettest restart partial sigpipe`, netd-test s): a recv and an accept an
  alarm interrupts go on with `SA_RESTART`, say EINTR without it or with `SO_RCVTIMEO`; a send that waited from its
  start goes on and sends all in order; a 30,000-byte send interrupted after progress returns the count (6,028 in the
  run), the rest follows, the reader sees each byte once; a closed peer: EOF, then (as Linux) "connection reset" once
  and "broken pipe", SIGPIPE ends a sender (141), an ignoring one gets EPIPE.
- **Five kills** (`nettest giveup`, then root's `ifup -a`): the kernel logs "svc: net: given up after 5 ends in 60 s",
  a socket gets ENETDOWN at once, `ifup` registers `/sbin/netd` again and says so, and `nettest tcp` passes after.
- **Noise on the SLIP line** (slip-test n): `zenix-slirp --damage 20` changes one byte in every 20th frame to the
  Amiga and sends three bytes of garbage between two ENDs before every 20th. **Found:** netd had no count of damaged
  frames (`ifconfig`'s "bad" was always 0: lwIP drops a bad sum silently). netd now checks each SLIP frame itself
  (`iface.c` `frame_ok`: the IPv4 header's sum and length, and the TCP, UDP or ICMP sum unless a fragment), counts a
  bad one and drops it; lwIP checks the sums again (at SLIP's 11.5 KB/s the second sum costs little, and lwIP stays
  unchanged). In the run: 13 bad frames counted, the 100 KB whole (3.9 KB/s in the emulator), netd never ended.
- **A SYN flood** (`nettest slipflood`, slip-test n): four listeners (backlog 255, the profile's cap 4) that never
  accept, while the PC opens 400 connections to them through `zenix-slirp`'s forwards (each a SYN to the Amiga,
  retransmitted by slirp). The small profile's 8 TCP connections held (7 got through the backlogs, plus the
  control connection), netd's memory stayed 108 KB, netd never ended; meanwhile 100 sleeps of 20 ms took 19 ms on
  average and 49 at most, `/bin/true` ran in 40 ms (emulator figures), and a connection to the PC worked after.

### Step 6: names, `host` (branch n678, 2026-10-27)

Most of step 6 was in already (step 5's choice 10: lwIP's resolver in netd, `getaddrinfo` through `SOCK_RESOLVE`). What
this step added, and its local choices:

1. **`host NAME...`** (`user/host.c`): BIND's output ("NAME has address A", "Host NAME not found"), exit 1 when a name
   was not found, 2 for wrong arguments. No reverse lookups (lwIP's resolver asks for A records only: an address as
   NAME is refused and said so), no record types, no server on the command line.
2. **"No such name" and "no answer" told apart** (binding choice 2 of the host-only parts changed here): lwIP calls
   back with NULL for both. Only its timer gives up on a server (`dns_tmr`, inside `sys_check_timeouts`), while a
   server's "no such name" arrives with a frame, so netd's turn marks the timers (`netd_in_timers`) and a lookup
   ending there is -ETIMEDOUT, otherwise -ENOENT; libz turns them into `EAI_AGAIN` and `EAI_NONAME`. No lwIP patch.
3. **`/etc/resolv.conf`'s `search`** (or `domain`) was parsed but never used: a name without a dot now gets that
   domain (glibc's `ndots` 1; only the first domain; a dotted name never gets one). `/etc/hosts` is asked with the
   name as given and with the domain, DNS with the domain only (`netd_set_search`).
4. **DHCP's code** went in with the driver interfaces (step 7: it needs an Ethernet interface) and is tested over the
   A2065 (step 8), as section 10 says.

Tests: `tools/host/net-test.sh` (the search domain; a name the test resolver never answers is -ETIMEDOUT, a missing
one -ENOENT); `tools/slip-test.sh` s through slirp's DNS: `host amiga.zenix.test`, `host amiga` (the search domain),
`host localhost` (`/etc/hosts`), `host nothing.zenix.test` ("not found"), `host slow.zenix.test` (the PC's test
server never answers: "no answer from the name servers"). Sizes: `host` 10,900 bytes; netd +200 bytes.

### Step 7: the driver protocol (branch n678, 2026-10-27)

Sections 2.3 and 2.4 as written: `kernel/drv.c` (`SYS_drv` 92), the blocks in `kernel/netproto.h`, netd's side in
`net/netd/iface.c` (the frames and buffers: core, host-built) and `net/netd/main.c` (the processes, the doorbell, the
supervision, the wire blocks), the drivers' shared code in `net/drv/drv.h` and the test driver `net/drv/fake.c`.
Local choices the note left open:

1. **The ops' arguments.** `DRV_CLAIM` takes the device's register base (GENET's base, a 64 KB window; or a Zorro
   board's address and size from the board list; a RAM board is refused) and answers a claim number 1-4.
   `DRV_DMAMEM (claim, bytes)`: one region a claim, at most 4 MB, cleared, owned by `0x80000100 + claim` (no pid, as
   `SEG_OWNER`). `DRV_QUIESCE (claim, struct udrvquiesce *)`: up to 8 steps, each a write or a read-modify-write of 1,
   2 or 4 bytes, aligned, inside the window (the interrupt check of 5.4 will add a test step to the same language).
   `DRV_CACHE` takes `DRVC_CLEAN` or `DRVC_INVAL` in the op word's second byte; the invalidate is always a clean and
   invalidate (Emu68's range opcode `0xF460 0x1800`, or `cpushl` every 16 bytes), so nothing is ever dropped unwritten;
   on a machine without Emu68 it does nothing (Zenix keeps a 68k's data cache off). `DRV_HWINFO` copies at most the
   caller's size of the table; `HWINFO_FIRST` 408 (the table today; it grows at its end from here: first from
   `reserved[]`, then after `board[]`).
2. **Who may call:** a child of the running `net` server (`svc_pid`); anyone else -EPERM, root too. When no `net`
   server runs every call is -ECONNRESET, so an orphaned driver learns netd is gone as the note says (a new netd is
   not its parent: -EPERM, which the drivers also take as "netd is gone").
3. **The blocks** (frozen, first sizes `UDRVATTACH_FIRST` 180, `UDRVXFER_FIRST` 872): each says its own size, which must
   equal the call's (`b`) and be at least the first size; the kernel checks the whole block is the driver's memory and
   sends `NETDRV_ATTACH` 64 / `NETDRV_XFER` 65 with a[0] the driver's pid, a[1] the block, a[2] its size, as a call that
   never starts a server (`SVC_NS_SKIP`); netd answers with `NETSRV_REPLY` (an unanswered call is -ECONNRESET).
   `DRV_MAXBUF` 32 receive buffers, `DRV_BATCH` 16 entries of each kind a transfer, frames of up to 4 pieces.
4. **Buffers:** 1,536 bytes each (64-byte lines, a 1,514-byte frame). Receive: zero copy into lwIP (a custom pbuf
   whose free gives the buffer back) while at most half the buffers are held by the stack, else a copy into a pool
   buffer and the buffer back at once; a DMA interface always copies (its buffers lie in the claim's region, which the
   kernel frees when the driver ends: no frame in the stack may point there). Send: up to 4 pieces of netd's pbufs,
   held (`pbuf_ref`) until their tag comes back, a longer chain copied into one; a DMA interface's frames are copied
   into its 16 send buffers in the region (after the receive buffers). netd queues at most 32 frames a driver (16 for
   DMA) and refuses the next (ERR_MEM, counted as dropped). Non-DMA buffers are a region of netd's own
   (`sys_memalloc`), outside lwIP's capped heap: the driver's fixed cost, freed when the driver is gone and the stack
   holds none.
5. **Supervision:** the restart waits 0 s after the first end, 0 after the second, 1, 2 and 4 s after the next (D11's
   "the start after the 2nd, 3rd and 4th end waits 1, 2 and 4 s"); the fifth end in 60 s gives the driver up until
   `ifup` (which starts it with fresh counts). The driver gets the interface's name as its first argument and the
   rest of the `driver` line after it. `ifdown` closes the doorbell (the driver stops its device and ends; the kernel
   stops it anyway). `ifup IFACE` waits (in `ifconfig`, not in netd) up to 10 s for the driver to attach.
6. **A wrong number from a driver** (a buffer it does not hold, a length over the buffer or under 14, a tag netd did not
   give) fails that entry only; it is counted in the interface's bad frames and the first four a driver start are
   logged ("netd: eth0: the driver gave a buffer that is not its own: 40: ignored").
7. **DHCP's code** (section 10's step 6): lwIP's client on `inet dhcp` stanzas of drivers' interfaces. lwIP's own
   handling of the name servers is off (`LWIP_DHCP_MAX_DNS_SERVERS` 0): option 6 is still asked for and reaches netd
   through `LWIP_HOOK_DHCP_PARSE_OPTION`, and netd uses DHCP's servers only when `/etc/resolv.conf` names none (6.1:
   nothing written behind the owner's back). A DHCP interface with its lease becomes the default route when none is.
   `struct unetif` grows (`hwaddr` where `reserved` was, then past the first size `lease`, `dhcp_server`, `rx_drop`;
   flags `NETIF_ETHER` 16, `NETIF_DHCP` 32) and `ifconfig` shows them.
8. **The fake driver** is built (`build/drv-fake.zx`) but on no image: `tools/drv-test.sh` writes it to `/sbin/net/fake`
   on its own image. Shipped drivers are the Makefile's `NETDRVS`, built as `drv-NAME` and installed as
   `/sbin/net/NAME` on every full root image and in the base package.

Sizes (*measured* 2026-10-27): `drv.o` 1,741 bytes of code and 624 of data (the note's estimate 0.8 KB: the checks of
the quiesce steps, the cache loop and the log lines are the rest); **the kernel 150,536 -> 152,472 bytes** (26,728 left
on the RAM floppy, 2,152 on the mount tests' floppy); netd +3.4 KB of code (the driver interfaces, DHCP's glue);
`fake` 10,848 bytes. Tests: `tools/drv-test.sh` (FS-UAE, the stock A1200 with FS-UAE's A2065 board so a claim has a
device: two fake drivers, one with DMA memory; ping through both; the DMA one's kernel checks: a second claim and a
second region -EBUSY, a quiesce step outside the window -EINVAL, a cache call outside the region -EFAULT, another's
claim -EPERM, a short hardware table -EINVAL, a transfer before the attach -ENOTCONN; `nettest drvperm`: `SYS_drv`
-EPERM for root's own program, a raw message to `net` shaped as a transfer -EACCES; the bad driver's three wrong
numbers logged and refused; a driver killed mid-ping, logged, started again; the DMA driver killed: "drv: claim of
0xe90000 (pid N) released (the driver ended): 2 quiesce steps run, its DMA memory freed"; five ends: given up, `ifup
eth0` brings it back; netd killed: the claims released ("the network server ended"), both drivers see the doorbell
close and end, `ifup -a` brings everything back); `tools/host/net-test.sh` (lwIP's DHCP tests with the request list
unchanged, netd's core built on the PC).

### Step 8: the A2065 driver, Ethernet in `make check` (branch n678, 2026-10-27)

`net/drv/a2065.c` (`/sbin/net/a2065`, `docs/man/a2065.txt`), polled, as 5.5 says. **FS-UAE gives the A2065 a working
network without root:** `network_card = a2065` alone uses FS-UAE's built-in slirp (its log says the backend and the
MAC); slirp's DHCP server gives 10.0.2.15/24, the gateway 10.0.2.2 and the name server 10.0.2.3, and 10.0.2.2 reaches
the PC's 127.0.0.1. No tap, no uaenet, no `slirp_*` option needed. Local choices:

1. **The board** (read from the Am7990 data sheet's layout and Linux's `a2065.c` for the board's addresses, byte order
   and MAC; the code is this file's own): the LANCE's RDP at +0x4000, RAP at +0x4002, 32 KB of RAM at +0x8000 that the
   LANCE sees at 0x8000; CSR3 `BSWP`; the MAC in the initialisation block with each byte pair swapped; 16 receive and
   4 send descriptors of 1,536-byte buffers in the board's RAM. MAC 00:80:10 + the board's serial number (FS-UAE's:
   00:80:10:32:33:34).
2. **No DMA region:** the LANCE writes only its own RAM, so the driver claims the board, registers a quiesce program
   (RAP 0, then `STOP` in CSR0), copies frames between the board's RAM and netd's buffers, and takes no `DRV_DMAMEM`.
3. **Sending:** netd's frames are copied into a free send descriptor as soon as there is one, and their tags go back at
   once (netd may reuse the memory); frames waiting for a descriptor stay netd's until then. Short frames padded to 60.
4. **Polling:** no sleep while frames moved in the last turn, then 1 tick, then 2 ticks when idle; netd's doorbell
   wakes it for a frame to send. A LANCE that stopped (CSR0 `STOP` or `MERR`) is started again and said so.
5. **Found by the test** (fixed here): DHCP's name servers never reached netd, because lwIP's `ip_current_netif()`
   is not set for a DHCP reply that arrives before the interface has an address (the hook's `netif` was NULL); netd
   now finds the interface by its DHCP client (`netif_dhcp_data`). netd logs the lease ("netd: eth0: DHCP gave
   10.0.2.15") and its loss. And step 7's driver interfaces moved out of netd's BSS (12 KB every machine paid, also
   on SLIP) into one region a driver, taken at its attach (a process has 8 regions; netd's heap takes up to 5).

Sizes (*measured* 2026-10-27): `a2065` 8,752 bytes (7,776 of code); netd 83,628 bytes of code, 14.8 KB of BSS (step 5:
72,392 and 13 KB; DHCP, ARP and Ethernet are now reached, and the driver interfaces); the kernel unchanged by this step.
Memory on the stock A1200 (2 MB chip RAM, FS-UAE, `free` with one login): Ethernet up with DHCP 1,120 KB free, netd
125 KB, the driver 21 KB. Tests: `tools/eth-test.sh` (FS-UAE in a user and network namespace with the PC's test servers
of `slip-host.py`, nothing leaves the PC: DHCP's address, route and name server, ping 10.0.2.2, 10 KB TCP echo, 100 KB
checked byte by byte at 41 KB/s *in the emulator*, UDP, `host` through DHCP's name server with no `/etc/resolv.conf`,
the driver killed: the LANCE stopped by the kernel, restarted, DHCP again, ping; `ifdown`: the driver stops the LANCE
and ends); `tools/drv-test.sh` and `tools/host/net-test.sh` again.

### As built: the host-only parts (side helper, merged 2026-10-27; reviewed by the main session)

lwIP 2.2.1 (STABLE-2_2_1_RELEASE) in `net/lwip/` (upstream files unchanged in the first commit; two local patches in
`net/lwip/patches`, applied in the tree with a test that each is still applied: `0001` a count limit for heap-backed
pools (9.4), `0002` random ephemeral ports, RFC 6056); Zenix's port in `net/port/` (lwipopts.h: IPv4, TCP, UDP, ICMP,
raw, DHCP, DNS with `LWIP_DNS_SECURE` 7, ARP, SLIP MTU 1006, loopback; no IPv6, ACD or `IP_FRAG`, reassembly on; a counted
heap with a byte cap via `MEM_CUSTOM_ALLOCATOR`; ChaCha20 with fast key erasure for `LWIP_RAND`; SipHash-2-4 ISNs,
RFC 6528); netd's core in `net/netd/` (`netd.h`), plain C without system calls, shared by netd and the host tests.
Measured m68k -Os: lwIP 47,656 bytes unlinked; with the port 50,044; netd's core + lwIP + port linked 59,388 of text.
Tests: `tools/host/net-test.sh` (in `check.sh host`): 46 netd-host checks over SLIP to zenix-slirp in `unshare -rnm`,
lwIP's own unit tests (83 run, 7 skipped for options Zenix turns off, 2 more for the patches), zenix-slirp's own test;
the libFuzzer harness `tools/host/lwip-fuzz/` (SLIP, Ethernet, DHCP; pools and heap must return to their start): its
first minute found two leaks in netd's interface shutdown (slipif reads END after ESC as data; `netif_remove` leaves
the loopback queue), both fixed; a 900 s run of 6.9 million inputs found nothing more.

Choices where the note was silent (binding for the main line unless changed here): (1) netd's core lives in
`net/netd/`; the wire protocol (NET_* numbers, `usockreq`, the epoch) is the main line's. (2) A name lookup answers up to
4 IPv4 addresses (4 bytes each, network order); a missing name and no answer are both -ENOENT. (3) Window, send buffer,
out-of-order and reassembly limits are variables set per machine profile; `TCP_SND_QUEUELEN` stays the constant 64.
(4) Profiles: large machines 32 TCP connections, 16 KB send buffer, a 200 KB heap; small machines a 24 KB heap with small
backlogs and UDP queues; which machine gets which profile is decided in step 4 with measurements (chip-only = small).
(5) `SOCK_RAW` gives -EPROTONOSUPPORT for now. (6) Socket timeouts are the kernel's; netd answers -ENOPROTOOPT if one
reaches it. (7) An ICMP socket's getsockname gives the echo id as the port, as Linux. (8) Config files: IPv6 lines in
`hosts`/`resolv.conf` and an ignored stanza's lines are skipped silently; `iface lo inet loopback` is reported as an
unknown method (loopback needs no stanza). (9) SLIP interfaces are static (no DHCP over SLIP); drivers other than slip
give -ENODEV until the driver protocol (step 7). (10) Ready bits come only from socket state: -EAGAIN always comes with
"not ready"; POLLOUT = one MSS or a third of the send buffer free. (11) With ACD off, lwIP sends a gratuitous ARP on
interface up and on a DHCP bind. (12) zenix-slirp writes the console bytes to `--log`; how the FS-UAE SLIP tests hand
them to serial-tap is step 5's choice. (13) netd's OS layer writes to `/dev/ttyS0` without waiting (at most the free
room), as 5.3. Not done here: the m68k assembler checksum/copy (step 4, needs m68k execution), the GENET model (follows
step 9's register layer), the 4.3 rules model (step 3).
