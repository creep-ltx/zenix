# The full audit before 1.0, 2026-11-14

Scope: the whole system on main at `c248d4d` (roadmap blocks 1-7): the kernel, the built-in and user servers, the
drivers, libz and the C library, the shell and commands, packages and updates (zup, zpkg, signing), the boot path,
ZINE's kernel side, the toolchain and the docs. Areas the block 6 audit read in full (D40-D44, its
`docs/audit-2026-11-12-block6.md`) and the network audit (`docs/audit-2026-10-28-network.md`) were not read again
unless their code changed. Blocks 4 (the Amiga world) and 5 (the desktop) never had their closing audits; they are
covered here.

Six independent read-only reviewers took the kernel core and its privilege checks, the updates and packages, the
filesystems and archives, the desktop and USB, the claims and docs, and the tests and the real-hardware gap. This
audit read the sizes and earlier audits' open items itself, and re-read every reviewer finding in the source before it
went in (one was narrowed: the zpkg list's expiry, FA-25, holds wherever the clock is trusted). "Verified" names the test or harness that showed it; "read" means from the
source only.

**Result:** 3 high, 12 medium, 28 low, 6 info (docs and tests included), and 35 real-hardware checks (section 6).
Fixed here, each with a test: FA-1, FA-2, FA-4, FA-5, FA-6, FA-8, FA-9, FA-10, FA-14, FA-15, FA-17, FA-18, FA-19,
FA-28, FA-43 to FA-45 and the doc items FA-37 to FA-40. Open:
FA-3 and FA-7 (both the main session's: the Makefile and D35/D52), FA-11 to FA-13, the low and info items listed with
their fixes. **Verdict: not ready for 1.0 yet** (section 5).

## 1. Findings

| Id | Severity | Where | Problem | Fix | Status |
|---|---|---|---|---|---|
| FA-1 | high | `kernel/signal.c:199` sig_setpgid, `kernel/syscall.c:655` SYS_setfg, `kernel/tty.c:149` fg_signal | Any user could join any existing process group (`setpgid(0, G)` checked only that a member exists) and make it the foreground of their own terminal (`setfg`'s group form needed only one member the caller may signal: itself). ^C and ^Z, also `\x03` written to their own pty, then signalled every member unchecked: root's daemons, another user's login session. Read | A group is joined only when the caller may signal every member (`may_signal`, root passes); setfg's group form the same (`sig_group`'s check 2). 68 bytes of kernel.bin with FA-14 (budget: signal 2,100 -> 2,150, the mount tests' RAM disk 677 -> 676 KB) | fixed (test: signal-host-test, five "K1" checks; they fail on the old code) |
| FA-2 | high | `zenixfs/zenixfs.c` ext_floor, f_map_clear_from, free_block, apply_pending | An extent item's block numbers were never range-checked outside fsck: a crafted ZenixFS image with good checksums made the server read the bitmap tables out of bounds (and likely write: `bmfull[i] = 0`), free the superblock and bitmaps for reuse, or loop 4 billion times; an orphan with nlink 0 does it at the first writable mount (the root at boot). The one-block read path also read any block of the volume as file data. Verified (the reviewer's ASan build on the host: heap-buffer-overflow at `bm_get`; the 4-billion loop) | `ext_ok`: an extent read from the tree must be non-empty, inside the data area, its logical range not wrapping, else `ZENIXFS_ECORRUPT`; `free_block` refuses a block outside the data area | fixed (test: `make -C zenixfs test` "crafted-extent", six crafted extents; all six failed on the old code (three read other blocks' data); the corruption lab's table unchanged; `make -C zenixfs asan` clean) |
| FA-3 | high (build) | `Makefile:350` `RAMDISK_INSTALL_HD_K := 3400`, `tools/mkinstall.py` | The install floppies no longer build: the start from AmigaOS carries "the whole set as files" in a 3,400 KB RAM disk, and the set is now 4.0 MB of packages (block 7 added `devel` 0.9 MB and `games` 0.4 MB). `make install-floppies` fails ("the AmigaOS start's RAM disk is too small"), so every zinstall-test job fails. Verified (the build) | A D35 decision: leave `devel`, `games` (and `zenix-tests`) out of the install set (zpkg fetches them later), which also keeps the RAM disk loadable on the 4 MB minimum; raising the number instead would not fit that machine | open (the main session's: Makefile, D35) |
| FA-4 | medium | `user/zup.c` main, `user/zpkg.c` main, `user/sudo.c` | zup and zpkg created their journal, undo logs, `.zup-new` files, cache and lists with the caller's umask, which sudo keeps: after `umask 0`, `sudo zpkg -Syu` left them writable by every user, between zpkg's hash check and zup's install (an unsigned file installed as root, on a machine with D41 protection on). The undo logs of the kept previous set stayed world-writable afterwards. Read; the test shows it | `sys_umask(022)` first thing in both | fixed (test: zup-host-test "an update run with umask 000 ... not even half-way"; the host model now honours the umask and says in its trace whatever is made writable by others; fails on the old code) |
| FA-5 | medium | `display/server/displayd.c` DPY_IMAGE | The request was read from the client's memory again after its checks (`d->depth`, `d->h`, `d->data`): a second process of the same user changing it meanwhile made displayd copy past the image's chip RAM block (a heap overflow in the desktop's server) or copy memory it never checked. Read | The request copied once, then only the copy used | fixed (read; dpy-test and zdesk-test unchanged) |
| FA-6 | medium | `user/tar.c:130` through_link | The "never write through a link this archive made" check compared names with their case; on FFS, FAT or PFS3 `X/f` is `x/f`, so a member `x -> ../../..` then `X/etc/passwd` went out of the target. Also, a link dropped from the list when memory ran out was forgotten silently. Read | Names compared without their case; running out of memory for the list stops tar | fixed (test: users-test "tar x refuses a link's name in another case"; on ext2 the old tar wrote `cl/f`, exit 0) |
| FA-7 | medium | `Makefile` (zenix.hdf's programs), `tools/mkroot.sh:168` installdb | The full disk image carries `/bin/tcc`, `zld`, `zas`, `zdb` but not the devel package's `/usr/lib` and `/usr/include`: tcc there has no C library, the package database does not list `devel`, so zpkg sees four files no package owns (an install of `devel` is refused by zup's ownership rule) and never updates them. zup-host-test fails ("the full root image holds the package database"). Verified | Either the whole devel package on the full image or none of it (zpkg -S devel); tcc-test keeps adding its own copy | fixed 2026-11-18: the whole package (3.2 MB unpacked, 900 KB packed) on `zenix.hdf` and `root-card.img` (`tools/mkroot.sh` stages it, `mkzup.py stage devel`; the build stops if the database lacks it), `zenix.hdf` 16 -> 20 MB; tcc-test uses the image's files and checks they are the build's (test: zup-host-test "the full root image holds the package database", which failed on the old image; tcc-test) |
| FA-8 | medium (tests) | `tools/signal-host-test.c`, `tools/term-test.c`, `tools/dos-host.c` | Three host tests in `make check` no longer linked: D54 added `dbg_held`/`dbg_sigint` to signal.c (2026-11-13), D55 `gcon_hold` to term.c (2026-11-14), the SIGWINCH work `tui_zenix_winch` to dos.c (2026-11-05, twelve days). Nothing they check was checked since. Verified | Their stubs, and a check that no untraced process asks the debugger | fixed (the three pass) |
| FA-9 | medium (updates) | `tools/mkdevel.sh`, `Makefile` BG_CFLAGS | Two builds of the same source made different `devel` and `games` packages: `objcopy --strip-debug` stamps archive members with the build's time (libgcc.a, libc.a), and fortune's `assert` names its source path in the build folder. Every release would ship both as "changed" (1.3 MB to fetch for nothing), and sysupdate-test fails ("V2 changes more than the base group"). Verified (two builds with different version numbers: before, libc.a, libc-pure.a, libgcc.a and fortune differ; after, every package's files are identical) | `objcopy -D`; `-fmacro-prefix-map=$(BG_SRC)/=bsdgames/` | fixed (sysupdate-test b; the two-build comparison) |
| FA-10 | medium | `display/server/displayd.c` DPY_WIN_NEW, `win.c:466` | The same double read: `kind` checked in the client's memory, then read again by win_new: a racing client got the window manager's kinds (a frameless window always in front, a desktop). Read | The request copied once | fixed (read; dpy-test, zdesk-test) |
| FA-11 | medium | `ports/ahiru/upstream/fs/ops.c:664` resolveone | B3-6 is still open: an archive member or ADF folder named `..` pastes into the target's parent. Read | Map `.` and `..` from an archive to `_` names, as `:` and `/` already are | fixed 2026-11-18 as a Zenix patch (`ports/ahiru/zenix/ops.c`: a picked member named `..`, `.` or empty refuses the paste with a message, and the delete from an lha/lzx/zip archive; upstream unchanged: on AmigaDOS `..` is a plain name) (test: ahiru-test's third start, a zip with `../esc.txt`; the old Ahiru hangs at its merge question and the test fails) |
| FA-12 | medium (memory) | `tools/zpkg-emu-test.sh` e, `user/zup.c:1366` | Part e fails on main: on its machine (the A2065 with 2 MB of chip RAM and no fast RAM, best effort since 2026-11-04) `zpkg -Syu` cannot install the 203 KB `zenix-tests` package: "zup: out of memory", rolled back, then `--back` has nothing. zup reads a whole package into memory (up to 16 MB) and needs several times its size with ~760 KB free; block 7's test programs made the package that big. Verified (twice; the transcript) | Either part e on the stock A1200 with 4 MB of fast RAM (the floor; its 2 MB run says where memory runs out, as part m does) or zup streams a package's tar instead of holding it | open (the main session's choice) |
| FA-13 | medium (tests) | `tools/remote-test.sh` s and l, `tools/host/wasabi-remote.py:455` | Fails on main in every run of this audit: part s, `termtest grab` through wasabid never answers (the client's read times out); part l, the 1 MB chip-only machine's daemon does not answer within 500 s (best effort). Verified (three runs) | s: why termtest's grab stalls under wasabid (it changed 2026-11-12: "termtest reads keys one at a time"); l: say where memory runs out instead of waiting | open |
| FA-14 | low | `kernel/dbg.c` DBG_WAIT | The debugger's record was looked up once, before the wait's loop; while the tracer slept, the target could become a system process (a reserved port), which detaches and frees the record; the next wake read freed memory, and the tracer waited for good. Root tracers only. Read | Looked up again after every block (-ESRCH) | fixed (test: zdb-test, dbgcheck's 20th check "a target made a system process while its debugger waits") |
| FA-15 | low | `display/server/displayd.c` card_take | A6-27: the test card's `bpr * height` allocated before `bpr` was checked | `fb_size_ok` in fb.c, used before the allocation and for the record | fixed (test: rtg-host-test, three checks) |
| FA-16 | low | `kernel/vfs.c:1170`, `:274` | `mount` takes any port name; after its server dies the kernel looks the name up again, so a user's port of that name would get the kernel's requests (and could answer FS_OPEN with a set-user-id root file). Root must have mounted a non-`fs.*` port by hand first (mount(8) never does). Read | Refuse a mount whose port is not a reserved name | open (low) |
| FA-17 | low | `user/gzip.c:75` | A long extra field moved the position past the end (`len - pos` wrapped); safe only by accident. Verified (host harness) | The header must leave room for the trailer | fixed (test: cmds-test "gzip-hdr", against GNU gzip) |
| FA-18 | low | `display/show/show.c:417` | A 7-plane HAM ILBM is HAM8: the copy took the control planes from planes 6 and 7, and the file has no plane 7 (a read up to 512 bytes past the picture). Verified (decoder harness) | `pic_plane`: the control planes are the file's top two whatever its depth | fixed (test: show-host-test, the HAM8 plane checks) |
| FA-19 | low | `display/server/displayd.c` DPY_IMAGE | A failed mask allocation kept the image's bytes in the client's budget | Given back | fixed (read) |
| FA-20 | low | `usb/core.c:274`, `:222` | A stick that NAKs everything costs about 22 s per failed read (CBW, reset, two clear-halts, each with its own timeout) where diskd waits 5 s; usbd answers nothing meanwhile. Every wait has a timeout. Read | A deadline for the whole recovery | open |
| FA-21 | low | `display/server/displayd.c:59` NIMG, window slots | One client can take all 48 image slots (within its budget): the window manager's gadgets then fail | A per-client cap, the manager exempt | open |
| FA-22 | low | `boot/zenixboot.c:462-466`, `:748`, `:766` | In the trial phase a `sigdigest` in the unsigned state file that equals the bundle's skips its signature and hash check. With B2-10 (the state file deleted reopens a downgrade) this says BOOT: is trusted; no worse than today (the kernel file itself is never checked at start). Read | Write down that BOOT: is trusted, or check the signature again in the trial | open (with B2-10) |
| FA-23 | low | `user/zpkgfmt.c:68` zf_num | No range check before the casts: uid 65536 becomes 0, mode 01000644 becomes 0644, an overflowing version 0xFFFFFFFF. Signed input only. Verified (reviewer's host harness) | Refuse values over each field's range | open |
| FA-24 | low | `user/zup.c:1212-1224` | A system bundle's install skips `paths_ok`/`path_safe` (no link in a parent folder, not into the database or cache, not on another mount) that live installs run. Signed base packages only: defence in depth. Read | The same checks | open |
| FA-25 | low | `user/zpkg.c:524`, `user/zpkg/index.c:446` | Without a battery clock or network time, "today" is the larger of the build date and `/var/lib/clock`: a mirror can withhold updates past the list's expiry unseen. Read | Say when the clock is untrusted and the list is older than N days | open |
| FA-26 | low | ffsd, pfs3d, exfatd, fatd readdir; `zenixfsd.c:169`, `:316` | A name on the disk that is exactly `.` or `..` is listed (a ghost entry; it cannot be opened). Read | Skip or rename them in each readdir, as the empty name | open |
| FA-27 | low | `kernel/ipc.c:63`, `:78`; `kernel/tty.c:485` | Bare error numbers: `-13` (now `-EACCES`, the same value), `-2` for a full port table (ENOENT's number: a frozen answer, kept), `-1` in tty_read when killed | The `-2` stays (frozen); the tty `-1` to `-EINTR` | partly fixed |
| FA-28 | low (tests) | `tools/check.sh` STOCK | lace-test, zine-input-test and demo-test pair the Amiga with the PC's clock (cage-rig screenshots and keys) but ran in fast mode. (ctop-test's part n, also named by a reviewer, fails at stock speed and passes in fast mode: left out) | Listed in STOCK | fixed (the three pass at stock speed) |
| FA-29 | low (tests) | `tools/tests/zine-null-test.txt:75`, `:83` | `wait $ 20` is a regular expression: `$` matches at once, not at the user's prompt | Wait for the prompt's text | open |
| FA-30 | low (tests) | `tools/demo-test.sh:63`, `:71` | The loading-bar and desktop screenshots are checked only for existing | Compare a region (as dpy-test does) | open |
| FA-31 | low (tests) | 17 host tests (a3000-test ... zpkg-test) | Write into `build/` instead of `ZENIX_BUILD`/`TMPDIR`: two runs in one tree collide (`make check` runs host tests one at a time, so only hand runs) | Their output under `TMPDIR` | open |
| FA-32 | low (docs) | `docs/man/zfsd.txt` | The ZenixFS server has no manual page or commands.md line (its siblings fatd ... usbd have) | Write it | open |
| FA-33 | low (docs) | `docs/commands.md:3` | "each with a manual page ... OPTIONS, EXAMPLES, EXIT STATUS": 75 pages have no EXIT STATUS, 152 no EXAMPLES, 137 no OPTIONS heading | Say what the pages hold, or add the sections | open |
| FA-34 | low (docs) | `docs/man/zld.txt`, `zas.txt`, `zup.txt` | Accepted but undocumented: zld `--stack=N`, `-h`; zas's gas-compatible no-ops (`-g`, `--gdwarf*`, `-W`, ...); zup's internal `-2` | One line each | open |
| FA-35 | low (docs) | `docs/features.md:25`, `:109`, `:130`; `docs/limits.md:59`, `:102`; `docs/d34-zenixfs-root.md:5`, `:134`; d19/d20/d21 "As built" headings | Still say "never on real hardware" for things later run there (D37's cache on the 1260, the ZenixFS root installed on the A1200, the SD card that Zenix booted from, telnet to a BBS) | Bring each to its d-note's record | fixed 2026-11-18 (FA411 and plipbox corrected here, FA-40): features.md (D37's cache on the 1260 with its 4.23 -> 0.95 s, the SD driver verified 2026-10-23/24, the installer's live runs, telnet to a BBS and its downloads 2026-11-03, ZINE's demos with sound 2026-11-03), limits.md (the SD card, the ZenixFS root), d34, d35, d37, ROADMAP, the d20/d21 headings; what is still not run says so (the floppies on a real drive, uploads, a 68040) (read: each against its record; docs-check) |
| FA-43 | medium (build) | `Makefile:1583` `zenix-zroot.hdf`, `tools/zroot-test.sh:19` | The ZenixFS-root disk image (12 MB) no longer holds every program (block 7's tcc, the tests' libctest: "error -28 (no space)"), so `make build/zenix-zroot.hdf` fails, and with it svc-test and zroot-test. Verified | 16 MB | fixed (the image builds: 192 programs; svc-test, zroot-test) |
| FA-44 | low (test) | `tools/sysupdate-test.sh:102` root_img | Part m failed on main: "zup: not enough space on the root: 174 KB and 18 files needed": the test's 16 MB root no longer held the downloads and kept versions beside the full image. Verified | 24 MB | fixed (sysupdate-test m) |
| FA-45 | low (test) | `kernel/main.c:74` stage, `tools/check-init.py:172` | diag-test's `DIAG_PANIC=11` kernel no longer built: with the stage a constant, gcc dropped everything after it as dead, `mmu_disarm` with it, and D45's check refuses a kernel where a name it trusts is missing. Verified | The stage compared through a `volatile` copy (test builds only) | fixed (diag-test) |
| FA-46 | medium (tests) | `tools/prio-test.sh` | Fails on main: "a user may lower their own process with renice" (nice works, root's renice works, raising stays refused). Verified (main and this branch) | To be traced in `user/renice.c` / `do_priority` | fixed 2026-11-18: not the kernel: renice read a process or user id with the niceness's parser, which clamps to -40..40, so pid 46 (the test's sleep; root's was 31) became 40 ("no such process") and `-u 1000` became uid 40; ids now parsed whole (`idnum`) (test: prio-test, the user's renice and a new `renice 6 -u 1000` check, both fail on the old renice) |
| FA-47 | low (tests) | `tools/zroot-card-test.sh` r | Fails on main: `fsck.zenixfs --old` on the card's root never reaches "the root is clean now" in 300 s. Verified (main and this branch) | To be traced | open |
| FA-48 | low (tests) | `tools/tcc-test.sh` | Fails on main: the test's image runs out of room ("no space left on device") with the devel files and the suite on it, and the test suite's 101_cleanup then differs. Verified (main and this branch) | A bigger test image (as FA-43, FA-44); then 101_cleanup again | fixed on main by a5da70d (24 MB, 2026-11-17, after this audit's run); passes on fix10 with FA-7 (107 + 3 programs, 25 checks) |
| FA-49 | low (speed budget) | `tools/conspeed-test.sh` | Fails on main: "020 lsR: the console's part at most 4.8 s" (an emulator figure at stock speed, the budget the console's speed work set) | Look at what grew since the budget; emulator time only | open |
| FA-50 | low (memory) | `tools/slip-test.sh` m | Fails on main: SLIP's run on 1 MB of chip RAM never ends its script (best effort) | Say where memory runs out | open |
| FA-37 | info (docs) | `docs/claims.md:25` | `MEMA_CHIP`: "other bits refused"; the code and frozen-interfaces.md: ignored | Corrected | fixed |
| FA-38 | info (docs) | `docs/claims.md:44`, `kernel/fsproto.h` | `FS_*` 21 was claimed in "1-24" but never defined | Noted as unused, never to be reused | fixed |
| FA-39 | info (docs) | `docs/architecture.md:1437`, `:866`; `docs/commands.md:134` | A doubled heading; "claimed in CLAUDE.md first" (claims.md since 2026-10-28); a doubled line | Corrected | fixed |
| FA-40 | info (docs) | `docs/features.md:96`, `:97`; `docs/ROADMAP.md:151` | The FA411 and the plipbox "never run on a real card/box", where d36 section 8 and d39 section 13 record their runs on the A1200 with the 1260 (2026-11-04) | Corrected | fixed |
| FA-41 | info | `kernel/dbg.c` dbg_new (`NDBG` 4) | The four debugger slots are shared by every user: four processes of one user hold them all (root too then cannot debug) | One per tracer, or root takes one back | open |
| FA-42 | info | `kernel/proc.c:364` | The comment says the relocation table is "never more than half the image"; the bound is `nreloc <= image/2`, so the table is up to twice the image (a 16 MB file: about 16 MB of kernel heap for a moment). Memory-safe | The comment, or read the table in pieces | open |

Info, noted (no change asked): a user's port under an unreserved service name blocks that service's registration
(-EBUSY, no impersonation: `svc_pid`); any client of a desktop session can open a full screen in front and draw a
fake greeter (no secure-attention key; DPY_SESSION(0) at logout drops it); `DPY_FONT_SET` makes displayd wait for
ttf2font (a stall, root-owned fonts only); a cross-linked FAT chain on a writable mount frees another file's clusters
(inside the volume, d30's rules); manifests may carry set-user-id modes (signed packages only; nothing lists them);
`zpkg --reset NAME` truncates a long name (root only); zpkg fetches over plain HTTP only (integrity from the
signatures; a man in the middle can stall a run for hours: D22 step 9); pool `.sig` files are stored unchecked (a bad
one only makes `--back` refuse later). The three host tests `audio-test`, `klog-test` and `joystick-test` are not in
`make check`, so the Zorro board list, `dmesg`'s ring and the mouse in port 1 have no checked test; the battery clock,
ROM Topaz's glyphs and the tick rate have none either (features.md says so).

Found sound (one line each; read unless named): every user pointer through `user_ok_rd/wr` (wrap checked, one area,
shared code read-only), `copy_str`, ZEXE and ZEX2 headers and every relocation checked before any is applied, the
loader's error paths; set-user-id only from the open's stat, never for scripts or under the debugger, NODEBUG set;
`may_signal`, priorities, setuid/setgid/groups; every root-only call; ipc's generations, UMSG_KERNEL never from a user,
posts re-checked; signal frames (even PC, condition codes only); `ev_block` and the tty/pty check-then-block order;
the debugger's `may()` (uid, NODEBUG, pid 0/1, system processes, cycles), PEEK/POKE ranges, POKE into shared code only
with one user, SETREGS; ZINE's root-at-the-console rule and ZINE_MAP; D55's CON_VBLANK, CON_HOLD and GEV_GONE; D49's
readdirs; the signature chain (header digest, table, part hashes, file hashes), the index rules (signature before
parsing, serial, channel, expiry), paths only from manifests, `O_EXCL` and rename, the ownership and takes rules,
zenixboot's checks and cut copies, zinstall's typed confirmations (FAT, 0x76, extended and mounted partitions never
touched), the root-only gates; fatd's BPB and chains (Brent), ffsd's headers, hash chains and rw check, pfs3d and
exfatd read-only with every field bounded, diskd's RDB (128 longs, 32 entries, loops) and partitions, utf16name,
arcread's Huffman tables and names, unlha's per-component lstat and `O_EXCL`, arclzx's window; displayd's Copper
lists from clients (only fixed registers written, every plane fetch inside the client's held chip block), the window
manager and session rules, DPY_DRAW's orders and clipping, the .font and ZCF readers, the ILBM, GIF and TrueType
parsers, dlogin; USB descriptors, transfer lengths, CSW, READ ranges, controller ids, every hardware wait timed; the
claims (about 330 numbers: system calls 1-100, every message, op, flag and error) and limits.md's numbers match the
headers.

## 2. Earlier audits' open items

| Id | Today |
|---|---|
| B2-10 (zenixboot's unsigned state file and downgrades) | open (design); FA-22 is its sibling. Only someone who can write BOOT: can use it, and they can replace the kernel already |
| B3-6 (Ahiru and `..`) | fixed with FA-11 (a Zenix patch) |
| B3-19, B3-20 (text-UI cells, three width tables) | open (accepted) |
| B3-23 (LHA/LZX readers' licences) | **closed in effect** by D33: Ahiru builds Zenix's own readers (`AHIRU_ARC := arcread arclha arclzx`), `core/lha.h` and `lzx.h` are GPL-2 shims, no LHa for UNIX or unlzx code is in the tree; DMS (public domain) stays |
| A6-14 (a 68030 left with AmigaOS's translation on) | open (needs a 68030) |
| A6-17 (the fault line's words) | open |
| A6-23 (ATAPI select after DEVICE RESET while BSY) | open (needs a real drive: checklist item 18) |
| A6-27 | fixed here (FA-15) |

## 3. Tests

`/tmp` had too little room for `make check` when the audit began, and its first run from a clean copy of main filled
`/tmp` with the images of its failed jobs (14 GB) and was stopped. The suite then ran in groups and as a whole under
`~/.cache` with each test's own build copy (tools/check.sh with its run folder there, 3 emulators at a time, the
STOCK tests at stock speed, the rest in fast mode), on this branch after its fixes (2026-11-17, 238 min).

**Result: 154 tests, 140 passed (4,789 checks), 13 failed, 1 skipped.** Every failure was run again on unchanged main:

| Test | On this branch | On main | Why | Id |
|---|---|---|---|---|
| zinstall-test (12 parts) | FAIL | FAIL | the install floppies do not build | FA-3 |
| zup-host-test | FAIL (1 of 111) | FAIL | the full image holds part of devel | FA-7 |
| zpkg-emu-test e | FAIL (2) | FAIL | zup out of memory on 2 MB chip-only | FA-12 |
| remote-test s, l | FAIL | FAIL | termtest's grab through wasabid; the 1 MB boot | FA-13 |
| prio-test | FAIL (1 of 24) | FAIL | a user's renice to lower | FA-46 |
| zroot-card-test r | FAIL | FAIL | fsck.zenixfs --old never ends | FA-47 |
| tcc-test | FAIL (2) | FAIL | its image full | FA-48 |
| conspeed-test | FAIL (1 of 8) | FAIL | over its emulator budget | FA-49 |
| slip-test m | FAIL | FAIL | 1 MB chip-only | FA-50 |
| update-test nozup | FAIL once (the new kernel said nothing after zenixboot loaded it) | PASS | passed on this branch when run again (90 checks): a one-off, to watch | - |
| ctop-test n | FAIL at stock speed | PASS (fast) | the STOCK entry taken back (FA-28); passes on this branch again | - |
| telnet-test, bbs-test z | FAIL | FAIL | lrzsz is not installed on this PC since its reboot (`pacman -S lrzsz`) | (the PC) |
| stage0-68k-test | SKIPPED | SKIPPED | the Python module unicorn is missing on this PC | (the PC) |

Fixed by this audit and passing now (they failed on main): signal-host-test, term-test, dos-host-test (FA-8),
zroot-test and svc-test (FA-43), sysupdate-test b and m (FA-9, FA-44), diag-test (FA-45). The cage rig's screenshot
tests need a desktop session or, as here with none running, `WAYLAND_DISPLAY` set to a name no session uses (the
rig refuses a cage whose display has the user's name; it is headless either way).

## 4. Sizes, and what could leave the always-loaded path

Measured (`make`): `kernel.bin` 205,992 bytes on main at c248d4d, 206,060 with this audit's fixes (206,192 with main's later D41 work merged, budget 206,200; 840 bytes left on
the mount tests' floppy, ROOM 800). Resident (loaded and kept on every machine) by part, from the objects' sections:
the embedded servers 42,144 (fsd 29,404 and diskd 12,740 as files), term 14,488, vfs 11,735, proc 10,483, syscall
10,296, gfx 7,749 ... dbg 4,030, gcon 4,153, emmc 2,515, a3000 1,670. Already given back after the boot (D45): 25,984
(boot), 752 (CD, without a drive), 2,928 (MMU code, protection off).

| # | Proposal | Saves | On | Cost and risk |
|---|---|---|---|---|
| 1 | **The embedded fsd and diskd run from the kernel's own copy.** Today the kernel keeps both files (for a restart without a filesystem) *and* copies them into a new block when they start: their code is in RAM twice. Built as D13 pure programs (code shared, only data and bss per start), the copy in the image would be the running code. | about 36 KB of RAM (fsd's code 25,706 + diskd's 10,671), every machine | every machine | a deep core change (the loader, relocation of the image in place once, D45's spans): a design note and two reviews |
| 2 | **The debugger's code in a D45 span**, kept only when asked (`zenixboot DEBUG=ON`, or a root call before the first DBG_ATTACH that keeps it): dbg.o is 4,030 bytes, its 24 hooks elsewhere stay as cheap tests | about 4 KB | every machine where nobody debugs | the MMU span's pattern (one gate table, the build's check); zdb needs the option |
| 3 | **Hardware spans for the PiStorm's SD driver and the A3000's SCSI**, kept only when their hardware answered at boot, as the CD's | emmc 2,515 + a3000 1,670 = about 4.2 KB | every machine but those two | small, the CD span's pattern |
| 4 | **The RTG part of displayd loaded only with a card**: the floppy already uses `displayd-nortg`; the disk image's displayd carries fb.c and the chunky paths (5.3 KB) on every desktop | about 5 KB per desktop | AGA/ECS machines from a disk | small: two programs, displayd chooses at start (or the installer installs one) |
| 5 | gcon (4,153, the desktop's grab of a console) | - | - | keep: the desktop is the default experience |

USB costs the kernel only its `drv.c` gate and stage0's boot-only lookup (in the boot span); usbd and exfatd are
programs started on demand. CD: done (D45).

Recommended order: 3 and 4 (small), then 2; 1 as one design note when RAM next matters (it is the biggest single gain
on the stock A1200).

## 5. Ready for 1.0?

**Not yet.** Must be done first:
1. FA-3: the install floppies build again (a D35 choice of what the install set holds).
2. FA-7: the full image either holds the devel package whole or not at all.
3. `make check` all green: the nine tests that fail on main (section 3: FA-12, FA-13, FA-46 to FA-50) fixed or
   their machines' limits written into the tests; it was not green on main for at least twelve days (FA-8), so a
   full run before each merge, and its table read, must be the rule again.
4. FA-11 (Ahiru and `..`): a file manager on 1.0 must not paste outside its target.
5. The real-hardware checklist's items marked **(1.0)** (section 6).
6. FA-35's doc statements brought in line (1.0's documents must say what was run where).

Should be done (low risk, small): FA-16, FA-20, FA-23, FA-24, FA-26, FA-29, FA-30, FA-32 to FA-34. May wait: the
size proposals, FA-21, FA-25, FA-41, FA-42, A6-14/17/23.

## 6. The real-hardware checklist (for the partner's A1200 sessions before 1.0)

Each item names the d-note section that says how to check it. **(1.0)**: needed before the release.

**The stock A1200 (68EC020, 2 MB chip; and with 4 MB fast, the minimum)**
1. **(1.0)** The DD demo floppy on a real drive (or Gotek), the floppy driver's step 0: d25 section 10 (T13), limits.md "floppy".
2. **(1.0)** Free memory with one login and the desktop on the minimum (4 MB fast): d28 section 13, d18 (the diet).
3. The 4-plane FMODE 3 console: d14 section 8.
4. The console's speed and `ls -laR /` figures on the screen and to /dev/null: d28 sections 13 and 21, ROADMAP "The real-world benchmark".
5. SLIP at 115,200 baud while the display draws: d19 section 8.3.
6. Crypto times (cryptotest, ssh, wget/sntp over HTTPS): d21 section 11.3, a1200-checklist-network.md sections 10-11.
7. `zpkg -Syu` times and an FFS volume written by zpkg: d22 section 7.3.
8. NFS speeds: d23 section 9.3.
9. Remote control end to end, AmigaOS to Zenix and back: d24 sections 8 and 12.3.
10. The ext2 clean-mark check on a repaired /home: d17 section 6.
11. The service manager's failure cases: d11 section 9.
12. The toolchain's first times (Lua, make, zld, zas, tcc, zdb, the games): d52 (each tool's "first run" line), d54 section on single steps, d53.
13. **(1.0)** The desktop's newer parts: `files` (d48), `show` (d50), TrueType (d51 section 8), screens and zintro (d55, the OCS/ECS values read from the manual).
14. **(1.0)** Sound and the MOD player heard, the halves against a floppy's track reads: d40 sections 6 and 13; the bell (features.md).

**The A1200 with the Apollo 1260 (68060, CF card, FA411)**
15. **(1.0)** Memory protection `zenixboot MMU=ON`: d41 section 18 (also never on a real 68030 or 68040).
16. Faster IDE (the boot line ends "long reads"): d42 section 6.
17. D45's give-back on the 060: d45 section 10.
18. **(1.0)** The installer on a real disk or card from the floppies (after FA-3): d35 section 9 (features.md and d35 now both say: run on the A1200 2026-11-03/04, not from the floppies; FA-35).
19. A CD drive as the IDE slave, and A6-23's reset: d44 section 9.
20. ZINE's 060 stand-ins (missing instructions, CAAR): d29 16.14-16.19.
21. The FA411's TCP transfer and a BBS over it: d36 section 8 (steps 1-4 done).
22. The plipbox's poll interval and speed rerun: d39 section 13 "The next run".

**The PiStorm A1200 (Emu68, CM4)**
23. The CM4's GENET Ethernet, the whole list: a1200-checklist-network.md sections 1-9.
24. Stage0's stop and the device-tree table with D45: d45 section 10.
25. The SD card driver (`emmc.c`): limits.md and features.md now agree (Zenix's own driver, verified on the A1200 2026-10-23/24, FA-35); left: its speed and card-detect: d34, limits.md.
26. USB sticks (`usbd`, steps U1-U3): d46 section 6.2.
27. exFAT on the partner's stick: d47 section 9.
28. RTG (`zenixboot RTG`, the desktop on the Picasso96 screen): d43 section 11.
29. Emu68's code-cache flushes after loads, and zdb's single steps on Emu68: features.md, d54.
30. ZINE: Bacon of Hope: d29 16.5.

**Not among the partner's machines (say so in 1.0's notes)**
31. The A3000's SCSI (WD33C93A, DMAC): features.md, limits.md.
32. A 68040 (D37's cache, ZINE): d37, d29 16.15.
33. ATA FLUSH CACHE on real drives: features.md.
34. The battery clock chips (MSM6242, RP5C01): features.md.
35. The 100 Hz tick against the real E-clock: features.md.

35 items, 9 of them for 1.0.

Established 2026-11-14 to 2026-11-17: six reviewers' reading and host harnesses, this audit's reading and re-checks,
and the tests named per item.
