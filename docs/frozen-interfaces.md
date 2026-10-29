# Frozen interfaces

<!-- Moved out of CLAUDE.md on 2026-10-28, word for word (CLAUDE.md points here). -->

Read this before changing any system call, struct, message, file format or `/etc` file a program reads; every review checks changes against it. New frozen items are added here (in the same commit as the code).

Updates install a new kernel before the programs (docs/d15-self-updates.md 2 and 4), so an older program always meets
a newer kernel and newer servers. So these never change meaning; they only grow, at the end:
- **Append-only:** system call numbers and their argument meanings; error numbers; message types and their argument
  meanings (`kernel/fsproto.h`, `kernel/diskproto.h`). Never reused, never changed. A new argument goes where every
  older caller passed 0, or into a new number (claimed in the table above first).
- **Structs the kernel or a server writes into a program's memory** (`ustat`, `uproc`, `ustatfs`, `udirent`,
  `umountinfo`, `diskinfo`, the `SYS_meminfo` words) grow only at the end, and the caller passes its buffer size and
  gets at most that many bytes: `SYS_stat`/`lstat`/`fstat`/`readdir`/`procinfo`/`statfs`/`mountinfo` take it in their
  third argument, `SYS_meminfo` in its second; size 0 (every program built before D15) gets the struct's first size,
  a frozen constant, never `sizeof` (`kernel/kernel.h` `*_FIRST`: `ustat` 28, `udirent` 72, `umountinfo` 88,
  `uupdate` 32, `uproc` 32, `ustatfs` 20, `SYS_meminfo` 16; `tools/usize-test.sh`), a size below that is -EINVAL;
  `DISK_INFOSZ`/`DISK_LISTSZ` take it in `a[2]` (the old `DISK_INFO`/`DISK_LIST` write the first 80 bytes). The structs servers answer in (`ustat`, `udirent`, `ustatfs`)
  are frozen at their size in the message protocol: a bigger one needs a new message type. libz passes `sizeof`.
