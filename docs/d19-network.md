# D19: the network (design note, revision 1)

Status: **design only, revision 1, not reviewed yet** (roadmap block 2, "Zenix reaches the world": the network, the
CM4's Ethernet, TCP/IP as a server, DHCP, DNS, `ping`; telnet, the crypto library, ssh, HTTPS, NFS, remote control and
`zpkg` come after it and only need what this note gives them). A deep core change (a new kernel file kind, new system
calls, a new message family, the way servers are loaded): two independent reviews before any code. The short version
is DESIGN.md D19. Facts about today's code are *read* from main at 7b5149d (2026-10-26); sizes marked *measured* were
compiled here, everything else is an estimate and says so.

## 1. What the owner gets, in plain words

- Zenix talks TCP/IP: `ping`, then `telnet` to BBSes, later ssh, wget, NFS, a remote control, `zpkg`.
- **Every Amiga can join**, also the stock A1200 with no expansion: through the serial cable to a PC (SLIP), where a
  small program on the PC (no root needed) gives the Amiga the PC's network and the internet. The PiStorm A1200 uses
  the CM4's own Ethernet port.
- **It costs nothing when it is not used.** The network server is a program on the disk that starts the first time a
  program opens a socket (D11), and the kernel image gets *smaller*, not bigger (section 3: the ZenixFS server leaves
  it).
- **Unix as people know it:** BSD sockets, `/etc/network/interfaces`, `/etc/hosts`, `/etc/resolv.conf`, `ifconfig`,
  `route`, `ping`, `netstat`, `host`.

## 2. Where the network lives

### 2.1 The options

| | A. In the kernel (BSD, Linux, AmigaOS-era stacks in a device) | B. One server process, drivers inside it | **C. One server, small driver processes below it (SANA-II-like)** |
|---|---|---|---|
| Kernel size | +60-90 KB: impossible (3,056 bytes left on the RAM floppy) | +4-5 KB (the socket layer only) | +4-5 KB (the same) |
| Cost when unused | always in memory | 0 (loaded on demand) | 0 |
| A bug in the stack | crashes the kernel | crashes the server; D11 starts it again | the same; a driver bug kills only the driver |
| Memory for a driver nobody has | always | every driver in every netd | only the drivers the machine has |
| Speed | best (no messages) | one message per socket call | the same, plus one message per batch of frames |
| Fits Zenix's principles (ROADMAP 1: "the network ... are servers behind named ports") | no | yes | yes |

**Decision: C.** One TCP/IP server, `netd` (port `net`, a D11 service, `/sbin/netd`), and one small driver process per
kind of hardware (`/sbin/net/genet`, later `a2065`, `ne2000`, ...), as AmigaOS has one stack and a SANA-II device per
card. Two parts of the network are not hardware drivers and live **inside netd**, as in Linux and BSD, where they are
line disciplines of the stack and not drivers: **SLIP** (later PPP), a framing on top of the kernel's existing serial
device `/dev/ttyS0` (D12), and **loopback** (`127.0.0.1`). The rule that separates them: *code that touches device
registers is a driver process; a link protocol over an existing Zenix device is part of netd.* So the stock A1200 on
SLIP runs exactly one process (netd) and no driver.

