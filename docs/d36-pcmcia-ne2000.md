# D36: an NE2000 PC Card in the A1200's slot (the Netgear FA411)

Approved by the owner on 2026-11-03 ("do the FA411 driver after the installer"; ROADMAP block 4, `docs/decisions.md`);
the details were left to this note. Built on branch `ne2000`, 2026-11-03. **Never run on a real card yet** (section 8).

## 1. What the owner gets, in plain words

The A1200 with the Apollo 1260 reaches the network through the Netgear FA411 in the side slot, with the same
`/etc/network/interfaces` stanza as any other card (`driver ne2000`): DHCP, ping, telnet, ssh, wget. Taking the card out
puts the link down; putting it back brings it up again. A machine that cannot use a card (memory in the slot's
addresses) is told so in one line and nothing is touched.

## 2. Where it lives (D19's way, nothing new)

`/sbin/net/ne2000` is a driver like `a2065` and `genet` (D19 2.3, 2.4, 5): a user process netd starts, which claims
its device, registers a quiesce program, attaches with its MAC and moves frames with `DRV_XFER` in batches. It copies
frames through the card's data port (D19 5.1's "programmed I/O" shape: no DMA region). It is **polled**, as `a2065` and
`genet` are, because D19's interrupt programs for drivers (5.4, `DRV_IRQ`) are not built: they are designed "with the
first interrupt-driven driver", and this driver does not need to be that one (section 6).

Files: `net/drv/ne2000.c` (the Zenix side: claim, quiesce, the protocol, the card leaving and coming back),
`net/drv/ne2000-core.c` and `ne2000.h` (the slot, the CIS, the chip; shared with the PC test),
`tools/ne2000-model.h`, `tools/ne2000-test.c`, `tools/ne2000-test.sh` (the PC test, as `genet-test`), and parts p, i, m
of `tools/eth-test.sh` (FS-UAE). The Makefile's `NETDRVS` ships it on every full root image and in the base package.

## 3. The kernel's part: one more device a claim may name

The only kernel change is in `kernel/drv.c`'s `window()`, the place a2065's and genet's claims already go through:
`DRV_CLAIM` takes one more base, **`DRV_PCMCIA` (0xA00000)**, the PC Card slot. Its window is 0xA00000-0xA4FFFF
(attribute memory, the I/O space with its odd-byte half, the card's reset byte), so the quiesce steps may write
there and nowhere else. The kernel grants it only:
- on a machine whose Gayle answered as an A600/A1200's (`ide_gayle()`, set by `ide_init` from the Gayle ID it already
  reads); otherwise **-ENODEV**;
- when no memory lies in the slot's addresses 0x600000-0xA4FFFF: neither one of Zenix's pools nor a RAM board in the
  boot's board list (one Zenix did not take still answers there); otherwise **-EADDRINUSE**, and the kernel logs
  "drv: memory at A-B is in the PC Card slot's addresses: the slot is not claimed".

No new op, no new system call, no new number: `DRV_PCMCIA` is a constant in `kernel/netproto.h` (frozen, as the other
`DRV_*` values), `-EADDRINUSE` is an existing error number with this meaning for this op. Kernel growth: 296 bytes of
code (`drv` 1,875 -> 2,171; its budget line 1,900 -> 2,200). The driver writes Gayle's card-control and configuration
registers (0xDA8000, 0xDAB000) by plain pointers, as every driver writes its registers; it never writes Gayle's
interrupt request or enable registers (0xDA9000, 0xDAA000), which stay the kernel's (`kernel/ide.c` keeps the enable
register 0: no PC Card interrupt can reach level 2).

## 4. The slot (Gayle), as the driver uses it

