# D46: USB - a host controller server, keyboards, mice and sticks

Roadmap block 6, "USB" (docs/ROADMAP.md "The order of work", approved by the owner 2026-10-25). Revision 2,
2026-11-12, branch `usb`, after two independent reviews (section 11). Steps U1 and U2 (section 9) are being built on
this branch against a model of the controller on the PC; **nothing has run on real hardware.** No emulator offers USB
on an Amiga, so every line of a USB driver can only be proved on a real machine, and real-hardware time is scarce:
this note says what to build so the first run on the partner's PiStorm is short and every step of it says what it
found. Each fact is marked *verified* (seen on the real machine, its card or a file from it), *read* (from a source
named next to it) or *proposed* (this note's choice). Decided by the main session 2026-11-12: the kernel bytes of
section 5 are accepted; sticks come read-only first; a USB keyboard follows the console's keymap (no USB-specific
layout); F11, F12 and the other keys the Amiga lacks are ignored for now (a later key-map item); the partner's
questions 1-4 are asked by the main session, the build does not wait for them.

## 1. In plain words

- **Where USB exists today:** only on the partner's PiStorm A1200, and there exactly **one USB 2.0 port**: the
  CM4's own, wired out through the PiStorm's CM4 adapter to the I/O board on the trapdoor (section 2). The A1200 with
  the Apollo 1260 has no USB unless an Amiga USB card is fitted (none is known: question 4).
- **What a first version gives:** a USB keyboard and mouse typing and pointing in Zenix exactly as the Amiga's own (the
  same keymap, Amiga+F1..F10, the desktop's pointer), through a hub when there is one; then a USB stick as a disk
  (`usb0`, `usb0p1`, mounted by the filesystems Zenix already has: FAT, ext2, ZenixFS, FFS, PFS3), read first, then
  written.
- **How:** one user-mode server, `/sbin/usbd` (a D11 service on port `usb`), owns the controller (the D19 driver pattern: a kernel claim, DMA memory
  that belongs to the claim, a quiesce program); keys and mouse movement go into the kernel's existing input path;
  sticks reach the filesystems through diskd like every other disk.
- **What the kernel pays:** about 200 bytes resident and about 300 bytes of boot-only code that D45 gives back after
  the boot for the keyboard and mouse (section 5); sticks later add diskd's part, which is built into the kernel image
  (1.2-1.8 KB, decided at that step). On a machine without the controller usbd stops at once.
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
| Under Emu68 the xHCI's registers appear at **0xF89C0000**: bus 0x7E9C0000 lies in `/scb`'s first range, which starts at bus 0x7C000000 (0x3800000 long) and which Emu68 moves to 0xF6000000, as GENET's 0x7D580000 appears at 0xF7580000. Its `reg` is **1 MB** long | *read* and worked out: `/scb` ranges in the card's device tree + the Emu68 range rewrite (knowledge base `amiga/emu68-network-usb-rtg-hardware.md`); **never hard-coded**: stage0 reads it (section 3.6) |
| The adapter wires the CM4's USB 2.0 pair (module pins 103/105) and the OTG ID line (pin 101) to its 40-pin FPC connector (pins 14, 15, 35), and nothing else of USB | *read*: the CM4 adapter's KiCad board, `github.com/PiStorm/cm4board`, `CM4_PiStorm32Lite` (v1.1, 2025-03-31) |
| The FPC goes to an **I/O board** on the trapdoor with HDMI, Ethernet and USB sockets; **whether it has a hub chip, and how many USB sockets, is unknown** | *read*: amiga-news.de 2023-03 (the adapter by Michal Schulz, the I/O board by Claude Schwarz), retailers' pages; no schematic found. **Unknown, ask** (question 2) |
| Do any USB devices work there under AmigaOS today | **unknown, ask** (question 3) |
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

### 3.2 Where it lives: one server, `/sbin/usbd`, a D11 service

D19's rule ("every driver not needed to reach the root is a user-mode process") puts USB in user mode. One process,
because keys must not wait for a message round trip between a controller process and a class process, and because
on a 4 MB machine a second process costs its stack and table slots for nothing:

| Part | Files | What it does |
|---|---|---|
| the xHCI back end | `usb/xhci-core.c`, `usb/xhci.h` | registers, rings, contexts, commands, events, the root port; plain C whose register and DMA-memory access, clock, cache calls and log come from the includer, as `net/drv/genet-core.c`, so the same file runs on Zenix and against the model |
| the USB core | `usb/core.c`, `usb/usb.h` | enumeration (reset, address, descriptors, configuration), the device table, hubs, the host-controller interface a later clock-port back end also fills (3.3) |
| the classes | `usb/hid.c` (U2), `usb/msc.c` (U3) | boot keyboard and mouse to the kernel's input (3.4); BOT + SCSI to diskd (3.5) |
| the server | `usb/usbd.c` | the claim, DMA memory and quiesce (D19 2.4), the main loop (3.5), port `usb`, the log |
| a tool | `user/lsusb.c` | the devices as usbd sees them (`-v`: their descriptors); `--hold` keeps usbd running (below); page `docs/man/lsusb.txt`, usbd's `docs/man/usbd.txt` |

**Who it is:** a service of the service manager (D11), the line `usb -5 /sbin/usbd` in `/etc/svctab`, so its port
is held and only the process the kernel started for it may take it, and the kernel knows it as `svc_pid("usb")`, as
the sound server is `svc_pid("audio")` (D40). That identity, not "root", is what the kernel's device calls check
(3.6). **How it keeps running:** a service is started by a call and nobody calls a keyboard driver, so `/etc/daemons`
gets the line `usb 0 /bin/lsusb --hold` (D24 7: init keeps it running): `lsusb --hold` calls `USB_HOLD`, which usbd
never answers while it runs. When usbd ends the call fails (-ECONNRESET), `--hold` calls again, and that call starts
usbd again with D11's numbers. A plain `lsusb` also starts usbd (on the first real run that is how U1 begins: no line
needs to change). **On a machine without the xHCI** (the hardware table names none, or its node is disabled) usbd
answers every request -ENODEV once and ends with status 0; `lsusb --hold` takes -ENODEV as "nothing to keep" and
ends with status 0, which init takes as "stopped until the next boot". So it costs nothing there but one start. The
`/etc/daemons` line ships commented out until U2 has run on the real machine (as `wasabid`'s).

**Port `usb`'s requests** (`usb/usbproto.h`, *proposed* numbers, claimed when built): `USB_LIST` 1 (a0 an index, a1 a
`struct usbdevinfo` the caller's: bus address, parent hub and port, speed, vendor, product, class, the strings),
`USB_DESC` 2 (a0 the device, a1 a buffer, a2 its size: the raw configuration descriptor), `USB_HOLD` 3 (never answered
while usbd runs; -ENODEV when there is no controller), `USB_STAT` 4 (counters for the tests: events, commands,
transfers, errors, the cache way). Root only (`m->uid` 0), except `USB_LIST` and `USB_DESC`, which are anyone's.

### 3.3 The host-controller interface inside usbd

The core talks to a back end through a small table of functions at U-Boot's level (*proposed*): `hc_init`,
`hc_poll` (events into completions), `port_status` / `port_reset` / `port_power` (the root hub), `dev_new` and
`dev_free` (xHCI's Enable Slot / Address Device / Disable Slot; a clock-port chip's no-ops), `ep_open` (Configure
Endpoint), `submit` (control, bulk, interrupt; a callback on completion), `hc_stop`. xHCI does the addressing and the
scheduling in hardware, an ISP1763 or SL811 needs the core to do more: the interface is shaped so those are added
calls, not a different core. Only the xHCI back end is built now.

### 3.4 Keyboards and mice: the kernel's input path

The kernel already takes keys and mouse movement from a program: `SYS_console` `CON_INPUT` (D24 6.3, D38 step 5),
root only, on any console's descriptor (usbd opens `/dev/tty1` for it). Raw keys go through `key_code` as the real
keyboard's do, so they follow the desktop's grab (D38) like the Amiga keyboard.

- **Mouse:** kind `CONI_MOUSE` (2) takes records {dx, dy, buttons} and adds them as JOY0DAT's counters would move,
  the buttons standing until the next record (`kernel/input.c` `input_inject`). A boot mouse report is that (signed
  8-bit dx and dy, three buttons). Two rules, because the buttons are only sampled at the 100 Hz tick (`input_poll`):
  usbd sends **at most one button change per tick** (a press and a release that arrive together go out a tick apart),
  and movement is summed between sends. The pointer speed is scaled in usbd (a divisor, `usbd -m N`, default 2,
  *proposed*, set on the real machine). Movement and buttons merge with the Amiga mouse port's.
- **Keyboard:** usbd turns each HID usage into the Amiga raw key code at the same place (a table in `usb/hid.c`), so
  the console's keymap (Swedish on the partner's machine) applies to both keyboards alike: no USB-specific layout
  (decided 2026-11-12). Left and right GUI are the left and right Amiga keys (0x66, 0x67), Alt 0x64/0x65, Ctrl 0x63
  (both Ctrls: the Amiga has one), Shift 0x60/0x61. **F11, F12, Print Screen, Scroll Lock, Pause, Insert, Home, End, Page
  Up/Down, Num Lock and the application key are ignored** for now (decided 2026-11-12; a later key-map item). usbd
  compares each report with the last and sends the changes as key-down and key-up codes; the rollover report (all
  0x01) changes nothing.