Why not B: every netd would carry every driver (GENET 6-8 KB, A2065, NE2000, CS8900A and plipbox 2-3 KB each,
*estimates*), 15-25 KB that a stock A1200 pays for hardware it cannot have ("no feature that costs every user for a
few"). Why not separate builds of netd per driver set: combinatorial, and an update (D15) would have to know which
build a machine runs.

### 2.2 Which TCP/IP code

| | Own stack | **lwIP 2.2** | uIP 1.0 | picoTCP |
|---|---|---|---|---|
| Licence | ours (GPL-2.0-only) | BSD-3-Clause: GPL-2 compatible (*read*, COPYING) | BSD-3: compatible | GPL-2.0 (TASS): compatible, but archived, no security fixes |
| Code (m68k, -Os) | 15-25 KB, *estimate* | **42.6 KB** IPv4+TCP+UDP+ICMP+raw+SLIP; **57.1 KB** with Ethernet/ARP, DHCP and DNS; 89.8 KB with IPv6 (*measured* below) | 5-10 KB | similar to lwIP |
| TCP quality | months of work to reach RFC 1122/5681/6298/6528, and every bug is remote | 20+ years in products; windows, retransmit timers, congestion control, keepalive, SYN backlog, IP reassembly | one segment in flight per connection: about one segment per round trip (a delayed-ACK peer makes it ~5 segments/s) | full |
| Fits a single-threaded server | yes | yes: `NO_SYS=1`, the raw (callback) API, no threads needed | yes | yes |
| Maintained | by us | yes (2.2.1, 2024-2025) | no (since ~2010) | no (archived) |

**Decision: lwIP 2.2.x**, `NO_SYS=1`, raw API only (lwIP's own `sockets`/`netconn` layers need threads and are not
used: Zenix's sockets are a kernel file kind, section 4), IPv4 first, IPv6 off (+33 KB; a later build option when a
use exists). Its files keep their BSD-3 headers (CLAUDE.md "Licence"); lwIP goes into `net/lwip/` as an unchanged
import with the version in a README line, our port layer (`net/port/`: `arch/cc.h`, `lwipopts.h`, `sys_now`, the
checksum in assembler) beside it. An own stack was the alternative with the best size, but a TCP exposed to the
internet is the worst place to start from zero, and lwIP's 57 KB are paid only while the network runs.

*Measured 2026-10-26* (lwIP STABLE-2_2_1_RELEASE, `m68k-elf-gcc -m68020-60 -Os -ffunction-sections -msoft-float`,
`size` of the objects, a test `lwipopts.h`): core IPv4 with TCP, UDP, ICMP, raw and `slipif` 42,617 bytes of code;
adding `ethernet`, `etharp`, `dhcp`, `acd`, `dns` 57,117 (the biggest: `tcp_in` 7.8 KB, `tcp_out` 7.7, `tcp` 7.1,
`dhcp` 6.9, `pbuf` 3.7, `ip4_frag` 3.2, `etharp` 3.0, `dns` 2.7); with IPv6 89,837. Data in that test was 46 KB of
static pools; netd takes its pools from the heap instead (`MEMP_MEM_MALLOC`, section 7), sized by the machine.

**Speed** (*estimates*, to be measured on the stock A1200 and the 060, never Emu68): lwIP's TCP input and output cost
roughly 2,000-4,000 instructions a segment; on a 14 MHz 68EC020 (~3 MIPS from chip RAM) with the checksum in an
`addx.l` loop and one copy each way, a 1,460-byte segment costs about 2 ms: a ceiling near 500-700 KB/s, far above what
SLIP (11.5 KB/s at 115,200 baud) or a PCMCIA NE2000 can carry. The parts the user feels (a key in telnet: write,
one message, `tcp_write`, SLIP out) are a few hundred microseconds. The hot loops (checksum, copies, SLIP's byte
loop) are assembler or `-O2`; the rest of lwIP `-Os` (golden path).

### 2.3 netd and the drivers talk without deadlock

Zenix's calls are synchronous (architecture.md section 8). Two processes that call each other deadlock, so **netd never
calls a driver; a driver is netd's client**, the way fsd is diskd's:

- **`NETDRV_ATTACH`**: the driver says who it is (interface name `eth0`, the MAC address, the MTU, how many receive
  buffers it wants, alignment and memory rules for DMA, checksum offload). netd answers with the interface number and
  hands it its buffers (2.4).
- **`NETDRV_XFER`** (`a[0]` a block in the driver's memory, `a[1]` its size): *in* - the receive buffers the driver
  has filled (buffer number, length, flags such as "checksum checked"), the send buffers it has finished, the link
  state, counters; *out* (netd writes into the block, zero-copy) - frames to send (buffer number, address, length,
  checksum request) and empty receive buffers to fill. One call per batch, never per frame.
- **The doorbell:** when netd has frames to send and the driver is idle, it must be woken without a call. netd starts
  each driver itself (as its child, `SYS_spawn`) with a pipe as the driver's standard input; netd writes one byte when
  it queues work and has not rung since the driver's last `XFER` (so the pipe never holds more than one byte and the
  write never blocks); the driver waits with `poll` (D16) on that pipe and on its hardware (an interrupt event,
  section 5.4, or a timer while polling). Signals were the other choice (`kill(SIGUSR1)`): they need a handler and
  turn every wait into -EINTR; a pipe is plain Unix and costs no new mechanism.
- **Supervision:** netd is the drivers' parent: it sees a driver's end (SIGCHLD/`wait`, D16), marks the interface
  down, logs it, and starts the driver again with D11's own limits (1, 2, 4 s back-off, given up after 5 ends in
  60 s), as `init` keeps a login on each console. A driver cannot be a D11 service: D11 starts a server by the first
  *call to* it, and nobody calls a driver. If netd itself ends, its drivers' next `XFER` fails with -ECONNRESET (D11)
  and they exit; the next socket call starts a new netd (D11), which starts new drivers (each resets its hardware).
- **Checks:** netd answers `NETDRV_*` only from its own children running as root, and checks every buffer number,
  length and count a driver gives back (a driver is trusted code, but a wrong number must fail the frame, never
  netd).

### 2.4 Buffers: netd owns them all

netd allocates every frame buffer (lwIP pbufs) and lends them to the driver: empty ones to fill on receive, full ones
to send. So a driver that copies anyway (NE2000 and CS8900A by PIO, the A2065 from its board memory, plipbox through
the CIA) copies **straight into netd's buffer**, and a DMA driver (GENET) lets the hardware write into it: no frame is
copied twice. The buffers come from the memory the driver's `ATTACH` asked for: on the PiStorm, fast RAM (the only
memory the Pi's DMA reaches, knowledge base `amiga/emu68-network-usb-rtg-hardware.md` section 1) in 64-byte-aligned
whole cache lines; elsewhere fast RAM when there is any, else chip RAM. **No network buffer needs chip RAM**: no
network hardware an Amiga has uses the chipset's DMA. SLIP and loopback use netd's buffers directly.

## 3. The kernel's RAM-floppy limit: servers from the disk

### 3.1 Today

