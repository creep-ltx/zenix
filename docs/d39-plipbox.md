# D39: the plipbox, Ethernet through the parallel port

Approved by the owner on 2026-11-04 ("I've connected the plipbox. Maybe we can get it on also while we wait?";
`docs/decisions.md`); the details were left to this note. Built on branch `plip`, 2026-11-04. **Never run on a real
plipbox yet** (section 9).

## 1. What the owner gets, in plain words

An A1200 (any Amiga) with a plipbox in its parallel port reaches the network with the same
`/etc/network/interfaces` stanza as any card (`driver plipbox`): DHCP, ping, telnet, ssh, wget, at the plipbox's
speed (about 100 KB/s through the box on AmigaOS, its author's own measurement; section 7). The box keeps the MAC
address it had under AmigaOS, so the router hands out the same address. With no box, or the box off, the driver says
so in one line and lets the port go. A box that goes away (unplugged, its power off) is noticed at the next transfer,
or by the online frame the driver sends every 5 s on an idle link: the link goes down after three failures (about 15 s
when idle) and comes back by itself when the box answers again; a box restarted on an idle link is back online within
5 s (section 6).

## 2. The plipbox (facts read from its source)

Christian Vogelgsang's open hardware and firmware, `github.com/cnvogelg/plipbox`, **GPL-2.0-or-later** (its files'
headers: "either version 2 of the License, or (at your option) any later version"), so it may be read and used in
Zenix (GPL-2.0-only); Zenix's code is its own, written from what the sources do. An AVR (an Arduino Nano, ATmega328 at
16 MHz) with an ENC28J60 Ethernet chip, a bridge: what the Amiga sends goes onto the Ethernet unchanged, and what comes
for the Amiga's MAC (and broadcasts) comes back. **Version assumed: 0.6** (17 January 2015, the last release; on
`master` since then only build fixes: two 2023 commits in `avr/`, 5d29c88 "fixed inlining" and fa1c5ea's Makefile line,
no change in behaviour; the rest is the Amiga driver's build). The branches `dev` (2015) and `proto`
(2024-2025, a reworked protocol) were never released. The owner's box: an Arduino Nano on an ENC28J60 board.

