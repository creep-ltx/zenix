# D24: the remote control, `wasabid` for Zenix (design note, revision 2)

Status: **design, revision 2 after two independent reviews (section 16); steps 0-2 built (the end of this note).** Roadmap block 2: "a
Wasabi-like remote control (early: it lets the A1200 be tested remotely)"; CLAUDE.md: "a Wasabi-like remote-access
daemon for Zenix (needs the network driver)". A daemon on Zenix that **speaks the protocol of the owner's Wasabi**, so
the `wasabi` command on the PC, its Python API, its MCP server (Claude's tools), the desktop and phone apps work with
Zenix as they do with AmigaOS: run a command, copy files, see the console as text or as a picture, type, wait for text,
read the kernel log, check memory, reboot into Zenix or AmigaOS. Its main purpose is testing Zenix on the real A1200
without the owner at the keyboard. It needs five small kernel interfaces (section 6) and makes one decision for D11
(section 7: servers nobody calls, such as network listeners, are kept alive by init), which also closes D20 7.2's open
point. The short version is DESIGN.md D24.

Facts about today's code are *read* from main at 22fd4ba (2026-10-27). Facts about Wasabi are *read* from its
repository (`PROTOCOL.md`, `docs/security.md`, the client `wasabi`, `wasabi_mcp.py`; MIT licence, the owner's own
project; nothing is copied: Zenix's daemon is new code for a different system, written from the protocol document).
Every size and time is an *estimate* unless it says *measured*.

## 1. What the owner gets, in plain words

- From the PC: `wasabi --host A1200 --port 1236 run 'ls /bin'`, `put`, `get`, `grab` (a picture of the console),
  `text` (the console's text, new), `key type 'ls\n'`, `wait text 'login:'` (new), `debug` (the kernel log as it is
  written), `health`, `speedtest`, `reboot --to amigaos --yes` (new). Claude uses the same through the MCP server.
- **The A1200 can be tested with nobody at it:** from AmigaOS (its own Wasabi) a session copies a new Zenix and starts
  it; Zenix's daemon answers; the tests run; a reboot to AmigaOS brings AmigaOS and its Wasabi back (section 8).
- **Off unless switched on** (one line in `/etc/daemons` and a key file), answers only addresses of the home network,
  needs a key of its own (not the AmigaOS Wasabi's). On the stock A1200 it takes about 45 KB while it runs, on top of
  the network's own ~115 KB (section 10).
- **Honest:** the first version is not encrypted, so anyone who can watch the home network can read its key; whoever
  has the key can do anything on the machine (section 9). While Zenix runs, this is an exception to your rule that the
  A1200 is reached only encrypted (owner question 2).

## 2. Today (read)

**Wasabi** (`PROTOCOL.md`, version 1): one TCP connection per command (streams stay open); frames `u8 tag, u32
length, payload` (at most 64 KB, a larger length closes the connection), big-endian; `HELLO` (version, key) answered
`WELCOME` (version, banner, **`caps`**: a comma list of what the daemon can do, which the client checks before it asks)
or `ERR` (code, message); discovery by a UDP probe answered with the name and **the TCP port**, which the client uses
and caches; only RFC 1918, loopback and link-local addresses are answered; the key is a shared secret compared as bytes,
sent in the clear unless TLS is used. **TLS** (optional since wasabid 0.8): TLS 1.3 only, Ed25519 certificates from the
owner's own CA, a client certificate required; the client tries TLS first, goes plain to a daemon that closes the
connection at the first byte (`0x16`), and **pins** every `ADDRESS:PORT` it once reached encrypted, refusing plain there
afterwards; 127.x may be plain. The owner decided (Wasabi 0.8, 2026-10-14) that encryption stays optional, and runs the
A1200's AmigaOS wasabid encrypted-only. Commands: files, `RUN` with live output and slots, `FREE`, `PS`, `KILL`, `DEBUG`
(heartbeats, a farewell before a deliberate end, the last crash replayed), `REBOOT`, `RESTART`, `QUIT`, `INSTALL`,
`GRAB`, `SCREEN`, `SUM`, `KEY`, `INPUTS`, `HEALTH`, `SPEED`, `WINDOWS`, and AmigaOS-only ones (`SNOOP`, `UI`, `UIACT`,
`AREXX`, `PORTS`, `CLIP`, `LIVE`, `MOUSE`). The client has one key per configuration (`--key`, `WASABI_KEY`).

**Zenix:** BSD sockets, `poll`, pipes (D16, D19); `spawn` with chosen standard files, process groups, `kill` of a
group; **16 descriptors per process** (`NFD`), 64 open files in all (`NFILE`), 32 sockets (`NSOCK`), and netd's small
profile 8 TCP connections (`MEMP_NUM_TCP_PCB`); `SYS_reboot` with a one-shot boot note (`BOOTNOTE_*`), which the
`reboot` command calls after writing the random seed (D19 9.3); the kernel log by position (`SYS_klog`, 8 KB ring, the
previous boot's with `KLOG_PREV`; its reasons: running, panic, reboot); `SYS_procinfo` (`uproc`: no controlling tty, no
program text), `SYS_meminfo` (chip and fast free, sharing); ten consoles drawn by the kernel (`kernel/term.c`: three
bytes a cell: glyph, style, colour), whose input comes from the keyboard ring and, for the tests, from the serial line
(`serial_work`: bytes to the shown console's tty, 0x1d plus a digit shows a console); the console ops of D20 4.5, named
by a descriptor (`SYS_console`'s op word); `FT_KLOG`, the kernel log as a file, given only to parentless processes (D11
step 8: the services); D21's crypto library (TLS 1.2 client only, no Ed25519 certificates; public-key steps take
seconds on the stock A1200, D21 7.3); D11 starts servers on a message to their port and decided "no start at boot" (4.4)
with "keep alive" left for later (1); init keeps a login on console 1 and on each other console when first shown, and
starts `ifup -a`; D20 7.2 left "how a TCP listener is started at boot" open.

## 3. The protocol: Wasabi's own

| | **Wasabi's protocol** | A new small protocol | ssh (`sshd` and a shell) |
|---|---|---|---|
| The owner's tools (CLI, Python API, MCP for Claude, desktop and phone apps) | work, for every command Zenix offers (`caps`) | all new | ssh only; no screen, keys, kernel log |
| Daemon size | ~30-35 KB | similar | `sshd` is a later D21 step; ptys first (D20 7) |
| Encryption | optional (section 9) | to design | built in, but seconds per login on the stock A1200 |

**Decision: Zenix's daemon speaks Wasabi protocol version 1** (the frames, the tags and their payloads as `PROTOCOL.md`
defines them) and offers the commands that make sense on Zenix (section 4), saying so in `caps`. The client copes with
a daemon that lacks a command (it reads `caps`); it needs some additions for Zenix (section 11), made in the Wasabi
project behind caps, never changing what AmigaOS's daemon does. The name is the same, **`wasabid`** (`/sbin/wasabid`):
one protocol, two daemons.

### 3.1 Its own TCP port

Zenix's daemon listens on **TCP port 1236**, not 1234: the owner's A1200 is reached at the same address whether it runs
AmigaOS or Zenix, the client has pinned `ADDRESS:1234` as encrypted, and a plain answer there would be refused, rightly.
On its own port Zenix is "another Amiga at this address". Port 1235 is the trial port of AmigaOS's `wasabi update`.
**Discovery:** the daemon answers probes on UDP **1234 and 1236** (only one system runs at a time; a client given `--port
1236` without `--host` probes 1236) with `WASABI!1 NAME 1236 BANNER`; the banner says `Zenix VERSION`, caps carry
`zenix`. Discovery does not cross slirp (the emulator, the cable): there the client names the host.

### 3.2 The connection rules

The address filter (RFC 1918, loopback, link-local; `allow CIDR` adds ranges, `allow any` turns it off and is warned
about at every start) is checked before `HELLO`; refused addresses are counted (`WELCOME`'s `refused`, the count kept
in `/var/lib/wasabid.refused`) and get no discovery answer either. **A transfer must keep moving:** no progress for 10 s
in either direction drops the connection (a whole 64 KB frame takes about 6 s over SLIP, so Wasabi's "finish a frame in
10 s" would drop real frames). An unknown tag gets `ERR` "not on Zenix". A connection whose first byte is `0x16` (a TLS
ClientHello) is closed at once (the client's sign to go plain). Behind NAT (zenix-slirp, FS-UAE's slirp) every peer
looks like 10.0.2.2: the filter and the per-address pause cannot tell callers apart there, so port forwards to the
daemon stay bound to the PC's 127.0.0.1 (zenix-slirp's default), said in the page.

## 4. The commands on Zenix

**caps** (exact): `ping,info,ls,put,get,run,del,mkdir,debug,reboot,rebootto,restart,ps,kill,speed,speedfile,quit,grab,`
`screen,windows,hb,key,health,detach,slots,sum,inputs,zenix,text`. Never `psfree` (it changes `PS`'s line), `guru`,
`snoop`, `tls`, `plainopt`, `install`.

| Tag | On Zenix | Notes |
|---|---|---|
| `PING`, `INFO` | yes | INFO's lines: the Zenix version (`uname -a`'s), CPU, chip and fast RAM free, Emu68 or not (from the kernel log's `pi:` line read at the start: the hardware table is netd's drivers' only, D19), the uptime, the interfaces, the daemon's version and settings (never the key) |
| `PUT` | yes | 5.3: regular files only, to a short temporary name in the same directory, renamed over the target at `END`. Mode: `0xFFFFFFFF` (what the client always sends) = the old file's mode when one is replaced, else 0755 for a Zenix program (ZEXE/ZEX2 magic, checked at `END`) and 0644 otherwise; with caps `zenix` a client may send a Unix mode with bit 31 set (`0xFFFFFFFF` is tested first); other values are AmigaDOS's bits mapped (`e` allowed: x for all; `w` allowed: the owner's w). **The daemon's own file `/sbin/wasabid` is refused**, as Wasabi's `PUT` refuses its own path |
| `GET`, `DEL`, `MKDIR` | yes | 5.3; `DEL` of a directory: `rmdir` (empty only) |
| `LS` | yes | Wasabi's line (`TYPE SIZE PROTBITS DAYS MINS TICKS NAME`): the mode mapped to AmigaDOS's bits, the time as a DateStamp (days since 1978-01-01, minutes, ticks); a symbolic link shows as what it points to |
| `RUN` | yes | 5.1; bit 0 clear: stderr in its own `STDERR` frames; bit 1 detach; bit 2 the slot. **Two slots** (the descriptor budget, 5.1), not Wasabi's four: a third gets `ERR` "all run slots are busy" |
| `SLOTS`, `FREE` | yes | `FREE` with bit 0 (the client's `--max-time`): SIGINT to the command's process group, answer `asked`; without: SIGINT, then SIGKILL after 3 s (a timer in the loop), answer `stopped`, or `freed` if it has not ended (a process waiting on a hung server cannot end until it answers, G4) |
| `PS` | yes | Wasabi's line: the address field the pid as `0x%08x` (`KILL 0x...` works with any client; a client that knows `zenix` shows the pid), kind `p`, priority `-nice`, state `run`/`ready`/`wait`/`stop`, stack `0` (not known), `cli` `-1` (`uproc` has no tty), the name for both name and command |
| `KILL` | yes | flag clear: SIGINT; set: SIGKILL. Never `boot`, `init` or the daemon; `netd`, `diskd` and the filesystem servers only with the flag (killing netd cuts the daemon off: D11 restarts it at the next socket call) |
| `SPEED` | yes | the network path (`target` empty) or through a filesystem; D19's speed table on the real machines |
| `DEBUG` | yes | the kernel log (5.4) |
| `REBOOT` | yes | 5.5; with caps `rebootto`, bit 1 "AmigaOS next", bit 2 "Zenix next" (the boot note), bit 3 "skip the guard" |
| `RESTART`, `QUIT` | yes | 7; both `ERR` while a slot is busy unless bit 0 (`--force`: the commands get SIGINT, then SIGKILL after 3 s) |
| `INSTALL` | **no** | Zenix updates itself one way, `zpkg`/`zup` (D15, D22), which keeps the previous version; `wasabi update` therefore does not work against Zenix. A new daemon is tried first as a trial instance (`RUN '/tmp/wasabid --trial --port 1237'`, which reads the same configuration and answers on that port), then shipped as a package |
| `GRAB` | yes | the shown console as RGB pixels, a display line at a time (6.2); slow over SLIP (a 640 x 256 picture is 480 KB: about 45 s at 11 KB/s; `TEXT` is the way there) |
| `SCREEN` | yes | the ten consoles as "screens" (`ttyN`, 640, rows x 8, depth, `ttyN`), front first; a title brings that console to the front (raw keys Amiga+F*N*, 6.3) |
| `WINDOWS` | yes | one `S` line for the shown console and no `W` lines, so the client's region options (`sum --region`, `wait change --region`) find the screen |
| `SUM` | yes | over the shown console's pens, line by line (6.2) |
| `KEY` | yes | mode 0 raw Amiga key codes into the keyboard path, the qualifier words ignored (the codes carry the modifiers); mode 1 text into the shown console's input (Latin-1: what Zenix's keymap gives) (6.3) |
| `INPUTS` | keys only | kinds 0 and 1, at most 340 records (one 4 KB frame); kind 2 (mouse) refused before any event; the waits are timers in the loop |
| `HEALTH` | yes | 5.6 |
| `TEXT` (new) | yes | the console's text, at once or when a pattern appears (6.1, 11) |
| the rest | no | not in caps; `ERR` "not on Zenix" |

## 5. How the daemon works

### 5.1 One process, one loop, a descriptor budget

`wasabid` is one process with one loop (as AmigaOS's), everything in one `poll` (D16), every socket and pipe
`O_NONBLOCK`; a frame is read and written in pieces as the socket allows. Timers (the 10 s progress rule, `FREE`'s 3 s,
`INPUTS`' waits, heartbeats, the log's 100 ms) are the `poll` timeout, never a sleep. SIGPIPE is ignored (and sends use
`MSG_NOSIGNAL`); SIGCHLD's handler only sets a flag, and while children live the `poll` timeout is at most 100 ms and
every wake-up calls `waitnb`, so no child's end is missed.

**The descriptors (16 a process, `NFD`):**

| Use | Descriptors |
|---|---|
| the log (`/dev/kmsg`, 7; standard input and error closed) | 1 |
| `/dev/null` kept open (the commands' standard input) | 1 |
| the TCP listener, the UDP discovery sockets (1234, 1236) | 3 |
| client connections, **at most 4** (the fifth is accepted, answered `ERR` "busy" and closed) | 4 |
| run slots, **at most 2**, each stdout and stderr pipe (one when merged) | 4 |
| one file transfer at a time (`PUT`/`GET`/`SPEED` through a file; another gets `ERR` "busy") | 1 |
| **Total** | **14**, and a spawn needs 2 more for a moment (the pipes' write ends) |

Every accept, spawn and open checks the budget first. The 4 connections are also what netd's small profile can spare
(8 TCP connections in all, the listener one of them; each Wasabi command costs two, the client's TLS try and the plain
one, plus TIME_WAIT). More slots need a larger `NFD`, the main session's call from a measurement.

**A command (`RUN`):** `spawn("/bin/sh", "-c", COMMAND)` with standard input `/dev/null`, standard output and error the
pipes, its own process group (set by the child's first act, D16's race noted in d16 12), the environment `HOME=/root`,
`PATH`, `USER=root`, `TERM=dumb`. Output is sent as it arrives. **`EXIT` is sent when `sh` has ended** and what was left
in the pipes is sent: a background job started by the command (`run 'x &'`) keeps no slot, and its later output is
lost (said). The status is the shell's (128 + n after a signal), the secondary word 0. A command whose client went away
gets SIGINT, then SIGKILL after 3 s. A detached `RUN` (bit 1) writes `/tmp/wasabi-bg-N`, takes no slot, is not ended.
A command's controlling tty is console 1 (init's, inherited): one that opens `/dev/tty` (`sudo`, `passwd`) waits for
console 1's keyboard (said; a pty later, D20 7). `FREE` and the end of a slot signal the process group and also the
descendants found by `uproc.ppid`.

### 5.2 Who it runs as

Root, and its commands as root: testing Zenix needs it (installing, mounting, `reboot`, `dmesg -p`), and without an MMU
whoever has the key owns the machine anyway (section 9). There is no setting for another user (a non-root user would
need a helper process per command, since root's `setuid` changes both ids at once; it buys nothing here).

### 5.3 Files, without wedging the loop

- **Regular files only:** `GET`, `PUT` and `SPEED` open with `O_NONBLOCK` and refuse anything `fstat` does not call a
  regular file (a GET of `/dev/tty1`, `/dev/random` or `/dev/zero` would block or never end; opening `/dev/ttyS0` takes
  the serial port). Paths are at most 63 bytes (the system calls' limit); the temporary name of a `PUT` is
  `.wsb-N` beside the target.
- **Network mounts:** the path is resolved first (`realpath`: symbolic links, `..`), then looked up in the mount table
  (`SYS_mountinfo`, type `nfs`). On a network mount the work is done by a short-lived helper, `wasabid` started again as
  a child with `--file` (no second program), which moves the bytes through a pipe (one of the slot's descriptors is not
  needed: it uses the transfer's); the daemon kills it after 10 s without progress (a kill cancels its waiting request,
  D23 5.1, so this needs D23's step 1) and answers `ERR` "the filesystem did not answer in 10 s". The helper costs a
  second copy of the daemon's code for its life (~35 KB; the daemon is a plain ZEXE, as servers are, D13).
- **A local server that hangs** (fsd, zfsd stuck in a request) wedges the daemon's file calls as it wedges every program
  (G4); D11's second kill of that server frees them. Said in the page.

### 5.4 `DEBUG`: the kernel log

The stream is Zenix's kernel log, the lines `dmesg` shows: the daemon reads `SYS_klog` from the position it has sent
so far, every 100 ms while a subscriber listens, and sends each line as a `LOG` frame (stream 0). **A log that wrapped
past that position** (the ring is 8 KB) is said ("[wasabi: N bytes of the kernel log were lost]"): this needs the log's
total, which no call gives today (`ring_read` clamps a stale position silently): kernel interface 6.4. **At its start
the daemon reads the previous boot's log** (`KLOG_PREV`) and keeps how it ended (the `klog: previous boot ... ended`
line: running, panic or reboot) and, after a panic, its last 20 lines; **every new subscription replays them first**:
Wasabi's "last guru", Zenix's way. Heartbeats every 5 s, the farewell line before a deliberate end.

### 5.5 `REBOOT` and its guard

The daemon answers `OK`, sends the farewell on every stream, waits (at most 1 s) for its sockets' send queues to drain,
closes them, and runs **`reboot` [`amigaos`|`zenix`]** as a child (so the random seed is written first, D19 9.3; then
`SYS_reboot`'s sync: each filesystem 5 s, audit A7; network mounts last and bounded, D23 5.4). **The guard:** `ERR`
naming the process while `zup` (an update or a package being installed, D15/D22), `fsck.*`, `mkfs.*`, `fdisk` or `dd`
runs (by name through `SYS_procinfo`; the list in the page); bit 3 skips it. The guard is advisory: `RUN reboot` or a
`RUN` of those tools is not stopped (said). A reboot to AmigaOS needs the boot note's place (a machine with very little
chip RAM has none: `reboot`'s own message comes back as `ERR`).

### 5.6 `HEALTH`

`key value` lines, absent keys left out (Wasabi's rule): `mem.chip.free`, `mem.fast.free` (`SYS_meminfo`), `emu68`
yes/no (5's `pi:` line), `zenix.version`, `uptime` (seconds), `procs`, `load.ready` (processes ready to run at that
moment), `net.IFACE.rx/tx/bad` (`SOCK_IFLIST`), `disk.ROOT.free` (`statfs`). The Pi's temperature and clocks need the
mailbox, which Zenix has no driver for: absent. The keys are listed in `wasabid`'s page.

### 5.7 The configuration

`/etc/wasabid.conf` (a new `/etc` format, frozen once shipped; root's, mode 600, refused otherwise): `key TEXT` (at
least 16 characters; the page says how to make a good one, `head -c 18 /dev/random | base64`, and to use a key of its
own, not the AmigaOS Wasabi's), `port N` (1236), `allow CIDR` (repeatable; `any`), `name NAME` (the discovery name;
default `zenix`). Read at the start. **Not shipped.** A missing or bad file ends the daemon with status 0 and a log line
(a stop: init does not start it again), so the `/etc/daemons` line alone is the switch once the file exists. The key is
compared in constant time; a wrong key closes the connection and makes the next `HELLO` from that address wait 1 s, and
is counted like a refused address. A network that is down (`-ENETDOWN`) is waited out by the daemon (it tries again
every 10 s), never ended on, so init's give-up does not happen for a cable.

## 6. Five small kernel interfaces (the main session's to claim; section 13)

All for root only, all named by a console's descriptor as D20 4.5's ops are (root may open any `/dev/ttyN`). None
allocates: an op on a console that was never used answers -ENXIO (`term.c`'s `get()` would allocate the grid and panic on
a full memory).

### 6.1 `CON_READ`: the console's text

`SYS_console` op `CON_READ` (`(fd, buffer, size)`): a header (rows, columns, cursor row and column, a change counter
raised on every draw, whether the console is the shown one, whether a PC font is loaded: then each glyph is CP437, D20)
and the cells, three bytes each as `term.c` keeps them (glyph, style bits, colour: 16 foregrounds and 16 backgrounds),
in Zenix's own documented format, frozen with the op (Linux's `vcsa` has one attribute byte, too few). The grid, not the
scrollback view. About 0.2 KB of kernel code.

**`TEXT`** (the new command, 11) is `CON_READ` by the daemon, optionally **after waiting**: with a pattern (an extended
regular expression, `user/regex.c` as `grep` links it: 4,558 bytes *measured* by the review) the daemon reads the console
every 100 ms while its change counter moves, until a row matches or the timeout passes. Waiting on the Amiga keeps SLIP
free (a client polling 2.5 KB twice a second would take half the cable).

### 6.2 `CON_LINE`: a line of pixels

`SYS_console` op `CON_LINE` (`(fd, line, buffer)`): copies one display line of that console, if it is the shown one, as
one pen byte per pixel, with the palette as the Copper shows it (the CGA palette when D20's rules ask for it) and the
display's generation (raised when the depth or the planes change); -ENXIO if the console is not shown. **The kernel keeps
the layout** (interleaved planes behind one base that scrolling moves at vblank, the blitter copy-back at the buffer's
end, the depth that `CON_COLOURS` changes): nothing about it is frozen, only "a line of pens". A picture taken over
seconds can still mix lines from before and after a scroll (each line is right, the picture may not be one moment):
`GRAB` reads the generation and the change counter before and after and says so in a `STDERR`-style note frame when they
moved. About 0.3 KB.

### 6.3 `CON_INPUT`: keys and text

`SYS_console` op `CON_INPUT` (`(fd, kind, bytes, count)`, at most 256 bytes a call; answers how many were taken):
- **kind 0, raw key codes** (Wasabi's mode 0): each code goes to `key_code()` from the system call (the kernel is not
  preempted, so this is the same context as `kbd_work`; the interrupt's ring is not written, so there is no second
  producer), so the keymap, the modifiers and Amiga+F1..F10 act as on the real keyboard. Codes 0x78 and 0xF8-0xFF (the
  reset warning and the keyboard's status codes) are refused; modifiers still held at the end of the call are released.
  The descriptor names no console here (the keyboard goes to the shown one); it must be a console. No randomness is
  credited (the timing comes from the network). D16's list of input producers gains it.
- **kind 1, text** (Wasabi's mode 1): the bytes go to that console's tty through `tty_input`, as the serial line's do
  (the line discipline, echo, Ctrl-C to the foreground group), without the serial hook's 0x1d switches (console switches
  are raw keys). When the tty's queue is full it takes fewer and says how many.
About 0.2 KB.

### 6.4 `KLOG_TOTAL`: where the log is

**Replaced by the main session's decision 1 (2026-10-28, below):** no `KLOG_TOTAL`. `SYS_klog`'s `KLOG_OLDEST` (audit
F10, built after this note was written) answers the offset of the oldest character the ring holds, so a reader whose
position fell below it knows exactly how much was lost; F10 also fixed the `dmesg` wrap bug (`user/klogread.h`). The
text below is kept as it was reviewed.

A flag bit in `SYS_klog`'s offset (as `KLOG_PREV`): the call answers the total number of characters written so far, so
a reader whose position fell behind the ring knows how much was lost and where to go on. About 50 bytes. **Found here, a
bug to fix next (CLAUDE.md "fix when found"):** `dmesg` adds what it got to a position the kernel had clamped, so once the
log has wrapped it prints the oldest chunk again and again (`user/dmesg.c`, `kernel/serial.c` `ring_read`); the main
session's.

### 6.5 `/dev/kmsg`: a file that writes to the kernel log

Root opens `/dev/kmsg` for writing and gets an `FT_KLOG` file (the kind D11 step 8 gives the services), so a program init
starts can log as the services do, never on console 1 (D11's rule: no server output on a console after boot). Linux's
name. About 0.1 KB. Init opens it as the standard output and error of what it starts from `/etc/daemons` (7).

## 7. Servers nobody calls: init keeps them alive (a D11 decision)

D11 starts a server when someone sends a message to its port; D11 4.4 decided against "start at boot" because no such
server existed, and D11 1 left "keep alive" for later. A network listener is that case: nobody sends it a message, it
must run from the boot on and come back after a crash. D19 2.3's rule ("a server may supervise its own helper processes,
with D11's numbers") is the pattern, but a listener is nobody's helper. **The decision: servers nobody calls are init's,
with D11's policy.** So:

- **`/etc/daemons`** (a new `/etc` format, frozen once shipped; a base config file, D15/D22's three-way compare; root's,
  not writable by others, else ignored with a log line): one line per server, `NAME NICE PROGRAM [ARGUMENTS]`, the layout
  of `/etc/svctab`, the program text at most `SVC_TEXT` (79 characters, six words), **at most 4 lines** (init keeps them
  in a 4 x 80-byte table: +0.4 KB of data for init, which lives for ever). One parser: a small libz helper shared with
  boot's svctab reader (the main session moves boot's into libz). Shipped with every line commented out
  (`#wasabid 0 /sbin/wasabid --init`): **off by default**.
- **init** starts each line's program after it has started `ifup -a` (waiting for neither), as root, standard input
  `/dev/null`, output and errors to `/dev/kmsg` (6.5), and starts it again when it ends, with D11's numbers (0, 1, 2, 4 s;
  given up after 5 ends in 60 s; logged in D11's format, "init: wasabid: /sbin/wasabid (pid N) ended, status S"). init
  waits in `sys_waitcon`, which has no timeout, so the back-off is an `alarm` with a handler and no `SA_RESTART`: the wait
  returns -EINTR and init's loop starts what is due. An end with status 0 is a stop (not started again until the next
  boot; root may start the program by hand). **Status 75 is init's own "start me again at once"** (not sysexits'
  meaning): no back-off, but it counts toward the five ends, so a daemon restarting in a loop is given up.
- **`RESTART`** ends the daemon with 75 after it closed its sockets; only a daemon started with `--init` accepts it
  (one started by hand answers `ERR` "not supervised: nobody would start me again").
- The telnet server and a later `sshd` (D20 7, D21 4.6) are further lines of the same file: one way for every listener.
- **Amended by name** (section 13): D11 1 and 4.4 ("keep alive" decided here), D19 2.3 (the supervision rule, also for
  init), D20 7.2 (its open point closed).

**Not taken:** an inetd (one process that listens for others: Wasabi's slots, streams and `FREE` across connections need
a daemon that lives, and an inetd is one more resident process on a 2 MB machine); a "keep alive" field in D11's kernel
table (a kernel change for what a program already does: init supervises the logins).

## 8. Testing Zenix on the real A1200 (the main purpose)

The A1200 dual-boots (the boot menu, `bootzenix`, `reboot amigaos`). With both daemons, a session on the PC can:

1. **In AmigaOS** (its Wasabi, encrypted, port 1234): copy a new Zenix update bundle and its `.sig` to the boot
   partition (`Zenix/zenix-update.zup.part`, renamed when whole: D15's rule; the signature once D21 signing ships), then
   `wasabi run bootzenix`.
2. **Zenix starts** (zenixboot installs the bundle; D15's trial), init starts `wasabid`, and the PC waits for port 1236
   (`wasabi wait up` across the two ports needs a client addition, 11).
3. **The tests run:** `wasabi run` of the test scripts (their output and exit codes, as the serial-tap scripts give them
   today), `text`/`wait text` for the console, `grab` for a picture of the real display, `debug` for the kernel log,
   `health`, `speedtest` for D19's speed table.
4. **Back:** `wasabi reboot --to amigaos --yes` (or `--to zenix`); AmigaOS's Wasabi answers on 1234 again.

**What it cannot do** (said in the page and the owner's checklist): a Zenix that hangs or panics before or without the
daemon cannot be reached; the A1200 has no hardware watchdog, so a hang needs the owner's reset. **A later step** (owner
question 3): a boot started "unattended" (a word in the boot note) makes a kernel panic show its screen for 30 s and then
reset into AmigaOS, where `zenixboot LOG` would print the previous Zenix boot's kernel log, if the log area at the top of
chip RAM survives AmigaOS's start (*not verified*: checked first in that step).

The shared-machine rule (CLAUDE.md "Testing": message the other sessions before using the A1200) applies to both
daemons.

## 9. Security, said plainly

- **What the key gives:** everything. The daemon runs commands as root on a machine without an MMU: whoever has the key
  owns the machine, its files, the NAS mounts and the `ssh` keys. It is a remote-control daemon by design, as AmigaOS's
  wasabid is.
- **Version 1 is not encrypted.** The key crosses the network in the clear at every command; anyone who can watch the
  home network (a compromised device on it, an open Wi-Fi) can read it and then control the machine. So: **a key of its
  own** (the client's per-host keys, 11: never the AmigaOS Wasabi's key, which would then cross the network in the clear
  too); off by default; home-network addresses only (3.2); forwards on 127.0.0.1 behind NAT; a wrong key costs a second;
  refusals counted and shown at the next `wasabi ping`; its own port, so the pin on AmigaOS's encrypted port stays
  meaningful. A 16-character minimum does not make a key strong: the page says how to make one.
- **Encryption later, where it is affordable.** Wasabi's TLS is TLS 1.3 with Ed25519 certificates and client
  certificates. D21's library speaks TLS 1.2 as a client only; a TLS 1.3 **server** with client certificates is not in
  D21 at all (its 3.2 names a 1.3 client layer as a possible step). Its cost per connection on the stock A1200 is two
  X25519, one Ed25519 signature and two Ed25519 verifications: about 17-34 s by D21 7.3's table, and Wasabi makes a
  connection per command: unusable there; milliseconds to a second on the PiStorm and the 060. A TLS 1.3 mode with an
  external pre-shared key (no public-key step per connection) would be cheap on every machine and stay inside TLS; both
  are a later step with their own decision (owner question 2). Not taken: a Zenix-only encrypted scheme outside TLS (a
  second security design in the owner's client, for one system).
- **Without an MMU** (D8, D19 9.2, D21 9.1): any program on the Zenix machine can read the key from the daemon's memory or
  the file; mode 600 keeps honest programs and other users out, nothing more.
- **Everything from the network is untrusted:** every length, count, path and string is checked before use; frames
  other than the streamed `DATA` of `PUT`/`GET`/`SPEED` are at most 4 KB (a longer one is refused with `ERR` and the
  connection closed); the daemon's own `DATA` frames are at most 4 KB, so a read error never breaks a promised length.
  The frame parser is fuzzed.

## 10. Size and memory (*estimates*; measured at their steps)

| Part | Size |
|---|---|
| Frames, `HELLO`, caps, discovery, the address filter, the configuration | 4-5 KB |
| Files, the mode and date mapping, the network-mount helper | 3-4 KB |
| `RUN`, slots, `FREE`, `PS`, `KILL`, `SPEED` | 4-5 KB |
| `DEBUG`, `HEALTH`, `INFO`, `REBOOT` and its guard | 3 KB |
| `GRAB`, `SUM`, `TEXT` with its wait (`regex.c` 4.6 KB), `KEY`, `INPUTS`, `SCREEN`, `WINDOWS` | 8-9 KB |
| libz's part (sockets, formatting, files, spawn) | ~6 KB |
| **wasabid code** | **30-35 KB** |
| Data: 4 connections at most, 2 x 4 KB buffers each (allocated per connection); a `GRAB` line; stack 8 KB | ~12 KB idle, ~45 KB with 4 connections |
| The kernel (6.1-6.5) | ~0.9 KB, every machine (the RAM floppy had 24,256 bytes of headroom and the mount tests' floppy 7,872 after D20 step 1; D23's 0.3 KB and the pty step's 2-3 KB are queued against the same) |
| init: `/etc/daemons`, the supervision, the table | ~0.8 KB code, 0.4 KB data |

On the stock A1200 with SLIP up (1,159 KB free with one login, netd 116 KB: D19, *measured in FS-UAE*) the daemon idle
takes about 45 KB, and a test session (two connections, one `RUN`: `sh` and the command) about 60 KB plus the command.
A machine that uses the network only for the daemon pays netd too. The 1 MB chip-only machine (232 KB free with SLIP up)
can run it with no second session.

## 11. For the Wasabi project (the client; owner question 1)

Additions behind caps, so AmigaOS's daemon and older clients are untouched (each claimed in Wasabi's `PROTOCOL.md` by
the session working on Wasabi, not here):
- **`TEXT`** (a new tag; 0x59 is free): payload `u32 flags` (bit 0: with the cells' style and colour), `u32 console` (0:
  the shown one), `u32 timeout` (seconds, 0: at once), `str pattern` (empty: none); answer `DATA` (the header and cells of
  6.1) then `END`, or on a timeout `DATA` (the grid as it was) then `ERR` "timeout". The client raises its socket timeout
  above the wait. Commands `wasabi text [--console N]` and `wasabi wait text REGEX [--timeout S]`; an MCP tool for each
  (Claude reads the screen as text instead of a picture).
- **`REBOOT`'s flags** behind caps `rebootto` (AmigaOS's daemon would ignore them and do a plain reboot): `wasabi reboot
  --to amigaos|zenix [--force]`.
- **Across the two systems:** `wait up` and `reboot --wait` try both ports (1234 and 1236) or probe again, so an AmigaOS
  -> Zenix -> AmigaOS cycle is one command each.
- **Per-host keys** (a key for `ADDRESS:PORT` in the configuration), so Zenix's key is not the AmigaOS Wasabi's.
- **A port known to be plain** (remembered like the pins, or `--plain` in the host's configuration): no failed TLS try
  per command, and the "no encryption" warning once per host, not on every command (it now reaches the MCP server's
  output too).
- **caps `zenix`:** `PS` shows pids; `PUT` may send a Unix mode (bit 31); paths are Unix paths (no `SYS:` checks).
- **Tests on the Wasabi side** against a Zenix in FS-UAE (12.2's set-up, which the Wasabi project may run).

## 12. Tests

### 12.1 On the PC

- **The daemon's core on the PC** (`net/wasabi/`: frames, the command parsers, the mode and date mapping, the guard's
  list, the configuration, the descriptor budget; system-call free, built for Linux and driven by a test, as netd's and
  telnet's cores).
- **A conformance client of Zenix's own**, `tools/host/wasabi-check.py` (Python, standard library, written from
  `PROTOCOL.md`): every command Zenix offers, wrong keys and the pause, short and oversized frames, unknown tags, a
  transfer that stops moving (dropped after 10 s), one connection over the cap, the address filter (the core with a
  configured peer address outside the allowed ranges).
- **The frame parser fuzzed** (libFuzzer, one run at a time with a memory limit).

### 12.2 In FS-UAE (`tools/remote-test.sh`)

The emulated A2065 with FS-UAE's slirp and a port forward from the PC's 127.0.0.1 (a port taken from the test's slot,
never 1234-1237, which the client probes for emulators) to Zenix's 1236 (FS-UAE's `slirp_redir`, which the installed
binary names: its syntax checked in step 1; else the SLIP path), the test's servers in a user and network namespace as in
`tools/eth-test.sh`:
- **c, conformance:** `wasabi-check.py`: `PING`, `INFO`, `HEALTH`; `PUT`/`GET` of 100 KB compared; a `PUT` cut half-way
  leaves the old file; `PUT` of `/sbin/wasabid` refused; `GET` of `/dev/zero` refused; `LS` against `ls -l`; `RUN` with
  output, stderr apart and merged, exit codes (0, 3, 130 after `FREE`, 139 after a fault), two slots and a third refused,
  `run 'sleep 100 &'` not holding its slot, a detached run; `FREE` with and without bit 0; `PS`/`KILL` and the refused
  targets; a wrong key and the pause.
- **s, the screen:** `KEY` text `echo hello\n`, then `TEXT` waiting for `hello`; raw keys Amiga+F2, then `TEXT` of
  console 2 (its login prompt); `TEXT` of a never-used console (-ENXIO, no allocation); `GRAB` compared pixel by pixel
  with the cage rig's screenshot of the same quiet moment (`tools/shot.py`), at 3 and 4 planes; `SUM` before and after a
  change; `WINDOWS`' one line.
- **d, the log and reboot:** `DEBUG` shows a line the test causes (a scratch image's `mount`); a log that wraps under a
  slow subscriber says how much was lost; heartbeats; a `DIAG_PANIC` build's next boot replays the panic to a new
  subscriber; `REBOOT` with the AmigaOS bit: the boot note set (the next boot's `dmesg -p`; FS-UAE has no AmigaOS on that
  disk, so the menu shows), the seed file written, the guard while a `zup` test runs.
- **r, the real client:** when `ZENIX_WASABI` names a Wasabi checkout (the repository names no path of this PC), its
  `wasabi` command and its MCP server's tool list against the daemon (`ping`, `run`, `put`, `get`, `ls`, `grab`,
  `health`, and `text` once Wasabi has it); skipped otherwise, as `stage0-68k-test` without unicorn.
- **l, over the cable:** the same daemon over SLIP through `zenix-slirp -f tcp:127.0.0.1:PORT:1236` (part c's file copy
  and a `TEXT` wait), on the stock A1200 and the 1 MB machine, the memory measured.
- **b, at boot:** `/etc/daemons` with the line: the daemon answers after boot without a login, its lines in `dmesg` and
  none on console 1; killed five times: given up and logged; `RESTART`: started again at once; a missing configuration:
  a stop, not restarted.

### 12.3 On the owner's A1200

Section 8's cycle end to end once (with the owner's yes, the shared-machine rule): from AmigaOS to Zenix and back with no
hands, the tests' output, a picture of the real display, the speed over the CM4's Ethernet; the stock A1200 over the
cable.

## 13. For the main session (interfaces to claim; not owner questions)

- `SYS_console` ops `CON_READ`, `CON_LINE`, `CON_INPUT` (proposed 4, 5, 6; descriptor-named as D20 4.5, root only, their
  structs' first sizes, -ENXIO for an unused console), the `KLOG_TOTAL` flag of `SYS_klog` (*replaced by decision 1:
  `KLOG_OLDEST`, nothing new*), `/dev/kmsg` (root, write
  only, `FT_KLOG`): frozen once built, in CLAUDE.md's list. D16's list of input producers gains `CON_INPUT`; the "kwork
  only" comments in `tty.c` and `term.c` are corrected.
- `/etc/daemons` (7) and `/etc/wasabid.conf` (5.7): new `/etc` formats base programs read, frozen once shipped; init's
  exit status 75; `/etc/daemons` a base config file in the package. The shared svctab/daemons parser moved into libz.
- **D11 1 and 4.4, D19 2.3, D20 7.2** amended by name with section 7's decision; its own DESIGN line (D24 carries it).
- `/sbin/wasabid` in `SBINAPPS` and `tools/mkroot.sh`'s `/sbin` list, its page `docs/man/wasabid.txt`, `commands.md`,
  `features.md`; `init`'s page gets section 7, `reboot`'s nothing.
- **The `dmesg` wrap bug** of 6.4: fix next. (*Done before this was built: audit 2026-10-26 F10, decision 1.*)
- **The feature halt:** the roadmap's approved order names the remote control (block 2); the owner's yes to this note is
  the yes to `wasabid`, the five kernel interfaces and `/etc/daemons`.
- **Review depth:** two reviews done (a root daemon on the network, kernel interfaces, a D11 decision); a second look at
  `CON_INPUT`'s code before its merge.

## 14. Build order (each step small, merged with its tests; the full suite before each merge)

0. *side:* **the daemon's core on the PC** (`net/wasabi/`), the conformance client, the fuzzer, its host test (the core's
   OS layer named in its header, approved by the main session before code: coherence first).
1. **The daemon without the screen:** section 4 but `GRAB`, `SUM`, `SCREEN`, `WINDOWS`, `KEY`, `INPUTS`, `TEXT`;
   `KLOG_TOTAL` (*replaced by decision 1*); `/etc/wasabid.conf`; started by hand. FS-UAE parts c, d, r, l. **The first thing a session can use:** run
   and copy on Zenix from the PC. (Network-mount files wait for D23 step 1's cancel mark: until then they are refused on
   an `nfs` mount.)
2. **`/dev/kmsg`, `/etc/daemons` and init's supervision** (7). Part b.
3. **`CON_READ`, `CON_LINE`, `CON_INPUT`** with their own tests (`termtest`'s checks of the grid against `CON_READ`, the
   text path against the serial path, raw keys against the keyboard test), then `TEXT`, `GRAB`, `SUM`, `SCREEN`,
   `WINDOWS`, `KEY`, `INPUTS`. Part s.
4. **The A1200** (12.3) with the owner; the Wasabi additions of section 11 in the Wasabi project alongside.
5. *Later, with the owner's yes:* the unattended panic reset and `zenixboot LOG` (8); encryption (9).

## 15. Open questions for the owner (one line each, the recommendation in brackets)

1. The remote control speaks your Wasabi's protocol, so `wasabi`, the MCP server and the apps work with Zenix too, and the Wasabi project gets a few additions for it (the console's text, `reboot --to amigaos`, a key per machine)? [Yes]
2. While the A1200 runs Zenix, it may be reached unencrypted (its own key, its own port, home network only, off unless you switch it on), an exception to "the A1200 only encrypted"; encryption comes later? [Yes]
3. Later: a Zenix that panics during an unattended test resets into AmigaOS after 30 s, so the test can go on remotely? [Yes, after the first version works]

## 16. Reviews

Two independent read-only reviews of revision 1 (2026-10-27); every finding is worked in above.

**Correctness, robustness, security** (read against the kernel's console, keyboard, serial, tty, proc, signal, socket
and klog code, user/init.c, reboot.c, dmesg.c, sh.c, and Wasabi's protocol, client and MCP server): *blocker* - the
design did not fit 16 descriptors (5.1: a budget table, 4 connections, 2 slots, one transfer). *Should fix* - `SYS_klog`
cannot show a loss and no call gives the previous boot's reason (5.4, 6.4; and `dmesg`'s wrap bug); raw keys written into
the keyboard ring race the interrupt (6.3: `key_code()` from the call, codes refused, modifiers released, the text path
through `tty_input` with its count); `setuid` around calls is impossible (5.2: root only); init cannot log to the kernel
log or time a back-off (6.5, 7: `/dev/kmsg`, `alarm`); `RESTART` by hand (7); `REBOOT` skipped the seed and could lose
its `OK` (5.5: `reboot` as a child, a bounded drain, the guard advisory); `FREE`'s bit 0, timers in the loop, G4, busy
slots at `RESTART`/`QUIT` (4, 5.1); SIGPIPE, SIGCHLD and when `EXIT` is sent (5.1); the 10 s rule as progress (3.2);
frame sizes against small buffers (9); file commands on devices, NFS detection by resolved path, a hung local server
(5.3); `GRAB` from a snapshot of plane addresses (6.2: `CON_LINE`); the cell format and an unused console's allocation
panic (6.1, 6); one shared client key and NAT (9, 3.2); the client's `wait up` across ports, discovery on 1236, the
warning on every command (3.1, 11). *Minor* - `PS`'s and `HEALTH`'s fields that do not exist (4, 5.6); the exact caps
string (4); the client's real command names (1); `--region` needs `WINDOWS` (4); `TEXT`'s timeout answer (11); `PUT`'s
mode for new programs (4); the test's `allow`, `-f`, the forward port (12); `KILL`'s targets, the controlling tty (4,
5.1); kind numbers aligned with `KEY` (6.3); the TLS 1.3 server not in D21 (9); FS-UAE figures labelled (10); silence to
refused addresses (3.2); the dependence on D23's cancel mark (14).

**Architecture fit, frozen interfaces, size** (with `regex.c` measured): *blocker* - `CON_SHOWN` froze the display's
internal layout while scrolling moves it and the 2 MB diet may change it (6.2: `CON_LINE`, the kernel keeps the layout).
*Should fix* - init's supervision needed hidden kernel interfaces (6.5, 7: `/dev/kmsg`, `alarm`, a 4-line table, the
shared parser); "the supervision rule already exists" was not true: it is a D11 decision, now said and the notes amended
by name (7, 13); 8 connections would take every TCP connection netd's small profile has (5.1: 4); the hardware table is
netd's drivers' only (4, 5.6: the kernel log's `pi:` line); ops without a descriptor changed D20's frozen op-word rule (6:
descriptor-named, `/dev/vcs*` dropped); vcs inode numbers would collide with login's console test (gone with the
devices); refusing `INSTALL` while advising `PUT` over the daemon dropped Wasabi's own safety rule (4: `PUT` of the
daemon refused, a trial instance, packages); owner questions: the root question was a technical call (dropped, 5.2), the
encryption question hid the conflict with the owner's TLS-only rule (15.2); the network-mount helper's second copy (5.3);
a daemon given up for a whole boot by a missing configuration or a cable (5.7). *Minor* - the floppies' headroom (10);
memory figures that left netd out (1, 10); `regex.c`'s place and size (6.1); the TLS server's cost and the external-PSK
option (9); zenix-slirp's `-f` (12); the previous boot's reasons (5.4); the keyboard ring (6.3); the controlling tty
(5.1); `REBOOT`'s flags behind caps (4, 11); the exact caps (4); `PUT`'s bit 31 (4); `FREE`'s bit 0; `RESTART`/`QUIT` with
busy slots; resolved paths for NFS (5.3); `/etc/daemons` a base config file (7); the `.part` and `.sig` in section 8;
a private-notes quote replaced by the owner's decision (2); wording (init's consoles, pid 1 is boot); step 0's directory
decided (14: `net/wasabi/`); the klog across a reset unverified (8); status 75 init's own (7). *Found sound:* Wasabi's
protocol as the choice, port 1236, the pins by address and port, the plain fallback, tag 0x59 free, 64 KB frames, the
trial port, the serial path, `KLOG_PREV`, `BOOTNOTE_*`, the memory figures quoted, BearSSL's gaps, D11 4.4 quoted right.

Revision 2 keeps the shape of revision 1 (one daemon speaking Wasabi's protocol on its own port, plain on the home
network first, the console as text and pixels, keys through the existing input paths, init keeping listeners alive);
both reviewers judged that shape sound. It grows the kernel part from three interfaces to five (all small, none
allocating, the display's layout kept inside the kernel) and shrinks the daemon (2 slots, 4 connections, no user
setting, `/dev/vcs*` dropped).

Last checked against the code: 2026-10-27 (main at 22fd4ba).

## Owner decisions (2026-10-28)

The owner answered "Yes" to every recommendation of this note's questions.

## Main session decisions, 2026-10-28 (step 0)

The core's header (`net/wasabi/wasabi.h`, its OS layer and entry points) was approved before code; these are the main
session's answers to the questions step 0 raised, built so in `net/wasabi/` and checked by `tools/host/wasabi-test.sh`.

1. **No `KLOG_TOTAL`** (6.4): the daemon uses `KLOG_OLDEST`; a subscriber whose position fell below the oldest
   character gets "[wasabi: N bytes of the kernel log were lost]" and goes on from there. The `dmesg` bug of 6.4 was
   fixed by audit F10. 6.4, 13 and 14 are amended by name above.
2. **ERR's code:** the nearest AmigaDOS code from a fixed table in the core (`wsb_errcode`, one place, host-tested), so
   the client's names read right and scripts that test codes keep working: `ENOENT` 205, `EEXIST` 203,
   `EACCES`/`EPERM` 223 for a write and 224 for a read (223 where the core cannot tell), `ENOSPC` 221, `ENOTEMPTY` 216,
   `ENOTDIR`/`EISDIR` 212, `EBUSY`/`ETXTBSY` 202, `EROFS` 214, anything else 0 (and 0 for the daemon's own refusals).
   The message is the daemon's words, then the error number's words (libz's `zstrerror`).
3. **REBOOT's order** (5.5): `OK` to the requester, the farewell on the log stream, the other connections drained
   and closed (at most 1 s), then `/bin/reboot [amigaos|zenix]` as a child with its output in a pipe, the requester's
   connection kept open meanwhile. If `reboot` comes back, its words follow the `OK` as an `ERR`. Today's client
   closes after the `OK` and loses that `ERR`: reading it is one more Wasabi addition for section 11.
4. **A bad `/etc/wasabid.conf`** is one with a line the daemon does not understand, a key under 16 or over 127
   characters, more than 8 `allow` lines, a second `key` or `port`, or a name that is not 1-31 of
   `A-Z a-z 0-9 . _ -`; so is a file that is not root's regular file with mode 600, or over 2 KB. Each is a stop:
   status 0 and a log line naming the line and the reason.
5. **`--trial`:** no discovery sockets (the main daemon holds those ports), the refused count never written, and
   `RESTART` refused as for a daemon started by hand.
6. **Detached `RUN`s** are reaped from a table of 8 (the commands let go of by `FREE` share it); a ninth gets `ERR`
   "too many background commands".
7. **The console descriptor:** the moment a `CON_*` call opens `/dev/ttyN` uses one of the 2 spare descriptors of
   5.1's budget, never during a spawn.
8. **The console structs:** the core sees `struct wsb_con_head` and `struct wsb_con_disp` (`wasabi.h`), which the OS
   layer fills from the kernel's; the kernel's own layouts are designed at step 3.
9. **Pipes:** plain pipes; the daemon reads only after `POLLIN`, and a child blocking on a full pipe is the
   back-pressure.
10. **The console commands before step 3:** the core implements all of section 4 and is checked against pretend
   consoles on the PC. Caps stay truthful: the OS layer's `wsb_os_con_ops()` answers whether the kernel has the
   console operations (0 until step 3 on Zenix); without them `grab`, `screen`, `windows`, `key`, `sum`, `inputs` and
   `text` are left out of caps, and those commands answer `ERR` "not in this Zenix yet".
11. **`SPEED` through a file** needs its size plus 64 KB free on that filesystem (Wasabi's 8 MB would refuse every
   small Zenix disk).

## Main session, 2026-10-28: step 0 reviewed

- Merged with its host test in `make check` (`wasabi-test`: core, check, size; the fuzz part is run by hand, one
  fuzzer at a time). The real Wasabi client's part runs when `ZENIX_WASABI` names a checkout.
- **wasabid is built at -Os** (step 1's Makefile lines): all of it is cold code; 37,100 bytes of text there, above
  section 10's estimate, measured again linked at step 1.
- **GRAB's "moved" note** is logged, not sent: today's client fails on any frame but DATA inside GRAB. Sending it is a
  section 11 addition (the client accepts it), then the daemon sends it.
- **HEALTH** has no largest-free-block key (Zenix has no call for it); the client's `health` tolerating a missing
  `mem.chip.largest` is a section 11 addition, no new kernel call. The disk key is `disk.root.free.kb`, in KB.
- The step's small choices (TEXT's 16-byte header and cells, LS read whole up to 32 KB, PUT's `.wsb-N`, the refused
  count saved at most once a minute, the two extra bad-configuration cases, SCREEN's bit 0 doing nothing, the banner
  "Zenix VER") are accepted as built. FREE's "freed" path and a down network are tested in FS-UAE at step 1.


## Steps 1 and 2 as built (branch wasabi1, 2026-10-28)

Built as section 14 steps 1 and 2 say, with the main session's decisions above; the choices the note did not make,
taken conservatively:

- **The OS layer** is `user/wasabid.c` (the core's `wasabi.h` interface over libz and the system calls, as
  `tools/host/wasabi-host.c` is over Linux's). At its start it closes standard input and output (the core counts the
  log, standard error, as its one inherited descriptor), sets the commands' environment once (`SYS_envset`) and
  ignores SIGPIPE. `wsb_os_con_ops()` is 0: caps leave the console commands out until step 3. A file command never
  opens `/dev/ttyS0` under any name (-EBUSY: opening it takes the serial port, SLIP's line). Interfaces are asked
  through the listener's socket (no descriptor of its own). libz gained `zrealpath` (realpath(1)'s resolver, moved,
  no printf) and `svc_line` (boot's `/etc/svctab` parser, moved and shared with init, the "one parser" of section 7).
- **Its own heap** (as netd's, D19): libz's malloc rounds blocks up to a power of two and grows its chunks four times
  over, which held 112 KB in `ps` with one connection (*measured in FS-UAE*); first fit in 16 KB regions taken when
  needed and given back when empty brings it to 88 KB. The core's send queue is 4 KB exactly (`WSB_OUTCAP`, so a
  frame's payload is at most 4,091 bytes: still Wasabi's "at most 4 KB"), one libz-sized block, not the next size up.
- **Found in the core and fixed** (both checked in FS-UAE, part c): the discovery sockets were never opened when the
  daemon started on a down network (now opened with the listener, when the network comes up); and when netd ends
  under the daemon its dead listener woke every `poll` at once (a busy loop): the listener and the discovery
  sockets are now closed and opened again at once (which starts netd again, D11), then every 10 s.
- **While a reboot is under way** (from the `OK` until `/bin/reboot` comes back) a new connection gets `ERR` "busy",
  as during `QUIT`: a client waiting for the machine to come back no longer reaches the old daemon.
- **GRAB's note** (the main session's decision) is sent as a `STDERR` frame only when the client's flags word after
  the name has bit 0 (`WG_NOTES`, Wasabi 0.11b3); otherwise only logged. Host-checked with a display that changes
  while it is read (`tools/host/wasabi-check.py`, `.moving`).
- **Size** (*measured*, -Os): `/sbin/wasabid` 55,496 bytes of image (62,592 on the disk with its relocations; section
  10 estimated 30-35 KB: the core alone is 41.7 KB with `regex.c`, libz's part 10 KB), 66 KB loaded with its 8 KB
  stack. **Memory** (*FS-UAE*, `ps`): 72 KB idle, 88 KB with one connection running a command; the stock A1200 with
  SLIP up and a session leaves 943 KB free. On the 1 MB machine (one login and the test's shell) 201 KB are free
  before it and 108 KB with it: enough for file copies, not for a command (`could not start /bin/sh: out of memory`).
  A package of its own, `wasabid` (not in the base group).
- **FS-UAE's slirp takes no port forwards**: `slirp_redir` (FS-UAE 3.2.35) is accepted and never opened, so every
  FS-UAE part runs over SLIP through `zenix-slirp`'s forwards (12.2's "else the SLIP path"), in a user and network
  namespace (`tools/remote-test.sh`: parts c, d and r in one run of two boots, l on 1 MB, b init's daemons).
- **Not checked in FS-UAE**, said here: the replay of a previous boot that ended in a **panic** (a `DIAG_PANIC`
  kernel panics at every boot, so its next boot cannot start the daemon; the replay of a boot that ended with
  `reboot` is checked, the panic case on the PC); "none of its lines on console 1" (true by construction, standard
  output and error are `/dev/kmsg`; reading a console needs step 3's `CON_READ`).
- **An idle connection is dropped after 10 s** (3.2's progress rule counts a connection without a command as waiting
  on its peer): Wasabi makes a connection per command, so this never meets its client; a script holding one open
  must keep sending.
- **FREE's "freed"** needs the slot's own `sh` held by a server: a child stuck in a call is reaped later, and sh
  itself ends at SIGINT ("stopped"). The test holds sh with the test server `msgpingd -fsstall` (a new test mode:
  every request but the root's never answered).
- **`/dev/kmsg`** is opened write only (`O_RDONLY` and `O_RDWR` -EINVAL), root by the effective id. **init** is built
  at -Os (cold code: its text grew by 2.1 KB, 2.6 KB of memory for ever), reads `/etc/daemons` in 128-byte pieces
  (lines over 127 characters skipped, said, unless a comment), keeps `/dev/kmsg` open (one descriptor), and its alarm
  re-arms itself only while init is in its wait. `/etc/daemons` ships commented out, in the base package's config
  files, mode 644 on every image.
- **Floppies:** the kernel grew by 408 bytes (18,868 bytes left on the RAM floppy, 2,484 on the mount tests'); the RAM
  disks lost 5 blocks (176 and 32 free).

One independent read-only review of init's supervision and `/dev/kmsg` (2026-10-28): no blocker; two should-fix
findings (an alarm that could keep waking init every tick after a race at its cancel; a child's own exit status -4
taken for the alarm) and the minor ones (the refusal not in the kernel log, a long comment, CR and trailing spaces,
a nice that could wrap, `O_RDWR`, the files' modes on the image, a comment on the wrong function, a bare -1) are fixed.
Left as they are: a daemon whose exit status is -1 or -2 (negative statuses look like `waitnb`'s own answers, an old
trap of init's sessions too: a kernel mask to 0-255 would end it); `/dev/kmsg`'s bytes reach the serial port
unfiltered and at its speed, as a service's output does.

## Main session, 2026-10-28: steps 1-2 merged

- **libz's allocator** (power-of-two classes, 4x chunk growth) costs the daemons dearly on small machines: wasabid and
  netd keep their own heaps for now. The allocator's policy is decided once, for every program, in D27 step 1 (the
  allocator moves into the C library's port there), measured, not here.
- **The 1 MB machine** (not one of the owner's) is not a target for a command through wasabid with a login.
- **Exit statuses -1/-2 taken for waitnb's own answers** (init's old trap): a frozen convention, so no kernel mask now;
  it goes on block 2's security audit as an item to weigh (a new wait form would be the additive fix).
