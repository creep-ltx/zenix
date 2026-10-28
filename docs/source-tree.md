# The source tree

What each folder holds and where to start reading. How the parts work together is in [architecture.md](architecture.md),
why they work that way in [why.md](why.md). Last checked against the code: 2026-10-24.

```
zenix/
├── boot/        the AmigaOS side: what runs before Zenix owns the machine
├── kernel/      the kernel: C and 68k assembler
├── user/        every program and server that runs on Zenix
├── zenixfs/     the ZenixFS library, its host tools and tests
├── net/         the network (DESIGN D19): lwIP, its Zenix port, netd's core, telnet's and ssh's cores
├── crypto/      the crypto library (DESIGN D21): BearSSL, Monocypher, OpenSSH's bcrypt, Zenix's zc/
├── rootfs/      files copied onto the Zenix root disk (/etc, test scripts)
├── link/        linker scripts (kernel, programs, stage0)
├── tools/       image builders, the emulator rig and every test
├── fsuae/       FS-UAE configurations (the A1200 setups and the test matrix)
├── docs/        all documentation, and the manual pages in docs/man/
├── archive/     old code kept for reference, not built (archive/README)
└── Makefile     builds everything; `make check` runs every test
```

## boot/: getting from AmigaOS to Zenix

| File | What it is |
|---|---|
| `bootblock.S` | The floppy's 1 KB bootblock: loads the kernel with Kickstart's help, collects the machine's facts, jumps in |
| `zenixboot.c`, `zenixboot-start.S` | The same from a hard disk, CF or SD card: an AmigaOS program the boot partition's Startup-Sequence runs |
| `handoff.S` | The hand-over both paths share: switch AmigaOS off, copy the kernel to its home, jump |
| `stage0.c` | Under Emu68: reads the Pi's device tree so the kernel can find the SD card and the Pi's hardware |
| `genet-stop.h` | stage0's stop of the CM4's Ethernet DMA that AmigaOS may leave running (shared with `tools/genet-test.c`) |
| `bootmenu.c`, `bootmenu-start.S` | The boot menu (AmigaOS or Zenix), and `bootzenix`, which reboots straight into Zenix |

## kernel/: the kernel