**The wiring as the Amiga sees it** (`amiga/src/plipbox/hwpar.asm`, `hw.c`): the eight data lines are CIA-A's port B
(0xBFE101, its direction 0xBFE301: 0xFF the Amiga drives, 0x00 it reads); CIA-B's port A bit 0 is BUSY (the box's
answer, "RAK"), bit 1 POUT (the Amiga's "REQ"), bit 2 SEL (the Amiga starts each transfer); ACK, the box's "I have a
frame" pulse, is CIA-A's FLAG input (an edge sets FLG, bit 4 of CIA-A's ICR, a level-2 interrupt when enabled). CIA-B's
bits 3-7 are the serial port's lines (DSR, CTS, CD, RTS, DTR) and are never touched.

**A transfer** (`avr/src/pb_proto.c`): the Amiga waits for BUSY low, drives the command byte, raises SEL; the box answers
BUSY high. Then each byte is one toggle of POUT answered by one toggle of BUSY: the size word first (high byte), then
the frame, always whole words (an odd frame carries one more byte). At the end the Amiga lets go of the data lines and
drops SEL; the box drops BUSY. Commands: 0x11 send (the Amiga's frame, every byte answered), 0x22 receive (the box's
frame, every byte answered: the Amiga switches its port to input before the first POUT edge, the box drives the lines),
0x33 burst send, 0x44 burst receive (0.6). In a burst the size goes as above, then the box raises BUSY and the other
side toggles POUT without waiting for answers, the receiver polling POUT with its interrupts off. The box gives every
wait 500 ms (`pb_proto_timeout`), except inside its burst loops, which end only when SEL drops. **No CRC on the line**
(magPLIP, which plipbox came from, had one; plipbox has only the ENC28J60's Ethernet CRC on the wire): a byte damaged on
the cable is seen only by IP's and TCP's checksums.

**Who starts a transfer:** always the Amiga. The box pulses ACK when it has a frame (or a magic frame) for the Amiga and
keeps that one request until the Amiga takes it with a receive; it pulses again only after that (`bridge.c`
`trigger_request`, `req_is_pending`). A receive with no request behind it makes the box read its ENC28J60's memory where
no frame is (`fill_pkt` calls `pio_util_recv_packet` unconditionally): the driver makes none on its own, but it cannot
tell a real ACK edge from a false one. A false edge (noise on the cable, the box losing or gaining power) counts as a
request: with the box off, that receive times out and nothing happens; with a running box and no request, the box
reads its chip where no frame is: the driver gets a wrong or empty frame (dropped by netd's checks or IP's checksum) and
the chip's receive pointer may be disturbed until the box is switched off and on. plipbox.device has the same exposure.

**The magic frames** (`bridge.c`, `hw.c`): a 14-byte Ethernet header whose type is 0xFFFF (online: the source is the
Amiga's MAC; the box keeps it in its EEPROM and writes it there only when it differs), 0xFFFE (offline: the box drops the
network's frames), 0xFFFD (loopback: the box answers with an online request). From the box, type 0xFFFF means "send me
your online frame" (after its restart, when a frame comes from the Amiga while it is offline, or after a loopback); its
source is the MAC the box keeps. The box forwards a frame with the size it took, whole words: an odd frame reaches the
Ethernet with one byte more (padding after the IP packet; harmless).

**The configuration channel:** the box's own USB serial console (57,600 baud: `m` the MAC, `fd` full duplex, `fc` flow
control, statistics, test modes 1-4; `doc/src/firmware.md`). Zenix does not use it: everything the driver needs goes
through the magic frames. The box must be in its bridge mode (mode 1, the default after power-on).

## 3. Where it lives (D19's way)

`/sbin/net/plipbox` is a driver like `ne2000` (D19 2.3, 2.4, 5): netd starts it, it claims the port, registers its
quiesce program before touching anything, attaches with its MAC and moves frames with `DRV_XFER` in batches, copying each
received frame straight into netd's buffer. Polled, as every Zenix driver: it looks at the ACK count on each doorbell,
at once again while frames move, after 10 ms, then every 20 ms when idle.

Files: `net/drv/plipbox.c` (the Zenix side: the claim, the quiesce, the driver protocol, the box going and coming),
`net/drv/plipbox-core.c` and `plipbox.h` (the line protocol, shared with the PC test), `tools/plipbox-model.h` (a model of
the box's firmware and the CIAs), `tools/plipbox-test.c`, `tools/plipbox-test.sh`, part l of `tools/eth-test.sh` and
`KBD_PLIPBOX=1` of `tools/kbd-test.sh` (FS-UAE). The Makefile's `NETDRVS` ships it on every full root image and in the
base package (`tools/mkzup.py` takes `NETDRVS` as it is).

`/etc/network/interfaces`: `driver plipbox [mac=XX:XX:XX:XX:XX:XX] [noburst]`. `mac=` gives the box a MAC (it writes it
into its EEPROM once); without it the driver uses the box's own. `noburst` sends with command 0x11 (every byte answered)
instead of bursts, as plipbox.device's `NOBURST`. Other words are logged and ignored.

## 4. The kernel's part: the port, and the ACK line's edges

The CIAs are shared with the kernel (the tick, the keyboard, the E-clock, the floppy, the serial lines), so the kernel
gives out only what is the port's:
- **`DRV_CLAIM` takes one more base, `DRV_PARPORT` (0xBFE101).** No window: its quiesce steps are checked one by one
  (`par_step_ok` in `kernel/drv.c`): one byte at 0xBFE101 or 0xBFE301, or a `DRVS_RMW` of bits 0-2 only at 0xBFD000 or
  0xBFD200. Every Amiga has the port: no -ENODEV. A second claim is -EBUSY, as always.
- **The ACK line's interrupt is on while the port is claimed** (CIA-A's ICR written 0x90 at the claim, 0x10 when the
  claim ends and before ZINE hands the machine over: a write to the ICR changes only the bits written as 1, so the
  tick's and the keyboard's stay).
- **The level-2 handler counts FLG** (`par_acks`, `kernel/main.c`: one test at every read of CIA-A's ICR, which it already
  makes at every tick and every key). Reading the ICR clears all its bits, so only that handler may: the driver cannot
  look at FLG itself.
- **`DRV_PARACK` (op 8)** gives the claim's holder the count since boot (bit 31 clear).

Kernel growth (unchanged by the review's fixes): `drv` 2,187 -> 2,444 bytes, `main` 4 bytes of bss; `kernel.bin` 180,752 -> 180,992 bytes (its budget
line `drv` 2,200 -> 2,500; `TOTAL` 181,000 leaves 8 bytes). Claimed in `docs/claims.md`, frozen in
`docs/frozen-interfaces.md`.

**The rule for CIA-B's port A** (D19 5.5 asked for one): its bits 3-7 are the serial port's and bits 0-2 the driver's;
everyone changes them with a single `bset`/`bclr`/`and.b` (an interrupt cannot fall between its read and its write) or
with interrupts masked. The kernel touches neither port A nor its direction today. The quiesce program's
read-modify-write is C in `kernel/drv.c`'s `quiesce()` (a read, then a write): it is safe only while no interrupt
handler writes CIA-B's port A or its direction; a handler that ever does (hardware handshake on the serial lines, say)
must come with interrupts masked around that step.

**Which ACK pulses are served.** The box cannot pulse during a transfer, and the level-2 interrupt is taken at once while
the driver runs (user mode, interrupts open). So the count the driver reads after the box has answered its receive
command (BUSY high) includes every request that receive serves; a pulse after it is a new request. The driver keeps that
value and makes a receive only when the count differs (as plipbox.device clears its pending flag before dropping SEL).

## 5. Decisions

- **Receives are handshaked (0x22), never bursts (0x44).** In the box's burst receive the Amiga toggles POUT and reads
  the byte without waiting; the box puts the next byte out about 1.6 us after each edge. plipbox.device does it with
  every interrupt off (`Disable()`). A Zenix driver is a user process: an interrupt or another process falling between its
  POUT write and its read makes it read the next byte, and no CRC sees it. The model shows it: 139 of 200 frames wrong
  with the gaps of a process, none without (section 8, test 17). The handshaked receive takes every gap (each byte waits
  for its answer; the box waits 500 ms for each).
- **Sends are bursts (0x33) by default.** The Amiga writes the byte before each POUT edge and the box samples it at the
  edge, so a gap only makes the burst longer: never a wrong byte (tested with gaps). The box polls with its interrupts
  off, so a gap stalls it (its serial console and timer); the ENC28J60 holds incoming frames meanwhile. `noburst` is the
  way back if a box disagrees. 0x33 is 0.6's; a 0.5 box knows only 0x11 and 0x22 (`noburst`), *read* from its tag, its
  magic frames not checked.
- **Timing:** every wait reads CIA-B's port A in a loop: each read is a CIA access, one E-clock cycle (1.41 us) on every
  CPU, so the loop is paced by the hardware, never an empty loop; every 32 reads it looks at the kernel's tick (100 Hz
  from a CIA timer whose latch the kernel sets for PAL or NTSC: no rate is assumed here) and gives up past the
  transfer's deadline (50 ticks, 500 ms, plipbox.device's default, also the box's), looking at BUSY once more first (the
  process may not have run while the clock did). After three failed transfers in a row the link is down and the box is
  looked for every 2 s with a 5-tick deadline. On an idle link an online frame goes every 5 s (the keepalive: one
  14-byte transfer, the same MAC, so the box writes nothing): a box restarted meanwhile is online again, one gone is
  seen.
