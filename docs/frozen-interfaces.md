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
  fault, `waitcon`'s console status, login's `ST_HANDED` (bit 30) plus the handed shell's pid, D18); the boot note's kinds and `SYS_reboot`'s arguments (REBOOT_HALT 0x100 since 2026-11-04); D15's own formats (the
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
- **One accepted change of meaning (DESIGN D28, branch utf8, 2026-10-29):** the bytes a console shows and the keys it
  sends are UTF-8 instead of Latin-1, also for every program built before D28. Accepted once, not repeated: the
  programs that depend on it (`telnet`, `sh`'s line editor, `edit`, `less`, `ls`) change in the same release (D28
  steps 2-5 ship together, and the `telnet` package requires that base), the 8-bit mode (a `CON_FONT`, or the new
  rule `CONR_8BIT`) keeps the old meaning for whoever asks for it, and the Latin-1 fallback keeps old Latin-1 text
  readable. Any later change is a new rule bit.
- **D28 (the console's UTF-8 and fonts, branch utf8, 2026-10-29):** `SYS_console` op 4 `CON_UFONT`: exactly 4 in the
  op word (a descriptor there: -EINVAL), root only (-EPERM), a2 the ZCF1 data and a3 its size (a2 0 drops the font:
  every character above U+00FF the box, the widths unchanged), -ENODEV without a display, -EINVAL for a3 above
  98,848 (refused before anything is allocated) or any check, -EFAULT, -ENOMEM (nothing changes); the checks (magic
  `ZCF1`, header size 32, flags 0, width and height 8, the reserved fields 0, 3-4,096 glyphs, 0-255 half-pages, the
  total size equal to a3 and to 32 + 512 + 256 P + 8 G, index bytes 0 and 1 zero, none above P and no two naming
  one half-page, every entry 0, 32-126, 160-255 or below 256 + G); kept when its setter ends (the system's, not a session's). The ZCF1 format
  (`tools/mkconfont.py`, all big-endian): the 32-byte header (offsets 0 magic, 4 header size, 6 flags, 8 width, 9
  height, 10 glyph count, 12 half-page count, 14 reserved, 16 total size, 20-31 reserved), then the 512-byte index (a
  byte per 128-code-point half-page of the BMP: 0 none, else 1 + its number), the half-pages (128 16-bit entries: 0
  no glyph, 32-126 or 160-255 Topaz's glyph of that Latin-1 byte, 256 + n glyph n), the glyphs (8 bytes, a byte a row, bit 7 the
  left pixel); glyphs 0, 1, 2 the box and the wide box's halves; entries for U+FFFE/U+FFFF ignored; any later meaning
  needs a flags bit. `CONR_8BIT` 128 (`CONR_ALL` 255): 8-bit mode with the console's Topaz, owned and dropped as the
  other rules, kept by `ESC c`; a `CON_FONT` implies the same mode; in it nothing is decoded, the keyboard sends the
  keymap's Latin-1 bytes, and a cell above 0xFF draws as '?' (unchanged). `CON_READ`'s a3 1 (`CONRF_CELL4`): cells
  of 4 bytes (the 16-bit value big-endian: up to 0xFF the stored byte, else the code point, 0xFFFE a character beyond
  U+FFFF, 0xFFFF a wide character's right half; then `CONA_*`, then the colour), `csize` 4; a3 0 as before (the low
  byte; `A_HI` never shown in the attribute byte), other values -EINVAL. The path
  `/usr/share/consolefonts/zenix-8x8.zcf` (full images and the base package) and `/etc/console`'s
  `font auto|none|NAME` (NAME a plain name: `/usr/share/consolefonts/NAME.zcf`; no line or no file: auto). The
  console's UTF-8 rules as documented behaviour (D28 section 4: nothing decoded inside an escape sequence, a byte that
  is not part of a character one cell of its Latin-1 glyph or the box, C1 controls written as UTF-8 ignored, U+FFFE
  and U+FFFF stored as U+FFFD) and its widths (picolibc's `wcwidth` in a non-CJK locale, section 9).
- **D20 7.2 (pseudo-terminals, D38 step 1, branch desk, 2026-11-04):** `SYS_ttyctl` 95 (fd, op, arg; -ENOTTY for a
  descriptor that is no terminal, -EBADF, -EINVAL for another op) and its ops: `TTYC_GWINSZ` 1 (arg `struct
  uwinsize *`, written: rows, columns, 0, 0; a console's is the screen's), `TTYC_SWINSZ` 2 (a pty's, on either end,
  rows and columns 1..32767 else -EINVAL, a console -EINVAL; a change sends SIGWINCH to the slave's foreground),
  `TTYC_OWNER` 3 (root, else -EPERM; a pty, a console -ENOTTY; arg a uid up to 65535), `TTYC_SCTTY` 4 (arg 0, a
  console (root only, else -EPERM) or a pty's slave; -EPERM when another living process leads it, -EIO when hung up;
  the old pty is freed if nobody names it any more); `struct uwinsize`
  (Linux's `struct winsize`, 8 bytes); `SYS_ttyinfo`'s meaning, "this process's tty's size", now a pty's for a process
  on one; the names `/dev/ptmx` and `/dev/pts/N`; a slave's `st_ino` its tty number `NCONS + 1 + N` (11 and up,
  never 1..10) and a master's `0x100 + N`; `isatty` true on both ends; a master's read -EIO when no slave is open and
  nothing is left, its `poll` POLLHUP then (also before the first slave open: unlike Linux, decided 2026-11-04 after
  the reviews, `openpty` opens both first), its write -EIO when no slave is open and no living process names the pty
  (no wait for a reader that will not come), its `poll` POLLOUT only when a printable byte fits; the hang-up's SIGHUP then SIGCONT, a hung-up slave's read 0 and write -EIO;
  LF written to a slave reaching the master as CR LF; libz's and the C library's `ioctl` numbers (Linux's).
- **D38 step 2 (a console in graphics mode, branch desk, 2026-11-04):** `SYS_console` ops 8 `CON_GRAB` (root, else
  -EPERM; the op word's descriptor a console; a2 `struct ucongrab`, first size `UCONGRAB_FIRST` 24: size, the Copper
  list and sprite 0 (the caller's chip RAM, even; the list ends 0xFFFF 0xFFFE within 4,096 moves, else -EINVAL;
  outside: -EFAULT), the sprite's height 1..255 and hot spot, back the screen's width and height, the text console's
  depth and `GCHIP_*`; -EBUSY when a console is grabbed, -ENODEV without a display; answers a new descriptor) and 9
  `CON_GACK` (the grab's descriptor; a2 the release's number; -EINVAL for none or an older one, -EBADF); the
  descriptor's reads (whole 16-byte records, -EINVAL for less room; waits unless `O_NONBLOCK`), its `poll` (POLLIN),
  its last close giving the console back; `struct ugevent` (type, code, qual, x, y, ch[4], time) and its record types
  1-5 with their fields' meanings; the `GQ_*` values (AmigaOS's `IEQUALIFIER`); the switch's contract (the release
  asked on Amiga+F*n*, the 1 s deadline, after which the kernel draws with the CPU until the release is answered or
  the server ends, a late `CON_GACK` of that number answering 0; `GEV_ACQUIRE` on return, every key then counting as
  up; the kernel writing sprite 0's two control words in the vertical blank with HSTART 0x80 at the first pixel, so
  the server's list keeps FMODE's sprite bits 0); the grab ending with the descriptor's last close or the server's
  end; `SYS_memfree` -EBUSY for a region holding the grab's list or sprite; `SYS_memalloc`'s second argument
  `MEMA_CHIP` 1 (its other bits ignored, as all of it was before); `MEMA_LOW` 2 (d29 16.19, branch zine9,
  2026-11-12: the region ends below 16 MB; with `MEMA_CHIP` too it is chip RAM, which always is; 0 when no such piece
  is free).
  `CON_MOUSE` 10 on the grab's descriptor (a2 THRESHOLD 0..127 | MULTIPLIER 1..8 << 8: in a frame the counts beyond
  THRESHOLD count MULTIPLIER times; 0x100 none, the grab's start; -EINVAL out of range, -EBADF not the grab's).
- **D38 step 3 (the display server, branch desk, 2026-11-04):** `SYS_peer` 96 (msg a request the caller serves, else
  -EINVAL; `PEER_CTTY` 1 the client's controlling tty; `PEER_RD` 2 and `PEER_WR` 3 with `struct upeermem` {addr,
  len}: 0 or -EFAULT; `PEER_PORT` 4, arg a port: 0 if the client owns it, else -EPERM); `SYS_post` 97 (`POST_BUF` 1: the port's owner, else -EPERM; `struct upostbuf` {addr, size,
  from}, size 0 none; `POST_WRITE` 2: only the owner of the window's `from` port, else -EPERM; `struct upost` {off,
  src, len}, -EINVAL past the window, -EFAULT when the window is no longer its owner's memory, len 0 only wakes;
  -ECONNRESET for a port that is gone); `POLLRDBAND` 0x80 on a port (reported once a post); `struct ucongrab`'s
  `topaz` at offset 24 (`UCONGRAB_TOPAZ` 28: 1,792 bytes, Latin-1 32-255); the `/etc/svctab` line of `display`; the
  display protocol `display/proto.h` version 1 whole: the port name `display`, the request and event numbers and
  their arguments, `struct dpyhello` (first size 44), `dpyring`, `dpywin` (68), `dpyinfo` (64), `dpyimage` (16),
  `dpykey` (8), `dpystyle` (64), `dpyev` (16 bytes), the drawing orders' words (op << 8 | length, unknown ones
  skipped by length), `DPYR_LOST`, `WK_*`, `WF_*`, `WST_*`, `WS_*`, `HIDE_*`, `DRAG_*`, `FP_*`, `TA_*`, `IMG_MASK`.