Facts *read* from Linux's `arch/m68k/include/asm/amigayle.h` and `arch/m68k/amiga/pcmcia.c`, and `apne.c` (Linux's
NE2000 PC Card driver for the A1200):
- **Addresses:** common memory 0x600000-0x9FFFFF (not used by this driver), attribute memory 0xA00000-0xA1FFFF (one
  byte at every even address), I/O 0xA20000-0xA2FFFF (words, and bytes at even ports), **odd ports' bytes at
  0xA30000 + port - 1** (apne's `GAYLE_ODD`), the card's reset at 0xA40000 (a write raises the reset line, a read
  drops it).
- **Gayle's registers:** card status 0xDA8000 (bit 6 a card is in, bit 3 writes on),
  interrupt request 0xDA9000 and enable 0xDAA000 (bit 6 card change, bits 5-2 the card's own lines), configuration
  0xDAB000 (bits 1-0 the programming voltage, bits 3-2 the access time).
- **Detecting a card:** card status bit 6, read whenever the driver looks (every turn): no interrupt is needed for it.
- **Bring-up order:** Gayle set to 0 V programming voltage and 250 ns access time (apne's choice), writes on (card
  status = 0x18, Linux's `pcmcia_write_enable`); the card reset (held 10-20 ms, two ticks, then 100 ms to wake: apne
  holds it 10 ms, the PC Card standard asks at least 20 ms before access); the CIS read; the card configured (section 5).
- **16-bit access:** the data port is read and written as 68k words; the card's even byte is the word's high byte
  (apne reads them raw), so a frame comes out in memory order with no swapping. Registers are bytes.
- **Interrupts:** none. Gayle's enable register stays 0 (the kernel's), so neither the card change nor the card's
  interrupt line raises level 2; reading the attribute memory may set Gayle's "write" request bit (a comment in
  Linux's `pcmcia.c`), which does nothing while the enable is 0. The chip's own mask (IMR) stays 0.
- **Insert and removal** (`ne_watch` in the core, called at the top of every turn of the driver's loop, so the PC test
  drives it as the driver does): a card put in while the driver is not running is found at the next start (`ifup
  eth0`, or netd's restart). Taken out while it runs: the next turn sees bit 6 clear (a look at the chip that failed
  because the card had gone is not retried: the turn after says it), the driver says so, replaces its quiesce
  program with an empty one (the steps must not write into the next card, or into the empty slot), reports the link
  down, drops frames to send (TCP sends them again) and touches nothing of the slot but the status byte until a card
  is back. A card must then stay in for half a second without a bounce before it is brought up from the reset; the
  same MAC carries on (the quiesce program registered again), another one ends the driver (netd starts it again,
  which attaches the new address), a card that cannot be used ends it too. Left: a driver that dies in the few
  milliseconds between a pull and the next turn has its quiesce steps written into the empty slot (two byte writes;
  Gayle is believed to end such cycles without a bus error, *not verified*).

## 5. The card: its CIS and its configuration

The CIS is read byte by byte from attribute memory, at most 8 KB (`CIS_MAXBYTES`), every tuple's length checked
against what is left: a damaged CIS can fail the card, never make the driver read past the window. Kept:
`VERS_1` (the maker and product, logged), `MANFID`, `FUNCID` (6 = network), `CONFIG` (the base of the configuration
registers, which must lie in the window, and which of them exist), and the first `CFTABLE_ENTRY` with a non-zero index
whose I/O (its own, or the default entry's) fits: 32 ports, or two windows of 16 one after the other, at a base Gayle
can reach (below 0xFFE0, 32-aligned); an entry with no ranges and at least 5 address lines takes any base, and the
driver gives it 0x300 (apne's). Power and timing descriptions are skipped by their own lengths.

Refused, with a line in the log: no CIS; a `FUNCID` other than network (a memory card, a disk: "not a network card
(function F): left alone", nothing written to it); no configuration registers; no entry that fits.

Configured as the known drivers do:
- **An AX88x90 card** (the configuration registers at 0x3C0, Linux's own test in `axnet_cs`'s `get_prom`, or a product
  string naming FA411, AX88190 or AX88790): option register 0x45 (Linux: index 5 with level interrupts, never enabled
  here), 40 ms, the status register 0, then the I/O base registers (0x3CA, 0x3CC) set to the base (FreeBSD: "we write
  the address the card is at"; Linux writes them for every AX88190 card: "don't trust the CIS on this").
- **Other cards:** the entry's index in the option register (apne), the status register 0 when it exists, the I/O
  base registers only when the entry takes any base and the registers exist.

## 6. The chip

**The FA411 is an ASIX AX88x90 card.** *Read:* Linux lists "NETGEAR" "FA411" in `axnet_cs` (the AX88190 driver), not
in `pcnet_cs`; FreeBSD's `if_ed_pccard.c` says "fa411 has ax88790" and notes that the D-Link DFE-670TXD, a DL10022
card, shares its ID; NetBSD's `if_ne_pcmcia.c` lists it as a plain NE2000. The AX88790 is the AX88190 with its PHY
inside. So the driver handles three chips and finds out which at run time:
- **AX88790 / AX88190** (`ne_probe`): the chip reset (its reset port read and written back, then ISR's reset bit, at
  most 30 ms); a 32-byte pattern written into the frame memory at 0x4000 in word mode and read back (FreeBSD's probe);
  the TEST register (0x15) non-zero means an AX88790, whose internal PHY is selected (GPIO 0x17 = 0x10, Linux); the
  MAC address from 0x400 of its memory (FreeBSD: "it's just at 0x400 in SRAM"; Linux reads the same words); the PHY
  found by reading its registers 1 and 2 at every MII address through the bit-banged MII port (0x14), and, when none
  answers, once more after the configuration status register's power-down bit (Linux's and FreeBSD's way). **No PHY
  at all, or no chip answering, means no AX88x90** (FreeBSD: every AX88x90 has one): the driver then configures the
  card again as its CIS says (the entry's own index instead of Linux's 5, the I/O base registers only where they
  exist) and reads it as a DP8390 (the DL10022-inside case).
- **DP8390 and clones:** ne.c's PROM read (32 bytes in byte mode; a 16-bit card gives each byte twice; an 8-bit NE1000
  is refused), then word mode and the same memory check. No PHY: the link is taken as up.
- A MAC address that is all zeros, all ones or a multicast one is refused.

**The AX88x90's quirks, handled as `axnet_cs` handles them:** RCR's bit 6 always set; full duplex in TCR's bit 7, set
from the PHY's partner abilities when the link comes up; an interrupt status bit that does not always clear at the first
write (written again, up to 10 times); no wait for the remote DMA's completion bit after a write (Linux does not wait;
for a DP8390 the driver waits at most 20 ms and restarts the chip if it never comes).

**Both chips the same way after that:** 16 KB of frame memory at 0x4000-0x7FFF: two send buffers of 6 pages, the
receive ring from page 0x4C to 0x80 (Linux's layout). National's start order (stopped, word mode, loopback, the ring,
the station address read back, started, then send and receive on: broadcasts and the station's own frames, no
multicast). Receiving: the ring is read from the driver's next page up to the chip's current page; each frame's
4-byte header is checked (its next page inside the ring and not its own page, its length at most 1,540, a buffer:
otherwise the chip is started again and the frames in the ring lost), a frame with a bad status or a length outside
64-1,518 is counted and skipped, a good one copied without its
CRC straight into a netd buffer (the chip's remote DMA wraps at the ring's end by itself). A next page that is not
where the frame's length puts it is logged (four times) and followed, as Linux does. A receive overrun gets
National's procedure (stop, 2 ticks, clear the count, loopback, start, empty the ring, back, send again what was cut
off). Sending: a frame (padded to 60 bytes) into a free send buffer, started at once when the chip is idle, the
next one when the first is done; its tag goes back to netd as soon as it is copied (as a2065). A send not finished
after a second restarts the chip. The PHY is read every second (its link bit read twice: it latches low).

## 7. Memory in the slot's addresses (the A1200 trap)

A PC Card answers at 0x600000-0x9FFFFF and 0xA00000-0xA4FFFF. Memory there (8 MB of Zorro II RAM fills
0x200000-0x9FFFFF; some A1200 accelerators put memory there too) clashes with any card in the slot. **Zenix refuses:** the kernel never grants the slot's claim then (section 3),
the driver says "memory lies in the PC Card slot's addresses (0x600000-0xa4ffff): a card cannot be used on this
machine", and netd gives it up after five tries; nothing of the slot is touched. The kernel's own line names the
memory's range. What Zenix cannot do is protect such a machine from a card put in while it runs: the clash is in the
hardware; the manual page says so.

**The owner's Apollo 1260:** a CPU-slot accelerator's 64 MB lies in the 68060's 32-bit space (from 0x08000000 on such
boards, *read*, not measured on this machine), so no clash is expected; the check runs on every start anyway and the
first run will say (section 8). **A1200s with an accelerator that do not start with a card in the slot** are reported
by users of other drivers (a forum report of the FA411 with a Warp1260); Zenix resets the card itself before use, so
the card may be put in after power-on.

## 8. Tests, and what has not run

- **On the PC, `tools/ne2000-test.sh`** (in `make check`'s host tests), the core against `tools/ne2000-model.h`, a model
  that is strict where hardware would misbehave silently (an odd port through the even window, a word at a register,
  the wrong transfer width, reading past the remote DMA's count, I/O before the card is configured, any write to
  Gayle's interrupt registers or outside the configuration registers): an FA411-like AX88790 brought up as Linux
  configures it, its link down and up (100 Mbit/s full duplex in TCR), 3,000 frames each way checked byte by byte with
  the ring wrapping, 500 each way at once, a netd without buffers (drops counted), an overrun, a damaged header (the
  chip started again), a stuck send, the quiesce program applied to the model; the card's comings and goings through `ne_watch` as
  the driver's loop calls it: out during a look at the chip (nothing touched, no frame made up), a card bouncing for
  0.4 s (not touched), back after half a second, another card (its MAC differs), a memory card put in instead; an AX88190 whose PHY is powered down; a plain DP8390 card (power and timing descriptions skipped,
  doubled PROM); a card named FA411 with a DP8390 inside (the model answers no I/O for a configuration the card does not have:
  configured again with the CIS's index, read as a DP8390); refusals (an NE1000, a memory
  card, an empty slot, no CIS, a card that decodes other ports); 20,000 damaged CISes under AddressSanitizer.
- **In FS-UAE, `tools/eth-test.sh` parts p, i, m**: FS-UAE 3.2.35 emulates no PC Card network card (only Gayle's slot
  with a memory or IDE card; *verified* from its binary's strings and options). So: p, an empty slot (the slot claimed
  and released with no quiesce step, "no card in the PC Card slot", given up after five, no stray interrupt, netd
  still serves); i, FS-UAE's PC Card IDE disk in the slot (its CIS read through the emulated Gayle: "UAE" "68000",
  function 4, "not a network card", left alone); m, 8 MB of Zorro II RAM (the kernel refuses the slot and names the
  memory, nothing claimed). Also `eth-test` e and g, `drv-test`, `netd-test` again (the code they share).
- **Not run anywhere yet:** a real FA411 (or any real PC Card) on a real A1200; the card's real CIS (the test's FA411 CIS
  is a stand-in built from what Linux and FreeBSD say: the strings, the registers at 0x3C0 with I/O base registers;
  its MANFID and entry are guesses); real Gayle timing at 250 ns; the MII bit-banging on the real chip; the few lines
  of the driver's loop around `ne_watch` (the quiesce program swapped, the driver's end), read but not run; DHCP, ping
  and a TCP transfer through the card.

**Run on the real hardware (2026-11-04, the A1200 with the Apollo 1260, the FA411, over the serial console):** it
works: the card's CIS read ("NETGEAR" "FA411", manufacturer 0x149 card 0x411), an AX88790 at port 0x300
(configuration 7, registers at 0x3c0), its internal PHY at MII address 0x10, link up at 100 Mbit/s full duplex, DHCP
gave an address, `ping google.se` answered (24-29 ms). Three things the PC's model had wrong, all fixed with the model
changed to match: the chip's ISR never shows the reset bit after the reset (Linux only logs it: so does Zenix now); a
byte read through Gayle's odd window returned the even port next to it on this machine, while byte writes there and
word reads were right (odd ports are now read by a word of the pair, its low byte); the command register reads 23
when stopped, the start bit still set (no longer looked at).

**The first run on the A1200 with the 1260** (the owner's machine, when it is home):
1. Boot Zenix; put the FA411 in after power-on if the machine does not start with it in.
2. As root, `/etc/network/interfaces` with `auto eth0`, `iface eth0 inet dhcp`, `driver ne2000`; then `ifup eth0`.
3. `dmesg | grep 'ne2000\|drv:'` and send the lines back: the claim of 0xa00000 (or the memory clash, with its
   range), the card's own words (`card: "NETGEAR" "FA411", manufacturer M card C, function 6`), the chip and its
   configuration (`AX88790 at port 300 (configuration N, registers at 3c0)`), the link, the MAC.
4. `ifconfig eth0` (a lease), `ping -c 3` the router, then `wget` a file of a few hundred KB and a telnet to a BBS.
5. Take the card out ("the card left the slot"), put it back ("the card is back"), ping again.
6. `ifdown eth0` ("the doorbell closed ... the card stopped").
Expected failure points, in the order they would show: the card not found (CIS or configuration: the log's `card:`
line says what the card said), the chip not answering at port 0x300 (the I/O base registers), no PHY (the MII
timing), frames not moving (the ring or the byte order).

## 9. Sizes (*measured* 2026-11-03, `-m68020-60 -Os`)

`/sbin/net/ne2000` 15,616 bytes (`a2065` 8,752: the CIS reader, the PHY, the two chips, the card's comings and
goings); in memory with its stack 19,436 bytes before its first frame. The kernel +296 bytes (section 3). A machine
whose `/etc/network/interfaces` does not name it pays nothing.

## 10. The review (one independent read-only pass, 2026-11-03)

It found nothing that could crash the machine or write outside the slot, and confirmed the Gayle, CIS, AX88x90, MII
and overrun facts against the sources. Fixed after it: a card pulled during a look at the chip skipped the "left"
handling (its quiesce program stayed registered): the comings and goings moved into the core (`ne_watch`) and are
tested; a card taken for an AX88x90 that is not one stayed on configuration 5: configured again from its CIS, and the
model now answers no I/O for a configuration the card does not have; a receive header pointing at its own page would
have stalled receiving: a restart now; an overrun could read more frames than one transfer carries: bounded; the
doc's numbers (the header's limit, the reset's 10-20 ms, a Gayle bit no source names) corrected.

## 11. Open questions for the owner (a safe default in place)

None that blocks the first run. One for later: an interrupt-driven version (less polling while idle) would be the
first user of D19 5.4's `DRV_IRQ` and needs its own design; the default is polling, as every Zenix driver today.
