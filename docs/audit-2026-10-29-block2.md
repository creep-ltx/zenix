# Block 2's closing audit, 2026-10-29

Scope: roadmap block 2's last checkpoint, "a security audit" (ROADMAP, the order of work). Everything merged since the
network audit of 2026-10-28 (`6eb8bbe`) up to `b86f10e`: D21 step 5 (zenixboot's signatures), D22 steps 1, 4 and 5
(zpkgfmt, zup -f/-r and the live journal, zpkg), D23 steps 1-4 (the cancel mark, POLLPRI, nfscd, writes, the silly
rename, users, SO_LINGER), D24 steps 1-3 (wasabid, `/dev/kmsg`, `/etc/daemons` and init, `CON_READ`/`CON_LINE`/
`CON_INPUT`), D26 (links across mounts), the clock (SYS_ticks 2, sntp, the floor), the IDE fix, sh's compound
redirections, and the network audit's open items (O1-O15).

Three independent read-only reviewers, each on one angle: (a) security, everything that takes input from the network
or from another user; (b) robustness without an MMU (memory, power cuts, killed servers, the frozen interfaces and the
claims); (c) size and speed on the stock A1200 (the kernel's parts, each daemon's memory, the golden path, O13).
Every finding was checked again from the source before it was fixed; "reproduced" says how, "read" means from the
source only.

**Result:** no high findings. 3 medium, 7 low, 2 info, and 1 speed finding. All of them fixed except one low (B2-5:
the fix would change a frozen policy, so it goes to the owner) and one info (B2-10, a design question). Closed in the
network audit: O1, O5 and O6 (O5 by B2-9), O14 (the kernel's size budget, B2-13) and O16 (a test's race, B2-14). O13's design note is section 3
below.

## 1. Findings

| Id | Severity | Where | Problem | Fix | Status |
|---|---|---|---|---|---|
| B2-1 | medium | wasabid, `net/wasabi/core.c` (`waits_on_peer`, the 10 s rule; `c->last` on every byte), `WSB_CONNS` 4 | Before HELLO a connection was dropped only after 10 s with no bytes at all, and a frame header may promise 4 KB: anyone the address filter allows (any local user too, on 127/8) opens 4 connections, sends a HELLO header for 4095 bytes and then one byte every 8 s, and holds every slot for about 9 hours. The owner's own client, with the right key, gets "busy". Reproduced by the reviewer with the core on the PC (`tools/host/wasabi-host.c`) and a slow client: "busy" at 8, 16, 24, 32, 40 s | A deadline from the accept: no WELCOME 10 s after it (`WSB_HELLO_MS`), however slowly bytes come, and the connection is dropped (`core.c:815`); d24-remote.md 3.2 and wasabid(8) say so | fixed (test: wasabi-test check, "a HELLO trickled a byte every 2 s: dropped 10 s after the accept"; with the old core it stays open past 16 s) |
| B2-2 | medium | `user/reboot.c` (the floor at reboot), `kernel/main.c:455`, `kernel/rtc.c` | At every reboot, a time set by the battery clock raised `/var/lib/clock` to `now` with no bound. One wrong battery clock (a typo in AmigaOS, garbage in its registers: say 2061) and one reboot put the floor in 2061: sntp then refuses every real answer (`t < floor`), every certificate is judged at 2061 (expired) and zpkg's list expiry uses the same floor. Only `sntp -r` by hand mends it; sntp(8) promised "cannot move the floor far". Read | `clockfile_rises()` (`user/clockfile.h`): sntp's checked answer raises the floor as before; the battery clock alone only within 400 days of the floor (sntp's own `SNTP_AHEAD`); the floor is never below the build date (`zc_clock_floor`, as sntp and wget). reboot(8) says so | fixed (test: clock-host-test part 5: 2063 from the battery clock does not raise it, 400 days does, a second more does not, sntp's does, the floor never falls, a hand-set time never raises it) |
| B2-3 | medium | wasabid, `user/wasabid.c:111` (`wsb_os_open`: `/dev/ttyS0` never for a file command), `net/wasabi/files.c:82` (`is_self`: `/sbin/wasabid` never replaced) | Both guards were `!zrealpath(...) && !strcmp(...)`: when the path could not be resolved, the request went ahead. zrealpath's limits are tighter than the kernel's (a link text of 159 bytes, the most the kernel takes, or a resolved path of 158 bytes), so a 159-byte link to `/dev/ttyS0` let a GET take the SLIP line, and a PUT through such a link replaced the running daemon. Read | Fail closed: a path that cannot be resolved is refused (`-ENAMETOOLONG` on open; PUT answers "the path cannot be resolved (a link loop or a link too long)"). The PC's OS layer resolves a new file through its directory, as zrealpath does on Zenix | fixed (test: wasabi-test check, "PUT through a link to wasabid refused" and "PUT to a path that cannot be resolved (a link loop) refused"; the open guard on Zenix is read only, the PC has no `/dev/ttyS0` rule) |
| B2-4 | low | wasabid, `net/wasabi/files.c` (PUT's temporary name) | A daemon killed or a power cut during a PUT leaves `PATH.wsb-N` (mode 600, up to the whole upload). The counter started at 0 at every start and had 8 tries, so after 8 such cuts in one directory the first PUT after each start failed with "cannot make the temporary file"; the leftovers were not mentioned anywhere. Read | The counter starts from the clock (`files.c:129`), so old leftovers block no name; wasabid(8) says to delete a left `.wsb-N` | fixed (no automated test: the cut needs a kill at the right moment; the 8-try loop is unchanged and covered by wasabi-test's PUT checks) |
| B2-5 | low | init, `user/init.c:193` (`daemon_ended`) | Status 75 ("start me again at once") counts toward the five ends in 60 s: five RESTARTs of wasabid in a minute (deploying builds one after another) lose wasabid until the next boot, a trip to the console on a machine run remotely. Read | Proposed: count a 75 only when the daemon ran less than 5 s (a restart loop is still given up). **Not changed:** "75 ... but counted" is frozen (docs/frozen-interfaces.md, D24 steps 1-2), so this goes to the owner (section 4) | open (owner) |
| B2-6 | low | sh, `user/sh.c` (`compound_redirs`, every node's source text; `spawn_stage`) | A here-document after a compound command run in a child shell lost its body: `while read l; do ...; done <<E \| cat` and `{ cat; } <<E &` gave the child `... <<E` and no text (the body follows the line, outside the node's source). A builtin with a here-document in a pipeline had the same gap. Read | `node_src()` (`sh.c:730`): a node's text for a child shell carries the bodies of its here-documents that lie after it, from the source as written (quoting kept, so the child expands it as the parent would); and the `<<` token's text now
ends after its delimiter word (it ended at `<<`, so a node ending in a here-document gave the child `<<` and no word) | fixed (test: cmds-test `sh-compound-redir4`, a loop fed by a here-document in a pipeline and a group with a quoted one in the background, compared with bash; also checked by hand in FS-UAE: a loop and a group in a pipeline and a loop in the background, each "sh: syntax error" before) |
| B2-7 | low | `docs/claims.md` | Five numbers in use were not claimed: SYS_ticks' argument 2 with `CLOCK_RTC` 1 / `CLOCK_NET` 2, SYS_settime's second argument, `FS_SYNC`'s a0 1 ("bounded"), `POLLPRI` 2 on a port, `SO_LINGER` 13. All were already in docs/frozen-interfaces.md. Read | Recorded in claims.md | fixed (documentation) |
| B2-8 | info | zup, `user/zup.c:568` (`ops_text`) | The journal text's block is `nops * 200 + 128` bytes and the `snprintf` results were added up unchecked: a line over 200 bytes would wrap `cap - n` and write past the block. Today's longest line is about 186 bytes, so not reachable. Read | Each line is checked against the room left; over it, -ENAMETOOLONG and nothing is written (`zup.c:575`) | fixed (hardening; not reachable, so no test can show the old failure) |
| B2-9 | low | `net/http/http.c` (chunk-size lines and the CRLFs after chunks were not counted), network audit O5 | A hostile mirror could send 4 KB of chunk extension per body byte up to the body limit (wget's is 4 GB). zpkg already has a per-file deadline (120 s + 1 s per KB, `user/zpkg.c:399`), so for zpkg this was only waste. Read | A chunk-size line is at most 256 bytes (`HTTP_CLINEMAX`); the framing as a whole is at most 8 bytes per body byte plus 4 KB (one-byte chunks take 5), `http.c:488` | fixed (test: zpkg-test core, "a chunk-size line of 300 bytes refused", "200 bytes of extension per data byte: refused after 4 KB", "90 one-byte chunks are fine") |
| B2-10 | info | zenixboot, `boot/zenixboot.c` (the state file's phase) | An older signed bundle is accepted when the state file's phase is `none` or `rejected`, and the state file is unsigned text on BOOT:, so deleting it reopens a downgrade to any older signed bundle. Whoever can write BOOT: can also replace zenixboot itself, which D21 6.4 accepts. Read | A minimum version kept somewhere other than BOOT: (proposal, section 4) | open (design) |
| B2-12 | speed | the console, `kernel/term.c:118` (`cursor`) | The cursor drawn on indexed `[cy][cx]`, a 32-bit multiply by 80 per call (cursor off and on at every write: each echoed key), about 6 µs at 14 MHz (read; cycles from the 68020 manual). The row table `rowx` was already there | `cursor()` uses `rowx` as `draw()` does | fixed (objdump: 28 multiplies in term.o before, 27 after; term-test and the console tests unchanged) |
| B2-13 | low | the kernel's size (network audit O14) | Nothing held the kernel's parts to a size: it grew 22.9 KB since 2026-10-27 unseen, and the mount tests' floppy, the binding limit, has 16.6 KB left. docs/architecture.md still gave the sizes of 2026-10-27 | `tools/ksize-check.sh` and `tools/ksize-budget`, run by the `kernel.bin` rule: every part to its line (code + data, bss), kernel.bin to 165,000 bytes and at least 1 KB left under `KERNEL_MAX_MOUNT` (12 KB was planned; main's 716 KB mount RAM disk of 2026-10-29 leaves 4.3 KB); a part over fails the build and is named. architecture.md points here | fixed (checked: with term's line set to 100 the script names term and exits 1; the build passes at 162,592 of 165,000) |
| B2-14 | low | con-test, `tools/tests/con-test.txt` (network audit O16) | "cooked lines" failed once under a full run: the serial path's line came out `61580at`, the text path's `61580a` as wanted. No byte was lost: the script's `wait (?m)^line:` matched as soon as `line:` arrived and typed the next command at once; the kernel echoes typed keys while termtest still prints, so the `t` of `termtest readraw` landed before the line's end (a loaded PC widens the gap). The raw part waited a fixed 3 s the same way. Read (the expected and the seen line differ by exactly that) | The script waits for the whole line, its newline included (`(?m)^line:[0-9a-f]*\n`, the same for `raw:` with `waitecho`: raw keys have no echo line to wait past), before it types again | fixed (test: con-test in the full run) |

(B2-11 is the dead code of section 2, a proposal.)

## 2. The kernel's size (O14)

Measured 2026-10-29 with `m68k-elf-size` on the build (`kernel.bin` 162,588 bytes: 158,852 of code and data plus
3,736 of header and relocations; bss 41,988 bytes). Room: 82,148 bytes under `KERNEL_MAX` (the RAM floppy), 16,612
under `KERNEL_MAX_MOUNT` (the mount tests' floppy, the limit that binds) at 704 KB of RAM disk, 4,320 since main made
it 716 KB the same day: the kernel's next 4 KB need either the shrink list below or a smaller mount RAM disk.

| Part | Code + data | bss | Budget (code + data / bss) |
|---|---|---|---|
| embed (fsd 29,352, diskd 9,360, boot 7,768) | 46,480 | 0 | 47,500 / 0 |
| vfs | 10,865 | 1,540 | 11,200 / 1,600 |
| proc | 10,373 | 15,573 | 10,700 / 15,600 |
| syscall | 9,750 | 0 | 10,000 / 0 |
| term | 8,680 | 46 | 9,000 / 64 |
| serial | 6,909 | 6,480 | 7,100 / 6,500 |
| main | 6,278 | 1,056 | 6,500 / 1,100 |
| gfx | 6,086 | 1,988 | 6,300 / 2,000 |
| svc | 5,401 | 1,393 | 5,600 / 1,420 |
| emmc | 4,741 | 620 | 4,900 / 640 |
| sock | 4,532 | 1,041 | 4,700 / 1,060 |
| floppy | 4,374 | 150 | 4,500 / 160 |
| start | 3,758 | 8,192 | 3,900 / 8,192 |
| random | 3,663 | 268 | 3,800 / 280 |
| kbd | 3,629 | 173 | 3,750 / 180 |
| ipc | 3,561 | 1,568 | 3,700 / 1,600 |
| ide | 3,542 | 532 | 3,700 / 560 |
| mem | 2,802 | 228 | 2,900 / 240 |
| tty | 2,508 | 48 | 2,600 / 64 |
| a3000 | 2,310 | 93 | 2,400 / 100 |
| signal | 2,042 | 0 | 2,100 / 0 |
| drv | 1,781 | 624 | 1,850 / 640 |
| rtc | 1,492 | 0 | 1,550 / 0 |
| seg | 1,210 | 256 | 1,250 / 256 |
| audio, event, blk, input | 756, 658, 430, 348 | small | 800, 700, 460, 400 |

By group: the core 56.7 KB, the embedded programs 46.5 KB, the console 28.2 KB, the drivers 17.6 KB, the network glue
10.0 KB. Where it could shrink (proposals for the main session, measured by recompiling, not yet on a 68020):
- **B2-11, dead code:** the kernel is linked with `-ffunction-sections` but without `--gc-sections`; with it only 264
  bytes go (`serial_puts`, `fs_call`, `term_back`, `kbd_set_keymap`/`kbd_keymap_name` (kept for `/etc/keymap`?),
  `diag_stage` outside DIAG builds). Small; their prototypes are in `kernel/kernel.h` (the main session's file).
- **-Os for cold files** (Makefile: all of the kernel is -O2): main 1,152, ide 580, audio 250 bytes at no speed cost;
  serial's klog 1,612; `proc_load`/`proc_spawn` in a cold file 2,786; syscall.c 3,540 only after a system-call
  benchmark on the stock A1200. About 7-11 KB.
- **Boot-only code and data never given back:** `boot_blob` 7,768, `stage0_blob` 3,096, the `*_init` functions and
  `kmain` about 11 KB: about 22 KB of memory (not file size) in a `.init` section freed after boot. Bends the loader: a
  design note first.
- **bss:** `procs[].cwd[160]` 5,120 bytes of 15,296 (a heap block sized to the path, as the environment: about
  4.5 KB); the serial send ring 4 KB (1 KB is 89 ms at 115,200 baud: 3 KB); the boot stack 8 KB (measure its high-water
  mark with the guard before halving it).

## 3. O13: the floppy driver as a server (design note)

**Facts.** `kernel/floppy.c`: 437 lines, 4,374 bytes of code, 150 of bss; at the first use it takes 13.6 KB of chip
RAM (the raw track) and 11 KB (the decoded cache) and never gives them back. Its only callers are `SYS_fd_info` (76)
and `SYS_fd_rw` (77), used only by diskd (its probe at start and its reads and writes), plus `fd_tick` from the 100 Hz
tick (the motor timeout). The floppy boot never uses it: the bootblock loads the kernel and the RAM disk through the
ROM. It has no interrupt (it polls): it writes `DSKPTH`, `DSKLEN`, `DSKSYNC`, `ADKCON`, `DMACON`, `INTREQ` (set/clear
writes, safe to share), CIA-B's port B (floppy only), reads CIA-A's port A and CIA-B's ICR (read-to-clear, which
main.c reads too). **It busy-waits inside the system call and the kernel is not preempted**: a track read spins
200-300 ms, a motor start up to 800 ms; while it spins nothing else runs (no echo, no console). Copying 100 KB from a
floppy freezes the machine for about 2 s. That is the strongest reason for O13, more than the 4.4 KB.

**Design.**
- **The server `fdd`**, port `floppy`, the file `/sbin/fdd` (a normal program, not built into the kernel image, or
  nothing is saved). Messages `FD_INFO` (drive) and `FD_RW` (drive, block, count, buffer, write): the old system
  calls' arguments, claimed in docs/claims.md when built. Only root may own the port.
- **How diskd reaches it:** for a `df` unit diskd sends a no-start call (`call_ns`) to `floppy`, the caller's buffer
  passed through. D11's rule becomes "diskd calls only `floppy`; `floppy` calls nobody".
- **Started outside diskd:** loading `/sbin/fdd` reads through fsd, which may be the one waiting on diskd, so diskd
  never starts it. `mount` (and an open of `/dev/dfN`) starts it in the user's context first, as a socket call starts
  netd; diskd answers `-ENXIO` for a df unit while no `fdd` runs, and its start-up floppy probe goes.
- **The hardware:** a 68020 without an MMU reaches the custom chips and the CIAs from user mode. `SYS_drv` gets a
  `floppy` claim kind: the 13.6 KB DMA buffer is the claim's chip memory (a DMA after a crash never writes memory given
  to someone else), and the claim's quiesce program stops it (`DSKLEN` 0x4000, disk DMA off, motors off). Nothing of
  it lands in the kernel's interrupt code.
- **The kernel keeps** about 300 bytes: the claim, the random credit for the index-pulse timing (`SYS_rnd`), and the
  reset warning passed to `fdd` (it finishes a track write, at most about 400 ms).
- **Idle:** the motor off after 3 s by `fdd`'s own timed wait (replaces `fd_tick`); the 24.6 KB of buffers freed 10 s
  after the last use; `fdd` ends when no df unit is mounted and it has been idle 30 s.
- **Failures:** `fdd` crashes mid-write: the quiesce runs, the track may be half written (as with a power cut), the
  request ends `-ECONNRESET` and the next start probes again. `fdd` missing from a disk: floppy mounts fail with
  `-ENXIO`, said plainly.
- **Saves** 4.5 KB of kernel memory on every machine and 24.6 KB whenever the floppy is idle, and ends the freezes (the
  tick preempts `fdd`). The RAM floppy saves about nothing (the kernel loses 4.4 KB, the RAM disk gains `fdd`, about
  6 KB); the hard-disk boot saves the whole 4.4 KB. **Costs** one more message per request (about 0.2 ms against a
  200 ms revolution) and, while loaded, about 6 KB of code, its stacks and the buffers.

## 4. For the owner / later

- **B2-5, init and wasabid's RESTART** (owner): today five RESTARTs in a minute lose wasabid until the next boot.
  Recommendation: count a 75 only when the daemon ran less than 5 s (a quick restart loop is still given up). It
  changes a frozen policy ("75 ... but counted"), so only with a yes.
- **B2-10, downgrades after the state file is deleted** (design, D21/D15): keep the lowest version zenixboot accepts
  somewhere other than BOOT: (the root's `/var/lib`, read by zup before it hands a bundle over). Recommendation: later,
  with D15's next step; anyone who can write BOOT: can already replace zenixboot.
- **O12 is wider than nfscd** (main session, D27 or now): libz's 4 KB output buffer is linked into every program
  because `_zexit` calls `oflush`; diskd, nfscd and wasabid never print to it. A flush through a pointer set by the
  first write lets the linker drop it: 4 KB each in diskd, wasabid and every nfscd mount.
- **The daemons as shared code** (D13): nfscd (54 KB of code) is loaded again for every NFS mount, wasabid too. As ZEX2
  each mount after the first costs about 18 KB instead of 72 KB.
- **Daemon memory on the stock A1200** (read from the build, emulator-free): netd + wasabid + one nfscd at rest is
  about 255 KB before the network's heaps, 22% of the 1,132 KB free; wasabid 8 KB per connection (4 at most); zfsd's
  32 KB stack is worth a measurement.
- **ipc's per-message multiplies** (main session, `kernel/kernel.h`): `struct port` is 56 bytes, so each round trip
  does about 4 32-bit multiplies (about 12 µs at 14 MHz); padded to 64 bytes (+224 bytes of bss) they become shifts.
  Measure with the message benchmark on the stock A1200.
- The kernel's shrink list of section 2 (B2-11, -Os, the `.init` section, the bss items), to the main session in that
  order.
- Still open from the network audit: O2, O3, O4, O7 (the owner's hardware), O8 (judged acceptable), O12 (above), O13
  (this note; building it is the owner's order of work), O15.

## 5. Checked and found sound (the reviewers' summary)

- **wasabid:** the key compared in constant time over its whole length; strings with a NUL refused; the wrong-key pause
  holds the frame and drops a client that keeps sending; the frame receiver bounded (4 KB / 64 KB);
  `/etc/wasabid.conf` root's, mode 600, at most 2 KB; PUT's temporary file `O_EXCL`; DEL by `lstat`; network mounts
  refused; per-request memory freed at the job's end and at a drop; run slots and background children reaped.
- **The kernel's new operations:** `CON_READ`/`CON_LINE`/`CON_INPUT` root only by the effective id, the console index
  and cell counts bounded, at most 256 input bytes, `kbd_inject` refuses the reset and status codes; `SYS_settime` root
  only and `a2 > 1` -EINVAL; `/dev/kmsg` root's and write only, never passed to a command's child; D26's link walk
  iterative (40 links, the mount instance checked, `FS_LINKED` nobody asked for is -EIO, `fsl_ok` limits what a server
  may rewrite); `ipc_cancel` takes back only messages still queued; the watchdog counts only the server's own time.
- **nfscd:** set-id bits never shown and never set; raw requests from non-root users -EACCES; READLINK and the link
  texts bounded; the silly-rename index checked; the groups file read bounded; a cancel never touches the client's
  buffers after it.
- **zpkg, zup, zenixboot:** manifest paths checked (absolute, no `.`/`..`/`//`, a restricted set, reserved names);
  `-U` copies into root's cache before checking; signatures checked before parsing (the list) and before install (the
  bundle, over the recomputed digest); a snapshot's list bound to its name (O1); zenixboot checks every bundle's
  signature with the key class tied to the channel, unsigned bundles only in a test build (O6); zup's live journal
  written whole and synced before anything changes, previous versions kept as links, every new file read back and its
  SHA-256 checked, commit or rollback synced before the clean-up, recovery idempotent; a damaged journal moves aside.
- **The clock floor file** written beside and renamed; a damaged file falls back to the build date.
- **Frozen interfaces:** every other new number and format since `6eb8bbe` is recorded in docs/frozen-interfaces.md;
  nothing existing changed meaning (B2-5 was not changed for that reason).

Established 2026-10-29: the findings read from source by three reviewers and checked again; B2-1 reproduced on the PC;
each fix's test as named in the table, all passing in this branch (`make check` at the end of the work).
