# D46: USB - a host controller server and USB sticks

Roadmap block 6, "USB" (docs/ROADMAP.md "The order of work", approved by the owner 2026-10-25). Revision 2,
2026-11-12, branch `usb`, after two independent reviews of revision 1 and two code reviews (section 12) and the partner's answers (section
8), which changed the scope from keyboards and mice to USB sticks. Steps U1 and U3 (section 9: the controller, then
sticks read-only) are built on this branch against a model of the controller and a stick on the PC; **nothing has run
on real hardware.** No emulator offers USB on an Amiga, so every line of a USB driver can only be proved on a real
machine, and real-hardware time is scarce: this note says what to build so the first run on the partner's PiStorm is
short and every step of it says what it found. Each fact is marked *verified* (seen on the real machine, its card or a
file from it), *read* (from a source named next to it) or *proposed* (this note's choice).

**Decided by the main session, 2026-11-12** (docs/decisions.md): after the partner's answers, the first version is
the controller and **one USB stick plugged straight into the one socket, read-only**, appearing in diskd as `usb0` and
`usb0p1`...; keyboards, mice, the boot-protocol input work and hubs are dropped for now; the kernel's bytes are
accepted; **exFAT is the next item**, separately (section 10). Revision 1's keyboard and mouse design (a new
`CON_INPUT` kind for keys held across reports, the mouse through `CONI_MOUSE`) is kept in git history (commit
a51ea90) for when it is wanted.

## 1. In plain words

- **Where USB exists today:** only on the partner's PiStorm A1200, and there exactly **one USB 2.0 port**: the
  CM4's own, wired out through the PiStorm's CM4 adapter to the one USB socket on the I/O board (section 2). The A1200
  with the Apollo 1260 has no USB, and no card for it (the partner, section 8).
- **What the first version gives:** a USB stick in that socket appears as a disk, `usb0`, with its partitions
  (`usb0p1`, ...), read-only, mounted by the filesystems Zenix already has (FAT, ext2, ZenixFS, FFS, PFS3). The
  partner's own stick is exFAT (a 128 GB Corsair GTX), which is the next item (section 10).
- **How:** one user-mode server, `/sbin/usbd` (a D11 service on port `usb`), owns the controller (the D19 driver
  pattern: a kernel claim, DMA memory that belongs to the claim, a quiesce program) and speaks USB's bulk-only
  transport and SCSI to the stick; diskd stays the one disk server and asks usbd for sectors by posts.
- **What the kernel pays:** 3,368 bytes of `kernel.bin` (measured): 2.3 KB in the built-in diskd, 0.7 KB of stage0's
  boot-only code (given back after the boot by D45), 422 bytes in `drv.o`. On a machine without the controller usbd
  stops at once.
- **How it is tested before the hardware:** the controller and a stick backed by an image file are modelled on the
  PC, strictly, as `tools/genet-model.h` models the Ethernet chip; the same driver C runs against it, with misbehaving
  sticks (section 6). diskd's side runs on the PC against a stub of usbd (`tools/disk-host-test.sh`).

## 2. The hardware (facts with sources)

### 2.1 The partner's PiStorm A1200