- **D38 steps 5-6 (zdesk and term, branch desk, 2026-11-04):** the protocol's `DPY_MENU` 12 (a window's menus,
  "Title\tItem...\nTitle...", at most `DPY_MENUMAX` 512 bytes), `DPY_WM_MENUGET` 43, `DPY_WM_MENUPICK` 44,
  `DPY_WM_PGRAB` 45, `EV_MENU` 9, `EV_WM_MENU` 40, and O_XLINE's fifth word, the pen; `CON_INPUT`'s kind `CONI_MOUSE`
  2 (records of 3 bytes: dx, dy as signed counts, the buttons held until the next record; a part record -EINVAL);
  the `/etc/zdesk/` formats (`theme`, `keys`, `bar`, `menu`, `icons`, `themes/NAME.theme`, docs/man/zdesk.txt) and
  their key and action names.
- **D38, the A1200's first findings (branch desk, 2026-11-04):** `DPY_WM_MOUSE` 47 (a[0] the acceleration, as
  `CON_MOUSE`), `DPY_STAT` 14 (a[0] six counters: blits, pixels, glyphs, batches, damage painted, copies onto the
  screen; a[1] bit 0 resets; bit 1 (2026-11-05): twelve, six more after them: a whole drag's moves and their E-clock
  ticks, shows of the drawn screen and their ticks, ticks waited for the beam, shows late for it), a `WK_BAR` window of a width asked (0: the screen's, as before); `/etc/zdesk/mouse`
  (`acceleration none|low|high`); `struct uzine_info`'s `machine` at offset 28 (`UZINE_INFO_MACHINE` 32 bytes or
  more; `UZI_EMU68` 1: the 68k is Emu68's) and its attn's 68060 bit (0x80) set by the kernel on a CPU with a PCR.
- **D38 revision 3 (dlogin, branch desk, 2026-11-04):** `DPY_SESSION` 13 (root's client and root's caller only, on the
  grabbed console, else -EBUSY; a[0] the uid, at most 65535: clients of other non-root uids are dropped), `DPY_WM_BYE` 46 (the manager leaves the role);
  `/etc/console`'s `login text|graphical`; `/etc/dlogin` (`session LABEL = PROGRAM`, PROGRAM an absolute path or
  `text`, at most 4; `theme NAME`); `login -s PROGRAM` (root only, after `-f`).