- **Is polling fast enough?** Inside a transfer the driver is never slower than the box: it answers each BUSY edge at
  its next CIA read. Between transfers only the ACK count is polled, by the driver's turn: a frame from the network waits
  at most 20 ms on an idle link (as `ne2000`), none while frames move. An interrupt that wakes the driver (D19 5.4) would
  take that away; not needed for a first version.
- **The MAC:** at its start the driver sends a loopback frame, takes the box's answer (an online request whose source is
  the MAC the box keeps), and sends its online frame with that MAC: nothing is written into the box's EEPROM, and the box
  keeps the address AmigaOS gave it (plipbox.device's default is 1a:11:af:a0:47:11; Roadshow may set another). An
  all-zero, broadcast or multicast address from the box falls back to plipbox.device's default. `mac=` overrides.
- **A box whose last request was never taken** (the Amiga restarted while the box was online and a frame came) pulses no
  more until that request is taken. The start's receive after the loopback takes it: the loopback's answer is a magic
  frame, never a read of the chip, whether or not a pulse came (the driver waits 50 ms for one).
- **A box held in a burst** (found reading `cmd_send_burst`): when SEL drops inside the box's burst, its way out
  (`send_burst_exit`: `while (!REQ) { if (!SEL) goto exit; } CLR_RAK; while (REQ) { if (!SEL) goto exit; }`) loops
  back to itself for as long as SEL stays low, and the box takes no command until SEL rises. It tests POUT before SEL,
  so a held box's BUSY can be high (POUT low) or low (POUT high: it has gone through `CLR_RAK`): BUSY says nothing. That
  happens when a burst is cut off: the end not answered, or the driver killed in it (the quiesce program drops SEL).
  `pb_unstick` frees it: POUT up, the data lines driven with 0x00, SEL up, wait for BUSY low, POUT down, wait for BUSY
  high, then SEL and POUT down. A box that was not held reads that as the command 0x00, not one of its four: it answers
  BUSY high and waits for SEL low, once (the test checks one unknown command per start). The lines are driven so this
  does not depend on the port's pull-ups (undriven lines read 0xFF only if pulled up; a floating 0x22 would be a
  receive without a request). Done at every start and every look for a lost box, whatever BUSY says (a few
  microseconds for a box that was not held), and after a burst send that failed at its start or end. Every transfer
  ends with SEL and POUT dropped in one write (`and.b`), as the quiesce program does.