- **Frozen:** the program start contract (the stack at entry, `a5`); the ZEXE and ZEX2 formats (the loader keeps both
  for ever); the bootinfo layout; the klog area and boot-note layout and the klog flags (zenixboot and bootmenu
  against the kernel); the `/etc` formats base programs read (svctab, fstab, passwd, group); the service manager's
  `SVC_*` numbers and limits (`SVC_TEXT` 80, `SVC_SAVED` 512, `SVC_MIDREQ`, exit status 86 for a damaged table), the
  port names (`disk`, `fs`, `fs.*`) and the built-in `@name` programs (`@zfsd` is an alias to `/sbin/zfsd` since D19 step 1); the tty mode bits (`TTY_RAW`, `TTY_NOSIG`),
  `O_*` flags and mode bits (`O_NONBLOCK` 0x800 and `S_IFSOCK` 0140000 since D19), the spawn limits (argument count and bytes), exit and wait status conventions (139 for a
  fault, `waitcon`'s console status, login's `ST_HANDED` (bit 30) plus the handed shell's pid, D18); the boot note's kinds and `SYS_reboot`'s arguments; D15's own formats (the
  bootinfo bundle fields, the update note, the state file, the bundle header and manifest, zup's journal and undo logs).
- **One accepted change of meaning (audit3 B7, 2026-10-26):** `uproc.mem` counts a process's memory regions
  (`memalloc`) and a private code copy since D13; before, only its load block (image, data, stack). Accepted once,
  not repeated: every program is built and shipped together with the kernel (no released program reads `mem` with
  the old meaning; `ps` and `free` show it), and it still means "the memory this process owns". Any later change is
  a new field.
- **One accepted change of meaning (audit block 2 B2-5, 2026-10-29):** init's policy for `/etc/daemons`: status 75
  ("start me again at once") counts toward the five ends in 60 s only when the daemon ran less than 5 s; before, every
  75 counted, so five RESTARTs of wasabid in a minute lost it until the next boot. Accepted once, not repeated: init and
  every daemon ship together with the kernel, and no released daemon depends on the old count (a restart loop is still
  given up). Any later change is a new status or a new word in the file.
- **D27 steps 0-1 (the C library, block 3, 2026-10-29):** the error numbers `EDOM` 33, `EOVERFLOW` 75, `EILSEQ` 84 (and
  `ENOTSUP` as a second name of `EOPNOTSUPP` 95); the C library bakes the system-call, error and signal numbers and
  `O_*` flags into every program built on it (DESIGN D27 section 6: already frozen, nothing new beyond the three).
- **D16 (events and signals, 2026-10-26):** the signal numbers and their default actions (Linux's); the 128 + n wait
  statuses; the signal frame (`struct sigframe`, first size `SIGFRAME_FIRST` 84: size, d0-d7, a0-a7, PC, condition
  codes, signal, blocked mask) and `sigreturn`'s contract (the frame's address in a0 to the entry, back in d1; only
  the condition codes taken; returns the frame's d0); `struct usigaction` (first size `USIGACT_FIRST` 20: size,
  handler 0/1/other, mask, flags, entry) and `sigaction`'s answer (0 default, 1 ignored, 2 caught); `SA_RESTART`
  (Linux's value); `struct upollfd` (Linux's `pollfd`), the `POLL*` bits (Linux's values) and `POLLPORT` 0x4000;
  `SIG_BLOCK`/`SIG_UNBLOCK`/`SIG_SETMASK` 0/1/2; `uproc.pgid` at the end of `uproc`; a process in the kernel's
  event wait reported by `procinfo` as state 3 (waiting for a child) or 4 (I/O), never a new number; `ST_STOPPED`
  0x114 for every stop; `PRIO_PGRP` stays -EINVAL. The kernel's event bits (`EV_*`) are internal.
- **D19 (the network, 2026-10-27):** `kernel/netproto.h`: the `SOCK_*` op numbers and which request fields each
  uses, `struct usockreq` (first size `USOCKREQ_FIRST` 32; the request size in `SYS_sock`'s op word, 0 = 32), the
  `NET_*` messages (a[0] the socket's name, a[1] a `struct netreq`; the reply's a[2] bits, a[3] epoch; `NET_CLOSE`
  a no-start call), `struct netreq`, `SYS_netsrv`'s ops and `struct unetready`, the Linux constants there (`AF_INET`,
  `SOCK_*`, `SOL_SOCKET`, `SO_*`, `MSG_*`, `FIONREAD`, `FIONBIO`); the random seed file's format (`user/randseed.h`:
  `ZRS1`, flags, 32 bytes) and root's write to `/dev/random` crediting it.
  Since steps 4-5: `struct unetstat` (first size 40), `unetif` (64), `unetifreq` (32), `uroute` (32); `FIONBIO` with a
  null value pointer answers the O_NONBLOCK flag; the `/etc/network/interfaces` subset (D19) and `/etc/hosts`,
  `/etc/resolv.conf`; `/dev/ttyS0`'s truthful POLLOUT (1 KB of the send ring free) and O_NONBLOCK reads/writes (the
  planned exception to D16 rule 11 for this one device); boot's `net` -> `/sbin/netd` at nice 0.
  Since steps 6-8: `SYS_drv`'s `DRV_*` ops 1-7, `DRVC_CLEAN` 1 / `DRVC_INVAL` 2; `struct udrvattach` (first size 180)
  and `udrvxfer` (872), each stating its own size; `udrvstep`/`udrvquiesce` (8 steps, `DRVS_WRITE` 1, `DRVS_RMW` 2);
  `DRV_MAXBUF` 32, `DRV_BATCH` 16, `DRVA_DMA` 1, `DRVA_LINK` 2; the NETDRV message layout (a[0] the driver's pid, a[1]
  the block, a[2] its size); `HWINFO_FIRST` 408 (the hardware table grows only at its end); `unetif`'s growth (`hwaddr`
  where `reserved` was; past the first size `lease`, `dhcp_server`, `rx_drop`; `NETIF_ETHER` 16, `NETIF_DHCP` 32);
  `SYS_drv`'s -EPERM (anyone but netd's children) and -ECONNRESET (no netd); the driver contract `/sbin/net/NAME IFACE
  ARGS` with a pipe as standard input for the doorbell.
  Since step 9: `/sbin/net/genet` in `NETDRVS` and its `driver` line words `cache=kernel`, `cache=user`, `noselftest`;
  (the warm start, branch t12) `hwinfo.genet_stop` (the next reserved word) and its `GSTOP_*` bits 1-32, and
  `HWINFO_DRYRUN` ("ZHWD") in the hwinfo magic from a caller that may return to AmigaOS (zenixboot CHECK): stage0
  writes no hardware then.