- **D38 "Screen modes" (branch modes, 2026-11-06):** `DPY_MODE` 15 (a[0] `DPYM_HIRES` 0, `DPYM_LACE` 1 or `DPYM_ASK`
  0xFF; the answer the mode now, -EBUSY, -ENOMEM, -EINVAL), `EV_SCREEN` 10 (w, h the screen's size, arg the mode),
  libdisplay's `dpy_mode`; `/etc/zdesk/screen` (`mode hires|hires-lace|auto`, `#` comments; read by displayd at its
  start).
- **D38 "Fonts" (branch fonts, 2026-11-07):** `DPY_FONT_INFO` 16 (a[0] the font: 0 the 8 x 8, `DPYF_UI` 1 the
  desktop's; a[1] `struct dpyfont`, first size 304: id, height, baseline, xsize, flags `DPYFF_PROP` 1 and
  `DPYFF_BUILTIN` 2, other (the width past 255), name[32], adv[256]; -EINVAL another font), O_TEXT's font word (0
  the 8 x 8, 1 the desktop's font, another number the 8 x 8; the text's box x .. x + its width, y .. y + the font's
  height), `DPY_FONT` with a[0] 0 (the desktop's font); libdisplay's `dpy_font`, `dpy_textw`, `dpy_textf`;
  `/etc/zdesk/screen`'s `font NAME SIZE` and `font builtin`; `/usr/share/fonts/amiga/` holding AmigaOS bitmap
  fonts as they are (`NAME.font`, `NAME/SIZE`: the AmigaOS formats, read, not ours).
- **D38 "Preferences" and D28 22 (branch prefs, 2026-11-07):** `DPY_FONT_SET` 17 (a[0] 32 bytes, the name ended
  by a 0, all 32 readable, or 0 for the 8 x 8; a[1] the size 1-255; the answer 0, -ENOENT, -EINVAL, -ENOMEM, -EBUSY), `EV_SCREEN`'s
  code `DPYS_FONT` 1 (the desktop's font changed; 0 a mode, as before), libdisplay's `dpy_fontset`; a user's
  `~/.config/zdesk/screen` (the lines of `/etc/zdesk/screen`, applied by zdesk at a session's start),
  `~/.config/zdesk/theme.use` (the session's theme, one word, read before `theme`, removed at a session's start and
  end), zdesk's `SIGHUP` (its theme read again); `/var/run/console`
  (the console's session settings: `/etc/console`'s `colours` and `font` lines, removed by init at boot);
  `deskprefs [-v]`, `conprefs [-q]` (its line `conprefs: colours N|auto, font NAME`).
- **D28 22, the console's mode (branch pubprep, 2026-11-12):** `SYS_console` op 11 `CON_MODE` (no descriptor; a2
  `CONM_HIRES` 0, `CONM_LACE` 1, `CONM_ASK` 0xFF; answers the mode shown; -EPERM a non-root set, -EBUSY a display
  server has the screen, -ENOMEM, -EINVAL); a console's size follows it (32 or 64 rows PAL, 25 or 50 NTSC, SIGWINCH
  to each console's foreground); `/etc/console`'s `mode hires|hires-lace|auto`, written by conprefs' Save and Use
  (`/var/run/console`); `conprefs -q`'s line grows at its end: `, mode hires|hires-lace` (when the kernel has
  `CON_MODE`). `CON_GRAB`'s width and height stay the frame's (640 x 256 or 200) in either mode.
- **D25 revision 3 (the demo floppy, branch demo, 2026-11-05):** `/etc/dlogin`'s `autologin NAME SESSION` (SESSION a
  label of its `session` lines; never root; once while `/tmp/.dlogin-autologin` does not exist); `login -s PROGRAM`'s
  PROGRAM may carry arguments split at spaces (at most 7 words in 127 bytes). Not frozen: the demo disk's layout and
  table (`boot/demounpack.h`, only its own loader reads it).
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
  Since 2026-11-04 (the card probe, D19 6.3): `NETIFREQ_BOOT` 4 (UP of every auto interface and the probe, netd may
  stop after it; an older netd answers -EINVAL and `ifup` asks UP); the interfaces file's word `noprobe [DRIVER...]`.
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
  Since D36 (branch ne2000, 2026-11-03): `DRV_CLAIM`'s base `DRV_PCMCIA` 0xA00000 (the PC Card slot, its window
  `DRV_PCMCIA_SIZE` 0x50000) and its answers -ENODEV (no A600/A1200 Gayle) and -EADDRINUSE (memory in 0x600000-0xA4FFFF);
  `/sbin/net/ne2000` in `NETDRVS` (its driver line takes no words).
  Since D39 (branch plip, 2026-11-04): `DRV_PARACK` 8 (the parallel port's claim: the ACK line's falling edges since
  boot, bit 31 clear; -EPERM for another claim) and `DRV_CLAIM`'s base `DRV_PARPORT` 0xBFE101 (the parallel port: its
  quiesce steps only bytes at 0xBFE101, 0xBFE301, and `DRVS_RMW` of bits 0-2 at 0xBFD000, 0xBFD200; CIA-A's FLG
  interrupt on while claimed); `/sbin/net/plipbox` in `NETDRVS` and its driver line words `mac=XX:XX:XX:XX:XX:XX` and
  `noburst`.
  Since D40 (branch sound, 2026-11-12): `DRV_CLAIM`'s base `DRV_PAULA` 0xDFF0A0 (Paula's four channels), taken only by
  the process the service manager runs for port `audio` and answered before the network gate (-EPERM for anyone
  else); every other op keeps the gate, so -EPERM and -ECONNRESET above read "anyone but netd's children, except
  `DRV_CLAIM` of `DRV_PAULA`"; the claim's stop (the four volumes 0, the channels' DMA off) at its end and before
  ZINE, the bell silent while it is held; netd's end does not end it.
