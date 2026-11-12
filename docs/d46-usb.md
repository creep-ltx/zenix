# D46: USB - a host controller server, keyboards, mice and sticks

Roadmap block 6, "USB" (docs/ROADMAP.md "The order of work", approved by the owner 2026-10-25; its last item).
Revision 1, 2026-11-12, branch `usb`: **a design study only, nothing is built.** No emulator offers USB on an Amiga, so
every line of a USB driver can only be proved on a real machine, and real-hardware time is scarce: this note says what
to build so the first run on the partner's PiStorm is short and every step of it says what it found. Each fact is
marked *verified* (seen on the real machine, its card or a file from it), *read* (from a source named next to it) or
*proposed* (this note's choice).

## 1. In plain words

- **Where USB exists today:** only on the partner's PiStorm A1200, and there exactly **one USB 2.0 port**: the
  CM4's own, wired out through the PiStorm's CM4 adapter to the I/O board on the trapdoor (section 2). The A1200 with
  the Apollo 1260 has no USB unless an Amiga USB card is fitted (none is known: question 4).
- **What a first version gives:** a USB keyboard and mouse typing and pointing in Zenix exactly as the Amiga's own (the
  same keymap, Amiga+F1..F10, the desktop's pointer), through a hub when there is one; then a USB stick as a disk
  (`usb0`, `usb0p1`, mounted by the filesystems Zenix already has: FAT, ext2, ZenixFS, FFS, PFS3), read first, then
  written.
- **How:** one user-mode server, `/sbin/usbd`, owns the controller (the D19 driver pattern: a kernel claim, DMA memory
  that belongs to the claim, a quiesce program); keys and mouse movement go into the kernel's existing input path;
  sticks reach the filesystems through diskd like every other disk.
- **What the kernel pays:** about 120 bytes resident and about 300 bytes of boot-only code that D45 gives back after
  the boot (section 5). Nothing on a machine without the controller: usbd is not started there.
- **How it is tested before the hardware:** the controller, a hub, a keyboard, a mouse and a stick are modelled on the
  PC, strictly, as `tools/genet-model.h` models the Ethernet chip; the same driver C runs against the model (section 6).

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
| Under Emu68 the xHCI's registers appear at **0xF89C0000** (bus 0x7E9C0000 in `/scb`'s first range, which Emu68 moves to 0xF6000000, as GENET's 0x7D580000 appears at 0xF7580000) | *read* and worked out: `/scb` ranges in the card's device tree + the Emu68 range rewrite (knowledge base `amiga/emu68-network-usb-rtg-hardware.md`); **never hard-coded**: stage0 reads it (section 3.6) |
| The adapter wires the CM4's USB 2.0 pair (module pins 103/105) and the OTG ID line (pin 101) to its 40-pin FPC connector (pins 14, 15, 35), and nothing else of USB | *read*: the CM4 adapter's KiCad board, `github.com/PiStorm/cm4board`, `CM4_PiStorm32Lite` (v1.1, 2025-03-31) |
| The FPC goes to an **I/O board** on the trapdoor with HDMI, Ethernet and USB sockets; **whether it has a hub chip, and how many USB sockets, is unknown** | *read*: amiga-news.de 2023-03 (the adapter by Michal Schulz, the I/O board by Claude Schwarz), retailers' pages; no schematic found. **Unknown, ask** (question 2) |
| Do any USB devices work there under AmigaOS today | **unknown, ask** (question 3) |
| DMA: the xHCI is an `/scb` bus master like GENET; `/scb`'s `dma-ranges` map bus = ARM physical for the first 16 GB, so the 68k address of Emu68 RAM is the DMA address, as for GENET; it reaches no chip RAM | *read*: the card's device tree; knowledge base `amiga/genet-under-emu68.md` "Cache and DMA" |
| The ARM's data cache is not coherent with the xHCI's DMA (no `dma-coherent` in its node) | *read*: the card's device tree; the xhci driver's `xhci-dma.c` cleans and invalidates around every transfer |

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
fit it as it is. Whether the partner owns one: **unknown, ask** (question 4). None of these is an xHCI: each would be
its own host-controller back end under the same USB core (section 3.3), built only when a card is in hand.

## 3. The design

### 3.1 Scope of the first version (smallest useful), and the order

1. **The controller alone** (step U1): stop, reset, start, rings, a No-Op command answered, the port's status, a
   device's descriptors read and printed by `lsusb`. Nothing is driven yet: this step proves the DMA, the cache rules
   and the rings on the real chip with nothing at stake.
2. **Hubs, keyboards, mice** (step U2), all in the HID **boot protocol** (fixed 8-byte keyboard reports, 3-4 byte mouse
   reports: no report-descriptor parser), through USB 2.0 hubs (low/full-speed devices behind a high-speed hub use the
   hub's transaction translator, which xHCI handles in the slot context).
3. **USB sticks, read only** (step U3): mass storage, bulk-only transport (BOT) with SCSI's transparent command set
   (INQUIRY, TEST UNIT READY, REQUEST SENSE, READ CAPACITY(10), READ(10)), 512-byte sectors, as `usb0` in diskd.
4. **Writes** (step U4): WRITE(10), SYNCHRONIZE CACHE, after U3 has read sticks right on the real machine.

**Keyboard and mouse first** because: they are the smallest slice that uses all three transfer kinds the later steps
need (control, interrupt; bulk is the same ring code); a mistake shows at once and harms nothing (a wrong key, not a
damaged stick); they need no diskd change and almost no kernel change (3.4); and a step that ends with "the key typed
appears" fits one short session at the real machine. **Hubs come with them** because the CM4 has one port: a keyboard
and a mouse together need a hub (on the I/O board, in the keyboard, or a separate one), unless they share one receiver.
Storage comes after because it is bigger (diskd's side, the SCSI layer, errors and quirks) and because writing to
someone's stick is the one step that can lose data.

**Out of the first version** (*proposed*): USB 3, isochronous transfers (audio, webcams), UAS, report-descriptor HID
(mouse wheels, media keys, game pads, touch screens: boot protocol has none of them), Ethernet and serial adapters,
printers, a USB root, suspend and link power management, interrupts (polled first, as GENET: 3.5), the 1260's cards.

### 3.2 Where it lives: one server, `/sbin/usbd`

D19's rule ("every driver not needed to reach the root is a user-mode process") puts USB in user mode. One process,
because keys must not wait for a message round trip between a controller process and a class process, and because
on a 4 MB machine a second process costs its stack and table slots for nothing:

| Part | Files (*proposed*) | What it does |
|---|---|---|
| the xHCI back end | `usb/xhci-core.c`, `usb/xhci.h` | registers, rings, contexts, commands, events, ports; plain C whose register access, clock, cache calls and log come from the includer, as `net/drv/genet-core.c`, so the same file runs on Zenix and against the model |
| the USB core | `usb/core.c`, `usb/usb.h` | enumeration (reset, address, descriptors, configuration), the device table, hubs (`usb/hub.c`), the host-controller interface a later clock-port back end also fills (3.3) |
| the classes | `usb/hid.c`, `usb/msc.c` | boot keyboard and mouse to the kernel's input (3.4); BOT + SCSI to diskd (3.5) |
| the server | `usb/usbd.c` | the claim, DMA memory and quiesce (D19 2.4), the main loop (3.5), port `usb` for `lsusb`, the log |
| a tool | `user/lsusb.c` | the device tree as usbd sees it (`-v`: the descriptors), with its page `docs/man/lsusb.txt` |

**How it is started:** a line in `/etc/daemons` (D24 7: "servers nobody calls, kept running by init"):
`usbd 0 /sbin/usbd`. init starts it after `ifup -a` and starts it again when it ends (D11's numbers). On a machine
whose hardware table names no xHCI, usbd says so once in the log and ends with status 0, which init takes as "stopped
until the next boot": it costs nothing there. Shipped commented out until U2 has run on the real machine (as
`wasabid`'s line). usbd creates the port `usb`, a name reserved for root as `net` and `audio` are.

### 3.3 The host-controller interface inside usbd

The core talks to a back end through a small table of functions at U-Boot's level (*proposed*): `hc_init`,
`hc_poll` (events into completions), `port_status` / `port_reset` / `port_power` (the root hub), `dev_new` and
`dev_free` (xHCI's Enable Slot / Address Device / Disable Slot; a clock-port chip's no-ops), `ep_open` (Configure
Endpoint), `submit` (control, bulk, interrupt; a callback on completion), `hc_stop`. xHCI does the addressing and the
scheduling in hardware, an ISP1763 or SL811 needs the core to do more: the interface is shaped so those are added
calls, not a different core. Only the xHCI back end is built now.

### 3.4 Keyboards and mice: the kernel's input path

The kernel already takes keys and mouse movement from a program: `SYS_console` `CON_INPUT` (D24 6.3, D38 step 5),
root only, on any console's descriptor (usbd opens `/dev/tty1` for it):

- **Mouse:** kind `CONI_MOUSE` (2) takes records {dx, dy, buttons} and adds them as JOY0DAT's counters would move,
  the buttons standing until the next record (`kernel/input.c` `input_inject`). A boot mouse report is exactly that
  (signed 8-bit dx and dy, three buttons): one record a report, no kernel change. The pointer speed is scaled in usbd
  (a USB mouse counts more finely than an Amiga mouse: a divisor in `/etc/usb.conf`, *proposed*, default 2, to be set
  on the real machine). Movement and buttons merge with the Amiga mouse port's, as two mice on one pointer do.
- **Keyboard:** usbd turns each HID usage into the Amiga raw key code at the same place on the keyboard (a 256-entry
  table, *proposed*), so the Amiga keymap (Swedish on the partner's machine) applies to both keyboards alike; keys the
  Amiga has not got (F11, F12, Print Screen, Pause, Insert, Page Up/Down, Home, End, the right-hand GUI key) map to
  nothing in U2 (logged once each) until a table for them is decided. usbd compares each report with the last one and
  sends the changes as key-down and key-up codes.
  **One kernel change is needed:** `CON_INPUT`'s raw kind 0 releases at the end of every call the modifiers that call
  pressed and stops its repeating key (right for the remote control: a client that goes away leaves no key held). A
  USB keyboard sends one report per change, so Shift held across two reports would be let go between them, and a held
  key would never repeat. So **a new kind `CONI_HELD` (3)**: the same raw codes, nothing released at the call's end, the
  repeat left running as the real keyboard's. usbd sends "all keys up" when a keyboard is unplugged, when its own end
  is orderly, and at its start (a restart after a crash releases what the dead usbd held). Kernel cost about 40 bytes
  (`term.c`'s kind check, `kbd_inject`'s flag). The number is claimed in `docs/claims.md` and frozen when built.
  (Rejected: re-pressing the modifiers inside every call. Shift held while clicking in the desktop would flicker up and
  down, and key repeat would be usbd's own timer racing the kernel's.)
- **The keyboard's LEDs** (Caps Lock): SET_REPORT to the keyboard when the kernel's Caps Lock state changes; usbd asks
  the state through the existing console calls if one says it, else the LED stays off in U2 (*proposed*, decided by the
  builder from what `kbd.c` exposes).
- **Not reached:** ZINE's programs (they read the Amiga's own ports; usbd is stopped before a ZINE program runs,
  3.7). A USB keyboard's reset combination does nothing (the Amiga's Ctrl-Amiga-Amiga stays the reset).

### 3.5 The main loop, and sticks through diskd

usbd's loop waits with `poll` on its port `usb` and on its post window (below), with a timeout of one tick (10 ms) while
any device is attached, 100 ms with none (a plug-in is then seen within 100 ms). Each wake: the event ring is read
until empty (completions, port changes), hubs' status endpoints handled, HID reports turned into input, finished
storage requests answered. **Polled, no interrupt** (as GENET first, D19 5.2): the controller's interrupt reaches the
68k only as Emu68's level 6, which Zenix leaves alone; the interrupt enable bit stays off. The keyboard's report
interval is 8-10 ms, so a key waits at most about 20 ms to show (*proposed*; measured on the real machine at U2).

**Sticks: diskd stays the one disk server** (architecture.md section 9), so `mount`, `fdisk list`, `df`, the overlap
and mount rules, D11's restart record and every filesystem server work on a stick unchanged. The rule that avoids a
deadlock: **diskd never calls usbd, and never waits for it.** (diskd calling usbd's port could, after a usbd crash,
make diskd the loader of `/sbin/usbd` through the root filesystem, which needs diskd: a hang.) So:

1. **A stick appears:** usbd calls diskd `DISK_USBADD` (*proposed* number 11) with its record: a USB disk number, its
   size in 512-byte sectors, the largest transfer, the capability bits (`DISKCAP_FLUSH` when SYNCHRONIZE CACHE works,
   `DISKCAP_RO` when write-protected or before U4), the vendor/product text as the label. diskd adds slot `usbN`
   (`phys` = a new medium kind) and scans its partition table as for any disk (RDB, MBR, the 0x76 container): `usb0p1`
   ... (MBR partitions of type 0x0B/0x0C and 0x83 are already mounted by type; a superfloppy with FAT at block 0 is
   `usb0` itself). diskd answers at once. **A stick goes:** `DISK_USBDEL`; diskd removes the slots; a mount on it
   answers -EIO from then on (`-ESTALE` as D44's changed disc, *proposed*), and `umount` frees it.
2. **A read or write on `usbN`:** diskd checks the request as for any device (root, ranges, overlaps, the buffer is
   the client's to write: `SYS_peer` `PEER_WR`), keeps the message unanswered, and **posts** a 16-byte record {slot
   number, operation, LBA, count, buffer} into usbd's post window (`SYS_post` `POST_WRITE`: the kernel copies it, never
   blocks, D38 step 3). usbd opened that window at its start (`POST_BUF`, `from` = the port `disk`); the post wakes it
   (`POLLRDBAND`). Up to 8 requests a disk may be held (*proposed*; one is enough for BOT, which runs one command at a
   time, the rest wait in diskd).
3. **usbd does it** (bulk transfers through a 64 KB bounce buffer in its DMA region, copied to or from the client's
   buffer: client buffers are neither in whole cache lines nor sure to be DMA-reachable) and calls diskd
   `DISK_USBDONE` (*proposed* 12) {slot number, result}; diskd answers the held request with `sys_reply` and answers
   usbd. diskd never waits: usbd's call is served in its next turn.
4. **usbd ends** (crash, ZINE, stop): diskd sees usbd's pid gone (`alive`, as it checks a dead mount's server) at its
   next turn or the next `usbN` request, fails the held requests with -EIO and marks the USB disks gone; the restarted
   usbd adds them again (new slots, unless the same stick at the same place: then the old slot, as D44's generation).
   diskd's restart record keeps `usbN` slots out (they come back from usbd).

The numbers 11 and 12 and the medium kind are claimed when built; `DISK_USBADD` and `DISK_USBDONE` are answered only
for the process that owns port `usb` (else -EPERM).

### 3.6 What the kernel and the boot do (all *proposed*)

| Change | Where | Resident bytes |
|---|---|---|
| The hardware table names the xHCI: `xhci` (its 68k address), `xhci_irq` (SPI), `xhci_stop` (what stage0 did, bits as `genet_stop`), from `reserved[3]` (the table's size does not change) | `kernel/hwinfo.h` | 0 |
| stage0 reads `/__symbols__/xhci`, refuses a node whose status is "disabled" (no `otg_mode=1`: the table says 0), translates `reg` through `/scb`'s rewritten ranges, takes `interrupts` | `boot/stage0.c` | 0 (boot-only, given back by D45) |
| **The warm start:** stage0 stops an xHCI AmigaOS's Neptune left running (Poseidon starts it in `User-Startup`, so `zenixboot` from the Workbench finds it running, its event ring and the keyboard's transfers still written by DMA into what becomes Zenix's memory): USBSTS checked, USBCMD Run/Stop cleared, a bounded wait for HCHalted (16 ms by the spec; 100 ms with a timeout and a log bit), interrupts off; no reset (usbd resets) | `boot/xhci-stop.h`, as `boot/genet-stop.h` | 0 (boot-only) |
| `SYS_drv` for the owner of port `usb` too (today only the network server's children and `audio`'s Paula claim): a claim of the table's `xhci` window (64 KB: the capability, operational, runtime and doorbell registers), its DMA memory, its quiesce, the cache calls | `kernel/drv.c` | ~70 |
| The network server's end releases only its own children's claims (today all but Paula's) | `kernel/drv.c` | ~10 |
| `CON_INPUT` kind `CONI_HELD` 3 (3.4) | `kernel/term.c`, `kernel/kbd.c` | ~40 |

The quiesce program (D19 2.4, at most 8 steps): USBCMD = 0 (Run/Stop and interrupts off), IMAN = 0. The kernel runs
it when usbd ends and before ZINE (`drv_zine`); the claim's DMA memory is freed only when usbd's zombie is reaped,
which is long after the controller halts (16 ms).

### 3.7 ZINE, restarts, unplugging

- **ZINE** (D29 7.2): `drv_zine` runs the quiesce and kills usbd; init starts it again after the program, which
  resets the controller and enumerates again (about 1-2 s until the keyboard answers; a mounted stick comes back as a
  new `usbN`: its mount answers -EIO and must be mounted again, said in the log, *proposed*).
- **usbd crashes:** the kernel's quiesce stops the controller; init restarts usbd with D11's numbers; keys held are
  released at the new start (3.4).
- **A stick pulled while mounted:** the requests in flight fail with -EIO, the filesystem server's dirty state is
  its own (FAT's dirty bit, ext2's clean mark: D17's check at the next mount).

## 4. Cache and DMA rules under Emu68 (binding for the build)

From the knowledge base (`amiga/emu68-network-usb-rtg-hardware.md` section 1, `amiga/genet-under-emu68.md` "Cache
and DMA", `amiga/emu68-jit-cache-rules.md`) and D19 2.4, applied to xHCI's structures:

1. **Everything the controller reads or writes lies in the claim's DMA region** (`DRV_DMAMEM`, 64-byte aligned,
   Emu68 RAM, freed by the kernel only after the quiesce). usbd sub-allocates it: the DCBAA (64-byte aligned), the
   command ring, the event ring and its segment table, the scratchpad array and pages (page aligned: xHCI's PAGESIZE
   register, 4 KB on every known controller), each slot's input and output contexts (64-byte aligned; 32 or 64 bytes a
   context by HCCPARAMS1.CSZ), a transfer ring per endpoint, the bounce buffers. **No ring and no buffer crosses a
   64 KB boundary** (xHCI's rule). The DMA address is the 68k address (2.1); HCCPARAMS1.AC64 is read and the high
   address words written 0.
2. **The data cache is not coherent** (the ARM's; the 68k's own is off on every Zenix machine, start.S, except D37's
   040/060 fast-RAM cache, which the same calls cover). Never `cinva` (it drops dirty data on Emu68): every invalidate
   is a clean and invalidate (D19 step 7's `DRV_CACHE`).
3. **Whatever the CPU writes and the controller reads** (TRBs, input contexts, the DCBAA, the segment table, OUT
   data): written, then cleaned, **then** the doorbell or the register write that hands it over.
4. **Whatever the controller writes and the CPU reads** (the event ring, output contexts, IN data, scratchpad pages):
   cleaned and invalidated before it is handed to the controller, and cleaned and invalidated again before every CPU
   read (the ARM may have loaded the line speculatively meanwhile). The event ring is read one 64-byte line at a time
   (four TRBs), each line invalidated before its cycle bits are looked at.
5. **No line is shared** between something the CPU writes and something the controller writes: the TRB rings the CPU
   produces and the event ring it consumes are separate allocations; a transfer ring's link TRB and its last TRBs
   are the CPU's only. Input contexts (CPU) and output contexts (controller) are separate 64-byte-aligned blocks.
6. **IN buffers are whole lines.** A short read leaves the rest of the line as the controller left it: usbd copies
   only the length the event reports.
7. **How the calls are made:** as the GENET driver chose (D19 step 9 choice 3): Emu68's range opcodes from usbd itself
   when a child test (`usbd --probe-rangeops`) shows they work in user mode, else `DRV_CACHE` (one system call a
   range). The xHCI's own self-test replaces GENET's loopback: before any device is touched, a **No-Op command** is
   placed on the command ring with the stale-value trick (the event ring's lines first filled with a pattern through
   the cache): a completion event with the right TRB pointer proves the clean (the controller read the TRB) and the
   invalidate (the CPU saw the event); a wrong one makes usbd try `DRV_CACHE`, and refuse when that fails too.
   Whether the range opcodes work in user mode was the GENET checklist's open question (`docs/a1200-checklist-network.md`
   step 2); its answer is not written down anywhere I found: the main session should record it.

## 5. Sizes (estimates, *proposed*; measured at each step)

| Part | Lines of C | Code at -Os | Memory while running |
|---|---|---|---|
| xHCI back end | 1,500-1,900 | 9-12 KB | - |
| USB core + hubs | 700-900 | 4-5 KB | device table: 16 devices, ~4 KB |
| HID boot keyboard + mouse | 250-300 | 1.5-2 KB | - |
| mass storage (BOT + SCSI) | 400-500 | 2.5-3 KB | - |
| usbd's server and glue | 300-400 | 2-3 KB | stack 16 KB, heap ~8 KB |
| **`/sbin/usbd` in all** | **3,200-4,000** | **20-26 KB** file | **~150-200 KB** with the DMA region below |
| DMA region | | | DCBAA 0.3 KB, command and event rings 8 KB, scratchpad 0-? pages of 4 KB (HCSPARAMS2, unknown until read: logged at U1), 5 KB a device (two contexts, three rings), bounce 64 KB: **128 KB** asked, more only if the scratchpad needs it |
| `lsusb` | 150-200 | 3-4 KB file | while it runs |
| diskd (`usbN` slots, held requests, the window) | 120-160 | +0.6-1 KB | +0.3 KB |
| **kernel, resident** | | **+~120 bytes** (3.6) | |
| **kernel, boot-only** (stage0 + `xhci-stop.h`) | | **+~300 bytes** in `kernel.bin`, given back after the boot by D45 | |

**The kernel's room** (*measured* 2026-11-12, this branch's base): `kernel.bin` 193,948 bytes against `TOTAL`
194,000 in `tools/ksize-budget` (52 bytes left), `drv.o` 2,602 against its 2,650; the mount tests' floppy has 1,636
bytes left against `ROOM`'s 800. So the build raises `TOTAL` by about 450 and `drv` by about 100 in the commit that
grows them (the budget file's rule), which leaves about 1,200 bytes on the mount tests' floppy. If that room is wanted
elsewhere, `CONI_HELD` is the part to weigh (3.4's rejected way costs 0 bytes and works badly).
On a machine without the xHCI nothing is loaded: usbd ends at once, its file sits on the disk.

## 6. Testing

### 6.1 On the PC: a model of the controller and its devices

`tools/xhci-model.h` (*proposed*, ~800-1,000 lines), in the style of `tools/genet-model.h` and `tools/emmc-fake.h`:
strict, counting as a fault everything the real chip would do wrong with or that would corrupt memory, and the same
`usb/*.c` compiled against it (`tools/usb-host-test.c`, `tools/usb-host-test.sh`, ASan and UBSan as
`disk-host-test.sh`).

- **The controller:** capability, operational, runtime and doorbell registers with their reset values (HCIVERSION
  1.0 and 1.1 both, CSZ 0 and 1, a scratchpad count, MaxSlots, one root port); Run/Stop, HCHalted, the reset's timing,
  CNR; the command ring (Enable Slot, Disable Slot, Address Device with and without BSR, Configure Endpoint, Evaluate
  Context, Reset Endpoint, Stop Endpoint, Set TR Dequeue Pointer, No-Op), the event ring with its segment table and
  ERDP, transfer rings with link TRBs, the toggle-cycle bit and chained TDs, control (setup/data/status stages),
  bulk and interrupt endpoints, short packets, STALL (halted endpoint, Reset Endpoint needed), babble, transaction
  errors, a device removed mid-transfer, port status change bits (write-1-to-clear kept honest).
- **The cache between the CPU and the DMA**, as `genet-model.h` keeps it: the CPU's view, RAM and a snapshot per 64-byte
  line; the controller reading a line the CPU wrote and never cleaned is a fault, a dirty line written back over what
  the controller wrote is a fault, and the CPU reading a line the controller wrote without an invalidate sees the stale
  value (so a missing call shows up as a wrong key or a wrong sector, not only as a counted fault).
- **The memory rules as faults:** a ring or buffer crossing 64 KB, a misaligned context or ring, an address outside
  the region, a doorbell rung while halted, a context field xHCI says is reserved set, a TRB of a slot never enabled.
- **Devices behind the port** (each a small state machine answering control requests and endpoint transfers): a
  USB 2.0 hub with 4 ports and a transaction translator (the slot context's TT fields checked for low/full-speed
  devices behind it), a boot keyboard (scripted reports: Shift held across reports, six keys at once, rollover error
  report 0x01), a boot mouse, a stick backed by an image file (BOT: CBW/CSW tags and signatures, the SCSI commands of
  U3/U4, a write-protected one, one that answers NOT READY twice before ready, one whose READ CAPACITY fails once), and
  **hostile devices**: descriptors with lengths past their buffer, zero or 255 interfaces, a configuration longer than
  it says, an endpoint with max packet 0, a CSW with the wrong tag, a phase error, a hub claiming 255 ports, a device
  that never answers (timeouts), and a fuzz of descriptor bytes.
- **Checked results** (compared, never only printed): the No-Op self-test passes with the cache calls and fails
  without them; enumeration of each device and through the hub; the keyboard's raw codes and the mouse records usbd
  would send (a stub of `CON_INPUT` records them; Shift held across reports stays held); a stick's sectors read back
  equal to the image; writes land in the image (U4); every hostile device refused with a log line and nothing else
  harmed; unplug in the middle of each transfer kind; usbd's quiesce leaves the model halted.

**diskd's side** on the PC: `tools/host/diskd-host.c` gains a USB stub that posts and answers as usbd would; the
`disk-host-test.sh` checks: `usb0` and its partitions appear and go, reads and writes reach the stub, a held request
fails -EIO when the stub "dies", a mounted stick that goes answers -ESTALE, diskd never blocks on the stub (a stub that
never answers holds only its own requests: `ide0` keeps working).

**In FS-UAE** (no USB there): usbd on a machine without the xHCI ends with status 0 and one log line; `CONI_HELD`
(termtest: Shift held across two calls, repeat running, all-up releasing); `SYS_drv` for port `usb`'s owner and the
refusals (a fake usbd that claims FS-UAE's A2065 board as `drv-test.sh` does: allowed for the port's owner, -EPERM for
others; the network server's end leaves its claim alone); diskd's `DISK_USBADD` and `DISK_USBDONE` refused from a
process that does not own `usb`.

### 6.2 The first run on the partner's PiStorm, step by step

Written for a short session; each step logs to the kernel log (`dmesg`, and the serial cable when it is in) with a
timeout on every wait, and each can be the last one of a session. A checklist file like
`docs/a1200-checklist-network.md` is written with the build (U1), with the expected lines.

0. **Before Zenix** (AmigaOS, the partner): which USB devices are plugged in and whether Trident (Poseidon's
   preferences) lists them and a hub; a photo of the I/O board's sockets. `config.txt` keeps `otg_mode=1`.
1. **The table:** `zenixboot CHECK` (the dry run) shows `xhci 0xf89c0000 irq 176` (or "no xHCI: disabled"). Then a
   normal boot from the Workbench with Poseidon running: the boot log's `xhci: stopped (it was running)` line (the warm
   start), and Zenix runs on without memory damage (the network checklist's step 6, "Memory", as after GENET's warm start).
2. **U1, nothing plugged in:** `usbd -v` by hand as root (the daemon line still commented): the capability values
   (HCIVERSION, MaxSlots, MaxPorts, scratchpad count, AC64, CSZ, PAGESIZE), the reset taking how long, the cache way
   chosen, `No-Op: completion OK`, port 1 "no device". Then `kill` usbd: `drv: claim ... released: 2 quiesce steps run`.
3. **U1, a keyboard plugged in:** the connect event, port reset, the speed, Address Device, `lsusb -v` shows its
   descriptors (compared with what Linux's `lsusb -v` says on the PC for the same keyboard). Unplug, plug again: the
   slot freed and enabled again.
4. **U2, the keyboard:** typing in the text console (Shift, Ctrl, Amiga+F2 to switch consoles, a held key repeating),
   both keyboards at once; Caps Lock. The key-to-screen delay felt (and the log's per-report time).
5. **U2, a hub, then the mouse:** the hub's ports, a low-speed mouse behind it (the transaction translator), the
   desktop's pointer moving, buttons, Shift-click; the speed divisor adjusted.
6. **U2, recovery:** usbd killed while typing (the quiesce, init's restart, keys back within seconds, no key stuck);
   a ZINE demo started and left (keys back after); reboot to AmigaOS: Poseidon finds the keyboard again (its driver
   resets the controller Zenix left halted).
7. **U3, a stick read-only:** `usb0` in `fdisk list`, mount its FAT partition read-only, `md5sum` of files compared
   with the PC's, a large file's read speed (`time cat big > /dev/null`: an Emu68 figure, labelled so), the stick pulled
   while a `cat` runs (an error, nothing hangs, usbd and diskd live).
8. **U4, writes, on a scratch stick only** (never one with the partner's data): files written, the stick checked on
   the PC (`fsck.vfat -n`, the files' sums).

Then the daemon line is uncommented in `/etc/daemons` and the checklist's results go into this note.

## 7. Risks

1. **Real hardware only.** Everything past the model is first seen at the A1200; the model shows the driver matches
   the spec and U-Boot's order, never that the BCM2711's xHCI behaves the same. Mitigation: the strict model, U1 with
   nothing at stake, logs at every stage.
2. **The I/O board is unknown:** if it has no hub, a keyboard and a mouse need one (or a combo receiver); if its hub is
   odd (a multi-TT hub, an over-current line), U2 finds it. Question 2.
3. **The warm start:** AmigaOS's xHCI may be mid-transfer when zenixboot takes over; stage0's halt must come before
   Zenix uses the memory (as GENET's). A controller that does not halt in 100 ms is logged; Zenix then refuses USB
   (`xhci_stop` says so) rather than reset a controller whose DMA is live.
4. **Cache rules wrong for the xHCI** (the event ring is read often, so a missing invalidate shows as "nothing
   happens"; a missing clean as a stuck command): the No-Op self-test catches both before any device is touched.
5. **Range opcodes in user mode** not known (section 4.7): `DRV_CACHE` works either way, at a system call a range;
   polling the event ring then costs a system call per 10 ms tick, nothing on Emu68.
6. **The kernel's room** is 52 bytes under its budget today (section 5): the build raises the budget by about 450 bytes,
   which only the main session should agree.
7. **Emu68 or firmware changes:** a new Emu68 may place `/scb` elsewhere (never hard-coded: read from the device
   tree), or a firmware may stop enabling the xHCI node (then the table says "disabled" and usbd says why).
8. **Sticks are quirky** (TEST UNIT READY needed several times, READ CAPACITY failing first, 4 KB sectors, card readers
   with several LUNs): U3 takes LUN 0, 512-byte sectors only (others refused with a log line), retries as Linux's
   usb-storage does for the common quirks; more after real sticks show them. rondoval's driver README warns "data
   corruption is possible in edge cases": writes come last and on a scratch stick.
9. **Boot protocol only:** a keyboard or mouse that does not do it (rare; some gaming mice, some wireless receivers)
   is logged and left alone; a report-descriptor parser is a later step.
10. **Deadlocks between servers:** avoided by the rule in 3.5 (diskd never calls or waits for usbd); the host test
    checks a stub that never answers holds nothing else.
11. **Power:** the CM4's port's current comes through the I/O board; a bus-powered hard disk may not spin up. Out of
    Zenix's hands; the port's over-current bit is logged.
12. **The 1260 has no USB** unless a card is found and its own back end is written: the design keeps that possible
    (3.3), nothing more.

## 8. Questions for the partner

1. **Which USB devices would you plug in**, and which matters most: a keyboard, a mouse, a stick from the PC, a card
   reader, a hub? Make and model if you know (to model them and to look up their quirks).
2. **The I/O board on the trapdoor:** how many USB sockets does it have? A photo of it (both sides) tells whether it
   has a hub chip.
3. **Do you use USB under AmigaOS today?** Poseidon is installed and started at boot; is anything plugged in, and
   does Trident list it?
4. **Do you own an Amiga USB card** (Subway, RapidRoad, or another clock-port or Zorro card) for the Apollo 1260
   machine? If not, USB stays a PiStorm feature.
5. **Sticks:** is read-only enough at first? What is on the sticks you would use (FAT32 from the PC is read and written
   by Zenix today; exFAT and NTFS are not)?
6. **The USB keyboard's layout:** Swedish too (then the Amiga keymap serves both), or another?
7. **What should F11, F12 and the keys the Amiga lacks do** (nothing, or e.g. Help and Amiga+F11/F12)?
8. **The kernel's room:** about 120 resident bytes and about 300 boot-only bytes (section 5). Agreed?

## 9. Build order (when approved; each step merged with its tests)

1. `tools/xhci-model.h` and `usb/xhci-core.c` against it (the No-Op self-test, rings, contexts); the kernel's
   `SYS_drv` change and stage0's table and halt; `/sbin/usbd` with U1's scope and `lsusb`; the A1200 checklist. Real
   run: section 6.2 steps 0-3.
2. Hubs and HID; `CONI_HELD`; the `/etc/daemons` line (commented). Real run: steps 4-6.
3. diskd's USB side (`DISK_USBADD`, the post window, `DISK_USBDONE`) and mass storage read-only. Real run: step 7.
4. Writes. Real run: step 8. Then the daemon line on by default on the PiStorm's images.

## 10. Sources

- The partner's card (backup image of 2026-11-06): `config.txt`, `bcm2711-rpi-cm4.dtb` (decompiled with `dtc`).
- `github.com/rondoval/emu68-xhci-driver`, branch `context_release` (2026-11-02): README (units, requirements),
  `xhci.device/src/unit.c` (unit 0's device-tree symbol and status check), `xhci-dma.c` (cache calls), read for facts;
  GPL-2.0+ from U-Boot. `github.com/rondoval/emu68-driver-stack` README and release notes (Neptune, xhci 6.x).
- `github.com/PiStorm/cm4board`, `CM4_PiStorm32Lite` (v1.1): README and the KiCad board's USB nets.
- Raspberry Pi `config.txt` documentation, `otg_mode`.
- amiga-news.de 2023-03 (the CM4 adapter and the I/O board); icomp wiki "RapidRoad"; Big Book of Amiga Hardware
  (Subway, UltraBus); amiga.resource.cx (Thylacine, Deneb); Wikipedia "Clock port".
- Public specifications to build from: Intel's xHCI 1.2, USB 2.0 (chapters 9 and 11), HID 1.11 (appendix B, the boot
  protocol, and the usage tables), USB Mass Storage Bulk-Only Transport 1.0, SCSI SBC/SPC. U-Boot's
  `drivers/usb/host/xhci*.c` and `common/usb*.c` (GPL-2.0+) for the order of operations, read, no code copied (as D19
  step 9 did for GENET).
- Knowledge base: `amiga/emu68-network-usb-rtg-hardware.md`, `amiga/genet-under-emu68.md`,
  `amiga/emu68-jit-cache-rules.md`, `amiga/emu68-and-pistorm.md`, `amiga/zenix-filesystems-research.md` section 1.

## 11. Reviews

(Filled in below after the independent review.)
