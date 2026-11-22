# Numbers that are claimed

<!-- Moved out of CLAUDE.md on 2026-10-28, word for word (CLAUDE.md points here). -->

Claim a number here **before** using it (system calls, message types, port names, error numbers, flags), in the same commit as its first use.

| System calls | Owner |
|---|---|
| 1-77 in use except 39, 40, 68 (removed with the workspaces in batch E; free again) | main, all merged (see `kernel/kernel.h`) |
| 78 `getpriority`, 79 `setpriority` (DESIGN D10, priorities) | main, merged 2026-10-24 |
| 80 `reboot` (save the disks, reset; optionally leave the boot menu a one-shot choice) | main, 2026-10-24 |
| 81 `svc` (DESIGN D11, the service manager: register, remove, self, save, load, opens) | main, merged 2026-10-25 |
| 82 `update` (DESIGN D15: INFO, RELEASE, NOTE) | built on branch d15, 2026-10-26 |
| 83 `evwait`, 84 `sigaction`, 85 `sigprocmask`, 86 `sigreturn`, 87 `alarm`, 88 `setpgid` (DESIGN D16); `kill`'s third argument 1 (the new form: a signal, groups) and `setfg`'s second argument 1 (a process group) | built on branch d16, 2026-10-26 |
| `exit`'s second argument (DESIGN D18: a child's pid, made the caller's parent's child first; login steps aside for init) | built on branch diet, 2026-10-26 |
| 89 `console` (the 2 MB diet, DESIGN D18: op 1 `CON_COLOURS`, the display's colours 2/4/8/16, 0 asks; D14 note section 11) | built on branch diet, 2026-10-26 |
| 89 `console`'s op word `op \| (fd + 1) << 8` (0 above the op: no descriptor, only `CON_COLOURS`), op 2 `CON_FONT` (a PSF1 font for that console, null drops), op 3 `CON_RULES` (bits, mask; `CONR_*` 1-64) (DESIGN D20 4.5) | main, branch t12, 2026-10-27 |
| 89 `console`'s ops 5 `CON_READ` (a console's grid: `struct uconread`), 6 `CON_LINE` (a display line's pens: `struct uconline`), 7 `CON_INPUT` (raw keys or text: kinds `CONI_RAW` 0, `CONI_TEXT` 1); the `CONRD_*` flags 1-8 and the cells' `CONA_*` bits 1-4 (DESIGN D24 6.1-6.3, root only; op 4 stays D28's `CON_UFONT`, planned) | main's builder, branch wasabi3, 2026-10-29 |
| 90 `sock` (DESIGN D19: every BSD socket call: op `SOCK_*` plus the request size << 16, fd, `struct usockreq`), 91 `netsrv` (the network server: `NETSRV_HELLO`, `READY`, `REPLY`), 92 `drv` (the network's drivers: `DRV_*` ops 1-7, `HWINFO` .. `XFER`; built on branch n678 2026-10-27; `DRV_CLAIM`'s base `DRV_PCMCIA` 0xA00000 and its -EADDRINUSE, D36, branch ne2000 2026-11-03; op 8 `DRV_PARACK` and `DRV_CLAIM`'s base `DRV_PARPORT` 0xBFE101, the parallel port, D39, branch plip 2026-11-04) | main, branch n023, 2026-10-27 |
| `ticks`'s (20) argument 2 (DESIGN D21 8.3: who set the clock this boot, `CLOCK_RTC` 1, `CLOCK_NET` 2) and `settime`'s (59) second argument (0 a hand-set time, clears the bits; 1 from a checked source, sets `CLOCK_NET`; other values -EINVAL) | branch clock, merged 2026-10-28 (recorded here by audit 2026-10-29 B2-7) |
| 89 `console`'s op 4 `CON_UFONT` (the console's Unicode font for every console: no descriptor in the op word, root; a2 ZCF1 data, a3 its size; a2 0 drops it), the rule `CONR_8BIT` 128 (`CONR_ALL` 255), and `CON_READ`'s a3 1 `CONRF_CELL4` (cells of 4 bytes: the 16-bit value big-endian, `CONA_*`, the colour) (DESIGN D28 7) | main's builder, branch utf8, 2026-10-29 |
| 93 `zine` (DESIGN D29: op 0 `ZINE_INFO` `struct uzine_info`, op 1 `ZINE_GO` `struct uzine_go`; root at the Amiga's own console only); the kernel log's end reasons `KLOG_END_ZINE` 3 and `KLOG_END_ZINE_DAMAGED` 4 (the second for step 5) | branch zine02, 2026-10-30 (step 2 tested in FS-UAE) |
| 94 `groups` (supplementary groups: op 0 `GROUPS_GET`, op 1 `GROUPS_SET`, root only; `NGROUPS` 8); `struct umsg`'s kflags bit 16 `UMSG_GROUPS` (a `struct umsg_groups` follows the message; only the kernel sets it) | branch groups, 2026-10-31 |
| 95 `ttyctl` (DESIGN D20 7.2, D38 step 1: a terminal named by a descriptor; ops `TTYC_GWINSZ` 1, `TTYC_SWINSZ` 2, `TTYC_OWNER` 3, `TTYC_SCTTY` 4; `struct uwinsize`, Linux's `winsize`); the device names `/dev/ptmx` and `/dev/pts/0`-`/dev/pts/7`, the slaves' `st_ino` 11-18 and the masters' `0x100`-`0x107` (`PTY_MINO`); libz's and the C library's `ioctl` numbers `TIOCSCTTY` 0x540E and `TIOCSWINSZ` 0x5414 (Linux's, beside `TIOCGWINSZ` 0x5413) | branch desk, 2026-11-04 |
| 89 `console`'s ops 8 `CON_GRAB` (root, a console's descriptor; `struct ucongrab`, first size 24: a new descriptor reading `struct ugevent` records of 16 bytes) and 9 `CON_GACK` (the grab's descriptor; the `GEV_RELEASE` number answered); record types `GEV_KEY` 1, `GEV_POINTER` 2, `GEV_RELEASE` 3, `GEV_ACQUIRE` 4, `GEV_LOST` 5; qualifier bits `GQ_*` (AmigaOS's `IEQUALIFIER` values: 0x1 0x2 0x4 0x8 0x10 0x20 0x40 0x80, `GQ_REPEAT` 0x200, buttons 0x1000 0x2000 0x4000); `GCHIP_AGA` 1; `memalloc`'s (18) second argument `MEMA_CHIP` 1 (chip RAM; other bits ignored: the code and frozen-interfaces.md, corrected by the full audit 2026-11-14) (DESIGN D38 step 2) | branch desk, 2026-11-04 |
| 96 `peer` (`PEER_CTTY` 1, `PEER_RD` 2, `PEER_WR` 3, `PEER_PORT` 4; `struct upeermem`), 97 `post` (`POST_BUF` 1, `POST_WRITE` 2; `struct upostbuf`, `struct upost`), the port poll bit `POLLRDBAND` 0x80 (Linux's value: something posted into the port's window), `struct ucongrab`'s field `topaz` (size 28, `UCONGRAB_TOPAZ`) (DESIGN D38 step 3) | branch desk, 2026-11-04 |
| 89 `console`'s `CON_INPUT` kind `CONI_MOUSE` 2 (DESIGN D38 step 5: mouse records dx, dy, buttons for the desktop's tests, root only as CON_INPUT) | branch desk, 2026-11-04 |
| 93 `zine`'s `struct uzine_info` field `machine` (`UZINE_INFO_MACHINE` 32; `UZI_EMU68` 1: the CPU the desktop names) | branch desk, 2026-11-04 |
| 93 `zine`'s op 2 `ZINE_MAP` (d29 16.23: root's; `struct uzine_map`, first size 20, `struct uzine_ext`, `UZINE_HOLE` 0xFFFFFFFF: an open file's runs of sectors on its device, asked of its server with `FS_BMAP`; -ENODEV when the session has no reader); `FS_*` 28 `FS_BMAP` | branch zine14, 2026-11-13 |
| 89 `console`'s op 10 `CON_MOUSE` (DESIGN D38: the grab's descriptor; a2 THRESHOLD 0..127 \| MULTIPLIER 1..8 << 8, the mouse's acceleration a frame; -EINVAL otherwise) | branch desk, 2026-11-04 |
| 89 `console`'s op 11 `CON_MODE` (DESIGN D28 22: the console's mode, every console's, no descriptor; a2 `CONM_HIRES` 0, `CONM_LACE` 1, `CONM_ASK` 0xFF asks; root to set) | branch pubprep, 2026-11-12 |
| 92 `drv`'s `DRV_CLAIM` base `DRV_PAULA` 0xDFF0A0 (Paula's four channels: the process of port `audio` only, before the network gate; DESIGN D40) | branch sound, 2026-11-12 |
| 98 `atapi` (DESIGN D44: a packet command to the IDE port's ATAPI CD drive, `struct uatapi` in `kernel/diskproto.h`, first size 28; the disk server only) | branch cd, 2026-11-12 |
| 92 `drv` for the service of port `usb` (DESIGN D46: `DRV_HWINFO`, the `DRV_CLAIM` of the hardware table's `xhci` and that claim's ops only, before the network gate); the quiesce step `DRVS_WAIT` 3 (until `(*addr & mask) == value`, at most 20 ms; not on `DRV_PARPORT`); the hardware table's `xhci`, `xhci_size`, `xhci_stop` where `reserved[3]` was, `XSTOP_DONE` 1, `XSTOP_RAN` 2, `XSTOP_SLOW` 4, `XSTOP_NOTXHCI` 8 | branch usb, 2026-11-12 |
| 18 `memalloc`'s second argument bit `MEMA_LOW` 2 (a region ending below 16 MB, the 24-bit addresses; d29 16.19: ZINE's loader) | branch zine9, 2026-11-12 |
| 99 `readdirs` (DESIGN D49: fd, buffer, size; whole `struct udirx` records, `kernel/udirx.h`; size 104-65,535, bits 16-31 reserved for request flags, the buffer 4-aligned); `stat`'s (13), `lstat`'s (57) and `fstat`'s (14) size 32 `USTAT_SIZE`: the device at offset 28 (the mount's instance, 0 the kernel's own) | branch readdir, 2026-11-13 |
| 100 `debug` (DESIGN D54: `debug(op, pid, arg)`, ops `DBG_ATTACH` 1 .. `DBG_INFO` 14 in `kernel/dbgproto.h`; `DBGW_*` 1-9; `struct udbgregs` (first size 76), `udbgstop` (24), `udbginfo` (28), `udbgmem` (12); `DBG_CONT`/`DBG_STEP`'s argument a signal; `NDBG` 4, `NBP` 16, `DBG_MEMMAX` 4096 as minimums; the breakpoint `ILLEGAL` 0x4AFC; `struct proc`'s `dbgf` bits `DBGF_*` 1-8, kernel-internal) | branch debugger, 2026-11-13 |
| 89 `console`'s op 12 `CON_VBLANK` (DESIGN D55 6: no descriptor, any process; a2 the vertical-blank count to wait for, `CONV_ASK` 0x80000000 only asks) and op 13 `CON_HOLD` (the grab's descriptor: a2 a pid or 0, a3 a region's base; `NHOLD` 4); the grab's record `GEV_GONE` 6 | branch intro, 2026-11-14 |
| 101 `shm` (DESIGN D60 7.4: `shm(op, addr, arg)`, `SHM_OFFER` 1 (arg a pid, `SHMF_RO` 0x80000000), `SHM_TAKE` 2, `SHM_DROP` 3, `SHM_INFO` 4 with `struct ushm` (first size 16: size, owner, holders, flags); `NSHM` 8 offers and holds a process; without memory protection ops 1-3 answer 0 and change nothing) | branch d60s3, 2026-11-23 |
| 102 `callv` (DESIGN D60 5.5: port, message, `struct ucalldecl *` {n 0-4, `struct ucallbuf` {addr, len, flags} x 4}; `CALLB_RD` 1, `CALLB_WR` 2; without protection `call` with the list not read); `peer`'s (96) op 5 `PEER_EXACT` (msg 0, arg a port the caller owns); `post`'s (97) op 3 `POST_READ` (up to 16 bytes out of the window); `meminfo`'s (22) fifth word at size 20 `UMEMINFO_PROT` (0 none, 1 D41's, 2 full); `blk_ram`'s (42) copy form (a2 sectors from sector a1, bit 31 write, buffer a3; with protection on only, else -EINVAL) | branch d60s3, 2026-11-23 |
| **next free: 103** | |
Same rule for message types in `kernel/fsproto.h` / `kernel/diskproto.h` and for port names: claim here first.
Message types claimed so far: `NET_*` 1-19 on port `net` (`kernel/netproto.h`, DESIGN D19 step 3: the `SOCK_*` op
numbers 1-18 and `NET_CLOSE` 19; 1-63 are the kernel's to the server), `NETDRV_*` from 64 (the drivers' blocks, step 7:
`NETDRV_ATTACH` 64, `NETDRV_XFER` 65, branch n678 2026-10-27);
`FS_*` 1-24 (24 `FS_CLOSE`: fix-c, batch C5; 21 was never defined: left unused, found by the full audit 2026-11-14), `FS_*` 25 `FS_MARK` (D15 step 2, 2026-10-26:
zup's "needs a check" mark), `FS_*` 26 `FS_HEALTH` (D34, branch zroot, 2026-11-01: boot asks whether a ZenixFS root could only be mounted read-only), `FS_*` 27 `FS_READDIRS` (D49, branch readdir, 2026-11-13: a directory's records in a batch, `struct fsrdx` in a3; the record's flag `UDX_STAT` 1), `FS_*` 28 `FS_BMAP` (d29 16.23, branch zine14, 2026-11-13: a file's runs of sectors on the server's device, for ZINE), `FS_SYNC`'s a0 1 "bounded" (D23 step 1, branch nfs12, 2026-10-28; recorded by audit
2026-10-29 B2-7), `DISK_*` 1-8, `DISK_*` 9 `DISK_INFOSZ` and
10 `DISK_LISTSZ` (D15 step 0, fix3, 2026-10-26: `DISK_INFO`/`DISK_LIST` with the caller's buffer size); `struct diskinfo`'s
caps bit `DISKCAP_RO` 8 (D44, branch cd, 2026-11-12: a read-only medium, the CD drive `cd0`); `DISK_USBHELLO` 11 (D46, branch usb, 2026-11-12: the USB server's hello, root and the owner of port `usb` only) and the posted records `struct urec` (32 bytes, `UREC_N` 8 a window) with kinds `UR_READ` 1, `UR_DONE` 16, `UR_ADD` 17, `UR_DEL` 18.
`struct umsg`'s `kflags` bits: 1 `UMSG_KERNEL`, 2 `UMSG_NETREPLY` (internal), 4 `UMSG_CANCEL` (D23 step 1, branch nfs12, 2026-10-28: only the kernel sets it, for a killed client), 8
`UMSG_LINKS` (D26, branch d26, 2026-10-28: only the kernel sets it, through `ipc_kcall`'s kflags argument; `call()`
ORs D23's `UMSG_CANCEL` in itself), 16 `UMSG_GROUPS` (branch groups, 2026-10-31: the supplementary groups follow the
message; only the kernel sets it, for vfs.c's requests). Answers in `kernel/fsproto.h`: a value or -errno; -1024 .. -1279 reserved (no
errno goes there, `kernel/errno.h`), of which `FS_LINKED` + `FSL_*` (`FSL_A1` 1, `FSL_LAST` 2: -1024 .. -1027) for a
sender that set `UMSG_LINKS` (D26, branch d26, 2026-10-28).
Error numbers: `ECONNRESET` 104 (D11, "the server is gone"; main; on a socket also "reset by the peer", D19); D19's
network numbers (Linux's, main, 2026-10-27): `ENOTSOCK` 88, `EDESTADDRREQ` 89, `EMSGSIZE` 90, `EPROTOTYPE` 91,
`ENOPROTOOPT` 92, `EPROTONOSUPPORT` 93, `EAFNOSUPPORT` 97, `EADDRINUSE` 98, `EADDRNOTAVAIL` 99, `ENETDOWN` 100,
`ENETUNREACH` 101, `ECONNABORTED` 103, `ENOBUFS` 105, `EISCONN` 106, `ENOTCONN` 107, `ETIMEDOUT` 110, `ECONNREFUSED`
111, `EHOSTUNREACH` 113, `EALREADY` 114, `EINPROGRESS` 115; `ETXTBSY` 26 (D13, "text file busy": a running
shared program opened for writing; claimed 2026-10-25, Linux's number); `ESTALE` 116 (D23, "stale file handle", Linux's;
branch nfs12, 2026-10-28); the C library's (D27 section 5, Linux's numbers, block 3, 2026-10-29): `EDOM` 33, `EOVERFLOW`
75, `EILSEQ` 84, and `ENOTSUP` the same number as `EOPNOTSUPP` (95, a second name, no new number). Update note kinds (D15, frozen, append-only):
1 done, 2 failed, 3 confirmed; 4 "confirmed, but the root half failed" (audit3 B1, 2026-10-26). `struct uupdate`'s (and
the bootinfo's) flags bits: 1 `UPD_F_ROLLBACK` (D15), 2 `UPD_F_TRIAL` (D22 4.3, claimed 2026-10-28: an unconfirmed trial;
the kernel keeps it after `UPD_RELEASE`; zenixboot sets it from D22 step 7). The bootinfo's boot options at `+0x84` (`kopts`, D37, branch cache,
2026-11-04): 1 `KOPT_DCACHE_OFF`, 2 `KOPT_DCACHE_WT`; 4 `KOPT_MMU_ON` and 8 `KOPT_MMU_OFF` (D41, branch mmu,
2026-11-12: memory protection on, off; both set: off); 16 `KOPT_MMU_AUTO` (D41 section 19, branch mmudef, 2026-11-17:
only in the kernel file's default long after `ZKOPTDEF`, never set by a loader: protection on where the CPU is known to
have an MMU, LC and EC 68040/68060 parts left alone). The hardware table's growth after `board[]` (D43, branch rtg, 2026-11-12): `struct hwfb` at offset 408 (`HWFB_OFF`), its magic `HWFB_MAGIC` 0x5A464231 ("ZFB1"), flags `HWFB_HVS` 1 and `HWFB_TEST` 2. Poll bits on a port (D23 step 1, branch nfs12, 2026-10-28; recorded by audit 2026-10-29 B2-7): `POLLPRI` 2 (Linux's
value) on a port: a message sent to it was cancelled (`UMSG_CANCEL`) since its owner last saw one. Socket options: `SO_LINGER` 13 (Linux's, D23 steps 3-4, branch nfs34,
2026-10-28; recorded by the same audit). The display protocol (DESIGN D38 step 3, `display/proto.h`, version 1): port `display` (the D11 service of
`/sbin/displayd`; a client's own ports `dpy.PID`), requests `DPY_HELLO` 1 .. `DPY_IMAGE_FREE` 11 and the window
manager's `DPY_WM_HELLO` 32 .. `DPY_WM_KILL` 42, events `EV_KEY` 1 .. `EV_MINIMISED` 8 and `EV_WM_NEW` 32 ..
`EV_WM_MENU` 40, `EV_MENU` 9, `DPY_MENU` 12, `DPY_WM_MENUGET` 43, `DPY_WM_MENUPICK` 44, `DPY_WM_PGRAB` 45 (step 5), `DPY_WM_MOUSE` 47 (the mouse's acceleration, the kernel's `CON_MOUSE`), `DPY_STAT` 14 (the server's counters, for the tests), `DPY_SESSION` 13 and `DPY_WM_BYE` 46 (D38 revision 3, `dlogin`), `DPY_MODE` 15 with `DPYM_HIRES` 0, `DPYM_LACE` 1, `DPYM_ASK` 0xFF and the event `EV_SCREEN` 10 (D38 "Screen modes", branch modes, 2026-11-06), `DPYM_RTG` 2 (D43, branch rtg, 2026-11-12: a graphics card's screen, only answered), `DPY_FONT_INFO` 16 with O_TEXT's font `DPYF_UI` 1 and the flags `DPYFF_PROP` 1, `DPYFF_BUILTIN` 2 (D38 "Fonts", branch fonts, 2026-11-07), `DPY_FONT_SET` 17 with `DPY_FONTNAME` 32 and `EV_SCREEN`'s code `DPYS_FONT` 1 (D38 "Preferences", branch prefs, 2026-11-07), `DPY_SCREEN` 18, `DPY_SCR_SET` 19 with `SS_PALETTE` 1 .. `SS_COLOURS` 5, `DPY_SCR_CLOSE` 20, `struct dpyscr` with `DPYSF_*` 1-32, `struct dpycop` with `CO_WAIT` 1 .. `CO_SCROLL` 5, `DPYSCR_COPMAX` 1024, a screen's events with `win` 0 (D55, branch intro, 2026-11-14), drawing orders `O_CLIP` 1 .. `O_XLINE` 6, and the flags and structs named there (branch desk,
2026-11-04). Port names
registered as services (D11):
`audio` (D40, branch sound 2026-11-12: the sound server `/sbin/sndd`, reserved for root like `net`; its protocol
`user/sndproto.h`: requests `SND_OPEN` 1 .. `SND_PAUSE` 9 and the tests' `SND_STATE` 10, `SNDF_STEREO` 1, `SNDF_FRAMES`
2, `SNDI_NTSC` 1, `SNDC_TRIG` 1, `SNDC_PER` 2, `SNDC_VOL` 4, `SNDC_LOOP` 8, `SNDC_STOP` 16, `SND_SILENT` 0xFFFFFFFF);
`usb` (D46, branch usb 2026-11-12: the USB server `/sbin/usbd`, a service, so only its process takes the port; its
protocol `usb/usbproto.h`: requests `USB_LIST` 1, `USB_DESC` 2, `USB_HOLD` 3, `USB_STAT` 4; `struct usbdevinfo` first
size 68, `struct usbstat` 32);
`disk`, `fs`, `fs.*`; `ping` (the test service, served by `msgpingd` since D19 step 0); `net` (D19, the network server:
reserved for root like `fs` and `disk`; `socktestd` serves it in the tests until netd).