- **D20 (telnet's console, step 1, 2026-10-27):** `SYS_console`'s op word `op | (fd + 1) << 8` (0 above the op: no
  descriptor, only `CON_COLOURS`; ops are 1-255, 4 reserved for D28's `CON_UFONT`; D24's ops 5-7 take a descriptor
  too); `CON_FONT` 2 (a2 PSF1 data, a3 its size; a2 0 drops it; PSF1 mode 0, height 8,
  2,052 bytes; meaning "8-bit mode: each byte one glyph of this font, no UTF-8 while loaded"), `CON_RULES` 3 (a2 bits,
  a3 mask, 0 asks, answers the bits before); the `CONR_*` bits 1-64 and their meanings; the target rules (a console,
  the caller's own in its foreground, or root's) and the errors -EBADF, -ENOTTY, -EPERM, -ENODEV, -EFAULT, -EINVAL,
  -ENOMEM; font and rules dropped when their last setter ends, kept by `ESC c`; the PC font's file format (PSF1) and
  path `/usr/share/consolefonts/cp437-8x8.psf` (full images and the base package).
- **`SYS_time` (audit 2026-10-26 F8, 2026-10-27):** its 32 bits are **unsigned** seconds since 1970 (UTC), good until
  2106 (the bits never changed; every caller reads them as `u32`), and they never jump back when the tick count wraps
  (`kernel/clock.h`); `SYS_settime` takes the same unsigned number. `SYS_ticks`'s argument (audit F9, 2026-10-27): 0
  the 100 Hz tick count (wraps after 497 days), 1 the whole seconds since boot (unsigned, the clock's own count),
  2 who set the time this boot (D21 8.3, branch clock, 2026-10-28): the bits `CLOCK_RTC` 1 (the battery clock at
  boot) and `CLOCK_NET` 2 (`SYS_settime` with its second argument 1); others -EINVAL. `SYS_settime`'s second argument
  (0 for every older caller): 0 clears both bits (a hand-set time), 1 "from a checked source" adds `CLOCK_NET`,
  others -EINVAL. `/var/lib/clock` (`zenix-clock 1 SECONDS`) and `/etc/sntp` (D21 8.3); the anchors file
  `/etc/ssl/anchors` (`ZTA1`, D21 8.1) and the folder `/etc/ssl/local/` for PEM roots.
- **D20 steps 4-5 (Zmodem and the phone book, branch m4, 2026-10-27):** `SYS_pipe`'s second argument (0 for every
  older caller): 0, or `O_NONBLOCK` for both ends (Linux's `pipe2`; an empty read -EAGAIN, a write what fits or
  -EAGAIN), any other bit -EINVAL; `rz`/`sz`'s standard input and output contract (lrzsz's; `sz` a link to `rz`); the
  phone book `~/.phonebook` (`NAME HOST [PORT] [amiga|pc|unix] [ice]`, `#` comments, unknown words ignored with a
  word). Not frozen: `rz -T`'s standard-error form (telnet and rz ship together), the options.
