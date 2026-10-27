# The A1200 checklist: the CM4's Ethernet (DESIGN D19 step 9)

For the owner's weekend of 2026-10-30/31, on the PiStorm A1200. The driver (`/sbin/net/genet`, man page
[genet](man/genet.txt)) has only been tested on the PC against a model of the chip; this is its first run on the real
one. About 20 minutes. Photograph the screen where it says **photo**; write down the lines marked **note**.

## Before you start

- The card has the session's package on it: Zenix with `/sbin/net/genet`, and `/etc/network/interfaces` saying
  ```
  auto eth0
  iface eth0 inet dhcp
      driver genet
  ```
- A network cable from the PiStorm's Ethernet socket to the router (the same as AmigaOS uses).
- The PC is on and the Claude session on it knows you are testing (it starts the PC's side for step 7).

## 1. Start Zenix from cold

Switch the A1200 **off**, wait 5 seconds, switch it on, and pick Zenix in the boot menu. (Why from cold: AmigaOS's
network driver may leave the chip running when the Amiga is reset; from cold it never ran. Step 9 tries the other way.)
Log in as root.

## 2. The driver's start

```
dmesg | grep genet
```
**Photo.** Expected, in this order (numbers and addresses may differ):
```
genet: eth0: GENET v5.0 at f7580000
genet: eth0: PHY 600d84a2 at MDIO address 0 (BCM54213PE): negotiating
genet: eth0: trying Emu68's cache range opcodes in user mode in a child (...)
genet: eth0: Emu68's cache range opcodes work in user mode
genet: eth0: self-test: 4 frames through the loopback, checked byte by byte
genet: eth0: cache calls: Emu68's range opcodes from this process
genet: eth0: interface 1, dc:a6:32:xx:xx:xx, 32 receive buffers
genet: eth0: link up: 1000 Mbit/s, full duplex
```
**The Emu68 cache check (note which one you see):** the 4th line is either "work in user mode" or, instead, a line
`[kernel] pid N (genet) killed: vector 11 ...` followed by `the range opcodes failed in user mode (status 139): the
kernel's cache calls`. Both are fine: the second means Emu68 allows its cache instructions only inside the kernel, and
the driver uses the kernel's calls (a little slower). **Note** the line, and the self-test line whatever it says.

Then:
```
dmesg | grep eth0
```
**Photo.** Expected among them: `net: eth0: started /sbin/net/genet (pid N)`, `drv: pid N (genet) claims the device at` the chip's
address `(64 KB)`, `netd: eth0 up: ... DHCP, 32 buffers in its DMA memory`, `netd: eth0: link up`, and
`netd: eth0: DHCP gave 192.168.x.y`.

**If something else is there** (no link after 10 seconds, the self-test "came back wrong", a refusal): photo, then
try the driver's options one at a time. `edit /etc/network/interfaces`, put the word after `driver genet` (for example
`driver genet cache=kernel`), save, then `ifdown eth0` and `ifup eth0`, and look at `dmesg | grep genet` again:
first `cache=kernel`, then `noselftest`. **Note** which one helped.

## 3. The address from the router

```
ifconfig eth0
route
```
**Photo.** Expected: `flags=<UP,BROADCAST,RUNNING,DEFAULT,DHCP>  mtu 1500`, `ether` the same address AmigaOS's
network shows (the router's list of devices shows it too), `inet 192.168.x.y`, `lease N s left, from` the router,
`nameservers`; `route` shows `0.0.0.0` through the router on `eth0`.

## 4. Ping

```
ping -c 5 192.168.x.1
ping -c 5 1.1.1.1
host aminet.net
```
(The first address: the router's, as `route` showed it.) **Note** the `rtt min/avg/max` lines and the address `host`
gives. Expected: 5 received, 0% loss, each.

## 5. The link going away and coming back

Pull the cable, wait 5 seconds, plug it back in, wait 10 seconds:
```
dmesg | grep eth0 | tail -6
ifconfig eth0
```
**Photo.** Expected: `genet: eth0: link down`, `netd: eth0: link down`, then `link up` again, and `ifconfig` with an
address again.

## 6. Memory

```
free
ps
```
**Note** the free memory and the sizes of `netd` and `genet`.

## 7. The speed table

Tell the session "ready for the speed test". It starts the test servers on the PC (`SLIP_HOST_ADDR=<the PC's address>
tools/slip-host.py - none none /tmp/ready`) and tells you the PC's address (say 192.168.1.10). Then:
```
nettest -p 192.168.1.10 sliptcp slipudp
nettest -p 192.168.1.10 -n 10485760 slipget
nettest tcp
```
**Photo** each answer. They should say PASS. The table to fill in:

| Measurement | Where it is | Value |
|---|---|---|
| ping to the router (avg) | step 4 | ms |
| ping to 1.1.1.1 (avg) | step 4 | ms |
| 10 KB echoed by the PC (sliptcp) | step 7 | ticks (1 tick = 10 ms) |
| 10 MB from the PC (slipget) | step 7 | bytes/s |
| TCP over the loopback (nettest tcp: the stack's own cost) | step 7 | ticks |
| cache calls | step 2 | range opcodes in the driver / the kernel's |
| link | step 2 | Mbit/s |

These are the PiStorm's figures (Emu68), labelled so: not a real 68020's speed.

## 8. Telnet to a BBS (later)

Not yet: `telnet` comes with its own design note after the network's audit (D19 step 11). Skip.

## 9. Optional, last: starting Zenix from a running AmigaOS

Only if everything above worked and there is nothing on Zenix's disk you would miss. Boot AmigaOS with its network
running (as every day), then start Zenix with `bootzenix`. Use Zenix for 5 minutes with the cable in (`ls -R /`,
`ping -c 20` the router). **Note** whether anything odd happens (a crash, a frozen screen, wrong output): AmigaOS's
network driver may leave the chip writing into memory after the reset, which Zenix cannot stop before its own driver
starts. If anything odd happens, switch off, and tell the session: this is a known open point (d19-network.md 14,
step 9).

## What to send back

The photos, the notes, and the speed table. Nothing on the card needs changing by hand; the session reads the rest
from `dmesg -p` on the next boot if needed.