- **The end:** an offline frame (50 ms deadline), then the port as the machine starts it (SEL and POUT low, then all
  three lines and the data lines inputs), as plipbox.device leaves it. The quiesce program does the same in three steps.
  Lines left as inputs are pulled high, which the box reads as SEL up: harmless (it acts on SEL only with POUT low), and
  the next start frees a box left in a burst.
- **A receive cut between its size bytes** (found by the review in `cmd_recv`): when the box gives up waiting for POUT
  after its size's high byte (the driver away for more than 500 ms just there), it returns without switching its data
  pins back to inputs (`par_low_data_set_input` is only on the later paths; `cmd_recv_burst` has the same early
  returns). It then keeps driving the data lines until a receive of its own ends, and reads its own byte as every
  command after: in practice it is stuck until switched off and on, and both sides drive the lines while the Amiga sends
  (the model counts it). The window is a few instructions long, so it needs a process kept away for half a second
  exactly there; the driver logs "a receive was cut between its size bytes: ... switch the box off and on" when its
  wait for the size's low byte fails, and the link goes down. A box switched off and on is found again (section 6).
- **A driver away for half a second inside a frame** (found 2026-11-05 by the model's gap test once the line's timing
  changed): the box gives up on a wait after 500 ms and, with SEL still up and POUT low, takes the lines' level (the
  pull-ups' 0xFF) as a new, unknown command and answers it, so BUSY moves again and a driver that comes back reads on:
  a frame of the right size with 0xFF bytes in it, which nothing on the line catches. Inside every handshaked frame
  (receives and `noburst` sends) the driver looks at the kernel's tick every 64 words and at the frame's end
  (`pb_gap`, a system call per 128 bytes); 30 ticks or more since the last look and the frame is dropped ("the
  driver was away as long as the box waits: not trusted"). Bursts need none: the box has no deadline inside them.
- **Frames from the box:** a size above 1,514 (or above the buffer) is refused before its bytes (the box ends its side
  when SEL drops); shorter than a header: dropped; a size of 0: an empty answer, counted; an online request: the online
  frame sent; a loopback frame (the box's test modes): sent back; everything counted (netd's `rx_bad`, `tx_err`).

## 6. The box going and coming

Gone in the middle of a frame: that transfer times out (the frame is lost; TCP sends it again). Gone on an idle link:
the next keepalive (within 5 s) or frame out times out. Three failures in a row: "the plipbox stopped answering: link
down", frames to send are dropped. Every 2 s the driver frees a held box and sends the loopback frame again (the same
MAC: netd's interface keeps its address); an answer: "the plipbox answers again: link up". A box restarted without the
driver seeing it (its reset button, USB power) is offline and drops what the network sends: the next frame out, or the
keepalive within 5 s, puts it online again (a frame out also makes it ask for the online frame).

## 7. Speed (the model's clock, not measured)

On the model (section 8), with each CIA access one E-clock cycle and the box's times estimated from its code: a 1,514-byte
frame takes 6.4 ms to send in a burst (230 KB/s), 11.1 ms handshaked (133 KB/s), 11.1 ms to receive (133 KB/s, the box's
2.3 ms from its chip included). The plipbox's own benchmark (`doc/src/benchmark.md`, 0.6, an A500 with an ACA 1230/33
under AmigaOS) measured 234.8 KB/s for burst sends on the line and about 114 KB/s through the whole bridge, which the
model's burst figure matches. Real numbers come from the A1200 (section 9).