- **Caps Lock is a toggle on the Amiga:** the Amiga keyboard sends "down" when its LED goes on and "up" when it goes
  off, and `kbd.c` sets Caps Lock from that (`caps_lock = !up`). So usbd sends 0x62 down on one press of the USB Caps
  Lock key and 0x62 up on the next, ignoring the key's release, and sets the keyboard's LED to match (SET_REPORT,
  output report bit 1).
- **A new kind `CONI_HELD` (3).** Kind 0 releases, at the end of every call, the modifiers that call pressed and stops
  its repeating key (right for the remote control: a client that goes away leaves no key held). A USB keyboard sends
  one report per change, so Shift held across two reports would be let go between them, and a held key would never
  repeat. `CONI_HELD` is kind 0 without that release: the repeat runs as the real keyboard's. **The kernel releases
  what a `CONI_HELD` caller holds when that process ends** (its pressed modifiers, its repeating key), and the mouse
  buttons a `CONI_MOUSE` caller set: a crashed usbd, a ZINE kill or a usbd given up leaves nothing held and nothing
  repeating. (Rejected: re-pressing the modifiers inside every call; Shift held while clicking in the desktop would
  flicker, and the repeat would be usbd's own timer racing the kernel's.) `kbd_inject` may take fewer codes than it
  was given (it stops after two ticks while input waits): usbd sends the rest in the next call.
- **Not reached:** ZINE's programs (they read the Amiga's own ports; usbd is stopped while one runs, 3.7). A USB
  keyboard's Ctrl-Alt-Del does nothing (the Amiga's Ctrl-Amiga-Amiga stays the reset).

### 3.5 The main loop, and sticks through diskd

usbd's loop waits with `sys_evwait` on its port `usb` (requests, and `POLLRDBAND` for a post, below) with a timeout of
one tick (10 ms) while any device is attached and 10 ticks with none (a plug-in is seen within 100 ms). Each wake: the
event ring is read until empty (completions, port changes), hubs' status endpoints handled, HID reports turned into
input. **Polled, no interrupt** (as GENET first, D19 5.2): the controller's interrupt reaches the 68k only as Emu68's
level 6, which Zenix leaves alone; the interrupt enable bits stay off. A boot keyboard's report interval is 8-10 ms,
so a key waits at most about 20 ms (*proposed*; measured at U2). **usbd never makes a call that can wait** (it calls
nobody: completions and news go out as posts, below), so a busy diskd, a floppy probe or a CD drive's 10-second wait
never holds the keyboard.