- **D40 (sound, branch sound, 2026-11-12):** the port name `audio` (reserved: only root creates it) and its
  `/etc/svctab` line (`audio -10 /sbin/sndd`); the sound protocol `user/sndproto.h` whole but `SND_STATE`: the
  request numbers `SND_OPEN` 1 .. `SND_PAUSE` 9 with their arguments and answers, the handle `(start << 8) | slot`
  (a handle of an earlier sndd answered -ECONNRESET), `SNDF_STEREO` 1, `SNDF_FRAMES` 2, `struct sndinfo` (first size
  32, `SNDI_NTSC` 1), `struct sndch` (24 bytes) and `struct sndframe` (first size 100, passed in `SND_FRAMES`' a[3]),
  `SNDC_TRIG` 1, `SNDC_PER` 2, `SNDC_VOL` 4, `SNDC_LOOP` 8, `SNDC_STOP` 16, `SND_SILENT` 0xFFFFFFFF, the limits
  `SND_BANKMAX` 1 MB, the periods (PCM 124 up, frames 113-4,095), lengths 2-131,070; `SND_STATE` 10 and `struct
  sndstate` are the tests' and may change. The commands `play` (`-v -m -q -b -r -u -2 -R -S`, exit 0/1/2) and
  `modplay` (`-v -l -q`, its keys).
  Since the quiet card probe (2026-11-05, D19 6.3): the driver line word `probe`, taken by every shipped driver
  (genet, a2065, ne2000, plipbox; netd's card probe gives it): nothing is logged until the hardware is there.
- **D20 (telnet's console, step 1, 2026-10-27):** `SYS_console`'s op word `op | (fd + 1) << 8` (0 above the op: no
  descriptor, only `CON_COLOURS` and D28's `CON_UFONT`; ops are 1-255; D24's ops 5-7 take a descriptor too); `CON_FONT` 2 (a2 PSF1 data, a3 its size; a2 0 drops it; PSF1 mode 0, height 8,
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
- **D37 (the 040/060 data cache, branch `cache`, 2026-11-04):** bootinfo `+0x84` `kopts`, `KOPT_DCACHE_OFF` 1 and
  `KOPT_DCACHE_WT` 2 (zenixboot `DCACHE=OFF|WT|CB`); 0, what every older loader leaves, means the defaults.
- **D41 (memory protection, branch `mmu`, 2026-11-12):** bootinfo `kopts` `KOPT_MMU_ON` 4 and `KOPT_MMU_OFF` 8 (both:
  off), zenixboot's `MMU=ON|OFF`; the kernel file's default options, a long after the eight bytes `ZKOPTDEF` (its bit
  4: on; `tools/kopts.py`); the boot line's start `mmu: memory protection on:` and the fault line's
  `memory protection:` (programs and tests read them: `crash write`, `tools/mmu-test.sh`); exit status 139 for a
  protection fault, as for every fault. No system call, message, struct or file format changes.
- **D43 (RTG, branch `rtg`, 2026-11-12):** the hardware table grows at its end: `struct hwfb` (32 bytes) at offset 408
  (`HWFB_OFF`, after `board[]`): magic `HWFB_MAGIC` "ZFB1", base, width, height (u16), bytes a row, Picasso96's
  `RGBFormat` number (u8), bytes a pixel (u8), flags (u16: `HWFB_HVS` 1, `HWFB_TEST` 2), the Pi's register address and
  its two values (`hvs`, `dl_rtg`, `dl_chip`); written by zenixboot after stage0, never by the kernel; `DRV_HWINFO` still
  copies at most the first 408 bytes. zenixboot's switch `RTG`. The display protocol's `DPYM_RTG` 2 (`DPY_MODE`'s
  answer on a card's screen, which no request changes). No system call, message or error number changes.
- **D44 (CD-ROMs, branch `cd`, 2026-11-12):** `SYS_atapi` 98 (the disk server only, else -EPERM): `struct uatapi`
  (`kernel/diskproto.h`, first size `UATAPI_FIRST` 28: the 12-byte packet, buffer, `len`, `skip`, `err` back), data from
  the drive only, bytes `[skip, skip + len)` of the transfer kept (`len` and `skip` even, else -EINVAL; the buffer anywhere), the answer the bytes the
  drive sent or -ENODEV (no CD drive), -EIO (`err` the ATA error register, 0 when a wait ran out or the drive sent nonsense: then it was given DEVICE RESET); `struct diskinfo`'s
  caps bit `DISKCAP_RO` 8 and the device name `cd0` (its sectors 512 bytes, `DISK_WRITE` -EROFS, more than 128 sectors -EINVAL, the
  mounting server's reads -ESTALE after the disc changed); diskd's saved table's medium number 6 (the CD drive); `/etc/fstab`'s and mount's type
  `iso9660` and its options (`uid=`, `gid=`, `fmode=`, `dmode=`, `norock`, `nojoliet`, `ro`; `rw` refused); `fsinfo.type`
  `iso9660`; the program text `/sbin/iso9660d DEV -id START,SIZE [-o OPTIONS]`; its inode numbers (half the record's byte
  position on the volume). No message type or error number is new.
- **D47 (exFAT, branch exfat, 2026-11-13):** `/etc/fstab`'s and mount's type `exfat` and its options (`uid=`, `gid=`,
  `fmode=`, `dmode=`, `ro`; `rw` refused); `fsinfo.type` `exfat`; the program text `/sbin/exfatd DEV -id START,SIZE
  [-o OPTIONS]`; the answer for a file of 4 GB or more (size 4294967295, `EFBIG` at open, as pfs3d's). No message type,
  struct or error number is new.
- **D48 (the file view, branch files, 2026-11-13):** the keys file `~/.config/files/keys` (Ahiru.keys's format: `KEYS
  ACTION [ARGUMENT]`, the key names in `<>`, `[main]`, `none`) and its action names; `files`' options (`-v`, a folder);
  zdesk's menu and icon lines name `files`. No message type, struct or error number is new.
- **D50 (the picture viewer, branch view, 2026-11-13):** the keys file `~/.config/show/keys` (files' format, D48) and
  its action names (`next prev first last zoom dither left right up down help quit none`); `show`'s options (`-v`,
  `-n`, `-f`, a picture); the keys files' parser is one (`display/lib/keys.c`) for `files` and `show`. No message type,
  struct or error number is new.
- **D49 (directories in batches, the device in `stat`, branch readdir, 2026-11-13):** `SYS_readdirs` 99 (fd, buffer,
  size: size 104 `UDIRX_MAX` .. 65,535, its bits 16-31 reserved for request flags (0 today, else -EINVAL), the buffer
  4-aligned and the caller's (-EINVAL, -EFAULT); answers the bytes of whole records, 0 at the end, the server's
  -errno, -ENOSYS when the directory's server lacks the request (nothing written, the position kept); the position
  shared with `SYS_readdir`). `FS_READDIRS` 27 (a0 the inode, a1 the position, a2 the buffer, a3 `struct fsrdx`
  {size, next}: the records written, `next` the position after them; an error after one record or more answers
  those). The record `struct udirx` (`kernel/udirx.h`): size (u16), type (u8, `udirent`'s numbers, ext2's file
  types), flags (u8: `UDX_STAT` 1), ino, next, a `struct ustat` of 28 bytes, the name (1-63 bytes, a 0, zeros to
  size); the growth rule: a reader steps by size (at least 44, a multiple of 4, inside the answer) and ignores flags
  it does not know and bytes after the name's 0; a longer record only when a request's flags ask for it.
  `UDX_STAT`'s meaning: st is what the server's `FS_LSTAT` gives for this entry, as `fstatat(dirfd, NAME,
  AT_SYMLINK_NOFOLLOW)` (the caller's search right on this directory checked, those above at the open); never on
  `.`, `..` or a name the server cut; a name that is a mount point keeps the directory underneath's (libz takes it
  out). `stat`, `lstat`, `fstat` with a size of 32 or more (`USTAT_SIZE`): the device at offset 28, the mount's
  instance number (never reused while the system runs; the root's 1), 0 for the kernel's own devices and sockets;
  the servers still write 28 bytes (`struct ustat` in the protocol is unchanged). `udirent`'s type from zfsd is
  ext2's numbers (1, 2, 7) as every other server's (it gave the library's 4, 8, 10: a fix). `cp`'s and `mv`'s refusal
  "'A' and 'B' are the same file" (status 1, GNU's text).
- **D22 (zpkg, 2026-10-28):** `UPD_F_TRIAL` 2 in the bootinfo and `uupdate` flags; the manifest line `takes OTHER`;
  zup's journal line `live 1 NAME` and the live-only operations `S`, `M`, `X`; `/var/cache/zpkg/live/` and
  `/var/lib/zpkg/lock` (a pid and a newline, linked whole from `lock.PID`); zpkg's `/etc/zpkg.conf` (the words
  `channel`, `server`, `hold`, `cache`; others ignored), `/var/lib/zpkg/deps` (a name a line) and `sync/CHANNEL.index`
  with its `.sig`; zpkg's built-in keys by build kind (D21 6.3); `zup -f`/`-r`'s command line is between programs of one base version
  and not frozen (docs/d22-zpkg.md section 10).
- **D22 amendment 2 (the system update from Zenix, 2026-10-31):** zpkg writes exactly the names every zenixboot reads,
  `Zenix/zenix-update.zup` and `zenix-update.zup.sig` (frozen by D15 and D21 6.2), as new files of the one FFS
  partition with a `Zenix` folder holding a kernel; the package `zpkg-new` (`/bin/zpkg-new`, `depends base
  2026103000`) and `zpkg-new` as a package program's name in zup's and zpkg's lock rule; `sysutils`'s dependency on
  it in the repository's lists (tools/mkzup.py `BOOTSTRAP`, `BOOTSTRAP_DEP`; both go together when the owner says).
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
- **D29 ZINE step 2 (`SYS_zine` 93, branch zine02, 2026-10-30):** ops `ZINE_INFO` 0 and `ZINE_GO` 1; `struct uzine_info`
  (28 bytes first: size, layout, attn, chip_total, chip_free, fast_total, fast_free) and `struct uzine_go` (32 bytes
  first: size, blob, blob_size, vol, vol_size, blk, blk_size, flags 0); both only grow at the end, the caller passes
  its size (kernel/zine.h). Not frozen: the kernel-blob interface after them (one build; a blob of another layout is
  -ENOEXEC).
- **D29 16.23 (branch zine14, 2026-11-13):** `SYS_zine` op `ZINE_MAP` 2 (root's) with `struct uzine_map` (20 bytes
  first: size, fd, first, n, ext) and `struct uzine_ext` {lba, count}, `UZINE_HOLE` 0xFFFFFFFF; answers the runs
  filled (0 past the end), -ENODEV (the session has no reader: not the IDE drive), -ENOSYS (the server lacks
  `FS_BMAP`), -EPERM, -EBADF, -EINVAL, -EFAULT. `FS_BMAP` 28 (a0 the inode, a1 its first 512-byte sector, a2 u32 pairs,
  a3 their room): runs of sectors on the server's device, each following the one before in the file, holes
  0xFFFFFFFF, the last ending at the file's last sector; 0 at or past the end; -EINVAL not a regular file; -ENOSYS a
  server without it. The kernel-blob interface's layout became 2 (`struct zine_args ide`; not frozen).
- **D29 12.1, running by name (branch zine04, 2026-10-31):** a file a program spawns that has the x bit and starts with
  the longword `0x000003F3` (AmigaOS's `HUNK_HEADER`) is run as `/bin/zine PATH ARGS...` (argv[0] "zine"); without
  `/bin/zine` the spawn is -ENOEXEC (the kernel log names the file). Such a file may be larger than 16 MB (Zenix
  programs are refused above that, -EFBIG; the limit was 256 KB until 2026-11-13: it only grows). Since the same day the
  loader also refuses more relocations than half the image's bytes (ZEXE: `image_size / 2`; ZEX2: the two lists
  together, `data_size / 2`): each names its own even word, so no linker writes such a file.
- **Audit block 3 B3-12 (`SYS_envset`, 2026-10-30):** a block whose last byte is not 0 is refused with -EINVAL
  (checked on the kernel's own copy; the old environment kept); a block of 0 bytes still empties it. An addition only:
  every caller in the tree ends its block with a 0, and before, such a block was taken and every child's `strlen` ran
  past it.
- **Supplementary groups (owner 2026-10-31, branch groups):** `SYS_groups` 94 with `GROUPS_GET` 0 (a2 a buffer of
  16-bit ids, a3 its room; 0 counts; a smaller room than the list -EINVAL) and `GROUPS_SET` 1 (a2 the ids, a3 the count,
  at most `NGROUPS`; root only, -EPERM); `NGROUPS` 8 (it may only grow); the list inherited on spawn and kept by
  setuid, setgid and set-user-id programs. `UMSG_GROUPS` 16 in `struct umsg`'s kflags, set by the kernel only, and
  `struct umsg_groups { u16 n, g[NGROUPS]; }` right after `struct umsg` in memory (`UMSG_GROUPS_AT`); `struct umsg`
  itself is unchanged, so a program's own `SYS_call` and older servers are as before. `id`'s `groups=` field (after
  `euid=`), Linux's form.
- **D30 (FAT, branch fat, 2026-10-31):** `/etc/fstab`'s type `fat` and mount's FAT options (`uid=`, `gid=`, `fmode=`,
  `dmode=`, `ro`, `rw`, `force`); `fsinfo.type` `fat`; the program text `/sbin/fatd DEV -id START,SIZE [-o OPTIONS]`.
- **D30 read-only mode (branch fatro, 2026-11-13):** added only: fatd reads `DISKCAP_RO` and then mounts read-only; `-o ro` writes nothing (no clean mark, FSInfo or date) and every change answers `-EROFS`; on a `DISKCAP_RO` device `mount` accepts type `fat` (as `iso9660` and `exfat`) and refuses `-o rw` (mount's and fatd's own message). No option, message type or error number is new; a writable device is as before.
- **D31 (OFS, FFS, branch ffs, 2026-10-31):** `/etc/fstab`'s type `ffs` and mount's FFS options (`uid=`, `gid=`, `fmode=`,
  `dmode=`, `ro`, `rw`, `blocksize=`, `reserved=`); `fsinfo.type` `ffs`; the program text `/sbin/ffsd DEV -id START,SIZE [-o OPTIONS]`.
- **D32 (PFS3, branch pfs3, 2026-10-31):** `/etc/fstab`'s type `pfs3` and mount's PFS3 options (`uid=`, `gid=`, `fmode=`, `dmode=`, `ro`, `blocksize=`; `rw` is refused); `fsinfo.type` `pfs3`; the program text `/sbin/pfs3d DEV -id START,SIZE [-o OPTIONS]`.
- **D33 (LhA and LZX, branch lha, 2026-11-01):** the commands `unlha` and `unlzx`, their commands `l`, `t`, `x`, options `-f`, `-q`, `-d`, `-L` and exit statuses (0, 1, 2).
- **D34 (ZenixFS as the root, branch zroot, 2026-11-01):** `FS_HEALTH` 26 (root only; bit 0: the server can only read; a server without it: -ENOSYS); `zfsd`'s word `-fsck`; `zpkgindex`'s header line `systemz VNUM VSTR FILE SIZE SHA256 min MIN`, its file `zenixz-VNUM.zup` (an older zpkg ignores it: other header words are growth); `fsck.zenixfs`'s `-y` and exit statuses (0, 1, 2); `mkroot.sh`'s `ROOTFS=zenixfs`. The format of ZenixFS volumes is unchanged (version 1).
- **Paths (owner's PFS3 work, branch paths, 2026-10-31):** `FS_PATHMAX` 256 (it was 160; it may only grow): a path given to a system call, an absolute path and the working directory's text at most 255 bytes, -ENAMETOOLONG beyond; `PATH_MAX` and `ZPATH_MAX` 256 (older programs: 64, and they keep working). No message or struct changed: `FS_PATHMAX` is the size of the two sides' buffers, and D26's TEXT is at most the server's own number, inside the kernel's. The rule that lets a server use the larger number is D15's, the kernel never older than the root, with the one exception architecture.md section 7 names. `getcwd`'s -ERANGE stands for a caller's smaller buffer. `udirent`'s 64-byte name and `umountinfo`'s 48-byte path are unchanged.
- Not on the list: the kernel's internal file kinds. libz is linked into each program, so it needs no rule of its
  own.
- **D35 (the installer, branch install, 2026-11-03):** `/etc/hostname` (one line, 1-63 of `a-z 0-9 -`; login shows it in its prompt and sets `$HOSTNAME`, `hostname` prints it); `zinstall`'s options `--list`, `--undo [FILE]`, `--hostname NAME`, `--root-password-file FILE`, `--user NAME`, `--user-password-file FILE`, `--user-name TEXT`, `--no-user`; `zpkg --fetch DIR [--zfs]`; the copy `/var/lib/zinstall/table-before` (`ZTB1`, read only by the same build's `--undo`). Grown (D35 3b, branch inst2, 2026-11-05): `zinstall`'s options `--startup`, `--root`, `--home` (`keep:NAME`, `format:NAME`, `none`) and `--server URL|none`.
- **D46 (USB sticks, branch usb, 2026-11-12):** the port `usb` (the service `/sbin/usbd`); `DISK_USBHELLO` 11 and the posted record `struct urec` between usbd and diskd (they only grow at the end); the quiesce step `DRVS_WAIT` 3; the hardware table's `xhci` entry; the device names `usb0`, `usb0pN`; `lsusb`'s options `-v` and `--hold`. Details in docs/claims.md and docs/d46-usb.md.
- **D51 (TrueType fonts, branch ttf, 2026-11-13):** `/usr/share/fonts/truetype/NAME.ttf` as a source of
  `font NAME SIZE` (after `/usr/share/fonts/amiga/`); the cache `/var/cache/fonts/NAME/SIZE` and `NAME.font` (AmigaOS's
  formats) and its stamp, the file's last 28 bytes: `HUNK_DEBUG` (0x3F1), 4, `ZTTF`, the .ttf's size, its time,
  `TTF_STAMP_VER << 16 | SIZE` (a new renderer raises the version: its files are made again), `HUNK_END`; the command
  `ttf2font` (`-q`, `-o DIR`, `-n NAME`, `-c`, sizes 4-64, exit statuses 0, 1, 2).
- **D53 (scripts by their `#!` line, branch shebang, 2026-11-13):** a file a program spawns that has the x bit and
  starts with the bytes `#!` runs as `INTERPRETER [ARG] PATH ARGS...`: the interpreter's path as written on the first
  line (no `$PATH` search; relative to the caller's working directory), at most one argument (the rest of the line,
  inner spaces kept, spaces and tabs at its end removed), the path the spawn was given, then the original arguments
  after `argv[0]`. The line (from `#!` to the newline) is at most 127 bytes; a longer one, or one with no interpreter,
  is -ENOEXEC, never cut. A carriage return is not removed. The interpreter must be a Zenix program (a script or an
  AmigaOS program there: -ENOEXEC). The list keeps spawn's limits (-E2BIG). Set-user-id: neither the script's bit nor
  its interpreter's is honoured. The process's name is the script's file name. A script may be of any size. A file with
  no program header, no `HUNK_HEADER` and no `#!` stays -ENOEXEC. These may only become more permissive (a longer line,
  more levels), never change meaning.
- **D54 (the debugger, branch debugger, 2026-11-13):** system call 100 `debug(op, pid, arg)` and its ops 1-14 with
  their argument meanings (`kernel/dbgproto.h`); `DBGW_*` 1-9; the structs `udbgregs`, `udbgstop`, `udbginfo` (each
  starting with the caller's size, 0 the first size: 76, 24, 28; they grow only at the end) and `udbgmem` (12 bytes);
  `DBG_CONT`/`DBG_STEP`'s argument a signal (0 none). The rules: only one's own processes (same effective and real id,
  not `nodebug`: a set-user-id start, an unreadable file, a `setuid`), root any; never pid 0/1, a process without a
  parent, a system process, `priv`; a program started under the debugger stopped at its entry (`DBGW_EXEC`, PC = the
  entry) with a private copy of shared code and without its set-user-id; the breakpoint is `ILLEGAL` (0x4AFC), only in
  the image; `SIGINT` to a traced process stops it for the debugger; a fault stops it, then it ends as before (139);
  `sigreturn` keeps the trace bits; a stray trace exception is harmless; the debugger's death detaches what it
  attached to and kills what it started; a target's end is reported once by `DBG_WAIT` (`DBGW_EXIT`). The crash lines
  `[kernel] pid N regs: base B code C sr S pc P d0..d7 X*8 a0..a7 X*8` and `[kernel] pid N frames: K R1 .. RK` (zdb
  reads them; not for root's or a `nodebug` process): words may be added at the end of each, never moved. `zld -M`'s map is `nm -n`'s format.
- **D55 (screens of their own, branch intro, 2026-11-14):** the display protocol's `DPY_SCREEN` 18 (a[0] `struct
  dpyscr`, first size 36: size, w, h, depth, flags, planes, bytes, pstride, bpr, ncop, ncol, pad, palette; `DPYSF_LACE`
  1, `DPYSF_HAM` 2, `DPYSF_EHB` 4, `DPYSF_SCROLL` 8, `DPYSF_BACK` 16, `DPYSF_MOTION` 32; the answer the vertical-blank
  count from which it shows, or -EINVAL, -EFAULT, -EBUSY, -ENODEV, -ENOMEM, -ENOSPC), `DPY_SCR_SET` 19 (a[0] `SS_PALETTE`
  1, `SS_COPPER` 2 (-EINVAL with the instruction's index in a[3], -1 the fetch), `SS_PLANES` 3, `SS_FRONT` 4,
  `SS_COLOURS` 5 (the next count `CO_COLOUR`s from an instruction, others passed over; -EINVAL with a[3] the colours
  set)), `DPY_SCR_CLOSE` 20; `struct dpycop` (8 bytes: op, a, b, c) with `CO_WAIT` 1, `CO_COLOUR` 2, `CO_PLANE` 3,
  `CO_MOD` 4, `CO_SCROLL` 5 and their checks (docs/d55-screens.md section 5; a list refused today may be accepted by a
  later displayd, never the other way); `DPYSCR_COPMAX` 1024 at least; a screen's events with `win` 0; libdisplay's
  `dpy_screen`, `dpy_scr_set`, `dpy_scr_close`, `dpy_vwait`, `dpy_vcount`. The kernel's `SYS_console` op 12
  `CON_VBLANK` (no descriptor, any process: a2 the count to wait for, bit 31 only asks; the count now, 31 bits; at
  most 100 ms; -EINTR not restarted; -ENODEV without a display) and op 13 `CON_HOLD` (the grab's descriptor and
  process: a2 a pid and a3 the base of a chip RAM region that pid's memalloc gave -> its size; a2 0 lets go; `NHOLD` 4
  at least; a held region's memfree -EBUSY; its owner's end hands the block to the grab's process); the grab's record
  `GEV_GONE` 6 (x the hold's slot, ch the base big-endian), and a record of type 0 that says nothing (readers skip it:
  an unread `GEV_GONE` whose block was let go becomes one).
- **D58 (the IRC client, branch irc, 2026-11-17):** `irc`'s settings file `~/.config/irc/config` (`SETTING VALUE` a
  line, `#` comments; the settings `server port tls nick user realname password sasl quit charset scrollback
  reconnect bell autojoin highlight`, the last two up to 8 words; an unknown setting is an error), its keys file
  `~/.config/irc/keys` (files' format, D48) with the action names of docs/man/irc.txt; the command's options
  (`-c -n -p -t -K`); `/lib/tls/tlspipe HOST PORT`'s contract (the socket its descriptor 2, the plain text on 0 and
  1, its first line `ok`, `ok clock DATE` or `error TEXT`, `info TEXT` lines before it, exit 0/1/3), shared with the
  e-mail client (D59; it was `/lib/irc/tlspipe` until D59 moved it into the package `tls`, before any release).