## 8. Tests

- **On the PC, `tools/plipbox-test.sh`** (in `make check`'s host tests; 1.2 s): the core against `tools/plipbox-model.h`,
  a model of the box's firmware as read and of the CIAs, strict where hardware would go wrong silently (both sides
  driving the data lines, a half-driven port, BUSY driven by the Amiga, a receive with no request, the serial port's bits
  touched). Checked: the start (the box's own MAC adopted with nothing written to its EEPROM; `mac=` written once);
  1,500 frames each way and 800 each way at once, byte for byte, for burst and handshaked sends, with a 68060's and a
  14 MHz 68020's instruction times; 600 each way with gaps of 5 us to 30 ms before one access in 200 (none wrong, none
  lost); gaps of 0.4-0.7 s (frames lost, none wrong, the line fine afterwards, the box switched off and on if a receive
  was cut between its size bytes); the frame times above; no box ("nothing
  answers ... BUSY stays high" within 0.7 s, the port left as found); a deaf box; a box whose last request was never
  taken; a driver killed in a burst (the quiesce program; the next start frees the box); SEL dropped in a burst with
  POUT high (held with BUSY low in 5 of 8 runs; every next start frees it); a receive cut between its size bytes (the box
  keeps the lines, both drive, the driver says to switch it off and on; afterwards frames go); the keepalive (a box
  restarted on an idle link online again, one gone: link down after 3); one unknown command per start; a burst short of an edge (no
  answer at its end, the box freed at once); a size of 3,000 and a 9-byte frame from the box; the box gone in a receive
  and in a burst send (link down after 3, back restarted: link up, the same MAC); a box restarted unseen; no free
  buffer (the request waits); the loopback frame; the end and the quiesce program as the kernel checks it; the box's
  burst receive as AmigaOS does it, with and without gaps (section 5); one gap of 0.6 or 1.2 s at 248 places of a
  receive and a handshaked send (the frame lost or whole, never wrong).
- **In FS-UAE, `tools/eth-test.sh l`**: FS-UAE has no plipbox. Nothing on the port: "nothing answers on the parallel
  port: BUSY stays high (no plipbox, or it is off)" and status 1, five times, then netd gives it up; the port claimed and
  released with its 3 quiesce steps each time; no fault, no eth0, `ping 127.0.0.1` answers; 200 sectors of the boot floppy
  read back as the host's image afterwards (the floppy's lines share CIA-B with the port). **`KBD_PLIPBOX=1
  tools/kbd-test.sh`**: the whole keyboard test with the driver trying five times from the boot on (it turns CIA-A's FLG
  interrupt on and off; the keyboard shares that ICR). Also `eth-test` e g p i m, `kbd-test`, `drv-test`, `netd-test`
  (the code they share).
