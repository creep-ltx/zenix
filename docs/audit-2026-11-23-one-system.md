# The "one system" audit, 2026-11-23

The partner's request (2026-11-23): "Do an audit of everything. Especially performance/memory and we want everything
to be consistent, like it's ONE system." Scope: main at `e8f9298`. Two parts: **A**, memory and speed against
CLAUDE.md's golden path, measured; **B**, consistency across every program and server. The correctness and security
work of the full audit (`audit-2026-11-14-full.md`) was not redone; its size proposals are measured again here.

How it was done: one reviewer measured memory and speed in FS-UAE (Kickstart 3.1, cycle-exact, time taken with the
E-clock inside Zenix) and read the kernel's and programs' sizes from the build; one swept every built program's
command line, messages and manual page by script; this audit read the keys, configuration, text-UI, network and
package code itself and re-read every reviewer finding in the source before it went in (three of the sweep's were
dropped: `reboot`'s usage, `docs/security.md` and `docs/ctop.md`, which name files of other repositories). **Every
time and memory figure is an emulator figure** (FS-UAE); ratios carry the weight, nothing here was run on a real
Amiga. "Measured" names an FS-UAE run, "read" the source or the build.

**Result:** 42 findings: 1 high (memory), 10 medium, 25 low, 6 info. Fixed here, each with a test where behaviour
changed: OS-15 (in part), OS-21 to OS-25 (in part), OS-27 (in part), OS-39, OS-40, OS-41 (in part). No speed
regression was found: every hot-path figure is within 0-7% of its earliest record (section 4). Free memory on the
stock A1200 is 54 KB below its best (OS-1); four small fixes win it back (section 2).

## 1. Findings

### Memory (A)

| Id | Severity | Where | Problem (measured cost) | Fix | Status |
|---|---|---|---|---|---|
| OS-1 | high (memory) | the kernel, `CLAUDE.md:25`, `docs/features.md:37`, `:50` | Free memory with one login on the stock A1200 (2 MB chip, no fast): **1,311 KB** today; **1,365 KB** at `49aa994` (2026-10-27, the figure features.md gives, booted again: the same method gives exactly 1,365), 1,316 at `c248d4d` (11-14). 54 KB lost, 49 of them 10-27 -> 11-14, almost all in the kernel: it takes 89 KB more at boot and gives 43 KB back (D45). By part (resident, KB): term +7.8, proc +6.2 (bss 15.6 -> 21.4: NREG 16, groups, cwd 256 bytes x 32 slots), sock +5.6, gcon +4.3, drv +4.2, dbg +4.0, tty +3.8, random +3.6, vfs +3.3, gfx +3.3, confont +3.0, ipc +2.8, zine +1.8, the MMU's resident part +1.5, syscall +1.5; start, emmc, main, rtc, serial about -8.7 (into the boot spans). Processes +9 KB (sh's shared code 80 -> 89, init 15 -> 20). CLAUDE.md's "1,132 KB" is older still and wrong today. Measured | The documents: CLAUDE.md and features.md to say 1,311 KB (2026-11-23, FS-UAE) with this audit as the source (the main session's files). The memory back: OS-3 to OS-6 (section 2: about +62 KB, above the 10-27 figure) | open |
| OS-2 | medium (memory) | `kernel/embed.S`, `kernel/proc.c` loader | fsd's and diskd's code in RAM twice (the full audit's proposal 1, still open): the kernel keeps their images (rodata blobs 29,428 + 13,732 bytes) and each running server is a copy. **37.3 KB** (fsd text 25,730 + diskd 11,559), every machine. Read (the build) | Run them from the kernel's image as D13 pure segments: a design note and two reviews (the loader). 16-24 h | open; **the next step** (2026-11-24, not started: the loader's deep change needs its design note and two reviews first). What the note must settle, read from the source: the built-in images are ZEXE files in rodata (`kernel/embed.S`) that `proc.c` loads by copying and relocating as any program; a pure segment (D13, `seg.c`) needs ZEX2's split of code and data (compiled through `tools/purecc.sh`, as sh), so fsd and diskd would be built as ZEX2; the kernel's rodata copy then *is* the segment (no `seg_sum` over a file: it cannot change), its pages read-only with protection on (D60: a server's tree maps the kernel's rodata pages, not a copy), and an update (D15) that replaces the kernel replaces them together. Gain 37 KB on every machine; cost the ZEX2 build of the two servers and the loader's new source of a segment |
| OS-3 | medium (memory) | `kernel/gfx.c:881` | The console's display ring keeps spare rows for fast scrolling (D28 20): without fast RAM half a screen (**30 KB** of the 90 KB ring, measured block 92,672 bytes); with fast RAM a whole screen (82 KB of chip RAM). Measured | No spare rows without fast RAM, or fewer; conspeed-test before and after (the scroll's speed is the trade). 2-3 h | **fixed, in part** (branch wins): 8 spare rows without fast RAM (`CHIP_SPARE`), +15 KB. Measured on the stock A1200 (FS-UAE, conspeed.sh's outputs, on the screen): 16 rows seq 3.63 s, long 4.75, colour 8.11, `ls -laR /` 18.2; 8 rows 3.65, 4.74, 8.04, 19.6 (+8%); 4 rows long 6.6, ls 37.7; none long 29.3, ls 48.0. None would be the full 30 KB, at 2-6 times the time a screen of text takes: not taken |
| OS-4 | medium (memory) | `kernel/term.c:34` HLINES 200, `:402` | Scrollback: 200 lines, **31.3 KB per console used** (console 1's block 32,016 bytes). Measured | 100 lines without fast RAM: 16 KB per console. 1 h | **fixed** (branch wins): `hlines` 100 without fast RAM, 200 with (test: term-test) |
| OS-5 | medium (memory) | `user/init.c:131` font_setup ("auto"), `kernel/term.c:1555` | The Unicode console font is loaded at every boot on every machine: **10.2 KB** (block 10,448), used only by text above U+00FF. Measured | Load it at the first such character, or `auto` = none without fast RAM. 3 h | **fixed** (branch wins): without fast RAM init gives `CON_UFONT` a3 `CONUF_LAZY`; the first cell above U+00FF to be drawn asks init (its wait's `ST_CONSOLE` + 0), which loads it then (tests: term-test, con-test's first step: 0 KB for a plain line, 10 KB at the first box line) |
| OS-6 | low (memory) | `kernel/kernel.h:536` `cwd[FS_PATHMAX]` | 256 bytes in each of the 32 process slots: 8 KB, about 6-7 KB of it unused. Read | The working directory as an allocated string. 3 h, kernel | **fixed** (branch wins): a counted heap string shared by the processes that inherited it, the built-in `"/"` uncounted; proc's bss 21,397 -> 13,333 bytes |
| OS-7 | low (memory) | dbg 4.0, gcon 4.3, emmc 2.5, a3000 1.7, zine 1.8, sock + drv 8.1 KB | Code only some machines use, resident on all (the full audit's proposals 2 and 3, open). Read | D45 spans: dbg (kept when `DEBUG=ON` or asked), emmc and a3000 (kept when their hardware answers). About 8 KB. 2-6 h each | open |
| OS-8 | low (memory) | pid 1 `boot` | boot stays after the boot (13 KB + its 4 KB kernel stack) for its service registrations. Measured | Its job into init or the kernel. 8 h+, design | open |
| OS-9 | low (docs/tools) | `kernel/proc.c:209` KSTACK, `user/ps.c` | `ps`'s MEM leaves out the 4 KB kernel stack each process owns (28 KB at one login): what users add up is less than what is used. Measured (the heap dump) | Count it in MEM, or a column. 1 h | open |
| OS-10 | info (memory) | every program | Libraries copied into every program: libz 1,016 KB on disk over 202 programs (about 5 KB each), libc 516 KB over 103, crypto 364 KB over 26, tui 74 KB over 10 (12.5 KB each in ahiru, markdown, sumi, maki, ctop). In RAM at one login about 14 KB of libz is duplicated (boot, diskd, fsd, init, sh); a desktop adds displayd, zdesk, term and dlogin's copies (26 KB). Read | A shared library segment (D13's second half): a design of its own, 40 h+. Not worth it for RAM alone today | open (proposal) |
| OS-11 | low (memory) | `Makefile:1530` (the ZEX2 list) | ZEX2 shares code only while two copies run (the segment goes at its last exit). Today: sh, login, ssh, ahiru, markdown, sumi, maki, zas, ctop, make, zld. **term** runs once per desktop terminal window: about 30 KB of code each, unshared; less, edit and man run on several consoles. Not worth it: ls, cat, grep. Read | term as ZEX2 (2 h, about 30 KB per extra window); then less, edit, man | open |
| OS-12 | low (size, one way) | `user/wget.c`, `user/tlspipe.c`, `Makefile:780` | Two TLS paths: wget links the crypto library (121,636 bytes) while irc and mail use `/lib/tls/tlspipe` (107,352). Read (the build) | wget over tlspipe as irc and mail (it then depends on the tls package): about 100 KB less on disk, one TLS code path to update. 4-6 h | **fixed** (branch onesys): wget runs `/lib/tls/tlspipe` (its optional third argument the ALPN, "http/1.1"; its error line names the kind: certificate, network, TLS) as irc and mail do; wget 121,636 -> 26,048 bytes; the package wget depends on tls (test: wget-test) |

### Speed (A)

| Id | Severity | Where | Problem (measured cost) | Fix | Status |
|---|---|---|---|---|---|
| OS-13 | medium (speed) | `kernel/vfs.c:87` pipe_read, `:116` pipe_write; `kernel/kernel.h:364` | Pipes copy one byte at a time with a modulo per byte through a 2 KB buffer: **65 KB/s on the 030, 102 KB/s on the 060** (protection off), against 801 / 1,419 KB/s for a 16 KB file read. Every shell pipeline pays. Measured (bench), read | memcpy in at most two pieces per wrap; a 4-8 KB buffer. 1-2 h, kernel, low risk; pipelines several times faster | **fixed** (branch wins): a 4 KB ring copied by memcpy in at most two pieces (under 16 bytes byte by byte, so a round trip is not slower), memcpy aligning the destination and reading the source at any address; `bench mmu`'s pipe (FS-UAE) on the stock A1200 44 -> 174 KB/s, the 030 with protection on 58 -> 184 KB/s, pipetrip the same (1,751 -> 1,732 us, 1,039 -> 1,040 us); kernel.bin + 32 bytes (test: cmds2-test's pipesum) |
| OS-14 | medium (speed) | `user/sh.c:2424` (and `:2185`, `:2395`), `kernel/seg.c:45`, `:159` | `$(...)` starts a whole `/bin/sh -c` even for a builtin: **91 ms per substitution** on the 020 + 4 MB, 140 ms chip-only (`sh -c true` 107 / 137 ms). Every reuse of shared code checks the whole code (`seg_sum`): for sh 92 KB, about 35 ms on a 14 MHz 020 (counted from the loop, not measured). Measured, read | Builtin-only substitutions inside the shell; the shared code's check made lazy (D13's rule kept: before the first run after a write). 6-8 h, medium | open |
| OS-15 | low (speed) | `user/wc.c:7`, `user/tee.c:12`, `user/tr.c:39`, `user/tar.c:51`, `:204`, dmesg; `user/libz.c:285` rd_getline | 512-byte reads: each call is a round trip to the filesystem server: `wc -l` on 96 KB 0.72 s (133 KB/s). libz's line reader goes a character at a time through a 2 KB buffer: `grep -c` on 96 KB 1.41 s. Measured | 4 KB reads; rd_getline with memchr. 2 h | **fixed for wc, tee and tr** (4 KB a call); tar (its 512-byte records), dmesg and rd_getline open |
| OS-16 | low (speed) | `kernel/start.S` trap0_entry, `kernel/syscall.c` | A null system call costs about 600 cycles (43 us on the 020 + 4 MB, 80 us chip-only): every register saved and restored, the stack-guard and signal checks on every call, the big switch's prologue. Read, not profiled | Profile, then a short path for the trivial calls (the checks stay: CLAUDE.md "Safety stays") | open |
| OS-17 | info (benches) | `user/bench.c` "read /bin/sh, 4K chunks, cached"; `kernel/ext2.c:130` | Not cached: /bin/sh is 96 KB and fsd's cache 24 or 48 KB, so the LRU throws everything out on each pass. The item measures disk reads under its name. A sequential read also empties the cache for everyone. Measured | Rename the item or read a file that fits; a scan-resistant cache (a big sequential read does not evict the directory and inode blocks) | open |
| OS-18 | info (D60) | `kernel/mmu.c:1990` mmu_switch_i, `:303` mmu_root, `:1039` win_make_i, `:1009` win_drop_i, `:508` tflush; `kernel/ipc.c:439` | Why small reads cost +36% (030) / +41% (060) with protection on, read 4 KB (measured again today: x1.36 / x1.41, msgcall x1.53 / x1.74, unchanged since D60 stage 3): two root switches per request (the 030's CRP load flushes the ATC; the 060's URP + PFLUSHAN), both sides' ATC refilled; ipc_recv's `give` copies the record and sorts each buffer argument; a window per buffer page (user_ok, dget/dset into fsd's tree, descriptor pushes on the 040/060, part pages bounced); the window dropped at the reply with a flush (on the 030 a full PFLUSHA: no global entries). `stat` lends nothing and stays at +11-14%. Read | D60 stages 4 and 6 (running on branch d60s4): not touched here | for D60 |
| OS-19 | info | the hot paths | No regression: null call 1.02-1.07x its first record (d49, d41), message round trip 1.04x, stat 0.92-0.94x, readdir 1.00x, the console 1.01-1.03x (d28 21), `ls -laR /` the same per line on screen and 0.58x to /dev/null (D49). Chip-only is 1.7-2x slower than with fast RAM on every item: the hardware. Measured | - | - |

### Commands (B)

| Id | Severity | Where | Problem | Fix | Status |
|---|---|---|---|---|---|
| OS-20 | medium (one system) | every program | No program takes `--help` (zld's prints usage with status 1) and only awk `--version`. `-h` is not free: `ls -h`, `du -h` (sizes), `reboot -h` (halt). `-v` is verbose in about 15 programs, the volume in play and modplay. Read (the sweep's table, 190 programs) | Section 3's rule: `--help` prints the usage to the output and exits 0; `--version` prints `NAME VERSION` (the system's). One libz helper called first in main, `zopt_std(argc, argv, usage)`; a shared-file change (libz) for the main session, then every program in one sweep, cmds-test checking each. About 6 h | **fixed** (branch onesys): `zopt` (libz, the C library) first in every main; the ports through `ports/common/zmain.c` (their init array, upstream unchanged); zld and zas (built on the PC too) by hand. Test: `rootfs/tests/conv.sh` runs every program in /bin with --help, --version and --bogus |
| OS-21 | low | `user/fdisk.c:177`, `:223`, `:258`, `fsck_ext2.c:645`, `fsck_zenixfs.c:58`, `mkfs_ext2.c:61`, `mkfs_zenixfs.c:21`, `mount.c:312`, `umount.c:7`, `zup.c:1545`, `zpkg.c:1547`; the servers fatd, ffsd, pfs3d, iso9660d, exfatd, nfscd, fsd, zenixfsd | Usage lines on the output, not the error stream; the disk tools (fdisk 26 sites, mount 27, fsck.zenixfs 16, umount 4, mkfs.zenixfs 6, zup 5) print their errors there too | Usage to the error stream; then the tools' errors (each site read: some lines are reports, not errors) | **usage fixed** for the eleven commands; umount's errors to the error stream (branch onesys); the other tools' errors and the servers' lines open |
| OS-22 | low | `user/man.c:44`, `user/less.c:199`, `user/sudo.c:13`, `user/zld.c:1831` | Usage with status 1 where the rest use 2; `man -k` with no word ended with status 1 and no message | Status 2, `man -k` says its usage | **fixed** for man, less, sudo; zld (branch onesys: status 2, "zld: unknown option X", --help to the output) |
| OS-23 | low | `user/chmod.c:34`, `:37`, `rm.c:9`, `mkdir.c:16`, `rmdir.c:9`, `stat.c:9`, `realpath.c:14` | Their own reason texts ("not found", "exists", "not empty", "cannot change") instead of the system's ("no such file or directory", "file exists", "directory not empty"), so one error reads two ways | `zstrerror` of the call's error | **fixed** (test: the conventions script; mounttest's stat line updated). readlink stays silent on a non-link, as GNU's |
| OS-24 | low | `user/wc.c:29`, `touch.c:9`, `tee.c:7`, `hexdump.c:6` | Unknown options taken silently (wc ignored the letter; touch, tee and hexdump took `-x` as a file name). tee also dropped files past 8 without a word and ended with 0 when a file could not be written (CLAUDE.md: a limit that is hit says so) | `PROG: unknown option` and the usage, status 2; `--` ends the options; tee refuses a ninth file and ends with 1 on a failed open | **fixed** (test: the conventions script; pages updated) |
| OS-25 | low | `user/fsck_ext2.c:645`, `sed.c:228`, `cut.c:28`, `kill.c:28`, `mount.c:313` | Usage lines without options the code takes (fsck.ext2 `-n`, sed `-r`, cut `-b`, kill `-l`); mount's said `mount -a  mount (list)` | Completed; `mount -a` says it mounts /etc/fstab | **fixed** (read; the pages already had them) |
| OS-26 | low | about 45 option loops; `user/cat.c:25`, `du.c:32`, `tr.c:32`, `tree.c:45`, `uniq.c:15` ... | Every program parses its options by hand (no getopt): combined flags in about 25, `--` honoured in about 20 and missing in about 45; a letter loop reads `--help` as "unknown option -" (ls.c:182 does it right) | With OS-20's helper: `--` and `--LONG` handled once | **fixed** (branch onesys): libz's `zoptget` (getopt), `zisopt` (a hand loop's test: `--` skipped, `--word` refused) and `zoperands` (no options) in every option loop; an unknown option says "NAME: unknown option X" and the usage, status 2 |

### Text UIs, keys and settings (B)

| Id | Severity | Where | Problem | Fix | Status |
|---|---|---|---|---|---|
| OS-27 | low | default keys: `user/modplay.c:62`, `user/mail.c:594`, `display/files/files.c:1272`, `display/show/show.c:47`, `user/irc.c` defkeys, `user/less.c:161`, `user/edit.c:697` | Help on different keys: modplay `?` F1, mail and files and show `?` Help, edit F1, less `h` (its `?` is a search), irc none (F1-F10 are its windows) | `?` + F1 + Help in every keys-driven program where `?` is not text | **fixed** for modplay and mail; files and show (`?`, F1, Help; branch onesys); irc: Help open (F1 stays window 1) |
| OS-28 | low | the same tables | Quit differs: modplay and show `q` and Esc, files `q`, mail `q` back and `Q` quit, edit Ctrl+Q, less `q`, irc `/quit`; Ctrl-C quits in modplay only by rule ("Ctrl-C always quits") | Section 3: `q` quits (or goes back one level, then quits at the top), Esc cancels the prompt or goes back, Ctrl-C always quits a viewer; an editor asks before losing work | show: Ctrl-C quits (branch onesys); the others open (each program's keys file) |
| OS-29 | medium (one system) | `ports/common/dos.c:82-120` (confpath), the ports' `ENVARC:`/`PROGDIR:` names | Keys and settings in five places: native programs `~/.config/NAME/keys`; Sumi, Maki, Markdown `~/.config/Sumi/Sumi.keys` (capitals, the AmigaOS names); Ahiru `~/.config/Ahiru/Ahiru.keys`; ctop `~/.config/ctop.keys` (no folder) | dos.c maps `ENVARC:Sumi/Sumi.keys` to `~/.config/sumi/keys` (and .prefs, .places to `prefs`, `places`), ctop's into `~/.config/ctop/`; an old file found is moved once with a line saying so. 3-4 h; upstream untouched | **fixed** (branch onesys): dos.c maps ENVARC:/PROGDIR: to `~/.config/NAME/` lowercase, the file named for what it holds (sumi/keys, ctop/keys, ahiru/theme, filecolours); a file at its old place is moved once with a line saying so (test: dos-host-test; the ports' emulator tests) |
| OS-30 | low (one way) | `display/lib/keys.c`, `display/zdesk/conf.c:191` key_line, `ports/{sumi,maki,markdown}/upstream/src/keys.c`, `ports/ahiru/upstream/ahiru/src/keys.c`, `ports/ctop/upstream/src/ctop.c:469` | Seven keys parsers. Five read the same format (Ahiru.keys's, the ports' upstream code); zdesk's global keys use another syntax: `lamiga+shift+return terminal` where the rest write `<S-Return>`, `<C-q>` | zdesk's names added to keys.c as `<A-x>` (left Amiga), `<RA-x>`, `<M-x>` (Alt), read by keys.c, zdesk's old syntax still read (frozen: an /etc format). The ports keep upstream's parser (the partner's libraries; same format) | **fixed** (branch onesys): keys.c's `keys_mods` reads `<A-x>`, `<RA-x>`, `<M-x>`, `<S-x>`, `<C-x>`; zdesk reads its keys through it, the older `lamiga+...` still read; /etc/zdesk/keys in the new names. The ports keep upstream's parser (the same format) |
| OS-31 | low | `display/files/files.c:1243`, `display/show/show.c:31` against `user/modplay.c:60`, `irc.c`, `mail.c:590` | The default keys files read differently: files and show use `;` comments and `[main]`, modplay, irc and mail `#` and no section (the parser takes both). Only modplay, irc and mail print their keys with `-K` | `#` and no section in all (as every /etc file); `-K` in files and show too (a new option: the partner's yes) | **fixed** (branch onesys): `#` comments and no section in files' and show's built-in keys; `files -K`, `show -K`; Sumi, Maki, Markdown, Ahiru and ctop answer -K too (their own table) |
| OS-32 | medium (one system) | `user/edit.c:15` SCR_COLS 80, `user/less.c:47`, `:195`, `user/modplay.c:697` | edit draws 80 columns whatever the terminal, less wraps at 80 and reads the rows once, modplay reads the size once; none follow SIGWINCH. In a desktop terminal window narrower or wider than 80 they draw wrong; irc, mail, the tui ports and termtest follow resizes | The size from the terminal and a redraw on SIGWINCH, through one libz call (OS-33) | **fixed** (branch onesys): edit (its cell rows made for the size), less (laid out again) and modplay follow SIGWINCH through libz's screen layer |
| OS-33 | medium (one way) | `user/irc.c:24-44` and `user/mail.c:28-55` (putn, putf, at, putw: the same code), `user/modplay.c:131` at, `user/edit.c:330` (its own shadow screen), `ports/bsdgames/zenix/curses.c` (546 lines), `ports/tui` | Five screen layers: irc's copied into mail, modplay's own, edit's shadow grid, the games' curses, tui (the ports). Already noted: B3-19, B3-20 (cells, width tables) | One small libz screen layer (an output buffer, `at`, a cut to columns, a reverse status line, the size and SIGWINCH) for irc, mail, modplay, less and edit; the games' curses over it. Native first: 1-2 days; curses after | **fixed** (branch onesys) but tui: libz's `zscr` (user/zscr.c, the C library's too: the size, SIGWINCH, the cursor, the reverse lines) under irc, mail, modplay, less, edit and the games' curses; tui is the partner's library (ports/tui/upstream), left as it is |
| OS-34 | low (one way) | `user/sh.c` (the line editor), `user/irc.c` input line, `user/less.c` prompt_line, `user/mail.c` prompts, `display/files/files.c:1175` the name field, `user/edit.c` ask | Six line inputs; sh's and irc's have history and the same Emacs keys, the others a subset each | One libz line editor (sh's keys, history optional) for sh, irc and the prompts; 2 days | **in part** (branch onesys): libz's `zled` (the editing keys) under sh's editing keys, irc's input row, mail's, less's and edit's prompts and files' typed line; the shell's suggestion, Tab, history and Ctrl-R and irc's history ring stay their own |
| OS-35 | low (one way) | `user/date.c:16`, `ls.c:13`, `cal.c:11`, `sntp.c:23`, `unlha.c:114`, `display/files/files.c:316`, `zpkg.c:506`, `arclha.c`, `fatd.c`, `ffsd.c`, `dlogin.c`, `zdesk/bar.c`, `tlstrust.c`, `zine/*`, `zinstall.c` | Seconds-to-date in 16 files (three algorithms: a year loop, the civil-from-days formula, BearSSL's day number) | `zdate(t, &y, &m, &d, &h, &mi, &s)` and `zdays(y, m, d)` in libz (the shared-file rule: its own commit) | **fixed** (branch onesys): `zdate`/`zdays` (user/zdate.h, the PC tests' too) in date, ls, cal, sntp, files, zpkg, unlha, tlstrust, fatd, exfatd, iso9660d; dlogin, zdesk's bar, ffsd, arclha and zinstall only split seconds (no calendar); ZINE's blob keeps its own (it runs without libz) |
| OS-36 | low (one way) | `user/irc.c:700`, `mail.c:814`, `zpkg.c:59`, `zup.c:92`, `zpw.c:36`, `ssh.c:129`, `ssh-keygen.c:34`, `nfscd.c:193`, `conprefs.c:56`, `zine.c:48`, `gzip.c:32`, `net/netd/main.c:412`, `display/zdesk/conf.c:39`, `display/server/font.c:99`, `display/ttf/ttf2font.c:25` | A whole file into memory: 15 copies, each with its own cap and message | `zslurp(path, max, &len)` in libz (the writing side is already one: `user/cfgfile.c`) | **fixed** (branch onesys): `zslurp`/`zslurpfd` in irc, mail, zpkg, zup, ssh, ssh-keygen, zine, gzip, zdesk, displayd's fonts, ttf2font, pw_slurp; nfscd and netd keep their own (the region heap, sys_memalloc), conprefs reads into a fixed buffer |
| OS-37 | info | `user/sh.c:293`, `:348`, `display/files/files.c:277`, `dlogin.c:326`, `term.c:66`, `libdisplay.c:197`, `exfatd.c:361`, `iso9660d.c:297` | UTF-8 stepping written inline where libz's `zu8prev`/`zu8next` exist | Use them | open |
| OS-38 | low (one system) | `user/zpkg.c:32` 15 s, `wget.c:15` 60 s, `irc.c:18` 60 s, `mail.c:14` 60 s; `telnet.c:735`, `ssh.c:426`, `sntp.c:72` (blocking connect: lwIP's 6 SYN tries, over a minute) | Four ways to time a connection and four limits. Names: one way (getaddrinfo -> netd), good; no proxy anywhere (one behaviour) | One `zconnect(host, port, ms)` in libz (socket, non-blocking connect, the wait, the message) with one limit (30 s) for every network program | **fixed** (branch onesys): `zconnect` (socket.h; 30 s) in telnet, ssh, wget, irc, mail (begin/end in their loops), sntp (a datagram socket) and zpkg |

### Packages and documents (B)

| Id | Severity | Where | Problem | Fix | Status |
|---|---|---|---|---|---|
| OS-39 | low | `tools/mkzup.py:360`, `:364`, `:388`, `:228-229` | `zup`, `zpkg` and `base` had no description (`zpkg -Ss` showed none); lua's and make's named design notes ("DESIGN D52") where the others say what the package is | Descriptions for the three; the D-numbers out; the build stops if a package of the system has none | **fixed** (test: the build's check; zup-host-test 111 checks, zpkg-test 159 checks pass) |
| OS-40 | low (docs) | `docs/man/` | zfsd had no page (FA-32) and docs-check looked only at UAPPS; six pages titled `EXAMPLE`; displayd, sha256sum, term, zdesk without SEE ALSO; less without EXIT STATUS; 75 pages without EXIT STATUS and about 37 without OPTIONS while their programs take options (FA-33); NOTES before EXIT STATUS in 26 pages where commands.md names the other order; the header's "Zenix manual" in four columns (2-4 tabs) | Section 3's page format | **fixed**: zfsd.txt and its commands.md line, docs-check checks SBINAPPS (it fails without the page), EXAMPLES, the four SEE ALSO, EXIT STATUS in the pages of the commands changed here (less, man, sudo, tee, wc, touch, hexdump). Open: the other EXIT STATUS and OPTIONS sections, the order, the header column |
| OS-41 | low (docs) | `docs/DESIGN.md:3`, `:245`, `:284`; `docs/architecture.md:5-7`; `docs/man/sudo.txt`; the D11-D13 headings; about 55 references to knowledge-base notes that the public repository lacks | DESIGN.md's opening said "D1-D13 ... checked 2026-10-23", D22 and D25 "nothing built"; architecture.md "about 9,000 lines of kernel ... 11,000" (24,000 and 86,000 now); sudo's page said the password is asked every time (it is kept 5 minutes) | Brought to the code | **fixed** for those (read); the older headings' build notes and the knowledge-base references open (each note should say "the partner's notes, not in this repository" or be summarised) |
| OS-42 | info (docs) | `docs/` | "owner" about 1,154 times, "partner" 172 (many "owner" are a file's owner); CLAUDE.md says owner throughout. Newer notes say partner | No mass rename (the dated records stay as written); new text says "the partner"; CLAUDE.md's choice is the main session's | open |

Configuration, checked and consistent (no finding): every settings file is "setting value" lines with `#` comments
(`/etc/console`, `sntp`, `zpkg.conf`, `daemons`, `svctab`, `zdesk/*`, irc's and mail's config); system defaults in
`/etc`, a user's own in `~/.config/NAME/`; irc and mail write a commented file (mode 600) on first run, files, show,
modplay, irc and mail write their keys file; the preference programs write through one `cfgfile.c`. zdesk's menu and
icons use `LABEL = COMMAND`, a different kind of line, fine. The ports' `.prefs` keep upstream's format.

## 2. Biggest wins, ranked

| # | Fix | Gain | Effort | Ids |
|---|---|---|---|---|
| 1 | Pipes: bulk copy, 4-8 KB buffer | pipelines several times faster (65-102 KB/s today) | 1-2 h, kernel, low risk | OS-13 |
| 2 | Display ring without spare rows on chip-only | +30 KB on the stock A1200 | 2-3 h, the scroll speed to measure | OS-3 |
| 3 | Scrollback 100 lines without fast RAM | +16 KB per console used | 1 h | OS-4 |
| 4 | Unicode font on demand | +10 KB everywhere | 3 h | OS-5 |
| 5 | cwd out of the process table | +6-7 KB | 3 h, kernel | OS-6 |
| 6 | `$(...)` builtins in the shell; the shared-code check lazy | about 90 -> under 10 ms a substitution; about 35 ms off each shell start (estimate) | 6-8 h | OS-14 |
| 7 | 4 KB reads and a memchr line reader | wc and grep 2-4x on big files (estimate; wc and tee done) | 2 h | OS-15 |
| 8 | fsd and diskd run from the kernel's image | +37 KB everywhere | 16-24 h + two reviews | OS-2 |
| 9 | term as ZEX2 | about +30 KB per extra terminal window | 2 h | OS-11 |
| 10 | dbg, emmc, a3000 as D45 spans | +8 KB | 2-6 h | OS-7 |
| 11 | wget over tlspipe | about -100 KB on disk, one TLS path | 4-6 h | OS-12 |

Items 2-5 together bring the stock A1200 to about 1,370 KB free with one login, above the 10-27 figure.
For "one system", in this order: OS-20/26 (one option helper: --help, --version, --), OS-33/32 (one screen layer with
resizes), OS-29/30 (one keys location and syntax), OS-38 (one connect), then OS-34 to OS-36.

## 3. The one-system conventions (proposed as a section of CLAUDE.md)

These are what most programs already do; the findings above list the ones that do not. Proposed for CLAUDE.md (the
main session's file, not edited here):

**Command line**
- `usage: NAME [-x] [-y ARG] file...` lowercase, on the error stream, status 2, for a missing argument and for an
  unknown option (`NAME: unknown option -x` before it). Placeholders in capitals in usage lines and SYNOPSIS alike.
- `--help`: the usage on the output, status 0. `--version`: `NAME VERSION`, status 0. `-h` and `-v` are not reserved
  (ls -h, du -h, -v verbose). `--` ends the options. Combined letters (`-la`) where a program has several.
- Exit status: 0 done; 1 something failed (the run went on where it could); 2 the command line; 126 found but not
  runnable, 127 not found (programs that run others); 124 timeout; 128 + N killed by signal N. Never negative.

**Messages**
- `NAME: OBJECT: REASON` on the error stream, lowercase, no full stop; REASON from the system's table (`zwhy`,
  `zstrerror`), the program's own words only where the system has none. A limit that is hit says so (CLAUDE.md).
- Reports and listings on the output; nothing else there.

**Keys (text and desktop programs)**
- One format, display/keys.h (Ahiru.keys's): `KEYS ACTION [ARGUMENT]`, `#` comments, names in `<>`; one parser,
  display/lib/keys.c. Built-in keys first, then `~/.config/NAME/keys` (written with the defaults when missing, all
  lowercase names, one folder per program). `NAME -K` prints the defaults.
- `?`, F1 and Help: help (where `?` is not text). `q`: quit (or back one level, quit at the top). Esc: cancel or back.
  Ctrl-C: always ends a viewer; an editor asks before losing work. Ctrl-L: redraw.
- Every key can be changed in the file.

**Screen**
- The terminal's size from the system at start and on SIGWINCH; never a fixed 80 x 25.
- A status line in reverse video at the bottom (a title at the top where the program has one); colours from the
  console's 8 base colours, never the only carrier of meaning (2- and 4-colour consoles fold them).

**Settings**
- `setting value` lines, `#` comments; the system's in `/etc/NAME` (or `/etc/NAME/`), a user's in
  `~/.config/NAME/` (folder mode 700 when it may hold a password, files 600). A missing user file is written commented
  out on first run; a bad line is named with its file and number and skipped. Written whole through `cfgfile.c`.

**Network**
- Names through getaddrinfo (netd); one connection limit and message (OS-38); TLS through `/lib/tls/tlspipe`.

**Manual pages**
- `NAME(N)` + tabs to column 24 + `Zenix manual`; sections in this order, each present when it applies: NAME,
  SYNOPSIS, DESCRIPTION, OPTIONS, KEYS, FILES, EXAMPLES, EXIT STATUS, NOTES, SEE ALSO. A page for another name of a
  program: NAME, DESCRIPTION ("another name for ..."), SEE ALSO. Every program and every /sbin server has one.

**Packages**
- Every package has a one-line description of what it is (no design-note numbers); one version for the whole set.

**Shared code**
- Before writing a helper, look in libz and display/lib: date, whole-file read, UTF-8 steps, path joins, the screen,
  keys, the line editor belong there once (CLAUDE.md "add a helper to libz rather than copying one").

## 4. Measurements

All FS-UAE, `free` with `ps` running after one login on console 1 (D18's method); logs kept with the reviewer's
scratch, not in the repository.

| Machine | Free, one login | Free, two logins |
|---|---|---|
| Stock A1200, 2 MB chip only | 1,311 KB chip | 1,235 KB (a session: 76 KB) |
| A1200 + 4 MB fast (the minimum) | 1,854 chip + 3,458 fast | 3,382 fast |
| 68030 + 8 MB, protection on | 1,854 chip + 7,484 fast | 7,403 fast |
| 68060 + 64 MB, protection on | 1,854 chip + 73,016 fast | 72,935 fast |

Processes (ps MEM, KB): boot 13, diskd 29, fsd 76 chip-only (24 KB cache) or 100 with fast RAM (48 KB), init 20,
sh 62 + 89 shared code; each also has a 4 KB kernel stack (OS-9). After boot only boot, diskd, fsd, init and sh run
(netd starts for `ifup -a`, finds no card and ends): no idle server. The kernel's heap on the stock A1200: 151 KB,
of which the display ring 92,672 bytes, console 1's scrollback 32,016, the Unicode font 10,448, grids and ttys about
9.6 KB (and do_spawn's 8 KB, for a moment). kernel.bin 223,576 bytes (text 115,548, rodata 58,420, data 1,052, bss
49,424); given back after boot: the boot span 26,128, CD 752, MMU 16,352 (kept with protection on). Protection on
costs about 41 KB of fast RAM after boot (434 against 393 KB used).

Speed (020 + 4 MB unless named): null call 43 us (chip-only 80, 030 36.3, 060 18.2); message round trip 506 us (chip-only
1,000); stat /bin/sh 4,667 us (chip-only 7,571); readdir 1,021 us a record; console seq 2.824 s, long 3.646 s, colour
6.077 s (stock speed); `ls -laR /` 12.79 s on screen and 6.26 s to /dev/null for 1,286 lines; a tiny program starts
in 26 ms (40 chip-only); `ls /bin` 416 ms, `ls -l /bin` 951 ms. Not measured: ide-speed-test, readdir-test's times,
Amiberry, the desktop session's memory.

## 5. Tests of this audit's fixes

- `rootfs/tests/conv.sh`, run by `tools/cmds2-test.sh`, compared with `tools/tests/conv.expected`: the usage lines'
  stream and status (man, sudo, wc, touch, tee, hexdump, umount, mount, fdisk, zpkg), tee's ninth file and failed
  open, less's unreadable file, the system's reasons in chmod, rm, mkdir, rmdir, stat and realpath.
- tracker-test and mail-test: help on `?`, F1 and Help in the default keys.
- The package build's description check; zup-host-test and zpkg-test.
- docs-check with the /sbin servers.
- cmds-test (wc against GNU), mounttest (stat's message), ffs-test and fat-test (chmod's refusal).

Run on this branch in fast mode, all passing: cmds2-test (26 checks, the conventions script among them), cmds-test,
ffs-test (37), fat-test (122), tracker-test (26), mail-test (28), mounttest (123); host: zup-host-test (111),
zpkg-test, docs-check. The script's first run caught its own mistake (it read `$?` after a command substitution's
line had changed it: a check of the shell, `status-before-subst`, now stays in it and passes).

Established 2026-11-23: two reviewers' measurements and sweep, this audit's reading and re-checks, the tests named.