- **D21 steps 2-3 (cryptotest, ssh, branch m4, 2026-10-27):** ssh's files are OpenSSH's (`~/.ssh/id_ed25519` and
  `.pub`, `~/.ssh/known_hosts`, root's `/etc/ssh/ssh_known_hosts`), followed, not ours to freeze; ssh's exit status
  (the remote command's, 255 its own) as OpenSSH's.
- **`SYS_klog`'s `KLOG_OLDEST` (audit F10, branch m4, 2026-10-27):** bit 0x40000000 in the offset answers the offset
  of the oldest character the ring holds (0 before a wrap; with `KLOG_PREV` the previous boot's); offsets below it read
  from it, as always.
- **D26 (symbolic links across mounts, 2026-10-28):** `UMSG_LINKS` 8 and its meaning; the `FS_LINKED` answer
  -(1024 + bits), `FSL_A1` 1, `FSL_LAST` 2, the reserved -1024 .. -1279; TEXT's form (absolute T + rest, or the link's
  directory without its leading `/` + T + rest; NUL-terminated, at most `FS_PATHMAX`); the table of which argument
  each request may rewrite (`fsl_ok`, `kernel/fsproto.h`). A server without the bit follows links as before.
- **D22 (zpkg, 2026-10-28):** `UPD_F_TRIAL` 2 in the bootinfo and `uupdate` flags; the manifest line `takes OTHER`;
  zup's journal line `live 1 NAME` and the live-only operations `S`, `M`, `X`; `/var/cache/zpkg/live/` and
  `/var/lib/zpkg/lock` (a pid and a newline, linked whole from `lock.PID`); zpkg's `/etc/zpkg.conf` (the words
  `channel`, `server`, `hold`, `cache`; others ignored), `/var/lib/zpkg/deps` (a name a line) and `sync/CHANNEL.index`
  with its `.sig`; zpkg's built-in keys by build kind (D21 6.3); `zup -f`/`-r`'s command line is between programs of one base version
  and not frozen (docs/d22-zpkg.md section 10).
- **D21 step 5 (signatures, branch sig5, 2026-10-28):** the detached `.sig` (80 bytes: `ZSG1`, format 1, algorithm
  1 Ed25519, two zero bytes, the key id, the signature) and its labels with their digests (`Zenix bundle signature v1`
  over SHA-256 of a bundle's header bytes 0-63 and its table; `Zenix package index v1` over the whole list); its life
  beside the bundle on BOOT:Zenix/ (`zenix-update.zup.sig`, copied as `.sig.part` and renamed before the bundle;
  renamed with it to `zenix-update.bad.sig`, `.done.sig`, `.rejected.sig`); zenixboot's state-file key
  `sigdigest HEX` (the 64-hex digest that verified; older zenixboots ignore it); which key a build trusts (`make
  RELEASE=1`: only `tools/keys/zenix-release-*.pub`, the channels `stable` and `testing`; a test build: only the TEST
  key, the channel `test`); zenixboot's argument `UNSIGNED_OK` (test builds only; a release build ignores it).
- **D24 steps 1-2 (the remote control, branch wasabi1, 2026-10-28):** `/dev/kmsg` (root by the effective id, opened
  write only; `O_RDONLY`/`O_RDWR` -EINVAL, anyone else -EACCES; its writes go into the kernel log as a service's output does);
  `/etc/daemons` (`NAME NICE PROGRAM [ARGUMENTS]`, svctab's layout and limits, at most 4 lines, root's and writable by
  nobody else) and init's policy for its lines (D11's numbers, status 0 a stop, 75 "start me again at once",
  counted only after a run under 5 s since B2-5, output and errors to `/dev/kmsg`); `/etc/wasabid.conf`'s words (`key`, `port`, `allow`, `name`) and its
  refusals (root's regular file, mode 600, at most 2 KB, decision 4 of d24-remote.md); wasabid's TCP port 1236, its
  discovery answer, its exit statuses (0 a stop, 1 could not start, 75 start again) and arguments (`--init`,
  `--trial`, `--port`). The Wasabi protocol itself is the Wasabi project's (its PROTOCOL.md), followed, not ours.
- **D24 step 3 (the console ops, branch wasabi3, 2026-10-29):** `SYS_console` ops 5 `CON_READ`, 6 `CON_LINE`, 7
  `CON_INPUT` in D20's op word, root only by the effective id (-EPERM before the descriptor's checks), on any console's
  descriptor (-EBADF, -ENOTTY); for `CON_READ`, `CON_LINE` and the text kind a console never used is -ENXIO with
  nothing allocated (raw keys act as the real keyboard). The structs' `size` is written back as the bytes the kernel
  filled. `CON_READ`: a2 a `struct uconread`
  (first size `UCONREAD_FIRST` 28: the caller's size, cells pointer and size; rows, cols, cursor row and column, the
  change counter, `CONRD_*` flags 1 shown, 2 an 8-bit font, 4 scrolled back, 8 the cursor shown, the cell size 3), a3 0
  (any other value -EINVAL: room for later forms); the cells row by row, 3 bytes (the stored byte, `CONA_*` 1 inverse,
  2 underline, 4 bold, the colour byte with the rules' bright colours in), whole cells only, answering the bytes
  written. `CON_LINE`: a2 the display line (below the height, else -EINVAL), a3 a `struct uconline` (first size
  `UCONLINE_FIRST` 56: size, pens pointer and size; width, height, depth, the generation, 16 palette words 0x0RGB as
  the Copper shows them, unused entries 0); a pen a pixel in whole groups of 8, the answer the pens written; -ENODEV
  without a display, -ENXIO for a console not shown. Only "a line of pens" is frozen, never the planes' layout.
  `CON_INPUT`: a2 the bytes, a3 the count (at most 256) | kind << 16; kind 0 raw key codes into the keyboard path
  (the shown console, whatever console the descriptor names; 0x78 and 0xF8-0xFF refuse the whole call with -EINVAL;
  at its end the modifiers it pressed and did not release are released and a key it left down does not repeat), kind
  1 text into that console's tty as the serial line's bytes (no 0x1d hook), stopping before a byte the queue could not
  keep (^C, ^Z, ^U and backspace always taken); a raw call may also stop early on a busy machine; the answer the bytes
  taken. The structs grow only at their end, each stating its own size.
- **D23 step 1 (the cancel mark, branch nfs12, 2026-10-28):** `UMSG_CANCEL` 4 (set by the kernel only, for a client
  killed while it waits and on every call a killed process makes after; never for a caught signal, never on
  `proc_exit`'s calls or `FS_CLOSE`; never cleared; a server may answer -EINTR, the call is never restarted; `FS_QUIT`
  is carried out marked or not, a bounded `FS_SYNC` ignores it, diskd ignores it); `POLLPRI` 2 on a port
  ("a message sent to it was cancelled since you last saw POLLPRI"; reported only when asked for, and the report takes
  it as seen); `FS_SYNC`'s a0 (1 "bounded": answer within 2 s; any other value as 0) and `vfs_sync`'s order (network mounts,
  by `fsinfo.type` `nfs`, last); `ESTALE` 116.
- **D23 step 2 (nfscd, branch nfs12, 2026-10-28):** `/etc/fstab`'s type `nfs` with `HOST:/EXPORT` and mount's NFS
  options (`hard`, `soft`, `timeo=`, `retrans=`, `ro`, `sync`, `actimeo=`); the service names `fs.nfs0` .. `fs.nfs7`
  and the program text `/sbin/nfscd nfsN HOST:/EXPORT [CODES]` with its codes (`h s tN rN o y aN l`, append-only);
  nfscd's saved record (`NFS1`, the root's handle and fileid, at most 77 bytes).
- **D23 step 3 (writes, branch nfs34, 2026-10-28):** `SO_LINGER` 13 at `SOL_SOCKET` with `struct linger` (Linux's
  `{l_onoff, l_linger}`, 8 bytes; shorter -EINVAL): `{1, 0}` closes by an abort (a reset, the port free at once); a
  longer linger is answered by `getsockopt` but never waited for. Not frozen: the `.nfs` stand-in names.
- Not on the list: the kernel's internal file kinds. libz is linked into each program, so it needs no rule of its
  own.

