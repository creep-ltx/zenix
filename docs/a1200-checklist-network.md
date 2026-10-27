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

## 1. Start Zenix from AmigaOS, the everyday way

Boot AmigaOS with its network running (as every day: Roadshow up, the cable in), then start Zenix with `bootzenix`.
Log in as root, then:
```
dmesg | grep pi:
```
**Photo.** AmigaOS's network driver may leave the chip writing received frames into memory after the reset; since
2026-10-27 the start stops the chip's DMA before Zenix uses that memory (`zenixboot` 0.2b4 or the floppy's bootblock,
through the kernel's stage0). Look for:
- `pi: genet: stopped the DMA a previous owner left running: receive send` (or only `receive`, or only `send`): the
  stop found the chip running and stopped it. **Note** the line.
- No `pi: genet:` line at all: the chip was already quiet (AmigaOS's driver stopped it at the reset). Also fine.
- `(slow to stop)` at its end, or `not a GENET v5, left alone`: **photo**, and tell the session; then carry on.

Anything odd later (a crash, a frozen screen, wrong output in `ls -R /`): switch off, and do step 9 (from cold) to
tell the two apart.

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

## 8. Telnet to a BBS

`telnet` is built (DESIGN D20, [telnet](man/telnet.txt)): `telnet bbs.example.org 2323` (`-p` for a PC board). A
download from the board starts by itself (Zmodem); files land in the directory you started telnet in. **Note** how a
screen looks and how long a 1 MB download takes (the bottom row shows the progress). A phone book saves typing: a line
`name host port amiga` (or `pc`) in `~/.phonebook`, then `telnet name`.

## 9. Only if step 1 or later went wrong: start Zenix from cold

Switch the A1200 **off**, wait 5 seconds, switch it on, pick Zenix in the boot menu, and do steps 2-5 again. From cold
AmigaOS's network never ran, so nothing can be left running: if the trouble is gone this way, it came from the warm
start. **Note** which way showed the trouble, and tell the session (d19-network.md 14, step 9, "the warm start").

## 10. The crypto timings (DESIGN D21 step 2; on the stock A1200 and on the A1200 with the Apollo 060)

These numbers decide which X25519 code ssh uses and whether assembler is needed; FS-UAE's figures do not count. On
each machine, as root, after a cold start (nothing else running):
```
cryptotest
cryptotest time 3
cryptotest mul
```
**Photo** each screen. `cryptotest` must end `0 failed: ok`. From `cryptotest time 3` **note** every `time` line
(X25519 BearSSL and Monocypher, Ed25519 sign and verify, chacha20-poly1305, SHA-256). From `cryptotest mul` **note**
the two summary lines: "the same time for every operand" is what we hope for; "THE TIME DEPENDS ON THE OPERANDS" means
the 020's multiplier leaks timing, and the session switches BearSSL to its constant-time multiply.

| Measurement | Stock A1200 | Apollo 060 |
|---|---|---|
| X25519 BearSSL | ms | ms |
| X25519 Monocypher | ms | ms |
| Ed25519 sign / verify | ms / ms | ms / ms |
| chacha20-poly1305 seal 16 KB | KB/s | KB/s |
| muls.l / mulu.w: same time for every operand? | | |

## 11. ssh to the PC (DESIGN D21 step 3)

On the PC (once): sshd must be running (`sudo systemctl enable --now sshd`). Over Ethernet (the PiStorm) the PC's
firewall must let the Amiga in: `sudo ufw allow from 192.168.x.y to any port 22 proto tcp` (the Amiga's address from
`ifconfig eth0`). Over the serial cable and SLIP the PC is `10.0.2.2` and no firewall rule is needed. Write down the
PC's key fingerprint: `ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub`.

On the Amiga, as the user who will log in:
```
ssh-keygen
cat ~/.ssh/id_ed25519.pub
```
`ssh-keygen` makes the Amiga's own key (no passphrase, it asks nothing). The second line
shows the public key, one line starting `ssh-ed25519`. Put that line into the PC's `~/.ssh/authorized_keys`, the easy
way with the PC's password once:
```
ssh you@192.168.x.10 'mkdir -p .ssh; cat >> .ssh/authorized_keys' < ~/.ssh/id_ed25519.pub
```
The first connection shows the PC's fingerprint and asks: compare it with the one you wrote down, then type `yes`.
Then log in with the key:
```
ssh you@192.168.x.10
```
**Note** how many seconds from Return to the PC's prompt (on the stock A1200 the key exchange takes a while; `ssh -v`
shows each step with its time: **photo** that once). In the session try `ls --color`, `top`, `nano` or `less`, a
Swedish letter, Ctrl-C on a `sleep 100`; leave with Return, `~`, `.`. **Note** `free` on another console (Amiga+F2)
while the session is up. If the PC says "Permission denied (publickey)", check the line in `authorized_keys`.

## What to send back

The photos, the notes, and the speed table. Nothing on the card needs changing by hand; the session reads the rest
from `dmesg -p` on the next boot if needed.