| Fact | Status, source |
|---|---|
| The board is a CM4 Lite, 4 GB, on a PiStorm32-lite, Emu68 1.1 (rangeops build) | *verified* (Emu68CP's board revision 0x00b03141, 2026-10-20; knowledge base `amiga/emu68-and-pistorm.md`) |
| **A CM4 has no VL805** (the Pi 4B's PCIe USB 3 chip): its USB is one USB 2.0 port only | *read*: Raspberry Pi's `config.txt` documentation ("Compute Module 4 and earlier do not include the USB 3.0 controller"); rondoval's `emu68-xhci-driver` README ("On a CM4, unit 1 exists only if you have attached a PCIe xHCI card") |
| **The CM4's one PCIe lane holds the NVMe drive** AmigaOS boots from, so no PCIe USB card can be added | *verified* (AmigaOS 3.2 boots from the NVMe on this machine since 2026-11-04; knowledge base `amiga/emu68-and-pistorm.md`) |
| That one USB 2.0 port can be driven by two controllers inside the BCM2711: the old DWC2 OTG core (`/soc/usb@7e980000`, "brcm,bcm2708-usb") or an **xHCI USB 2.0 host controller** (`/scb/xhci@7e9c0000`, "generic-xhci", SPI 176) | *read*: the CM4 device tree on the partner's card (`bcm2711-rpi-cm4.dtb`, decompiled 2026-11-12) |
| `otg_mode=1` in `config.txt` selects the xHCI as the port's host controller; the DWC2 stays for device mode | *read*: Raspberry Pi's `config.txt` documentation ("`otg_mode=1` requests that a more capable XHCI USB 2.0 controller is used as an alternative host controller") |
| **The partner's card has `otg_mode=1`** (commented "Activate USB port") | *verified*: `config.txt` read from the card's backup image of 2026-11-06 (the card itself was not touched) |
| **AmigaOS drives it as `xhci.device` unit 0**, under "Poseidon 6.2", which is rondoval's Neptune (an incompatible Poseidon fork); `S:User-Startup` runs `PsdStackLoader xhci.device 0`; driver stack 2.3.0 (xhci.device 6.x) | *verified* for the install and the line (2026-11-05, knowledge base `amiga/genet-under-emu68.md`); *read* for unit 0 = the on-chip port (the xhci driver's README and `unit.c`: unit 0 opens the device-tree symbol `xhci` and refuses a node whose status is "disabled") |
| Under Emu68 the xHCI's registers appear at **0xF89C0000**: bus 0x7E9C0000 lies in `/scb`'s first range, which starts at bus 0x7C000000 (0x3800000 long) and which Emu68 moves to 0xF6000000, as GENET's 0x7D580000 appears at 0xF7580000. Its `reg` is **1 MB** long | *read* and worked out: `/scb` ranges in the card's device tree + the Emu68 range rewrite (knowledge base `amiga/emu68-network-usb-rtg-hardware.md`); **never hard-coded**: stage0 reads it (section 3.6) |
| The adapter wires the CM4's USB 2.0 pair (module pins 103/105) and the OTG ID line (pin 101) to its 40-pin FPC connector (pins 14, 15, 35), and nothing else of USB | *read*: the CM4 adapter's KiCad board, `github.com/PiStorm/cm4board`, `CM4_PiStorm32Lite` (v1.1, 2025-03-31) |
| The FPC goes to an **I/O board** on the trapdoor with HDMI, Ethernet and USB: **one USB socket** | *read*: amiga-news.de 2023-03 (the adapter by Michal Schulz, the I/O board by Claude Schwarz); the socket count from the partner (2026-11-12: "The PiStorm has 1 USB socket") |
| What is used there under AmigaOS: **USB sticks, through Poseidon**, mainly a 128 GB Corsair GTX formatted exFAT | the partner, 2026-11-12: "On AmigaOS they use Poseidon only for USB sticks", "Mostly USB sticks, mainly a 128 GB Corsair GTX formatted exFAT" |
| DMA: the xHCI is an `/scb` bus master like GENET; `/scb`'s `dma-ranges` map bus = ARM physical for the first 16 GB, so the 68k address of Emu68 RAM is the DMA address, as for GENET; it reaches no chip RAM | *read*: the card's device tree; knowledge base `amiga/genet-under-emu68.md` "Cache and DMA" |
| The ARM's data cache is not coherent with the xHCI's DMA (no `dma-coherent` in its node) | *read*: the card's device tree; the xhci driver's `xhci-dma.c` cleans and invalidates around every transfer |
| Every register and every structure the controller reads (TRBs, contexts, the DCBAA, the segment table) is little-endian; the 68k swaps | *read*: xHCI 1.2 section 4.2 (little-endian throughout); `boot/genet-stop.h` does the same for GENET |
| Power: rondoval's driver makes no mailbox call to power the USB block (the node names firmware power domain 6) and works from `User-Startup`, so the firmware leaves it powered with `otg_mode=1` | *read* (`xhci.device/src/`, no mailbox use); a cold start straight into Zenix (the boot menu) is the same firmware state, *proposed*, checked at U1 (a register read of all ones means "not powered": usbd says so and stops) |

**The answer:** what Poseidon drives on this Pi is the BCM2711's built-in **xHCI USB 2.0 controller** on the CM4's
single USB port (not the DWC2, not a VL805, which a CM4 does not have). Zenix's USB driver is therefore an **xHCI
driver**, USB 2.0 speeds only (high, full, low): no USB 3, no SuperSpeed hubs, no streams, which keeps it small.

### 2.2 The Apollo 1260 machine, and Amiga USB cards

| Card | Bus | Chip, speed | Status, source |
|---|---|---|---|
| Subway (E3B) | clock port | full speed (USB 1.1 class), up to 4 ports | *read*: Big Book of Amiga Hardware, Wikipedia "Clock port"; chip not confirmed |
| RapidRoad (Individual Computers) | clock port, or X-Surf-100's expansion port | ISP1763, high speed (480 Mbit/s peak), 2 ports | *read*: icomp wiki "RapidRoad" |
| Highway (E3B) | Zorro II | full speed | *read*: named in product lists only; not confirmed |
| Deneb | Zorro II / III | USB 2.0 high speed, DMA on Zorro III | *read*: amiga.resource.cx, amigakit (availability not confirmed) |
| Thylacine (Ambience Creation Technology, 2002) | Zorro II | SL811HS, one full-speed port | *read*: amiga.resource.cx; NetBSD's `slhci` supports it |
| UltraBus | Zorro II | four ports, no high speed | *read*: Big Book of Amiga Hardware (unreleased when written) |

The 1260 sits in the trapdoor of an A1200 without a Zorro busboard, so only **clock-port cards** (Subway, RapidRoad)
fit it as it is. The partner owns none (2026-11-12: "No USB card for the 1260"). None of these is an xHCI: each would be
its own host-controller back end under the same USB core (section 3.3), built only when a card is in hand.


## 3. The design

### 3.1 Scope of the first version, and the order

1. **The controller alone** (step U1): stop, reset, start, rings, a No-Op command answered (the self-test, 4.8), the
   root port's status. Nothing is driven yet: this proves the DMA, the cache rules and the rings on the real chip
   with nothing at stake.
2. **USB sticks, read only** (step U3; U2, keyboards and mice, is dropped for now): a device on the root port
   addressed and configured; mass storage with bulk-only transport (BOT 1.0) and SCSI's transparent command set:
   INQUIRY, TEST UNIT READY with REQUEST SENSE (a stick waking up, a card reader without a card), READ CAPACITY (10),
   READ CAPACITY (16) when (10) says the stick is bigger than it can say (past 2 TB; the partner's 128 GB stick
   answers (10)), READ (10); 512-byte sectors; LUN 0; as `usb0` in diskd.
3. **Writes** (step U4): WRITE (10), SYNCHRONIZE CACHE, after U3 has read sticks right on the real machine.

**Out of the first version:** hubs (one socket, one stick straight in: the partner), keyboards and mice, USB 3, UAS,
isochronous transfers, report-descriptor HID, network and serial adapters, more than one stick, card readers' other
LUNs, 4 KB-sector disks, interrupts (polled, as GENET first: 3.4), Amiga USB cards. A device that is not a stick is
listed by `lsusb` and left alone.

### 3.2 Where it lives: one server, `/sbin/usbd`, a D11 service

D19's rule ("every driver not needed to reach the root is a user-mode process") puts USB in user mode.

| Part | Files | What it does |
|---|---|---|
| the xHCI back end | `usb/xhci-core.c`, `usb/xhci.h` | registers, rings, contexts, commands, events, the root port, control and bulk transfers; plain C whose register and DMA-memory access, clock, cache calls and log come from the includer, as `net/drv/genet-core.c`, so the same file runs on Zenix and against the model |
| the USB core and storage | `usb/core.c`, `usb/usb.h` | the root ports (settling, reset, a device refused once is not tried again until it is pulled), enumeration, BOT and SCSI, the stick's start and reads |
| the server | `usb/usbd.c` | the claim, DMA memory and quiesce (D19 2.4), the posts to and from diskd (3.5), port `usb`, the log |
| a tool | `user/lsusb.c` | the devices as usbd sees them (`-v`: their descriptors); `--hold` keeps usbd running; pages `docs/man/lsusb.txt`, `docs/man/usbd.txt` |

**Who it is:** a service of the service manager (D11), the line `usb -5 /sbin/usbd` in `/etc/svctab`, so its port
is held and only the process the kernel started for it may take it, and the kernel knows it as `svc_pid("usb")`, as
the sound server is `svc_pid("audio")` (D40). That identity, not "root", is what the kernel's device calls check
(3.6). **How it keeps running:** a service is started by a call, and a stick should be served without one, so
`/etc/daemons` gets the line `usb 0 /bin/lsusb --hold` (D24 7: init keeps it running): `lsusb --hold` calls
`USB_HOLD`, which usbd answers only when it ends; then `--hold` calls again, which starts usbd again with D11's
numbers. A plain `lsusb` also starts usbd (on the first real run that is how U1 begins: no line needs to change).
**On a machine without the xHCI** usbd answers every request -ENODEV once and ends with status 0; `lsusb --hold`
takes -ENODEV as "nothing to keep" and ends with status 0, which init takes as "stopped until the next boot". The
`/etc/daemons` line ships commented out until U3 has run on the real machine (as `wasabid`'s).

**Port `usb`'s requests** (`usb/usbproto.h`): `USB_LIST` 1 (a0 an index, a1 a `struct usbdevinfo` of the caller's:
slot, speed, port, kind, a stick's size and label, the device descriptor), `USB_DESC` 2 (the raw configuration
descriptor), `USB_HOLD` 3 (root; answered only when usbd ends; -ENODEV when there is no controller), `USB_STAT` 4
(counters for the tests). Buffers are checked with `SYS_peer` before usbd writes into them.

### 3.3 The host-controller interface inside usbd

The core calls the back end through the `xc_*` functions (`xc_dev_new`, `xc_dev_free`, `xc_set_mps0`, `xc_ep_open`,
`xc_control`, `xc_bulk`, `xc_poll`, `xc_portsc`, `xc_port_ack`, `xc_port_reset`): with one back end a table of
pointers costs bytes and buys nothing; a second back end (an Amiga clock-port card's chip) turns these names into one.
xHCI does the addressing and the scheduling in hardware; an ISP1763 or SL811 would need the core to do more, as added
calls.

### 3.4 The main loop

usbd waits with `sys_evwait` on its port `usb` (requests, and `POLLRDBAND` for diskd's posts) for at most 10 ticks
(100 ms): each wake serves the port, takes diskd's requests (each read into the bounce buffer and answered), and looks
at the root port. **Polled, no interrupt** (as GENET first, D19 5.2): the controller's interrupt reaches the 68k only as
Emu68's level 6, which Zenix leaves alone; the interrupt enable bits stay off. A request waits for nothing: diskd's
post wakes usbd at once. A plug-in is seen within 100 ms, then **settles 100 ms** (USB 2.0 7.1.7.3) before the port
reset, and the device gets **10 ms of reset recovery** (7.1.7.5) before its first request. **A device left alone**
(impossible descriptors, no answer, not ready, a CD-ROM, 4 KB sectors) is said once in the log and never reset again
until it is pulled out: its port's own change events after a reset are no news (revision 1's review found a reset
loop there).

### 3.5 Sticks through diskd

**diskd stays the one disk server** (architecture.md section 9), so `mount`, `fdisk list`, `df`, the overlap and mount
rules, D11's restart record and every filesystem server work on a stick unchanged. The traffic goes both ways by
**posts** (`SYS_post`, D38 step 3: the kernel copies a small record into the receiver's window and wakes it, never
blocks the sender): usbd opens a window the owner of port `disk` may post into; diskd opens one the owner of `usb` may
post into. **diskd may wait for usbd, bounded; usbd never waits for diskd** but at its hello (point 1), when diskd
cannot be waiting for it. One direction of waiting cannot deadlock. The protocol is `kernel/diskproto.h` (`struct
urec`).

1. **The hello:** once usbd can serve, it calls diskd `DISK_USBHELLO` 11 (a0 its port; root, the owner of port `usb`
   only, checked with `SYS_peer`). diskd forgets the stick an earlier usbd told it of (a mount of it stays, answering
   -ESTALE), opens its window and answers at once. usbd knows no stick yet, so diskd is not waiting for it. **A diskd
   that started again has no window:** usbd posts nothing (a post of 0 bytes) once a second while it has said hello;
   that post fails without a window, and usbd says hello again and tells it the stick (the code review found that
   without the heartbeat an idle usbd never noticed).
2. **Records** (32 bytes: kind, seq, unit, lba, count, result, a, b), a ring of `UREC_N` 8 in each window, numbered
   from 1 after the hello; a reader takes the records past the last it took. diskd to usbd: `UR_READ` (lba, count).
   usbd to diskd: `UR_DONE` (a: the request's number, result 0 or -errno), `UR_ADD` (the stick: its size in 512-byte
   sectors, the most a request may ask, 128; a: the bounce buffer, b: its 32-byte label), `UR_DEL`.
3. **A read of `usb0`** is synchronous in diskd, as an IDE read is: it posts `UR_READ` and waits (`sys_evwait` on its
   port for `POLLRDBAND`, other requests left queued) for the `UR_DONE` with that number, **at most 5 s** (then -EIO, a
   log line, and the stick taken as gone). **The data never passes through a client's buffer in usbd:** usbd reads
   into its 64 KB bounce buffer in the DMA region and diskd copies from there into its client's buffer after
   `UR_DONE`, so if diskd or its client dies usbd never writes into memory that was theirs. usbd keeps a whole read
   inside 4 s (1.5 s a bulk stage, tries again only while 4 s have not passed); a read that still ends past diskd's 5 s
   (a stick that answers very late) makes usbd tell diskd the stick again (`UR_DEL`, `UR_ADD`), so the dropped stick
   comes back (its mounts stale).
4. **The partition table is read the same way** (diskd's scan reads `usb0`'s sectors through point 3), so an MBR (the
   PC's sticks: FAT32 0x0C, exFAT 0x07 listed, never mounted yet), an RDB and the 0x76 container work on a stick as on
   the SD card. It is read once a stick: a rescan for `df0` or `cd0` lists the stick's devices as they were read (so
   its slots are kept, revision 1's review, and a slow stick never holds a floppy's open, the code review).
5. **A stick that goes** (`UR_DEL`, or no answer): its slots are kept as *stale* while mounted (the mount answers
   -ESTALE, as D44's changed disc) and freed when unmounted, so a slot number never moves to another disk under a
   filesystem server's handle. A new diskd (D11) keeps a mounted stick's slot in its saved record; the first stick
   the server tells it of takes that mount over (the server cannot tell two sticks apart yet: section 7).
6. **diskd's loop stays one trap a request** (`replyrecv`), with or without a USB server: it looks at usbd's window
   (one compare) each time it wakes. **usbd wakes it after news** with a request diskd answers at once (a `DISK_LIST`
   past the end of its list), after first answering any read diskd may be waiting for. If more than `UREC_N` records
   ever came before diskd took them, it says so, takes the oldest still there, and looks at the stick again.

### 3.6 What the kernel and the boot do

| Change | Where | Bytes (*measured*) |
|---|---|---|
| The hardware table names the xHCI: `xhci` (its 68k address, 0 if none or disabled), `xhci_size` (its `reg` length), `xhci_stop` (what stage0 did: `XSTOP_*`), in place of `reserved[3]` (the table's size and first size do not change) | `kernel/hwinfo.h` | 0 |
| stage0 finds the node through `/__symbols__`' `xhci` (as AmigaOS's driver does; no alias names it), refuses a node whose status is "disabled" (no `otg_mode=1`) or whose window is not 64 KB-1 MB, translates `reg` through `/scb`'s rewritten ranges | `boot/stage0.c` | boot-only |
| **The warm start:** stage0 halts an xHCI AmigaOS's Poseidon left running (its event ring and transfers would go on being written by DMA into what becomes Zenix's memory): Run/Stop and the interrupt enable cleared, at most 100 ms for HCHalted (the beam's clock, the wait shared with `genet-stop.h`). No reset (usbd resets). The last hardware step before the takeover | `boot/xhci-stop.h` | boot-only (stage0 +656 together) |
| `SYS_drv` for the service of port `usb` (`svc_pid("usb")`), **only** `DRV_HWINFO`, `DRV_CLAIM` of the table's `xhci` (its window `xhci_size` long) and its own claim's `DRV_DMAMEM`, `DRV_QUIESCE`, `DRV_CACHE`; anyone else's claim of `xhci` -EPERM. Checked before the network server's checks, so a machine without netd still answers it | `kernel/drv.c` | |
| The network server's end releases only claims its own children hold (it released all but Paula's) | `kernel/drv.c` | |
| **A quiesce step that waits**, `DRVS_WAIT` 3: until `(*addr & mask) == value`, at most 20 ms by the E-clock (an xHCI may take 16 ms to halt, xHCI 5.4.1); one a program. Revision 1 had the kernel wait 20 ms after every quiesce with DMA memory; the code review found that wait runs with interrupts off in `proc_exit`: a wait for the bit itself is microseconds in practice. **A wait that runs out keeps the claim's DMA memory for good** (said in the log): the device may still write there | `kernel/drv.c`, `kernel/netproto.h` | |
| `DRV_DMAMEM` asks fast RAM only (`MEM_FASTONLY`) under Emu68 (`HW_DT`): the Pi's DMA reaches only Emu68's RAM (a fix for GENET too) | `kernel/drv.c` | drv +422 together |
| diskd's `usb0` (3.5) | `user/diskd.c` (built into the kernel image) | +2.3 KB of kernel.bin (diskd.zx 10,464 -> 12,740) |

usbd's quiesce program: USBCMD = 0, a wait for HCHalted, IMAN = IP (writing 1 clears a pending interrupt; IE off).
The kernel runs it when usbd ends and before ZINE (`drv_zine`). **Four claims** (`NCLAIM`): GENET, Paula and the xHCI
take three on the PiStorm; a fourth device there (the PC Card or the parallel port) still fits, a fifth would be
refused -EBUSY.

### 3.7 ZINE, restarts, pulling a stick

- **ZINE** (D29 7.2): `drv_zine` runs the quiesce and kills usbd; after the program `lsusb --hold` starts it again,
  which resets the controller and finds the stick again (about 1 s); diskd marks it stale and back, so a mount of it
  answers -ESTALE and must be made again (said in the log). The kernel counts the kill as an end (D11): **five ZINE
  programs within 60 s give USB up** until root registers `usb` again (`svc`) or the next boot. A limit, written in
  `usbd(8)`.
- **usbd crashes:** the kernel's quiesce stops the controller; D11 restarts it at `--hold`'s next call; diskd's waiting
  read ends in -EIO after at most 5 s.
- **A stick pulled while mounted:** its requests fail with -EIO, the mount answers -ESTALE, the filesystem's own dirty
  state is its own (read-only: none).

## 4. Cache, DMA and byte-order rules under Emu68 (binding for the build)

From the knowledge base (`amiga/emu68-network-usb-rtg-hardware.md` section 1, `amiga/genet-under-emu68.md` "Cache
and DMA", `amiga/emu68-jit-cache-rules.md`) and D19 2.4, applied to xHCI's structures:

1. **Everything the controller reads or writes lies in the claim's DMA region** (`DRV_DMAMEM`: fast RAM, Emu68's,
   64-byte aligned; freed by the kernel only after the quiesce and 20 ms, 3.6). usbd asks 4 KB more than it needs and
   lays its structures out from the first 4 KB boundary: the scratchpad pages on page boundaries; the DCBAA, the
   segment table, the control buffer, the CBW and CSW buffers and each slot's input and output contexts (32 or 64
   bytes a context by HCCPARAMS1.CSZ) each inside one page; every ring inside one page (a link TRB at its end); the
   **64 KB bounce buffer on a 64 KB boundary** (64 KB of slack asked for it), so one TRB of at most 64 KB never crosses
   a 64 KB boundary (xHCI 6.1, 4.11.7.1) and a bulk transfer is always one TRB. The DMA
   address is the 68k address (2.1); HCCPARAMS1.AC64 is read and the high address words written 0.
2. **Byte order:** every register, TRB, context, DCBAA entry and segment-table entry is little-endian. **The driver
   never touches DMA memory or registers through a C pointer**: DMA memory is a 32-bit bus address, read and written
   only by the includer's `dm_r32`/`dm_w32` (and registers by `rg_r32`/`rg_w32`), which swap on the 68k. On the PC the
   same addresses are the model's, not host pointers, so a stray direct access fails there at once instead of
   passing on a little-endian PC by luck.
3. **The data cache is not coherent** (the ARM's; the 68k's own is off on every Zenix machine, start.S, except D37's
   040/060 fast-RAM cache, which the same calls cover). Never `cinva` (it drops dirty data on Emu68): every
   invalidate is a clean and invalidate (D19 step 7's `DRV_CACHE`).
4. **Whatever the CPU writes and the controller reads** (TRBs, input contexts, the DCBAA, the segment table, the CBW): written, then cleaned, **then** the doorbell or the register write that hands it over.
5. **Whatever the controller writes and the CPU reads** (the event ring, output contexts, the sectors and the CSW, scratchpad
   pages):
   cleaned and invalidated when it is handed to the controller, and cleaned and invalidated again before every CPU
   read (the ARM may have loaded the line meanwhile). The event ring is read one 64-byte line (four TRBs) at a time,
   each line invalidated before its cycle bits are looked at.
6. **No line is shared** between something the CPU writes and something the controller writes: rings the CPU produces
   and the event ring it consumes are separate pages; input contexts (CPU) and output contexts (controller) are
   separate 64-byte-aligned blocks.
7. **IN buffers are whole lines** (the bounce buffer, the CSW's 64 bytes). usbd copies out only the length the event
   reports; diskd copies out of the bounce buffer only after `UR_DONE`, which usbd posts after its invalidate.
8. **How the calls are made:** as the GENET driver chose (D19 step 9 choice 3): Emu68's range opcodes from usbd itself
   when a child test (`usbd --probe-rangeops`) shows they work in user mode, else `DRV_CACHE` (one system call a
   range). **The self-test** before any device is touched: a **No-Op command** is placed on the command ring and
   cleaned; the event ring's first line is then *read by the CPU* (so the line sits in the cache with its stale,
   cycle-bit-wrong contents) and only then the doorbell is rung; usbd waits up to 100 ms, invalidating that line
   before each look. A completion event with the right TRB pointer proves both the clean (the controller read the
   TRB) and the invalidate (the CPU saw the controller's write past its own cached copy). A missing clean or a missing
   invalidate shows as **no event** within 100 ms: usbd then halts the controller (the stale TRB may still run later),
   resets it (halted first: a stale command may still run), tries `DRV_CACHE`, and refuses when that fails too. Whether the range opcodes work in user mode was the
   GENET checklist's open question (`docs/a1200-checklist-network.md` step 2); its answer is not written down anywhere
   I found: the main session should record it.

## 5. Sizes (measured 2026-11-12, this branch)

| Part | Lines of C | On the disk | Memory while running |
|---|---|---|---|
| `/sbin/usbd` (xHCI back end ~710 lines, core and storage ~440, server ~380) | ~1,530 | 23,064 bytes | its code and data, a 8 KB stack, and the DMA region: `(5 + scratchpad pages + 4) x 4 KB` plus 128 KB (the 64 KB bounce buffer and its alignment): **164 KB** without a scratchpad |
| `lsusb` | ~90 | 8,964 bytes (libz's share) | while it runs |
| **kernel.bin** | | **193,948 -> 197,316 (+3,368)** | |
| ... the built-in diskd's `usb0` (3.5) | ~140 | +2.3 KB (diskd.zx 10,464 -> 12,740: code and its relocations) | +0.6 KB |
| ... stage0's lookup, size check and halt | ~45 | +664, **boot-only** (given back after the boot by D45) | 0 |
| ... `drv.o`: the service's claim, `DRVS_WAIT`, fast RAM, netd's end | ~45 | +422 | |

The budgets were raised in their own commit (`tools/ksize-budget`: TOTAL 197,400, start 4,850, drv 3,100, embed
48,600; the main session accepted the bytes) and the mount tests' RAM disk went from 688 to 685 KB (Makefile; 1,340
bytes left on their floppy against `ROOM`'s 800, the RAM disk 105 blocks free). Revision 1's estimates (about 120-200 resident bytes,
diskd 1.2-1.8 KB) were for the keyboard path; the stick's diskd part came in at its upper end.
On a machine without the xHCI usbd starts once and stops; its file sits on the disk.

## 6. Testing

### 6.1 On the PC

**The controller and a stick** (`tools/xhci-model.h`, `tools/usb-host-test.c` and `.sh`, ASan and UBSan): the model
in the style of `tools/genet-model.h`, strict, counting as a fault everything the real chip would do wrong with or that
would corrupt memory, and the same `usb/*.c` compiled against it.

- **The controller:** capability, operational, runtime and doorbell registers; Run/Stop, HCHalted, the reset, CNR; the
  command ring (Enable/Disable Slot, Address Device, Configure Endpoint, Evaluate Context, Reset/Stop Endpoint, Set TR
  Dequeue, No-Op); the event ring with its segment table and ERDP; transfer rings with link TRBs and the cycle bit;
  control transfers in three stages, bulk IN and OUT, short packets, stalls (the endpoint halted until Reset
  Endpoint, the device's until CLEAR_FEATURE), babble when a device sends more than the TD holds, a device removed
  mid-transfer; the root port's change bits (write 1 to clear); **the data toggles on both sides** (the device's set
  to DATA0 by CLEAR_FEATURE and SET_CONFIGURATION, the host's by Reset Endpoint and by dropping and adding the
  endpoint; packets with the wrong toggle thrown away, as a real device and controller do).
- **The cache between the CPU and the DMA** (the CPU's view, RAM, lines the CPU wrote and has not cleaned, lines the
  controller wrote): a missing clean is a fault and stale data for the controller, a missing invalidate stale data for
  the CPU; a dirty line written back over the controller's data is a fault.
- **The memory rules and the byte order** as faults: a TRB's buffer across 64 KB, a ring, context or DCBAA across a
  page, an address outside the memory; the memory is bytes, little-endian, at the model's own addresses (never host
  pointers, 4.2).
- **The stick** (`MD_MSC`): bulk-only transport (CBW, data, CSW, tags), SCSI's INQUIRY, TEST UNIT READY, REQUEST
  SENSE, READ CAPACITY (10) and (16), READ (10), its sectors from an image file (8 MB of random bytes, so no two
  sectors are alike); and **misbehaving sticks**: not ready for a while, never ready, no medium, a CD-ROM, 4 KB
  blocks, a size that says "more" (10) or wraps (16), past 2 TB, a read failing once or always, a data stage that
  stalls, a status with a wrong signature or tag, a phase error, babble, a status that never comes, a stick pulled in
  the middle of a read, one that stalls GET_DESCRIPTOR, one that never answers, an impossible EP0 packet size,
  damaged configurations, a device that is not a stick, 300 random configuration descriptors.
- **Checked** (53 checks, compared, never only printed; without the host-side toggle reset of the reset recovery two
  of them fail, tried): the start, its self-test failing without cache calls and
  without invalidates, registers reading all ones; the stick's settling, enumeration, label and size; 1 MB read in
  128-sector requests and odd sizes compared with the image; every misbehaving stick refused or recovered (reset
  recovery, halts cleared on both sides) with the right line said, its port reset once only, nothing told to diskd,
  and it reading again after; nine reads in a row after the recoveries with no packet thrown away; a stick pulled in
  the middle of a read noticed at once between the stages; a status that never comes ending inside diskd's 5 s; a
  refused device pulled and plugged in again served; no model fault anywhere.

**diskd's side** (`tools/disk-host-test.sh`, a stub of usbd behind the posts, the stick an image with an MBR, a FAT32
and a Linux partition, 21 checks): nothing before the hello; a hello from another process refused; `usb0`, `usb0p1`
(FAT32) and `usb0p2` (ext2) listed read-only; sectors compared with the image; past the end, more than 128 sectors and
writes refused; the server's -EIO passed on; a rescan keeps the stick's devices; a server that stops answering gives
-EIO after the wait and nothing hangs; a stick pulled under a mount answers -ESTALE and its slot goes with the unmount;
a restarted diskd keeps the mount's slot and the server's heartbeat (its post failing without a window) says hello
and brings the stick back to it; an answer that comes while diskd waits; one that never comes (-EIO after 5 s, the
stick dropped); more news than the window holds (said, the newest taken); news before a hello ignored. The stub
answers inside the post or at a later wait; diskd's real main loop is not run on the PC (it is in FS-UAE, without a
stick).

**stage0** (`tools/stage0-test.sh`, the CM4's real device tree): 0xF89C0000, 1 MB, a disabled node gives none, the
halt's bits (running, halted already, never halts, not an xHCI), the dry run writes nothing.

**In FS-UAE** (no USB there), `tools/usb-test.sh`: `lsusb` on a machine without the xHCI starts usbd, which says so
once in the log and stops with status 0, and `lsusb` says "no USB controller" (exit 1); `lsusb --hold` ends with 0;
`/sbin/usbd` started by hand cannot take port `usb`; a second `lsusb` gets the same answer. The disk and mount tests
(`mounttest`, `disktools`, `mbr-test`, `cd-test`) cover diskd's unchanged paths.

### 6.2 The first run on the partner's PiStorm, step by step

Written for a short session; each step logs to the kernel log (`dmesg`, and the serial cable when it is in) with a
timeout on every wait, and each can be the last one of a session. A checklist file like
`docs/a1200-checklist-network.md` is written with it.

0. **Before Zenix** (AmigaOS, the partner): the stick works under Poseidon; `config.txt` keeps `otg_mode=1`. A
   **scratch stick formatted FAT32** for the test (the partner's exFAT stick is listed but not mounted until the exFAT
   item, section 10; it is never written in any case: read-only).
1. **The table:** `zenixboot CHECK` (the dry run) shows `xhci 0xf89c0000` and its size (or "no xHCI: disabled"). Then
   a normal boot from the Workbench with Poseidon running: the boot log's `xhci: stopped (it was running)` line (the
   warm start), and Zenix runs on without memory damage (the network checklist's step 6, "Memory").
2. **U1, nothing plugged in:** `lsusb` as root starts usbd (the daemon line still commented); its log: the
   capability values (HCIVERSION, MaxSlots, MaxPorts, scratchpad pages, AC64, CSZ), the cache way chosen, `self-test:
   a No-Op command answered`, and lsusb says "no USB devices". Then `kill` usbd: `drv: claim ... released: 3 quiesce
   steps run`, and `lsusb` starts it again.
3. **U3, the FAT32 stick:** plugged in, `dmesg`: its speed, `04xx:xxxx`, the storage lines (label, size). `lsusb -v`
   (compared with Linux's `lsusb -v` on the PC for the same stick). `fdisk list usb0` shows its partition; `mount -t
   fat -o ro usb0p1 /mnt/usb`; `md5sum` of files compared with the PC's; `time cat big > /dev/null` (an Emu68 figure,
   labelled so); the stick pulled while a `cat` runs (an error, nothing hangs, usbd and diskd live); plugged in again
   and mounted again.
4. **The exFAT stick:** plugged in: `usb0` and `usb0p1` (type 0x07) listed, not mounted (the next item).
5. **Recovery:** usbd killed under a `cat` (the read fails within 5 s, `lsusb --hold`'s restart, the stick back); a
   ZINE demo started and left (the stick back, the mount stale); reboot to AmigaOS: Poseidon finds the stick again
   (its driver resets the controller Zenix left halted).

Then the daemon line is uncommented in `/etc/daemons` and the checklist's results go into this note.

## 7. Risks

1. **Real hardware only.** Everything past the model is first seen at the A1200; the model shows the driver matches
   the specifications and U-Boot's order, never that the BCM2711's xHCI or the partner's stick behave the same.
   Mitigation: the strict model, U1 with nothing at stake, logs at every stage.
2. **The warm start:** AmigaOS's xHCI may be mid-transfer when zenixboot takes over; stage0's halt must come before
   Zenix uses the memory (as GENET's). Poseidon's task runs until the takeover and has a watchdog: if it noticed the
   halt and started the controller again before AmigaOS stops, DMA would run on. So the stop is stage0's last
   hardware step, and usbd's start halts a controller it finds running before it writes anything. A controller that
   does not halt in 100 ms is logged; usbd then refuses USB rather than reset one whose DMA is live.
3. **Cache rules wrong for the xHCI:** the No-Op self-test (4.8) shows a missing clean or invalidate as no event before
   any device is touched; the sectors themselves are checked by `md5sum` against the PC at step 3.
4. **Range opcodes in user mode** not known (4.8): `DRV_CACHE` works either way, at a system call a range.
5. **Sticks are quirky** (TEST UNIT READY needed several times, READ CAPACITY failing first, 4 KB sectors, card
   readers with several LUNs): handled as section 3.1 says, more after real sticks show them. rondoval's driver README
   warns "data corruption is possible in edge cases": read-only, and writes (U4) on a scratch stick.
6. **diskd's loop with a USB server:** one compare more a request; a wake call from usbd after news. Speed is not
   measured on the PiStorm yet (Emu68 figures would say nothing of a real 68020 anyway, and only the Pi has it).
7. **Two sticks cannot be told apart yet:** a stick pulled and another put in while diskd restarts could take the old
   one's mount (3.5 point 5). `UR_ADD` can carry the serial number (the string descriptor) later; until then a restart
   of diskd under a mounted stick is rare (diskd ends only on a crash).
8. **The bounce buffer after usbd's death:** diskd copies after `UR_DONE`; if usbd died between its post and diskd's
   copy, the region could be freed (after the quiesce and the reap) and diskd would hand out stale bytes. The window
   is microseconds against a freeing that comes after the reap; a later step can keep the region until diskd says it
   copied.
9. **Emu68 or firmware changes:** a new Emu68 may place `/scb` elsewhere (never hard-coded: read from the device
   tree), or a firmware may stop enabling the xHCI node (then the table says "disabled" and usbd says why). A
   controller left unpowered may answer all ones (usbd stops), or fault the ARM on a read: only U1 shows.
10. **Speed:** one request at a time (BOT), 64 KB at most, polled: fine for files, slow for a 128 GB stick's whole
   surface. UAS and bigger transfers are later.
11. **ZINE and D11's count:** five ZINE programs within 60 s give USB up (3.7).
12. **Power:** a bus-powered hard disk may not spin up on the CM4's port; the port's over-current bit is logged.

## 8. Questions for the partner, and the answers

Asked by the main session; the partner's answers, 2026-11-12, as relayed:

1. **Which USB devices would you plug in?** "Mostly USB sticks, mainly a 128 GB Corsair GTX formatted exFAT."
2. **How many USB sockets on the PiStorm's I/O board?** "The PiStorm has 1 USB socket."
3. **Do you use USB under AmigaOS today?** "On AmigaOS they use Poseidon only for USB sticks."
4. **An Amiga USB card for the Apollo 1260?** "No USB card for the 1260."

What the main session decided from them (2026-11-12, docs/decisions.md): sticks first and only, read-only; no
keyboards, mice or hubs for now; exFAT next, as its own item. Earlier answers of the main session: the kernel's bytes
accepted; read-only first.

Open: none for U1 and U3. For U4 (writes): which stick may be written to in a test (a scratch one).

## 9. Build order (each step merged with its tests)

1. **U1:** the controller, its self-test, the root port; the kernel's `SYS_drv` change, `DRVS_WAIT`, the fast-RAM
   rule; stage0's table and halt; `/sbin/usbd`, `lsusb`. Real run: section 6.2 steps 0-2.
2. **U3:** mass storage read-only and diskd's `usb0`. Real run: steps 3-5. (U1 and U3 are built together on branch
   `usb`, 2026-11-12, as the main session asked.)
3. **exFAT** (section 10), read-only.
4. **U4:** writes (WRITE (10), SYNCHRONIZE CACHE, diskd's `DISK_WRITE` and `DISK_FLUSH` for `usb0`). Then the daemon
   line on by default on the PiStorm's images.
5. Later, when wanted: hubs, keyboards and mice (revision 1's design, a51ea90), UAS, an Amiga USB card.

## 10. Next: exFAT

The partner's stick is exFAT, so a stick is of little use to them until Zenix reads exFAT. **A separate item, not
built here.** What is known (*read*): Microsoft published the exFAT specification openly in 2019 (learn.microsoft.com,
"exFAT file system specification") and put its exFAT patents into the Open Invention Network's pool; the knowledge
base's `amiga/zenix-filesystems-research.md` section 3 has the format notes and already proposed "each as a separate
server, loaded on demand". Against D30's `fatd` (*read*, docs/d30-fat.md): exFAT shares only the idea of a FAT. It has
an allocation bitmap instead of free entries in the FAT, files that need no FAT chain at all (NoFatChain, contiguous),
directory entry sets with a checksum (a file entry, a stream extension, name entries of 15 UTF-16 characters), an
up-case table on the volume for case-folding names, 64-bit sizes, a boot region with its own checksum, and no 8.3
names, no VFAT long names, no FSInfo. `fatd`'s name, inode, crash-order and dirty-mark rules are about structures
exFAT does not have. So **proposed: its own server, `/sbin/exfatd`, read-only first**, sharing with fatd only what is
truly common at the source level (the sector cache, the FS protocol's request loop, the UTF-16 to UTF-8 conversion,
D13's shared code if both are loaded), and `mount` learning the type from MBR type 0x07 (which NTFS shares) plus the
boot sector's "EXFAT   " name. A design note first (D47), then the build, tested on the PC against images made by
Linux's `mkfs.exfat` and checked with `fsck.exfat -n`, as fatd's are with `fsck.fat`.

## 11. Sources

- The partner's card (backup image of 2026-11-06): `config.txt`, `bcm2711-rpi-cm4.dtb` (decompiled with `dtc`).
- `github.com/rondoval/emu68-xhci-driver`, branch `context_release` (2026-11-02): README (units, requirements),
  `xhci.device/src/unit.c` (unit 0's device-tree symbol and status check), `xhci-dma.c` (cache calls), read for facts;
  GPL-2.0+ from U-Boot. `github.com/rondoval/emu68-driver-stack` README and release notes (Neptune, xhci 6.x).
- `github.com/PiStorm/cm4board`, `CM4_PiStorm32Lite` (v1.1): README and the KiCad board's USB nets.
- Raspberry Pi `config.txt` documentation, `otg_mode`.
- amiga-news.de 2023-03 (the CM4 adapter and the I/O board); icomp wiki "RapidRoad"; Big Book of Amiga Hardware
  (Subway, UltraBus); amiga.resource.cx (Thylacine, Deneb); Wikipedia "Clock port".
- Public specifications built from: Intel's xHCI 1.2, USB 2.0 (chapter 9; 7.1.7 for the timings), USB Mass Storage
  Class Bulk-Only Transport 1.0, SCSI SBC-3 and SPC-4 (the commands of 3.1); Microsoft's exFAT specification for
  section 10. U-Boot's
  `drivers/usb/host/xhci*.c` and `common/usb*.c` (GPL-2.0+) for the order of operations, read, no code copied (as D19
  step 9 did for GENET).
- Knowledge base: `amiga/emu68-network-usb-rtg-hardware.md`, `amiga/genet-under-emu68.md`,
  `amiga/emu68-jit-cache-rules.md`, `amiga/emu68-and-pistorm.md`, `amiga/zenix-filesystems-research.md` section 1.

## 12. Reviews

Revision 1 had two independent reviews (2026-11-12), read against the code. Both confirmed: a server may keep a
message and answer it later (`ipc_reply` finds any message it is serving); diskd, owner of `disk`, may post into a
window opened for `disk`; `CON_INPUT` is root's on any console and its raw keys follow the desktop's grab; the
`CONI_HELD` reasoning matches `kbd_inject`; `drv_exit` and `drv_zine` were read right; status 0 is "stopped" for
init; the measured room figures. What they found, and what the keyboard-and-mouse revision 2 did (its rows about
keys, the mouse and hubs went with that scope; the rest stand, and the section numbers are that revision's):

| Finding | Severity | Revision 2 |
|---|---|---|
| A stick's add deadlocked: diskd's scan reads sectors synchronously from usbd, which waited in its `DISK_USBADD` call | must-fix (both) | 3.5: all traffic by posts, usbd never waits; diskd waits for usbd, bounded |
| A rescan for `df0`/`cd0` dropped the `usbN` slots | must-fix | 3.5.3: the scan is per medium |
| A diskd restart failed held requests while usbd still copied into the clients' buffers; slot numbers could move under a mounted handle | must-fix (both) | 3.5.2, 3.5.5: diskd does the copies; stale slots kept in the saved record; HELLO records |
| diskd's `sys_recv` without a timeout: requests hung when usbd died | must-fix (both) | 3.5.4: `sys_evwait` with a timeout, a 5 s bound per request |
| The `SYS_drv` gate "owner of port `usb`" was weaker than D19/D40's service identity and gave every op | must-fix (both) | 3.2, 3.6: usbd is the D11 service `usb`, gated by `svc_pid`, the xHCI claim and its ops only, checked before netd's checks |
| diskd is built into the kernel image: its USB part is kernel bytes | must-fix (both) | section 5: U3's diskd part counted in `kernel.bin`, its room decided at U3 |
| Caps Lock is a toggle on the Amiga (`caps_lock = !up`) | must-fix | 3.4: usbd toggles |
| `NCLAIM` 4 | should-fix | 3.6: three on the PiStorm, the limit written |
| DMA memory freed before the controller halts; `DRV_DMAMEM` could fall back to chip RAM | should-fix (both) | 3.6: a 20 ms wait after the quiesce, fast RAM only under Emu68 |
| The window is 1 MB, not 64 KB; no field left for the size | should-fix (both) | 3.6: `xhci_size` kept instead of the unused interrupt number |
| Keys and buttons stuck when usbd dies; a press and release in one tick lost; `kbd_inject` may take fewer codes | should-fix (both) | 3.4: the kernel releases at the caller's end; one button change per tick; the rest resent |
| ZINE kills count as D11 ends | should-fix (both) | 3.7: written as a limit |
| The DMA region did not add up; 64 KB and page rules | should-fix (both) | 4.1, section 5 |
| The model's estimate was low | should-fix (both) | 6.1: 2,000-2,500 lines |
| Byte order not mentioned | should-fix | 4.2: no pointer access to DMA memory or registers |
| The No-Op test proved the invalidate only with the line cached before the doorbell; a missing clean gives no event | should-fix | 4.8 |
| The cold start and the USB power domain | should-fix | 2.1, risk 7 |
| Neptune's watchdog during the warm start | should-fix | risk 3 |
| The reason given for "diskd never calls usbd" was wrong (usbd was then no service) | should-fix | 3.5: the real reason (one-way waiting) |
| Right Amiga key exists (0x67); a hub's slot context and route string; the 20-byte record; IMAN's IP bit is write-1-to-clear; stage0 needs a `/__symbols__` lookup; "last item" of block 6 | nits | 3.4, 6.1, 3.5.1, 3.6, header |

**The code review of the first build** (keyboards and mice, 2026-11-12, one independent reviewer, read against the
xHCI specification and the code; a test case of its own confirmed the first finding): a device refused on a root port
was reset and enumerated again forever (the reset's own port-change event set the port's bit again): fixed for the
stick build by the refused-port bit and the settle timer (3.4), with root-port cases in the host test; no connect
debounce or reset recovery (USB 2.0 7.1.7.3, 7.1.7.5): added; `xc_start` wrote its structures before halting a
running controller (a stale command could pass the next self-test): it halts first now; the kernel's 20 ms wait ran
with interrupts off in `proc_exit`, per claim: replaced by the quiesce's `DRVS_WAIT` step; stage0 took whatever cell
came last as the window's size: checked (64 KB-1 MB) now; xhci-stop.h and `xc_probe` disagreed on the versions
accepted: the same now; `1u << 32` for a port 32: ports 1-31; an "invalid" completion code read as -EPERM: a plain
failure now; usbd by hand with no netd said "no USB controller": says "not started by the service manager" now. Its
findings about the keyboard and mouse code (the global held-key state, the report lengths, the hub's change bits,
Caps Lock's per-device state) went with that code. Confirmed by it: the `SYS_drv` gate, `drv_exit`'s use of the
holders' parents, the quiesce's byte order, the ring and event-ring arithmetic, the control stages, the contexts, the
PORTSC write mask, the scratchpad and page layout, the cache rule set, the descriptor bounds.

**The code review of the stick build** (2026-11-12, one independent reviewer, read against BOT 1.0, USB 2.0 and the
code): a restarted diskd never got the stick back (an idle usbd never posted): usbd's heartbeat, 3.5.1; usbd's
timeouts (10 s a bulk stage, three tries) ran far past diskd's 5 s and a dropped stick never came back: 1.5 s a
stage, 4 s a read, and the stick told again after a late read, 3.5.3; **the data toggle went out of step after the
reset recovery** (CLEAR_FEATURE sets the device's to DATA0, the host's was left): the endpoint dropped and added after
every CLEAR_FEATURE, and toggles in the model, which catches it; more records than the window holds stopped the
reader for good: the gap taken, 3.5.6; a stick pulled mid-read ran all its tries: the port looked at between the
stages; a quiesce wait that ran out was silent and the memory freed: said, the memory kept, one wait a program; every
scan re-read the stick, blocking the disk server: read once a stick; the root disk's cost after a hello (reply +
recv + evwait): back to one trap a request, usbd waking diskd after news; the tests skipped diskd's waits: late
answers and floods in the stub; a pulled stick's stale mount blocked a new one: a stale mount blocks nothing;
stage0 took the last cell of `reg`: the first entry's, by `#size-cells`. Confirmed by it: no lost wakeup in diskd's
waits (POLLRDBAND counts posts), the unchanged path without a USB server, the hello's trust, the gate, no reset loop on
a root port, the cache calls, the bounce buffer's fit, the BOT flow (6.7.2).

## 13. As built: steps U1 and U3 (branch `usb`, 2026-11-12)

**Nothing has run on a real PiStorm.** Files: `usb/xhci.h`, `usb/xhci-core.c` (the controller), `usb/usb.h`,
`usb/core.c` (root ports, enumeration, BOT and SCSI), `usb/usbproto.h`, `usb/usbd.c` (the Zenix side: registers and
DMA memory through byte-swapping accessors, the cache calls, the claim, the quiesce, the hello and the posts, the
loop), `user/lsusb.c`; `boot/xhci-stop.h` and stage0's lookup; `kernel/drv.c`, `kernel/hwinfo.h`, `kernel/netproto.h`
(`DRVS_WAIT`); `kernel/diskproto.h` (`DISK_USBHELLO`, `struct urec`), `user/diskd.c` (`usb0`); `tools/xhci-model.h`,
`tools/usb-host-test.c` and `.sh`, `tools/host/diskd-host.c`'s usbd stub with its checks in `tools/disk-host-test.sh`,
`tools/stage0-test.c`, `tools/usb-test.sh` with `tools/tests/usb-test.txt`; the `usb` package family
(`tools/mkzup.py`), `/sbin/usbd` on the images (`tools/mkroot.sh`), the lines in `/etc/svctab` and (commented)
`/etc/daemons`; pages `usbd(8)`, `lsusb(1)`. Where the code differs from sections 3-5 it is written there (3.6's
`DRVS_WAIT` in place of revision 1's 20 ms wait, 5's measured sizes).