**Sticks (step U3, designed here, built later): diskd stays the one disk server** (architecture.md section 9), so
`mount`, `fdisk list`, `df`, the overlap and mount rules, D11's restart record and every filesystem server work on a
stick unchanged. The traffic goes both ways by **posts** (`SYS_post`, D38 step 3: the kernel copies a small record
into the receiver's window and wakes it, never blocks the sender): usbd opens a window that the owner of port `disk`
may post into; diskd opens one that the owner of `usb` may post into. **diskd may wait for usbd, bounded; usbd never
waits for diskd.** One direction of waiting cannot deadlock.

1. **Records** (`kernel/diskproto.h`, 24 bytes, *proposed*): {kind, sequence number, slot, LBA, count, result}.
   diskd to usbd: `UREQ_READ`, `UREQ_WRITE`, `UREQ_FLUSH`, `UREQ_HELLO`. usbd to diskd: `UDONE` (the request's
   sequence number, the result), `UADD` (a USB disk: its number, size in 512-byte sectors, the largest transfer,
   capability bits, a label pointer into usbd's memory), `UDEL`, `UHELLO`. Each window is a ring of 8 records; the
   writer numbers them, the reader keeps the last number it took and drops older ones (a record from before a restart
   is ignored).
2. **diskd's turn for `usbN`** is synchronous, as an IDE read is: it posts `UREQ_*` and waits (`sys_evwait` on its
   port for `POLLRDBAND`, other requests left queued) for the `UDONE` with that sequence number, **at most 5 s**
   (then -EIO and a log line; usbd is presumed gone and the USB disks are marked gone). **The data never passes
   through a client's buffer in usbd:** usbd reads into a 64 KB bounce buffer in its DMA region and diskd copies from
   there into its client's buffer after `UDONE` (a write: diskd copies in, then posts). So if diskd or its client dies,
   usbd never writes into memory that was theirs. The bounce buffer's place is in `UADD`.
3. **The partition table is read the same way** (diskd's `scan` asks for sectors of `usbN` through point 2), so RDB,
   MBR and the 0x76 container work on a stick as on the SD card (`part_name` names `usb0p1` ...). diskd's scan becomes
   per medium: a rescan for `df0` or `cd0` keeps the `usbN` slots, and only `UADD`/`UDEL` change them.
4. **News from usbd** (`UADD`, `UDEL`) arrive as posts; diskd's main loop waits with `sys_evwait` on its port
   (`POLLIN`, `POLLRDBAND`) instead of `sys_recv` (a timeout of 1 s also lets it notice that usbd's port lost its
   owner and fail what waits). A stick that goes: its slots are kept as *stale* while mounted (the mount answers
   -ESTALE, as D44's changed disc) and freed when unmounted, so a slot number never moves to another disk under a
   filesystem server's handle.
5. **Restarts:** a new diskd (D11) keeps the `usbN` slots in its saved record as stale, then posts `UREQ_HELLO`;
   usbd answers with an `UADD` for each stick it has; a stick seen again at the same place with the same size and
   serial takes its old slot back. A new usbd posts `UHELLO`; diskd marks every `usbN` stale and takes the `UADD`s
   that follow.
6. **Who may post:** the window's `from` is the other server's port, checked by the kernel at every post (D38). diskd
   answers `UREQ_*`'s results only to the owner of `usb` (`PEER_PORT`-style check of the port's owner).

What this costs in diskd is in section 5: it is the larger part of U3, and diskd is built into the kernel image.

### 3.6 What the kernel and the boot do

| Change | Where | Resident bytes (*estimate*) |
|---|---|---|
| The hardware table names the xHCI: `xhci` (its 68k address, 0 if none or disabled), `xhci_size` (its `reg` length), `xhci_stop` (what stage0 did: `XSTOP_*` bits as `genet_stop`'s), in place of `reserved[3]` (the table's size and first size do not change). The interrupt number is not kept (polled); it goes after `fb` when it is needed | `kernel/hwinfo.h` | 0 |
| stage0 finds the node through `/__symbols__`'s `xhci` (rondoval's driver does the same; no alias names it), refuses a node whose status is "disabled" (no `otg_mode=1`), translates `reg` through `/scb`'s rewritten ranges | `boot/stage0.c` | 0 (boot-only, given back by D45) |
| **The warm start:** stage0 stops an xHCI that AmigaOS's Neptune left running (Poseidon starts it in `User-Startup`, so `zenixboot` from the Workbench finds its event ring and the keyboard's transfers still written by DMA into what becomes Zenix's memory): when USBSTS says it runs, USBCMD's Run/Stop and interrupt enable cleared, a bounded wait (100 ms, the beam's clock) for HCHalted. No reset (usbd resets). Like `genet_stop`, run as the last hardware step before the takeover | `boot/xhci-stop.h` | 0 (boot-only) |
| `SYS_drv` for the service of port `usb` (`svc_pid("usb")`), **only** `DRV_HWINFO`, `DRV_CLAIM` of the table's `xhci` (its window `xhci_size` long) and its own claim's `DRV_DMAMEM`, `DRV_QUIESCE`, `DRV_CACHE`: no other board, no PC Card, no parallel port, no `DRV_ATTACH`/`DRV_XFER`. Checked before the network server's checks, so a machine without netd still answers it | `kernel/drv.c` | ~60 |
| The network server's end releases only claims its own children hold (today: all but Paula's) | `kernel/drv.c` | ~10 |
| **Before a claim with DMA memory is released, the kernel waits 20 ms after its quiesce** (by the E-clock, `hr_time`): the quiesce can only write, and xHCI may take 16 ms to halt (xHCI 5.4.1); GENET's DMA gets the same margin | `kernel/drv.c` | ~24 |
| `DRV_DMAMEM` asks fast RAM only (`MEM_FASTONLY`) under Emu68 (`HW_DT`): the Pi's DMA reaches only Emu68's RAM, never chip RAM (a fix for GENET too) | `kernel/drv.c` | ~10 |
| `CON_INPUT` kind `CONI_HELD` 3, and the release of a `CONI_HELD` or `CONI_MOUSE` caller's keys and buttons when it ends (3.4) | `kernel/term.c`, `kernel/kbd.c`, `kernel/input.c` | ~90 |

The quiesce program (D19 2.4): USBCMD = 0 (Run/Stop and the interrupt enable off) and IMAN = IP (writing 1 clears a
pending interrupt; IE off). The kernel runs it when usbd ends and before ZINE (`drv_zine`). **Four claims**
(`NCLAIM`): GENET, Paula and the xHCI take three on the PiStorm; a fourth device there (the PC Card or the parallel
port) still fits, a fifth would be refused -EBUSY. A fifth claim costs about 156 bytes of `drv`'s bss, so it waits
until a machine needs it.

### 3.7 ZINE, restarts, unplugging

- **ZINE** (D29 7.2): `drv_zine` runs the quiesce and kills usbd; after the program, the next `USB_HOLD` call of
  `lsusb --hold` starts it again, which resets the controller and enumerates again (about 1-2 s until the keyboard
  answers). The kernel counts the kill as an end (D11): **five ZINE programs within 60 s give USB up** until root
  registers `usb` again (`svc`) or the next boot. A limit, written in `usbd(8)`; programs that run that briefly are
  rare.
- **usbd crashes:** the kernel's quiesce stops the controller and releases what usbd held (keys, buttons); D11
  restarts it with its numbers at `--hold`'s next call.
- **A device pulled:** its slot disabled, its transfers ended; a keyboard's held keys released by a last empty report.
  A stick pulled while mounted (U3): requests fail with -EIO, the mount answers -ESTALE, the filesystem's own dirty
  state is its own business (FAT's dirty bit, ext2's clean mark: D17's check at the next mount).

## 4. Cache, DMA and byte-order rules under Emu68 (binding for the build)

From the knowledge base (`amiga/emu68-network-usb-rtg-hardware.md` section 1, `amiga/genet-under-emu68.md` "Cache
and DMA", `amiga/emu68-jit-cache-rules.md`) and D19 2.4, applied to xHCI's structures:

1. **Everything the controller reads or writes lies in the claim's DMA region** (`DRV_DMAMEM`: fast RAM, Emu68's,
   64-byte aligned; freed by the kernel only after the quiesce and 20 ms, 3.6). usbd asks 4 KB more than it needs and
   lays its structures out from the first 4 KB boundary: the scratchpad pages and the 64 KB bounce buffer (U3) on
   page boundaries; the DCBAA, the segment table and each slot's input and output contexts (32 or 64 bytes a context
   by HCCPARAMS1.CSZ) each inside one page; every ring inside one page (256 TRBs = 4 KB, a link TRB at its end). A
   transfer buffer that crosses a 64 KB boundary is split into two TRBs there (xHCI's rule is per TRB). The DMA
   address is the 68k address (2.1); HCCPARAMS1.AC64 is read and the high address words written 0.
2. **Byte order:** every register, TRB, context, DCBAA entry and segment-table entry is little-endian. **The driver
   never touches DMA memory or registers through a C pointer**: DMA memory is a 32-bit bus address, read and written
   only by the includer's `dm_r32`/`dm_w32` (and registers by `rg_r32`/`rg_w32`), which swap on the 68k. On the PC the
   same addresses are the model's, not host pointers, so a stray direct access fails there at once instead of
   passing on a little-endian PC by luck.
3. **The data cache is not coherent** (the ARM's; the 68k's own is off on every Zenix machine, start.S, except D37's
   040/060 fast-RAM cache, which the same calls cover). Never `cinva` (it drops dirty data on Emu68): every
   invalidate is a clean and invalidate (D19 step 7's `DRV_CACHE`).
4. **Whatever the CPU writes and the controller reads** (TRBs, input contexts, the DCBAA, the segment table, OUT
   data): written, then cleaned, **then** the doorbell or the register write that hands it over.
5. **Whatever the controller writes and the CPU reads** (the event ring, output contexts, IN data, scratchpad pages):
   cleaned and invalidated when it is handed to the controller, and cleaned and invalidated again before every CPU
   read (the ARM may have loaded the line meanwhile). The event ring is read one 64-byte line (four TRBs) at a time,
   each line invalidated before its cycle bits are looked at.
6. **No line is shared** between something the CPU writes and something the controller writes: rings the CPU produces
   and the event ring it consumes are separate pages; input contexts (CPU) and output contexts (controller) are
   separate 64-byte-aligned blocks.
7. **IN buffers are whole lines.** usbd copies out only the length the event reports.
8. **How the calls are made:** as the GENET driver chose (D19 step 9 choice 3): Emu68's range opcodes from usbd itself
   when a child test (`usbd --probe-rangeops`) shows they work in user mode, else `DRV_CACHE` (one system call a
   range). **The self-test** before any device is touched: a **No-Op command** is placed on the command ring and
   cleaned; the event ring's first line is then *read by the CPU* (so the line sits in the cache with its stale,
   cycle-bit-wrong contents) and only then the doorbell is rung; usbd waits up to 100 ms, invalidating that line
   before each look. A completion event with the right TRB pointer proves both the clean (the controller read the
   TRB) and the invalidate (the CPU saw the controller's write past its own cached copy). A missing clean or a missing
   invalidate shows as **no event** within 100 ms: usbd then halts the controller (the stale TRB may still run later),
   resets it, tries `DRV_CACHE`, and refuses when that fails too. Whether the range opcodes work in user mode was the
   GENET checklist's open question (`docs/a1200-checklist-network.md` step 2); its answer is not written down anywhere
   I found: the main session should record it.

## 5. Sizes (estimates, *proposed*; measured at each step)

| Part | Lines of C | Code at -Os | Memory while running |
|---|---|---|---|
| xHCI back end | 1,500-1,900 | 9-12 KB | - |
| USB core + hubs | 700-900 | 4-5 KB | device table: 8 devices in U1-U2, ~2 KB |
| HID boot keyboard + mouse | 250-300 | 1.5-2 KB | - |
| mass storage (BOT + SCSI, U3) | 400-500 | 2.5-3 KB | - |
| usbd's server and glue | 300-400 | 2-3 KB | stack 16 KB, heap ~8 KB |
| **`/sbin/usbd` in all** | **3,200-4,000** | **20-26 KB** file | **~110-130 KB** in U1-U2, **~180-200 KB** with U3's bounce buffer |
| DMA region | | | a 4 KB margin for alignment, DCBAA and segment table 4 KB, command and event rings 8 KB, scratchpad pages (HCSPARAMS2: unknown until read, logged at U1), per device 4 KB of contexts and 4 KB a ring (EP0, one interrupt or two bulk endpoints): **~64 KB** for 8 devices in U1-U2, **+64 KB** bounce in U3 |
| `lsusb` | 150-250 | 3-4 KB file | while it runs |
| **kernel, resident (U1-U2)** | | **+~200 bytes** (3.6) | |
| **kernel, boot-only** (stage0 + `xhci-stop.h`) | | **+~300 bytes** in `kernel.bin`, given back after the boot by D45 | |
| **diskd (U3)**: the post windows, the per-medium scan, stale slots, the evwait loop, the copies | 250-350 | **+1.2-1.8 KB, in `kernel.bin`** (diskd is built into the kernel image: the `embed` line of `tools/ksize-budget`) | +0.4 KB |

**The kernel's room** (*measured* 2026-11-12, this branch's base): `kernel.bin` 193,948 bytes against `TOTAL`
194,000 in `tools/ksize-budget` (52 bytes left), `drv.o` 2,602 against its 2,650, `term.o` 14,454 against 14,500,
`kbd.o` 4,211 against 4,300; the mount tests' floppy has 1,636 bytes left against `ROOM`'s 800. **U1-U2:** the
budget is raised by about 500 (accepted by the main session 2026-11-12, its own small commit), leaving about 1,100 on
the mount tests' floppy. **U3** then needs diskd's 1.2-1.8 KB, more than that room above `ROOM`: the mount tests' RAM
disk shrinks by 1-2 KB again (as D44 did), or diskd's USB side moves out: decided by the main session at U3, with the
measured number.
On a machine without the xHCI usbd starts once and stops; its file sits on the disk.

## 6. Testing

### 6.1 On the PC: a model of the controller and its devices

`tools/xhci-model.h` (~2,000-2,500 lines with its devices; the first review's figure of 800-1,000 was low: `tools/genet-model.h` is 428 lines for a much simpler chip), in the style of `tools/genet-model.h` and `tools/emmc-fake.h`:
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
- **Byte order:** the model's memory is bytes, read and written little-endian; the driver's DMA addresses are the model's (`M_BASE` + offset), not host pointers (4.2).
- **The memory rules as faults:** a TRB whose buffer crosses 64 KB, a ring, context or DCBAA crossing a page, a misaligned context or ring, an address outside
  the region, a doorbell rung while halted, a context field xHCI says is reserved set, a TRB of a slot never enabled.
- **Devices behind the port** (each a small state machine answering control requests and endpoint transfers): a
  USB 2.0 hub with 4 ports and a transaction translator (the slot context's TT fields checked for low/full-speed
  devices behind it, after the hub's slot was told it is a hub: Hub = 1, its port count, TTT and MTT through Configure Endpoint or Evaluate Context, and each device's route string and TT hub slot and port), a boot keyboard (scripted reports: Shift held across reports, six keys at once, rollover error
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

**In FS-UAE** (no USB there), `tools/usb-test.sh`: `lsusb` on a machine without the xHCI starts usbd, which says so
once in the log and stops with status 0, and `lsusb` says "no USB controller" (exit 1), `lsusb --hold` ends with 0;
`CONI_HELD` through `termtest` (Shift held across two calls types capitals; a held key repeats; the caller's end
releases Shift and stops the repeat; `CONI_MOUSE`'s buttons let go at the caller's end); `SYS_drv` from a root
program that is not the `usb` service is -EPERM (a program can not pretend to be usbd). diskd's side is tested with
U3.

### 6.2 The first run on the partner's PiStorm, step by step

Written for a short session; each step logs to the kernel log (`dmesg`, and the serial cable when it is in) with a
timeout on every wait, and each can be the last one of a session. A checklist file like
`docs/a1200-checklist-network.md` is written with the build (U1), with the expected lines.

0. **Before Zenix** (AmigaOS, the partner): which USB devices are plugged in and whether Trident (Poseidon's
   preferences) lists them and a hub; a photo of the I/O board's sockets. `config.txt` keeps `otg_mode=1`.
1. **The table:** `zenixboot CHECK` (the dry run) shows `xhci 0xf89c0000` and its size (or "no xHCI: disabled"). Then a
   normal boot from the Workbench with Poseidon running: the boot log's `xhci: stopped (it was running)` line (the warm
   start), and Zenix runs on without memory damage (the network checklist's step 6, "Memory", as after GENET's warm start).
2. **U1, nothing plugged in:** `lsusb` as root starts usbd (the daemon line still commented); its log (`dmesg`): the capability values
   (HCIVERSION, MaxSlots, MaxPorts, scratchpad count, AC64, CSZ, PAGESIZE), the reset taking how long, the cache way
   chosen, `No-Op: completion OK`, port 1 "no device". Then `kill` usbd: `drv: claim ... released: 2 quiesce steps run`, and `lsusb` starts it again.
3. **U1, a keyboard plugged in:** the connect event, port reset, the speed, Address Device, `lsusb -v` shows its
   descriptors (compared with what Linux's `lsusb -v` says on the PC for the same keyboard). Unplug, plug again: the
   slot freed and enabled again.
4. **U2, the keyboard:** typing in the text console (Shift, Ctrl, Amiga+F2 to switch consoles, a held key repeating),
   both keyboards at once; Caps Lock. The key-to-screen delay felt (and the log's per-report time).
5. **U2, a hub, then the mouse:** the hub's ports, a low-speed mouse behind it (the transaction translator), the
   desktop's pointer moving, buttons, Shift-click; the speed divisor adjusted.
6. **U2, recovery:** usbd killed while typing (the quiesce, init's restart, keys back within seconds, no key stuck);
   a ZINE demo started and left (keys back after, nothing held during it); reboot to AmigaOS: Poseidon finds the keyboard again (its driver
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
   Zenix uses the memory (as GENET's). Neptune's task runs until the takeover and has a watchdog: if it noticed the
   halt and started the controller again before AmigaOS stops, DMA would run on. So the stop is stage0's last hardware
   step, and usbd's start checks HCHalted before anything (a running controller found there is logged and halted
   before the reset). A controller that does not halt in 100 ms is logged; usbd then refuses USB (`xhci_stop` says so)
   rather than reset a controller whose DMA is live.
4. **Cache rules wrong for the xHCI**: the No-Op self-test (4.8) shows a missing clean or invalidate as no event
   before any device is touched.
5. **Range opcodes in user mode** not known (4.8): `DRV_CACHE` works either way, at a system call a range; polling the
   event ring then costs a system call per 10 ms tick, nothing on Emu68.
6. **The kernel's room** (section 5): U1-U2 accepted; U3's diskd part needs a decision then.
7. **Emu68 or firmware changes:** a new Emu68 may place `/scb` elsewhere (never hard-coded: read from the device
   tree), or a firmware may stop enabling the xHCI node (then the table says "disabled" and usbd says why). A
   controller left unpowered may answer all ones (usbd stops), or, worse, fault the ARM on a read: only U1 shows.
8. **Sticks are quirky** (TEST UNIT READY needed several times, READ CAPACITY failing first, 4 KB sectors, card readers
   with several LUNs): U3 takes LUN 0, 512-byte sectors only (others refused with a log line), retries as Linux's
   usb-storage does for the common quirks; more after real sticks show them. rondoval's driver README warns "data
   corruption is possible in edge cases": writes come last and on a scratch stick.
9. **Boot protocol only:** a keyboard or mouse that does not do it (rare; some gaming mice, some wireless receivers)
   is logged and left alone; a report-descriptor parser is a later step.
10. **Deadlocks between servers:** avoided by 3.5's rule (usbd never waits for anyone; diskd waits for usbd at most
    5 s and never inside usbd's turn); the host test checks a usbd stub that never answers holds only its own disks.
11. **ZINE and D11's count:** five ZINE programs within 60 s give USB up (3.7).
12. **Power:** the CM4's port's current comes through the I/O board; a bus-powered hard disk may not spin up. Out of
    Zenix's hands; the port's over-current bit is logged.
13. **The 1260 has no USB** unless a card is found and its own back end is written: the design keeps that possible
    (3.3), nothing more.

## 8. Questions for the partner

Questions 1-4 are asked by the main session (2026-11-12); the build does not wait for them. 5-8 were answered by the
main session the same day.

1. **Which USB devices would you plug in**, and which matters most: a keyboard, a mouse, a stick from the PC, a card
   reader, a hub? Make and model if you know (to model them and to look up their quirks).
2. **The I/O board on the trapdoor:** how many USB sockets does it have? A photo of it (both sides) tells whether it
   has a hub chip.
3. **Do you use USB under AmigaOS today?** Poseidon is installed and started at boot; is anything plugged in, and
   does Trident list it?
4. **Do you own an Amiga USB card** (Subway, RapidRoad, or another clock-port or Zorro card) for the Apollo 1260
   machine? If not, USB stays a PiStorm feature.
5. Sticks read-only first: **yes** (main session). What is on the partner's sticks stays a question for U3.
6. The USB keyboard's layout: **the console's keymap**, nothing USB-specific (main session).
7. F11, F12 and the keys the Amiga lacks: **ignored for now**, a later key-map item (main session).
8. The kernel's bytes: **accepted** (main session).

## 9. Build order (each step merged with its tests)

1. **U1:** `tools/xhci-model.h` and `usb/xhci-core.c` against it (the No-Op self-test, rings, contexts, the root
   port, enumeration of a device's descriptors); the kernel's `SYS_drv` change, the DMA wait and fast-RAM rule;
   stage0's table and halt; `/sbin/usbd` with U1's scope and `lsusb`. Real run: section 6.2 steps 0-3.
2. **U2:** hubs and HID; `CONI_HELD` and the release at a caller's end; the `/etc/daemons` line (commented). Real
   run: steps 4-6. (U1 and U2 are built together on branch `usb`, 2026-11-12, as the main session asked.)
3. **U3:** diskd's USB side (3.5) and mass storage read-only, with the kernel-room decision. Real run: step 7.
4. **U4:** writes. Real run: step 8. Then the daemon line on by default on the PiStorm's images.

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

Revision 1 had two independent reviews (2026-11-12), read against the code. Both confirmed: a server may keep a
message and answer it later (`ipc_reply` finds any message it is serving); diskd, owner of `disk`, may post into a
window opened for `disk`; `CON_INPUT` is root's on any console and its raw keys follow the desktop's grab; the
`CONI_HELD` reasoning matches `kbd_inject`; `drv_exit` and `drv_zine` were read right; status 0 is "stopped" for
init; the measured room figures. What they found, and what revision 2 did:

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