The RAM floppy (`zenix-ram.adf`) holds the bootblock, the kernel and a 704 KB RAM disk in 880 KB, so the kernel may be
at most 179,200 bytes; it is 176,144 (3,056 left, `make`, 2026-10-26). 80,624 bytes of it are four server images
(`kernel/embed.S`: fsd 28,676, zfsd 36,688, diskd 9,360, boot 5,900, *measured* `build/*.zx`). They stay in memory
with the kernel, and each running server is a second copy (DESIGN D18's "not taken" list). D11 already starts a
service from a file (`/etc/svctab`: an absolute path, read as root); only the four built-in images are special.

### 3.2 What must stay built in

To read the root, Zenix needs the disk server and the root's filesystem server *before* any file can be read, and D11
starts them again after a crash from the image in the kernel: **the restart lifeline must not need the disk it
serves.** So `diskd`, `fsd` (ext2, the root of the RAM floppy, the SD card and every image the owner uses) and `boot`
(pid 1, before any filesystem) stay built in. **zfsd leaves the kernel:** it is needed only for a ZenixFS mount or a
ZenixFS root, never by the owner's card or the RAM floppy.

### 3.3 The change (step 1 of the build order)

1. **`@NAME` keeps its meaning** (frozen: "the built-in `@name` programs", CLAUDE.md): it names the server program
   NAME. svc.c looks for a built-in image first; if there is none, it starts the file `/sbin/NAME` as a file service
   (read as root, as `/etc/svctab`'s paths are; D11's rules unchanged: the deadline, the counting, the log). So
   `mount`'s registration `@zfsd DEVICE fs.DEVICE -id ID` works unchanged with zfsd on the disk. About 40 bytes.
2. `kernel/embed.S` loses zfsd: **the kernel image shrinks by 36,688 bytes** and the running system has 36 KB more free
   memory on every machine (the blob is no longer resident). `/sbin/zfsd` is installed on every root image (the base
   package, D15).
3. **A ZenixFS root** still needs zfsd before any file can be read. Today only the FS-UAE tests boot one
   (`zroot.fs-uae`); no real machine does (ZenixFS as the root on real hardware is roadmap block 4). So the build makes
   a second kernel, `kernel-zfs.bin` (today's image list: fsd, zfsd, diskd, boot) for those tests. When block 4 makes a
   ZenixFS root real, its own decision chooses between that variant in the update bundle (D15's `boot/...` entries)
   and a *boot module* (zenixboot handing the root's server to the kernel in bootinfo, as it already hands the RAM disk
   and the update bundle). Not decided now: neither is needed before then.
4. **The room** (*from the numbers above*): 3,056 + 36,688 = 39,744 bytes free for the kernel. The network's kernel
   part needs 4-5 KB (section 7.1). The rest is shared between the kernel's headroom (at least 12 KB is kept, for D10-D18
   follow-ups and fixes) and the RAM disk (`RAMDISK_K`), which grows by what the mount test image needs to carry
   `/sbin/zfsd` (D13 section 10 rejected moving zfsd because "the RAM root has too few free blocks for it": a smaller
   kernel is exactly what gives the RAM disk those blocks; measured at the build).
5. The network itself is never built in: `/sbin/netd` and `/sbin/net/*` are files from the start.

**Not taken**, and why: running the built-in servers in place from the kernel image (execute-in-place as ZEX2,
saving the second copy of fsd, diskd and boot, about 44 KB): the pristine image *is* the restart lifeline, and a stray
write through a raw message (D13 section 5's reason for keeping raw-message senders as ZEXE) would damage the only
copy; freeing boot's 5.9 KB image after its start (it would need a hole in the kernel's reserved range: small win, its
own change); compressing the images (a decompressor in the boot path, D13 section 10).

## 4. The programming interface: BSD sockets

### 4.1 A socket is a file descriptor

A new kernel file kind, **`FT_SOCK`** (the kernel's internal file kinds are not frozen, CLAUDE.md), next to pipes and
ttys in `vfs.c`. Each socket has a small kernel object, a **ksock** (a table of `NSOCK` = 32: netd's handle is the
ksock's index and a generation, the ready bits, the D16 waiter slot mask, a reference count, the flags `O_NONBLOCK`
and "dead"). `read`, `write`, `close`, `dup`, `dup2`, spawn's standard files (a server can give a socket to a child,
as inetd does) and `poll`/`select` work on it as on a pipe. netd always names sockets by the **kernel's** ksock
number: for `accept` the kernel takes a free ksock first and tells netd its number, so there is no second table to
keep in step.

### 4.2 System calls (the next free numbers, 90-92 today; claimed in CLAUDE.md before building)

- **`SYS_sock` (op, fd, request)**: every BSD socket call, multiplexed like Linux's `socketcall`, so the system call
  table grows by one: `SOCK_SOCKET`, `BIND`, `CONNECT`, `LISTEN`, `ACCEPT`, `SENDTO`, `RECVFROM`, `SHUTDOWN`,
  `GETSOCKOPT`, `SETSOCKOPT`, `GETSOCKNAME`, `GETPEERNAME`, `SETFL` (`O_NONBLOCK`), `RESOLVE` (4.4), `IFLIST`, `IFSET`,
  `ROUTE`, `NETSTAT` (section 6). The op numbers are frozen and append-only. `request` points to a `struct usockreq`
  (first size frozen, grows at the end, the caller passes its size, as `usigaction`): buffer, length, flags, address,
  address length, option and value. The kernel checks the request and each pointer in it by a small per-op table of
  directions (`user_ok_rd` for what netd reads, `user_ok_wr` for what it writes; D13 section 5), with a sockaddr at
  most 28 bytes and a buffer at most 64 KB, then sends one `NET_*` message to `net` (`ipc_kcall`, stamped
  `UMSG_KERNEL`): netd reads and writes the program's own buffers in place, **no copy in the kernel**. (TCP itself must
  keep sent data until it is acknowledged, so netd copies once into its send buffer; received data is copied once into
  the program's buffer. That is one copy each way, the minimum for TCP.)
- **`SYS_netsrv` (op, list, count)**: only for the process the kernel started for the `net` service. `NETSRV_READY`:
  a list of (ksock, generation, ready bits) - netd tells the kernel which sockets became readable, writable, closed or
  failed, **in one call per turn of its main loop**, and the kernel sets the bits and wakes the waiters (a D16
  producer: bit set and `ready()` under the mask; stale generations ignored).
- **`SYS_drv` (op, a, b)**: for user-mode drivers, root only: `DRV_HWINFO` (a copy of the hardware table at 0x1100,
  `kernel/hwinfo.h`, under the grow-at-the-end rule), `DRV_CACHE` (clean, invalidate, or clean+invalidate a range for
  DMA: `cpushl` per line on 040/060 and Emu68, the Emu68 range opcode when `HW_RANGEOPS`; 020/030 have no data cache
  in use - Zenix runs with the data cache off, `start.S` `cache_enable`), and later `DRV_IRQ` (section 5.4).

Message family: **`NET_*`** (kernel to netd) and **`NETDRV_*`** (driver to netd) in a new `kernel/netproto.h`, frozen
and append-only like `fsproto.h` (claimed in CLAUDE.md first). Port name `net` registered as a service (claimed).

### 4.3 Blocking, non-blocking, signals

- **netd never keeps a caller waiting.** Every `NET_*` request is answered at once: done, or -EAGAIN. A client in a
  message call cannot be interrupted (G4, D16 section 4), so a `recv` that waited *inside* netd would make ^C useless.
- **Blocking happens in the kernel**, in D16's event wait on the ksock: a blocking `read`/`recv`/`accept`/`connect`/
  `send` checks the ksock's ready bits (no message); if the operation can go on it sends the message; on -EAGAIN (or
  not ready) it waits for the bits (registered on the ksock's slot mask, Exec's order, D16 section 3), and loops. So a
  signal ends the wait with -EINTR (or restarts it with `SA_RESTART`), exactly as for a pipe; timeouts
  (`SO_RCVTIMEO`) are the timed form of the same wait. `O_NONBLOCK` (from `socket(..., SOCK_NONBLOCK)` or `fcntl` in
  libz) returns -EAGAIN instead; `connect` then gives -EINPROGRESS and `poll` says POLLOUT when it is done.
- **`poll`/`select`** (D16's `SYS_evwait`) read the ready bits of a socket like a pipe's: no message per poll.
- **SIGPIPE:** a write to a connection the other side has closed gives -EPIPE and SIGPIPE, as a pipe does (D16), unless
  `MSG_NOSIGNAL` is set.
- **netd's end:** every ksock carries the `net` service's start count when it was made; after a restart, any call on
  an older socket fails with **-ECONNRESET** (D11's "the server is gone"), its waiters are woken (POLLERR|POLLHUP), and
  only `close` succeeds. A restart of netd loses every connection, as a reboot of a router would; programs see a reset.
- **A dying process:** its last `close` of a ksock sends `NET_CLOSE`, a **no-start call** (like `FS_CLOSE`, D11): it
  never starts netd just to close a socket of a netd that is gone.

### 4.4 libz

`socket`, `bind`, `connect`, `listen`, `accept`, `send`, `sendto`, `recv`, `recvfrom`, `shutdown`, `getsockopt`,
`setsockopt`, `getsockname`, `getpeername`, `inet_pton`, `inet_ntop`, `htons` and friends, `getaddrinfo`,
`freeaddrinfo`, `gai_strerror`, `gethostbyname` (for ports), with the BSD/Linux structures and constants (`AF_INET` 2,
`SOCK_STREAM` 1, `SOCK_DGRAM` 2, Linux's `SOL_SOCKET`/`SO_*` numbers, so ported programs need no changes). Each is a few
lines over `SYS_sock`; `--gc-sections` keeps them out of programs that do not use them. **Name lookup** happens in
netd, not in each program (a resolver in every network program would be one more copy, golden path "no copies"):
`getaddrinfo` parses numbers itself, else calls `SOCK_RESOLVE`, which makes a ksock that becomes readable when the
answer is in (so the wait is the same interruptible socket wait), reads the addresses and closes it. netd answers from
`/etc/hosts` first, then its cache, then DNS (section 6.2).

## 5. Drivers

### 5.1 The interface that fits every card

Every Amiga network card is one of three shapes, and the driver protocol (2.3, 2.4) covers all three:

| Shape | Cards | What the driver does with netd's buffers | Interrupt |
|---|---|---|---|
| DMA into host memory | CM4 GENET (PiStorm) | posts them in the receive ring, cleans/invalidates the cache lines (`DRV_CACHE`) | GIC through level 6, or polled |
| Own buffer memory on the card | A2065 (LANCE, 32 KB on the board), Ariadne (PCnet-ISA) | copies from the card's memory into them | INT2 (shared line) |
| Programmed I/O | NE2000 (PCMCIA), CS8900A (clockport), plipbox (parallel port) | reads/writes the data register straight into them | INT2/INT6, the CIA FLAG for plipbox |

Drivers run in user mode: without an MMU a user process reaches device registers by plain pointers, so a driver needs
the kernel only for the cache instructions (privileged) and for interrupts. The 68k data cache is off on every Zenix
machine (`start.S`), so register access needs no cache rules on real Amigas; on Emu68 the device windows are mapped
uncached by Emu68 (knowledge base, `amiga/emu68-network-usb-rtg-hardware.md` section 1).

### 5.2 (a) The CM4's Ethernet under Emu68: GENET v5 (`/sbin/net/genet`)

What is known (knowledge base `amiga/emu68-network-usb-rtg-hardware.md`, `emu68-driver-stack-why-fast.md`,
`zenix-pi-drivers-progress.md`; *read* from rondoval's `emu68-genet-driver` and the Linux/U-Boot `bcmgenet` driver,
not run):
- **Where it is:** stage0 already reads Emu68's device tree while AmigaOS is alive and writes the hardware table
  (`kernel/hwinfo.h`): `genet` (the register base after Emu68 rewrote `/scb`'s ranges: 0xF7580000 on Emu68 1.1, but
  0xFD580000 was seen on 1.0.7, *verified on the A1200 2026-10-23*: never hard-coded), `genet_irq0/1` (GIC SPIs),
  `phy_addr` and `mac` (`local-mac-address` from the firmware). The driver reads them with `DRV_HWINFO`.
- **Bring-up** (the order U-Boot and Linux use): flush the receive buffer, UMAC soft reset, MIB counters reset, RBUF
  control with the 64-byte status block on and EEE off (EEE breaks reception), the MAC address, the DMA rings (256
  descriptors in the register window, all in the default queue 16), the PHY through GENET's own MDIO registers (a
  BCM54xx on RGMII with receive delay, `rgmii-rxid`), link negotiation, then DMA on. Every stage logs a line (driver
  rule 3: only the A1200 can test it) and every wait has a timeout counted with the E-clock (CLAUDE.md).
- **Memory:** descriptors are registers; frames are netd's buffers in fast RAM, 2 KB each (64-byte aligned: every
  receive frame starts with the 64-byte status block). The 68k address is the DMA address (the SD driver's mailbox
  works that way on the A1200, *verified 2026-10-24*; the GENET path is *read*).
- **Caches:** the ARM's data cache is not coherent with DMA. A receive buffer is invalidated before it is posted and
  after it is filled; a frame to send is cleaned before the descriptor is written. With `HW_RANGEOPS` (rondoval's
  Emu68 build) the driver uses the range opcodes itself (AmigaOS drivers issue them from task code, which runs in user
  mode, so they should work from a Zenix process: **to check on the A1200 first**, with a one-page test program);
  without them `DRV_CACHE` does `cpushl` per line in supervisor mode (the slow path rondoval measured at 104 Mb/s).
- **Polled first:** the driver checks the rings at every wake-up: the doorbell, and a timer of one tick (10 ms) while
  the interface is up, more often while frames are flowing (it does not sleep while the last turn moved frames).
  Simple, and enough for telnet, ssh and wget (a 256-entry ring holds 2.5 ms of a full 1 Gb/s line, and Emu68 empties
  it in microseconds). The GIC interrupt through Emu68's level 6 (one level-6 interrupt for every ARM interrupt, only
  with INTENA's EXTER on) comes with `DRV_IRQ` (5.4), after measurements on the A1200.
- **Offloads later:** GENET's transmit checksum and receive full-frame checksum (rondoval's figures show where the time
  goes); lwIP's software checksums are switched off per interface when the driver says it checks them.
- **Effort** (*proposed*): ~800-1,000 lines (rondoval's estimate for a minimal polling driver: ~800), plus a host model
  test like the SD card's (`tools/emmc-fake.h`): a GENET register and ring model, so the bring-up order and the ring
  arithmetic are tested on the PC before the A1200.

### 5.3 (b) SLIP over the serial port, for every Amiga (inside netd)

- netd opens `/dev/ttyS0` (D12: root only, one opener; while it is open the console's mirror is off, the kernel log
  stays in `dmesg` but not on the wire, and `cu` gets -EBUSY) and runs lwIP's `slipif` over it: RFC 1055 framing (END
  0xC0, ESC 0xDB), the MTU 1006 (the traditional SLIP MTU; MSS 966). Bytes are read with `poll` on the device in
  netd's main loop; the receive interrupt still only queues (D12, audit B4). 115,200 baud 8N1, no handshake, as D12.
- **The PC side** (`tools/host/zenix-slirp`, *proposed*, GPL-2.0-only, linked with the PC's libslirp, BSD-3): reads
  SLIP from a serial port or a pty and gives each IP packet to libslirp, QEMU's user-mode network: NAT to the PC's
  network and the internet, a DNS forwarder, port forwards into the Amiga; **no root, no network setup on the PC**
  (the Amiga is 10.0.2.15, the gateway 10.0.2.2, DNS 10.0.2.3, slirp's defaults). The same program serves the FS-UAE
  tests (section 8) and the owner's cable (`zenix-slirp /dev/ttyUSB0`). For a PC that already runs `slattach`, netd
  speaks plain SLIP: nothing Zenix-specific on the wire.
- **PPP later:** lwIP has a full PPP (LCP, IPCP, PAP/CHAP), about 30-40 KB (*estimate*), and some of its files come from
  pppd under licences that need checking against GPL-2 (old BSD-style notices with acknowledgement clauses): a
  licence check before it is used. SLIP needs none of it.

### 5.4 Interrupts for drivers (with the first interrupt-driven driver, not in the first steps)

Amiga interrupt lines are shared and level-triggered (INT2 with CIA-A, the keyboard, Gayle's IDE; INT6 with CIA-B and,
on Emu68, every ARM interrupt). A user-mode handler cannot run in the interrupt, and the line must be quiet before the
interrupt returns. So, as Linux's UIO: **the kernel silences the device, the driver handles it.** `DRV_IRQ` attaches a
tiny check-and-silence program (a few steps of: read a register, test bits, write a register; e.g. NE2000: read ISR,
test, write IMR 0; GIC: read IAR, test the SPI, disable it, write EOIR) to INT2 or INT6, run in the interrupt after the
kernel's own sources; when it claims an interrupt, the kernel sets the driver's event (D16: an object with a slot mask,
woken at `kwork`, interrupts only queue). The driver handles the device and re-enables its interrupt. A CIA source
(plipbox's FLAG on CIA-A, whose ICR is read-to-clear and shared with the keyboard) is a fixed source the kernel's own
CIA handler passes on. Designed in detail with the A2065 driver (FS-UAE emulates it: 8.2), not before.

### 5.5 (c) Later: plipbox, NE2000, CS8900A, A2065, Ariadne

All fit 5.1. plipbox (knowledge base `zenix-hardware-driver-survey.md`, GPL-2+ reference driver) needs the CIA FLAG
source and the kernel to own the CIA registers it shares with the serial lines. NE2000 on PCMCIA needs Gayle's PCMCIA
I/O window and its interrupt; CS8900A on the clockport is PIO with a jumpered interrupt line; A2065 and Ariadne are
Zorro II boards found in the hardware table's board list (stage0 copies expansion.library's list: the A2065 shows up
there as manufacturer 0x202 product 112, *verified in FS-UAE 2026-10-26*, section 8.2).

## 6. Configuration, DHCP, DNS, commands

### 6.1 Files (new `/etc` formats: frozen once shipped, unknown words reported and ignored)

- **`/etc/network/interfaces`**, the Debian format people know, a small subset: `auto IFACE`, `iface IFACE inet dhcp|static`,
  and inside a stanza `driver NAME [ARGS]` (Zenix's word: `genet`, `slip /dev/ttyS0 115200`), `address A/PREFIX`,
  `gateway G`, `pointopoint P` (SLIP), `mtu N`, `hostname H` (sent with DHCP). Example for the owner's two machines:
  ```
  auto eth0
  iface eth0 inet dhcp
      driver genet

  iface sl0 inet static
      driver slip /dev/ttyS0 115200
      address 10.0.2.15/24
      gateway 10.0.2.2
  ```
- **`/etc/resolv.conf`** (`nameserver`, `search`; standard), **`/etc/hosts`** (standard). DHCP's DNS servers are kept
  in netd's memory and shown by `ifconfig`, not written to `/etc` (no file changes behind the owner's back).
- **A static address** is the `inet static` stanza, or for now `ifconfig eth0 192.168.1.50/24` and `route add default
  192.168.1.1` (as root; until the next restart of netd).

### 6.2 DHCP and DNS live in netd

lwIP's DHCP client (6.9 KB) and DNS client (2.7 KB, with its cache and random query ids and ports,
`LWIP_DNS_SECURE`) run inside netd: they need its timers and sockets anyway, a lease belongs to the interface, and a
separate `dhcpcd` would be another process on a 2 MB machine. The ARP probe of an offered address (`acd`, 1.1 KB) is
left out (a later option). Lease renewal and expiry are lwIP's timers; `ifconfig` shows the lease.

### 6.3 Bringing the network up

netd starts on the first socket call (D11, `/etc/svctab` line `net -10 /sbin/netd`). At its start it reads
`/etc/network/interfaces` and brings up the `auto` interfaces (starting their drivers or opening the serial port). At
boot, **`init` runs `ifup -a` when `/etc/network/interfaces` has an `auto` line** (one line in init, as `mount -a`):
that first call is what starts netd. A machine without the file never starts it.

### 6.4 Commands (each with its manual page, `commands.md`, `features.md`; claimed with the build steps)

- **`ifconfig`** (BSD/Roadshow style, short output; `ifup`, `ifdown` are names of the same program, as `more` is of
  `less`): show the interfaces (address, MAC, MTU, link, lease, DNS servers, counters), `up`/`down`, an address; root
  for changes.
- **`route`**: show the table, `add default GW`, `del`.
- **`ping`**: ICMP echo with Linux's unprivileged ICMP datagram sockets (`SOCK_DGRAM`, `IPPROTO_ICMP`; netd lets
  everyone send echo requests and nothing else on them), so `ping` needs no root. **Today's `ping` and `pingd` are the
  message-passing test programs** (D11's test service, port `ping`): they become `msgping` and `msgpingd` in step 0
  (the port name `ping` stays: it is in the registered list).
- **`netstat`** (`-t`, `-u`, `-l`, `-a`): connections and listening sockets, from `SOCK_NETSTAT` (a table of
  `unetstat` rows, first size frozen, the caller's buffer size).
- **`host NAME`**: a DNS lookup through netd (`getaddrinfo`).

## 7. Size and memory budgets

### 7.1 The kernel (always in memory, *estimates* at -Os, measured at each step)

| Part | Size |
|---|---|
| `FT_SOCK`, the ksock table code, `SYS_sock` with its pointer table, the blocking loop, `evwait`'s socket case, SIGPIPE, `NET_CLOSE` | 2.5-3.5 KB |
| `SYS_netsrv` | 0.3 KB |
| `SYS_drv` (hwinfo copy, cache ranges) | 0.4 KB |
| The random pool and `/dev/urandom`, `/dev/random` (section 9) | 0.9-1.3 KB |
| `@NAME` from `/sbin` | 0.05 KB |
| **Total** | **4-5.5 KB**, against **36,688 bytes** zfsd frees |
| Data: 32 ksocks (~16 bytes each) and the 64-byte pool | ~0.6 KB |

All new kernel code is `-Os` in its own files (`sock.c`, `random.c`), only the ready-bit test on the poll path inline.

### 7.2 A machine that does not use the network (the stock A1200 as the owner runs it today)

**About 31 KB more free memory than today**: the kernel grows by 4-5.5 KB of code and 0.6 KB of data, and the
36,688-byte zfsd image is no longer resident. netd is never loaded (no `auto` line, no socket call). The disk holds
`/sbin/netd` (~70 KB) and the manual pages.

### 7.3 A machine that does (*estimates*, measured at steps 5 and 8)

| | Stock A1200, SLIP | PiStorm or a 060, Ethernet |
|---|---|---|
| netd code (lwIP 57 KB *measured*, our netd 6-8 KB, libz ~6 KB) | ~70 KB | ~70 KB |
| netd data: pools from the heap, sized by the machine like fsd's cache (D18) | 16-24 KB: MSS 966, window 2 x MSS, 4 KB send buffer, 8 TCP connections, 16 buffers | 100-200 KB: MSS 1460, window 16-32 KB, 64-128 receive buffers |
| netd stack + kernel stack | 8 + 4 KB | 8 + 4 KB |
| Driver process | none (SLIP is in netd) | GENET ~10 KB code, 4 KB stack, 4 KB kernel stack |
| **Total** | **~100 KB** of 1,332 KB free (*measured* D18 figure for one login) | ~200-300 KB of 64 MB / 2 GB |

The 1 MB chip-only matrix machine (395 KB free with one login, D18) can run netd on SLIP with a session less; the
matrix test says so rather than promising it. netd's memory comes in a few large regions (`SYS_memalloc` allows 8 per
process, `NREG`), which its allocator splits.

## 8. Tests

### 8.1 On the PC (no emulator)

- **netd on the host**, as `zup-host` (D15): lwIP and netd's code built for Linux against a fake of the kernel's
  socket layer, talking SLIP over a socketpair to `zenix-slirp`: real TCP to a host echo server, UDP, DNS from a test
  resolver, ICMP, a netd restart. No root, no network setup.
- **The socket layer's wait logic** in the model of `tools/signal-host-test.c` (every interleaving of netd's
  `NETSRV_READY` and a waiter; a stale generation; netd's death).
- **lwIP's own unit tests** (`test/unit`, the `check` framework) with Zenix's `lwipopts.h`.
- **Fuzzing** the input path (lwIP's `test/fuzz` harness: Ethernet, ARP, IP, TCP, UDP, DHCP and DNS replies), one fuzz
  run at a time with a memory limit (the machine's rule after 2026-09-30).
- **GENET's register and ring model** (5.2) and the driver against it.

### 8.2 In FS-UAE

- **Loopback first** (no hardware): sockets, `poll`, blocking and ^C, `SA_RESTART`, `O_NONBLOCK`, SIGPIPE, an
  `accept`ed socket given to a child, many connections, netd killed (every socket -ECONNRESET, the next call starts a
  new netd), bind below 1024 refused to a user, raw messages to `net` from a user refused, `netstat`.
- **SLIP:** FS-UAE's serial port is a pty on the host; `zenix-slirp` on it. The test rig types into that same port, so a
  SLIP test runs its commands from a script started at boot (the test image's `/etc/autologin` profile), ends with
  `ifdown sl0` (the port is the console's again, D12) and prints its results: `zenix-slirp` passes every byte that is
  not a SLIP frame on to the tap. Checks: `ping 10.0.2.2`, TCP to a host server through slirp's forward, DNS, a 100 KB
  transfer compared by checksum, a serial line with noise (bad frames dropped, counted).
- **Ethernet in the emulator:** FS-UAE 3.2.35 emulates an **A2065** (`network_card = a2065`, slirp inside; *verified
  2026-10-26*: on the A1200 config it appears as Zorro II board 0x202/112 at 0xE90000, 64 KB, in Zenix's own board
  list). It needs an A2065 driver (5.5). With it, ARP, DHCP (slirp's server), the driver protocol, interrupts and a
  driver crash are tested in the matrix. **Owner question 2:** build the A2065 driver right after GENET as the
  emulator's Ethernet, or later in the card order?
- **The driver protocol before any card:** a fake driver (`/sbin/net/fake`, test image only) that answers ARP and
  echoes ICMP itself, killed mid-transfer.

### 8.3 Only on the real machines

GENET at all (FS-UAE has no Pi); the range opcodes from user mode; SLIP at 115,200 baud through Paula on a 14 MHz
68EC020 (overruns while the display and the disk are busy); the speed table (ping round trip, TCP throughput over
SLIP and over loopback as a measure of the stack's CPU cost) on the stock A1200, the 060 and the PiStorm, labelled by
machine; the entropy sources' real behaviour (section 9).

## 9. Security

### 9.1 What the server checks

- netd answers `NET_*` only with `UMSG_KERNEL` (sent by the kernel for a system call), `NETDRV_*` only from its own
  children running as root; anything else -EACCES (as fsd answers only the kernel and root).
- **Ports below 1024: root only** (the uid the kernel stamped on the message). Raw IP sockets (`SOCK_RAW`): root only.
  ICMP datagram sockets: echo only. Interface, route and address changes: root only.
- No listening service by default: nothing answers from the network until the owner starts one (telnetd, the remote
  control). No firewall in revision 1; lwIP's input hook is where one would go (knowledge base
  `amiga/lwip-amiga-firewall.md`).
- Everything from the network is untrusted input: lwIP's parsing, fuzzed (8.1); netd's own parsing (config files,
  driver blocks, `usockreq`) range-checks every number (CLAUDE.md "Memory without an MMU").

### 9.2 What a system without an MMU can and cannot promise

Can: a program uses the network only through the kernel's checks (its own fds, its own buffers, root for low ports
and configuration), and a crash of netd or a driver is contained and restarted. **Cannot:** any program can read or
write netd's memory (other users' connection data) and poke the network card's registers, so users are a convenience,
not protection (D8's honest limit, limits.md). And a remote bug in netd's parsing is a bug with the whole machine's
rights (no MMU, no non-executable memory). So: lwIP rather than new code, the fuzzing, few listening services, and the
focused security audit the roadmap puts at the end of block 2.

### 9.3 Random numbers

TCP's initial sequence numbers (RFC 6528: a keyed hash of the connection's addresses and ports plus a clock; lwIP's
`LWIP_HOOK_TCP_ISN` and its `contrib/addons/tcp_isn`, BSD), random ephemeral ports (RFC 6056, `LWIP_RAND`), DNS
query ids and ports, and later the crypto library (keys, nonces) all need numbers an attacker cannot guess. So the
kernel gets **one entropy pool** (64 bytes, mixed and read out with the ChaCha20 block function, as Linux since 5.17
reads out with ChaCha20) with **`/dev/urandom`** (never blocks) and **`/dev/random`** (blocks until the pool was first
seeded with an estimated 256 bits, then the same as urandom, as Linux since 5.6). netd reads 32 bytes at start for its
keys and runs its own ChaCha20 for `LWIP_RAND`.

**Sources on an Amiga** (none is a hardware random generator):
- the **beam position** (`VHPOSR`, one chip-bus read, ~280 ns resolution) read at events that come from the outside
  world: key presses and releases, mouse movement, disk and SD completions, the floppy's index pulse (the drive motor's
  speed jitter is physical), each received frame or SLIP packet. Interrupt handlers only store the 16-bit sample in a
  small ring (the "interrupts only queue" rule); `kwork` mixes. Not every serial byte: per frame. (The CIA E-clock is
  the same crystal as the beam and costs a slow CIA access; not used.)
- at a cold start, a hash of a few KB of not-yet-used chip RAM (its power-on pattern; worth little after a warm reset,
  counted as 0);
- on Emu68, the Pi's own hardware generator (BCM2711 RNG200), when stage0 finds it in the device tree (a field of the
  hardware table's reserved words), credited fully;
- **a seed file**, `/var/lib/random-seed` (mode 600, root): read at boot and at once rewritten (so a crash never
  reuses it), written again at `reboot` and shutdown (as Linux distributions do). It is credited only if the boot that
  wrote it had itself been seeded (a flag in the file), so a stock A1200 that boots headless still has a seeded pool.
- Credit is conservative: one bit for each key event, half a bit for each disk or frame event.

## 10. Build order (each step small, merged with its tests; the full suite before each merge)

0. **`ping` and `pingd` become `msgping` and `msgpingd`** (tests, svc-test, docs). Test: the D11 suites unchanged.
1. **zfsd from the disk** (section 3.3): `@NAME` falls back to `/sbin/NAME`; zfsd out of `kernel.bin`, into every root
   image; `kernel-zfs.bin` for the ZenixFS-root tests; the RAM-floppy count and `RAMDISK_K` measured. Tests: the mount
   tests (a ZenixFS mount from `/sbin/zfsd`, a killed zfsd started again from the file), the ZenixFS-root test, the
   RAM floppy builds, `free` on the 2 MB matrix machine shows the 36 KB.
2. **Random numbers** (9.3): the pool, the sources, `/dev/urandom`, `/dev/random`, the seed file in boot/init/reboot.
   Tests: host (the mixing; ChaCha20 against RFC 8439's vectors), FS-UAE (two boots give different bytes; the seed
   file rewritten; `/dev/random` waits on a fresh image without a seed until keys are typed).
3. **The kernel's socket layer** (4.1-4.3) with a small test server in place of netd (`socktestd`: answers `NET_*`
   from a table, pushes ready bits). Tests: the host model; FS-UAE: blocking, ^C, `SA_RESTART`, `poll`, SIGPIPE,
   -ECONNRESET after the server's end, the no-start close, pointer checks (a ZEX2 program's literal as a receive
   buffer: -EFAULT).
4. **netd with lwIP, loopback only**, libz's socket calls, `netstat`. Tests: host netd; FS-UAE TCP and UDP on
   127.0.0.1 (8.2 "loopback").
5. **SLIP, `/etc/network/interfaces`, `ifconfig`/`ifup`/`ifdown`, `route`, `ping`, `zenix-slirp`**. Tests: 8.1 host
   netd over SLIP; 8.2 SLIP in FS-UAE. **First thing the owner can use:** the stock A1200 on the cable.
6. **DNS and `host`, `getaddrinfo` by name**; DHCP code in (used from step 7). Tests: host and FS-UAE through slirp's DNS
   and a test resolver.
7. **The driver protocol** (`NETDRV_*`, buffers, doorbell, supervision, `SYS_drv`) with the fake driver. Tests:
   8.2 "driver protocol", a driver killed and restarted, bad buffer numbers refused.
8. **GENET** (polled): the host model first, then the A1200 checklist (link, DHCP from the owner's router, ping, a
   telnet to a BBS, the speed table). **The CM4's Ethernet in daily use.**
9. **(owner question 2)** the A2065 driver with `DRV_IRQ`, FS-UAE's Ethernet test.
10. A focused audit of the network (security first), its findings fixed; then telnet (its own note: pseudo-terminals,
   SIGHUP, the BBS terminal), the crypto library, ssh, wget, NFS, remote control, `zpkg`, as the roadmap orders them.

Kernel steps (1, 2, 3, the `SYS_drv` part of 7) touch the shared files (`kernel/kernel.h`, `Makefile`, `user/usys.h`,
`user/libz.*`): they are done by the main session or in small separate commits it merges first (CLAUDE.md "Sessions").

## 11. Open questions for the owner

1. **Is lwIP (BSD licence, 57 KB) right, rather than writing our own TCP/IP?** The note recommends lwIP: proven,
   GPL-2-compatible, and only loaded while the network is used. (Technical call made; said here because it is the
   biggest outside code Zenix would take in.)
2. **The A2065 driver as the emulator's Ethernet** right after GENET (testing the Ethernet path in every `make check`),
   or later in the card order (plipbox, PCMCIA, clockport, Zorro)?
3. **The PC side for the serial cable:** a small Zenix tool on the PC (`zenix-slirp`, no root) is proposed, instead of
   setting up `slattach` and NAT as root on the PC. Fine?

Not owner questions (decided here, reviewed): C over A/B, the doorbell pipe, DHCP/DNS in netd, zfsd from the disk.

## 12. Reviews

(To be added after the two reviews.)

Last checked against the code: 2026-10-26.
