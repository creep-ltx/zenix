# The network over the serial cable (SLIP): a stock A1200 and a Linux PC

Any Amiga that runs Zenix can join the network through its serial port: a null-modem cable to a Linux PC, and a small
program on the PC, `zenix-slirp`, that passes the Amiga's packets on to the PC's network and the internet. No
network card, no root on the PC. This is the way for a stock A1200 (DESIGN D19, docs/d19-network.md section 5.3).

What it gives: `ping`, and every program that uses sockets, reaching the PC and the internet. Speed: the serial port
runs at 115,200 baud, so at best about 11 KB a second; plenty for a terminal, slow for big files.

## What you need

- **A null-modem serial cable**: DB25 on the Amiga's serial port; on the PC a 9-pin serial port or a USB-serial
  adapter (RS-232 levels, not a 3.3 V TTL one). Three wires are enough: transmit, receive, ground (no handshake).
- **A Linux PC** with libslirp (the user-mode network of QEMU) and its GLib:
  - Arch, CachyOS: `sudo pacman -S libslirp`
  - Debian, Ubuntu: `sudo apt install libslirp-dev build-essential pkg-config`
- **This repository** on the PC, to build `zenix-slirp` (one time):

  ```
  make -C tools/host/zenix-slirp
  ```

  It ends up as `build/zenix-slirp`.

## On the PC

1. Plug in the cable and find the port: `dmesg | tail` shows it, usually `/dev/ttyUSB0` for an adapter or
   `/dev/ttyS0` for a built-in port.
2. Your user needs the right to open it: on Arch and CachyOS add yourself to the group `uucp`, on Debian and Ubuntu
   to `dialout` (`sudo usermod -aG uucp $USER`, then log out and in once).
3. Start it:

   ```
   build/zenix-slirp /dev/ttyUSB0
   ```

   It says "the Amiga is 10.0.2.15, the gateway 10.0.2.2, DNS 10.0.2.3" and keeps running; stop it with Ctrl-C.
   Zenix's own console text on the cable (before and after the network) is printed too; `--log FILE` puts it in a
   file instead. `-f tcp:2323:23` would let the PC reach port 23 on the Amiga as port 2323 on the PC (for later
   servers such as telnet); it listens on the PC's own address 127.0.0.1 only.

## On the Amiga (Zenix, as root)

1. Describe the interface in `/etc/network/interfaces` (with `edit /etc/network/interfaces`):

   ```
   auto sl0
   iface sl0 inet static
       driver slip /dev/ttyS0 115200
       address 10.0.2.15/24
       gateway 10.0.2.2
   ```

   The file must belong to root and only root may write it (it does when root makes it).
2. For names, `/etc/resolv.conf`:

   ```
   nameserver 10.0.2.3
   ```

3. Bring it up: `ifup sl0`. From now on every boot brings it up by itself (the `auto` line), without making the login
   wait for it.
4. Check:

   ```
   ifconfig            sl0 is UP and RUNNING, address 10.0.2.15
   ping -c 3 10.0.2.2  three answers from the PC
   route               the default route goes through 10.0.2.2
   netstat -a          the connections
   ```

## What changes while it runs

- **The serial port belongs to the network.** `cu` cannot open it, and the kernel's messages stay in `dmesg`
  instead of going out on the cable. `ifdown sl0` gives the port back (and `ifup sl0` takes it again).
- The network server, netd, takes about 100-120 KB of memory while it runs (on 2 MB of chip RAM about 1,160 KB stay
  free with one login; measured in the emulator). A machine that never opens a socket never loads it.

## When it does not work

- `ping` gets no answer: `zenix-slirp` counts the frames it got at its end ("frames in N" when you stop it); none
  means the cable or the port is wrong (swap transmit and receive: the cable must be a null-modem one).
- `dmesg | grep netd` shows what the network server did ("netd: sl0 up: SLIP on /dev/ttyS0 at 115200, 10.0.2.15").
- "serial: N bytes lost (input ring full)" in `dmesg`: the Amiga was too busy to read the line for a while; TCP sends
  those bytes again, so only speed suffers.
- `ifup` says "/etc/network/interfaces must belong to root ...": make it root's with `chown root
  /etc/network/interfaces` and `chmod 644 /etc/network/interfaces`.

## Without zenix-slirp

Any PC that speaks plain SLIP can be the other end: on Linux, as root, `slattach -p slip -s 115200 /dev/ttyUSB0`,
then give `sl0` an address and route or masquerade the Amiga's address yourself. zenix-slirp needs none of that.

Last checked against the code: 2026-10-27 (in FS-UAE through zenix-slirp, `tools/slip-test.sh`; not yet on a real
A1200 on a cable).
