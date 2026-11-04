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
| 89 `console`'s ops 8 `CON_GRAB` (root, a console's descriptor; `struct ucongrab`, first size 24: a new descriptor reading `struct ugevent` records of 16 bytes) and 9 `CON_GACK` (the grab's descriptor; the `GEV_RELEASE` number answered); record types `GEV_KEY` 1, `GEV_POINTER` 2, `GEV_RELEASE` 3, `GEV_ACQUIRE` 4, `GEV_LOST` 5; qualifier bits `GQ_*` (AmigaOS's `IEQUALIFIER` values: 0x1 0x2 0x4 0x8 0x10 0x20 0x40 0x80, `GQ_REPEAT` 0x200, buttons 0x1000 0x2000 0x4000); `GCHIP_AGA` 1; `memalloc`'s (18) second argument `MEMA_CHIP` 1 (chip RAM; other bits refused) (DESIGN D38 step 2) | branch desk, 2026-11-04 |
| 96 `peer` (`PEER_CTTY` 1, `PEER_RD` 2, `PEER_WR` 3, `PEER_PORT` 4; `struct upeermem`), 97 `post` (`POST_BUF` 1, `POST_WRITE` 2; `struct upostbuf`, `struct upost`), the port poll bit `POLLRDBAND` 0x80 (Linux's value: something posted into the port's window), `struct ucongrab`'s field `topaz` (size 28, `UCONGRAB_TOPAZ`) (DESIGN D38 step 3) | branch desk, 2026-11-04 |
| **next free: 98** | |
Same rule for message types in `kernel/fsproto.h` / `kernel/diskproto.h` and for port names: claim here first.
Message types claimed so far: `NET_*` 1-19 on port `net` (`kernel/netproto.h`, DESIGN D19 step 3: the `SOCK_*` op
numbers 1-18 and `NET_CLOSE` 19; 1-63 are the kernel's to the server), `NETDRV_*` from 64 (the drivers' blocks, step 7:
`NETDRV_ATTACH` 64, `NETDRV_XFER` 65, branch n678 2026-10-27);
`FS_*` 1-24 (24 `FS_CLOSE`: fix-c, batch C5), `FS_*` 25 `FS_MARK` (D15 step 2, 2026-10-26:
zup's "needs a check" mark), `FS_*` 26 `FS_HEALTH` (D34, branch zroot, 2026-11-01: boot asks whether a ZenixFS root could only be mounted read-only), `FS_SYNC`'s a0 1 "bounded" (D23 step 1, branch nfs12, 2026-10-28; recorded by audit
2026-10-29 B2-7), `DISK_*` 1-8, `DISK_*` 9 `DISK_INFOSZ` and
10 `DISK_LISTSZ` (D15 step 0, fix3, 2026-10-26: `DISK_INFO`/`DISK_LIST` with the caller's buffer size).
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
2026-11-04): 1 `KOPT_DCACHE_OFF`, 2 `KOPT_DCACHE_WT`. Poll bits on a port (D23 step 1, branch nfs12, 2026-10-28; recorded by audit 2026-10-29 B2-7): `POLLPRI` 2 (Linux's
value) on a port: a message sent to it was cancelled (`UMSG_CANCEL`) since its owner last saw one. Socket options: `SO_LINGER` 13 (Linux's, D23 steps 3-4, branch nfs34,
2026-10-28; recorded by the same audit). The display protocol (DESIGN D38 step 3, `display/proto.h`, version 1): port `display` (the D11 service of
`/sbin/displayd`; a client's own ports `dpy.PID`), requests `DPY_HELLO` 1 .. `DPY_IMAGE_FREE` 11 and the window
manager's `DPY_WM_HELLO` 32 .. `DPY_WM_KILL` 42, events `EV_KEY` 1 .. `EV_MINIMISED` 8 and `EV_WM_NEW` 32 ..
`EV_WM_SIZE` 39, drawing orders `O_CLIP` 1 .. `O_XLINE` 6, and the flags and structs named there (branch desk,
2026-11-04). Port names
registered as services (D11):
`disk`, `fs`, `fs.*`; `ping` (the test service, served by `msgpingd` since D19 step 0); `net` (D19, the network server:
reserved for root like `fs` and `disk`; `socktestd` serves it in the tests until netd).