| Area | Files |
|---|---|
| Start and boot order | `start.S` (first instructions, relocation, vectors), `main.c` (`kmain`: the boot order, the 100 Hz tick) |
| Processes and scheduling | `proc.c` (spawn, exit, wait, the scheduler, priorities) |
| Memory | `mem.c` (chip and fast RAM, an owner for every block) |
| Messages | `ipc.c` (named ports, synchronous zero-copy calls, priority inheritance) |
| The network's kernel part (DESIGN D19) | `sock.c` (sockets: the calls to netd and the waiting), `drv.c` (the network's drivers: claims, DMA memory, quiesce, their blocks to netd), `netproto.h` (the protocol), `random.c` (the entropy pool) |
| System calls | `syscall.c` (the TRAP #0 table, every pointer checked), `errno.h` |
| Files | `vfs.c` (open files, pipes, the mount table, devices), `ext2.c` (the ext2 code the `fsd` server runs) |
| Terminal and screen | `tty.c` (line discipline, consoles), `term.c` (the ANSI/VT100 parser), `gfx.c` (Copper and blitter drawing) |
| Input | `kbd.c` (keyboard, keymaps, console keys), `input.c` (mouse and joystick) |
| Disks | `blk.c` (picks the disk), `ide.c` (Gayle and A4000 IDE), `emmc.c` (the Pi's SD card), `a3000.c` (A3000 SCSI), `floppy.c` |
| Other hardware | `serial.c` (serial port, kernel log, `/dev/ttyS0`), `rtc.c` (battery clock), `audio.c` (the bell) |
| Shared definitions | `kernel.h` (system call numbers, limits), `clock.h` (the wall clock behind `SYS_time`), `fsproto.h` and `diskproto.h` (the server protocols), `hw.h`, `hwinfo.h`, `diag.h` (boot colours) |
| Built-in servers | `embed.S` puts `diskd`, `fsd` and `boot` inside the kernel image (and `zfsd` in the test-only `kernel-zfs.bin`; the shipped system has it as `/sbin/zfsd`, DESIGN D19) |

## user/: programs and servers

- **Servers:** `diskd.c` (disks and partitions), `fsd.c` (ext2), `zenixfsd.c` (ZenixFS), `boot.c` (starts init),
  `init.c` (logins on the consoles), `msgpingd.c` (the message benchmark's partner).
- **The C library:** `libz.c`, `libz.h`, `usys.h` (system call wrappers), `socket.h` (the BSD socket calls, DESIGN D19), `crt0.S` (program start-up). Zenix has its
  own small library, not newlib.
- **The shell:** `sh.c`.
- **Commands:** one file per program, `NAME.c`, each with a manual page in `docs/man/NAME.txt`. The list:
  [commands.md](commands.md).
- **Taken from other projects:** `miniz/` (for `gzip`) and `regex.c` (from CUtils), both MIT; `xfer/` (DESIGN D20 5:
  the Zmodem engine from CTerm, MIT, notice `xfer/LICENSE`, with Zenix's download rules in `rzrules.c`; for `rz`/`sz`,
  `rz.c`, which telnet starts by itself).

## net/: the network (DESIGN D19, docs/d19-network.md)

| Folder | What it is |
|---|---|
| `lwip/` | lwIP 2.2.1 (BSD-3, `COPYING`): only the files netd builds or tests, unchanged but for the local patches in `lwip/patches/`; `lwip/README` names the version, the files and the patches |
| `port/` | Zenix's port of lwIP: `lwipopts.h` (the configuration), `arch/cc.h`, the counted heap and the limits set by the machine (`port.c`), ChaCha20 and the SipHash ISNs (`rand.c`), the checksum and the copy in 68020 assembler (`m68k.S`), stand-in C headers for the Zenix build (`shim/`) |
| `drv/` | the network's drivers, user processes netd starts as `/sbin/net/NAME` (`drv.h`: what they share): `a2065.c` (the Commodore A2065, polled), `genet.c` (the CM4's Ethernet under Emu68, polled; the chip and its PHY in `genet-core.c` and `genet.h`, shared with the PC test against a model of the chip, `tools/genet-model.h`), `fake.c` (the driver protocol's test driver, test images only) |
| `telnet/` | telnet's core (DESIGN D20): the protocol with RFC 1143's options (`proto.c`), the BBS and Unix stream filter (`filter.c`), the key translation (`keys.c`), the CP437 tables (`cp437.c`); `telnet.h` is its interface; plain C with no system call, shared by `user/telnet.c` and the PC tests (`tools/host/telnet-test.sh`) |
| `ssh/` | ssh's core (DESIGN D21 4): the transport with strict key exchange, the packets, the logins, the session channel, OpenSSH's key files, known_hosts, the config subset and the text cleaner; `ssh.h` is its interface; no system calls, shared by `user/ssh.c`, `user/ssh-keygen.c` and the PC tests |
| `netd/` | netd's core: sockets, SLIP and loopback, names, the configuration files, the limits (`netd.h` is its interface); plain C with no system call, so netd's main program and the PC tests use the same files. `main.c` is netd's main program on Zenix (`/sbin/netd`): the operating system layer and the message loop |

## crypto/: the crypto library (DESIGN D21, docs/d21-crypto-ssh-https.md)

| Folder | What it is |
|---|---|
| `bearssl/` | BearSSL at 7bea48e (MIT): only the files used, unchanged (`README`): TLS 1.2, X.509, RSA, ECDSA, AES, SHA-2, ChaCha20, Poly1305, X25519 |
| `monocypher/` | Monocypher at f5f0a19 (CC0 chosen): Ed25519, ChaCha20, Poly1305, X25519 |
| `openssh/` | OpenSSH's `blowfish.c` and `bcrypt_pbkdf.c` (key passphrases), unchanged, with two shim headers |
| `zc/` | Zenix's own: wiping, the random generator, X25519 with the all-zero check, ssh's packet framing and key derivation, the signature file, the X.509 wrapper and trust anchors |

The Makefile builds them into `build/libcrypto.a` (and a pure copy for `ssh`), linked with `--gc-sections` into
`cryptotest`, `ssh` and `ssh-keygen`.

## tools/: building and testing

| What | Files |
|---|---|
| The gate | `check.sh` (`make check`: every test, a table at the end) |
| Images | `mkzexe.py` (ELF to ZEXE), `mkadf.py`, `mkroot.sh`, `mkdisk.py`, `mkrdb.py`, `mkmbr.py`, `mkcard.py`, `mkdualcard.sh` (the dual-boot SD card), `write-card.sh` (writes a card safely), `mkcardpkg.sh` (the owner's card package), `mkdom.sh` (the DoM image), `write-root.sh` and `cardroot.py` (a new root onto the dual card's partition 3, the owner's settings carried) |
| The emulator rig | `fsuae_conf.py` (writes a configuration), `run-test.sh`, `serial-tap.py` (types a script into the serial port and waits for answers), `shot.py` (screenshots) |
| Emulator tests | `*-test.sh` and the scripts they type, in `tools/tests/` |
| Host tests | `host/` (the disk and ext2 servers built for the PC; the network's tests, `host/net-test.sh`: lwIP's unit tests, netd on the PC against `zenix-slirp`, the fuzzer), `*-test.c` (drivers against software models of the hardware) |
| The PC's end of the SLIP line | `host/zenix-slirp/` (`zenix-slirp`: SLIP to libslirp, no root; needs libslirp and GLib); `slip-test.sh` and `slip-host.py` (Zenix in FS-UAE on the line, the PC's test servers; `eth-test.sh` uses the same servers behind FS-UAE's A2065) |
| Rules | `docs-check.sh` (the documentation stays in step with the code), `check-68020-60.sh` (no instruction a 68060 lacks) |