- **Not run anywhere yet:** a real plipbox; the AVR's real reaction times (the model's are estimated from its code); the
  A1200's ACK edge reaching FLG and the level-2 interrupt; the pull-ups' levels on the A1200's port; DHCP and TCP
  through the box.

## 9. The first run on the A1200 (the owner's, with the partner)

1. The plipbox in the parallel port, powered (its USB), in its bridge mode (a fresh power-on), its Ethernet cable in.
2. Boot Zenix. As root, `/etc/network/interfaces` with `auto eth0`, `iface eth0 inet dhcp`, `driver plipbox` (another
   interface for the FA411 is fine: `eth1`); then `ifup eth0`.
3. `dmesg | grep 'plipbox\|drv:'` and send the lines back: "drv: pid N (plipbox) claims the parallel port", "plipbox
   found (burst sends), MAC ... (the box's own)" and "interface N", or the line saying why not.
4. `ifconfig eth0` (a lease), `ping -c 3` the router, then `wget` a file of a few hundred KB (its KB/s) and a telnet to
   a BBS.
5. Pull the box's USB (power) for a few seconds while pinging: "link down", then "answers again: link up" within 2 s of
   power coming back, and pings again. The same once with no traffic: the link goes down within about 15 s.
6. `ifdown eth0` ("the doorbell closed ... the port let go"). If the box's serial console is open on a PC: its log shows
   `[MAGIC] online` at step 2 and `[MAGIC] offline` here.
7. If anything fails: the same with `driver plipbox noburst`.
Expected failure points, in the order they would show: BUSY read high with the box on (the pull-ups, the cable), no
answer to the command (the box not in bridge mode, another firmware), no ACK counted (the frame from the box never
comes: the FLG interrupt), frames wrong (the burst's timing on the 060: `noburst` tells).

## 10. Sizes (*measured* 2026-11-04, `-m68020-60 -Os`)

`/sbin/net/plipbox` 12,308 bytes on the disk (code 6,775, bss 3,332; in memory 14,372 bytes, with its 8 KB stack about
22 KB; `ne2000` is 18,188). The kernel
+240 bytes (section 4). A machine whose `/etc/network/interfaces` does not name it pays the kernel's 240 bytes only.

## 11. Open questions for the owner (a safe default in place)

None that blocks the first run. Later: an interrupt that wakes the driver at the ACK edge (D19 5.4) would remove the
up-to-20 ms wait on an idle link.

## 12. The review (one independent read-only pass, 2026-11-04)

It found no bug that breaks normal use, and confirmed the handshakes, word counts, magic frames, the ACK accounting,
the kernel's checks and the driver's buffer and tag handling against plipbox 0.6's source. Fixed after it: the model's
burst exit tested SEL before POUT (the firmware tests POUT first, so a held box can show BUSY low): modelled as written,
and the box is now freed at every start and every look for it, not only when BUSY is high, with SEL and POUT dropped in
one write at every transfer's end; the freeing drives 0x00 on the data lines instead of relying on the pull-ups; it no
longer overwrites the failed stage that the log names; the model freed the data lines on a failed receive where the
firmware does not (section 5: the box kept driving after a receive cut between its size bytes, now modelled, counted,
logged); the deadlines are in the kernel's ticks, not a hard-coded PAL E-clock rate; an idle link sends a keepalive every
5 s; the notes on false ACK edges, the 2023 firmware build fixes and the kernel's C read-modify-write corrected.

## 13. On the real hardware (2026-11-04)

The owner's plipbox (an Arduino Nano with an ENC28J60) on the parallel port of the A1200 with the Apollo 1260, release
2026110402, `driver plipbox` as eth1 with the FA411's eth0 taken down: it worked at the first start. The box's own MAC
(1a:11:af:a0:47:11, plipbox's default), DHCP gave an address, `ping google.se` 8 of 8 answered, 33-48 ms (the FA411:
24-29 ms). Throughput not yet measured. Read from a photo of the screen.

