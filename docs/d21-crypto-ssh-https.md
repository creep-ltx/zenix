# D21: the crypto library, `ssh` and HTTPS (design note, revision 1)

Status: **design, revision 1, nothing built.** Roadmap block 2 (owner 2026-10-26: "I need to be able to SSH into my
Linux machine, properly"): "one small crypto library: modern, small and constant-time, e.g. BearSSL (MIT) for TLS and
Dropbear's client (MIT) for SSH, with Ed25519/X25519/ChaCha20 and a real random source; it also signs `zpkg`'s
packages; then `ssh` (a proper client, keys, known hosts) and HTTPS for `wget`". D19 section 10 step 11 puts it after
telnet. The short version is DESIGN.md D21. Facts about today's code are *read* from main at f10de79 (2026-10-27);
sizes marked *measured* were compiled here with the project's m68k-elf-gcc 16.1 (`-m68020-60 -Os`, linked with
`--gc-sections`, 2026-10-27); every time on a 68020 is an *estimate* and says so. Outside facts (licences, project
status, OpenSSH's defaults) are *read* from the projects' own sources and pages on 2026-10-27.

## 1. What the owner gets, in plain words

- `ssh user@pc` from any console logs in to the Linux PC with a key made on the Amiga: the PC's shell in colour, the
  Swedish keys working, Ctrl-C going to the PC, `~.` to leave, as on any Linux machine. The first time it shows the
  PC's fingerprint and asks; a changed key later is refused loudly. Password logins work too.
- `ssh-keygen` makes the Amiga's own key; its one-line public half goes into the PC's `~/.ssh/authorized_keys`.
- `wget https://...` fetches from the web with the certificate checked (in `wget`'s own step).
- **Updates and packages are signed:** zenixboot and `zpkg` refuse a bundle or package list that is not signed by
  the owner's key (D15 step 7).
- **Nothing changes for a machine that does not use it:** no kernel change, no new system call, no resident part.
  The code is in the programs that need it.
- **Honest about speed:** on the PiStorm and the 060 a login takes a moment; on the stock 14 MHz A1200 the first
  seconds of a login are public-key arithmetic (about 10-25 s, *estimate*, section 7), and HTTPS there is slow enough
  that some servers may give up (section 7). Typing in a running session is not slowed.
- **Honest about safety:** without an MMU any program on the machine can read any other's memory and the disk. The
  key file is protected from honest programs and other users' tools, not from hostile code running on the Amiga
  (section 9).

## 2. Today (read)

- **Random numbers** (D19 9.3, step 2 as built): the kernel's pool (BLAKE2s, ChaCha20 with fast key erasure);
  `/dev/urandom` never waits; `/dev/random` waits until 256 bits were credited once, then never waits again and polls
  readable (Linux 5.6+ semantics); `O_NONBLOCK` gives -EAGAIN while unseeded. A fresh machine needs about 128 key
  presses; the seed file `/var/lib/random-seed` credits only on a writable persistent root.
- **Sockets** (D19 steps 3-9): BSD sockets in libz, `poll` over sockets and ttys (D16), `getaddrinfo` through netd,
  SLIP at 115,200 (11.5 KB/s) for every Amiga, the A2065 in FS-UAE, GENET on the PiStorm.
- **The terminal side** (D20, step 0 built): `net/telnet/` holds a system-call-free core; its key translation
  (`tk_keys`, the Unix column: Backspace 0x7F, Del `ESC[3~`, Latin-1 keys as UTF-8) and its stream filter in the Unix
  mode (`tf_*`: UTF-8 from the host shown as Latin-1, `?` otherwise) are exactly what an ssh session to Linux needs.
  telnet runs at nice 1 and writes the screen 256 bytes a call so netd can drain SLIP's 1 KB ring (D20 section 6).
- **Hashes in the tree:** `user/sha256.c` (SHA-256, linked into `login`, `passwd`, `sha256sum`, zup and zenixboot;
  "about 1 ms per 64-byte block on a 68020", the project's estimate). Passwords are 1000 rounds of SHA-256 (`pw.c`).
- **Updates** (D15): a bundle is a 96-byte header (with a SHA-256 over header bytes 0-63 and the table of contents),
  the table (each part's SHA-256), then the parts. Signatures are step 7, not built: "Ed25519 over the table of
  contents, two public keys built in (current and next), checked once per transport: by zenixboot for BOOT:, by
  `zpkg` for the network"; the private key on the owner's PC outside every repository. zenixboot is an AmigaOS
  program built by the same compiler, freestanding, already linking `sha256.c`.
- **Time:** `SYS_time` (seconds since 1970, 32 bits), set at boot from a battery clock (MSM6242 or RP5C01) when one
  ticks; no network time. The owner's A1200 has an Individual Computers RTC module (seen in photos, 2026-10-24); the
  stock A1200 has no clock.
- **CPUs:** every program is built `-m68020-60`; `tools/check-68020-60.sh` refuses the 64-bit `mulu.l`/`divu.l`
  forms (a 68060 traps on them and Zenix has no 68060.library). A program cannot ask which CPU it runs on (no call
  reports it).
- **Licence:** GPL-2.0-only; outside code must be GPL-2-compatible and keep its header (miniz MIT, lwIP BSD-3);
  licence notices travel in `/usr/share/licenses/` (owner decision, D19).

## 3. The library

### 3.1 The candidates (licences checked against GPL-2.0-only)

| | Licence | GPL-2-only? | What it has | What it lacks | State |
|---|---|---|---|---|---|
| **BearSSL** (Pornin) | MIT | yes | TLS 1.0-1.2 client and server, X.509 validation (`x509_minimal`), RSA, ECDSA P-256/384/521, X25519 (ECDHE), AES (constant-time `ct`), GCM, ChaCha20-Poly1305, SHA-1/2, HMAC, HMAC-DRBG. No `malloc`. **15-bit-word code paths (`i15`, `m15`) made for CPUs without a fast 32x32->64 multiply.** | **TLS 1.3; Ed25519**; tickets (RFC 5077) | "0.6, beta-quality" since 2018, but maintained for fixes: last commits 2026-04-06 (*read*, git log) |
| **Monocypher 4** | BSD-2-Clause OR CC0-1.0 | yes | X25519, EdDSA (BLAKE2b) and **Ed25519 with SHA-512** (the optional file), ChaCha20/XChaCha20, Poly1305, BLAKE2b, Argon2. Small, audited code base, test vectors in the tree | RSA, ECDSA, AES, TLS, X.509 | active (commit 2026-10-20) |
| **TweetNaCl** | public domain | yes | X25519, Ed25519, XSalsa20-Poly1305, SHA-512 in 100 tweets | ChaCha20; very slow (16 limbs of 64-bit products) | finished |
| **Mbed TLS 3.6** | Apache-2.0 OR GPL-2.0-or-later | yes, **under its GPL-2.0-or-later choice only** (Apache-2.0 alone is not GPL-2-compatible; a few `3rdparty/` files are Apache-only and must stay out) | TLS 1.2 and 1.3, X.509, RSA, ECDSA, X25519, AES, ChaCha20-Poly1305 | Ed25519; small size (its own config docs speak of 60-100+ KB for a TLS client), heap-heavy, the constant-time story is weaker than BearSSL's | active |
| **libtomcrypt + libtommath** (inside Dropbear) | Unlicense / public domain | yes | everything ssh needs | constant-time bignum (libtommath's `mp_exptmod` is not); large | active |
| wolfSSL | GPL-3.0-or-later or commercial | **no** | | | |
| OpenSSL 3 / AmiSSL | Apache-2.0 | **no** | | | |

### 3.2 Choice: one library, `crypto/`, made of BearSSL and Monocypher's Ed25519 half

- **BearSSL** for TLS 1.2, X.509, RSA and ECDSA verification, AES-GCM, SHA-1/2, HMAC and the DRBG. Its `i15`/`m15`
  arithmetic suits the 68020-68060 rule exactly: it multiplies 16-bit pieces, never needs the 64-bit `mulu.l` the
  68060 lacks, and is constant-time wherever the CPU's multiply is (section 9.2). No global writable data (*measured*:
  0 bytes of data in every linked test), so it works in pure (D13) programs.
- **Monocypher** for X25519, Ed25519 (sign and verify), ChaCha20 and Poly1305. BearSSL has no Ed25519, which ssh
  (OpenSSH's default host and user keys) and package signatures need, and Monocypher's one field implementation then
  serves X25519 too: BearSSL's TLS engine takes its X25519 through an `br_ec_impl` table, so a 40-line adapter gives
  TLS the same code (one curve25519 field in the system, the one that gets the assembler, 3.5).
- **Not taken:** Mbed TLS (TLS 1.3, but two to three times BearSSL's size on a 2 MB machine and no Ed25519);
  TweetNaCl (several times slower than Monocypher on a CPU where every multiply counts); libtomcrypt/libtommath (not
  constant-time where secrets are multiplied, and the largest); writing our own primitives (never: the risk of a
  subtle bug outweighs every saving).
- **TLS 1.3 is the gap, said plainly.** Most servers still accept TLS 1.2 in 2026 (no reliable figure for 1.3-only
  servers was found; the step measures the owner's own list: GitHub, Aminet, the NAS, the sites `wget` is used for). A
  server that refuses 1.2 gets a clear message ("this server needs TLS 1.3, which Zenix's wget does not speak yet").
  If that list ever needs it, the plan is a TLS 1.3 client layer on these same primitives and BearSSL's X.509 engine
  (TLS 1.3's client is simpler than 1.2's: one key exchange, AEAD only), its own design step with reviews; Mbed TLS
  under its GPL choice is the fallback if that is judged too risky (owner question 1).
- **Pinned versions:** BearSSL at commit 7bea48e (2026-04-06), Monocypher 4.0.3 (a release tag), each imported
  unchanged under `crypto/bearssl/` and `crypto/monocypher/` with its licence file, local changes as patches beside
  them (as `net/lwip/patches/`), and the notices in `/usr/share/licenses/` (D19 owner decision 6). Upstream fixes are
  followed: a security fix upstream is "fix when found".

### 3.3 Algorithms (what is offered, in order; nothing weaker is ever accepted)

**ssh** (client side; the owner's PC runs OpenSSH 9-10, which offers all the first choices):

| | Offered | Refused |
|---|---|---|
| Key exchange | `mlkem768x25519-sha256` (step 6, 3.4), `curve25519-sha256`, `curve25519-sha256@libssh.org`; plus `ext-info-c` and **`kex-strict-c-v00@openssh.com`** (the Terrapin fix, CVE-2023-48795: sequence numbers reset at every NEWKEYS, no message before KEXINIT) | every DH group (slow and needs big modular exponents), ECDH on NIST curves (one curve is enough), SHA-1 |
| Host keys (verify only) | `ssh-ed25519`, `ecdsa-sha2-nistp256`, `rsa-sha2-512`, `rsa-sha2-256` (RSA at least 2048 bits) | `ssh-rsa` (SHA-1), DSA, certificates (later) |
| Ciphers | `chacha20-poly1305@openssh.com`, `aes128-gcm@openssh.com`, `aes256-gcm@openssh.com`, `aes128-ctr`/`aes256-ctr` with `hmac-sha2-256-etm@openssh.com` | CBC, 3DES, `none`, MAC-then-encrypt MACs, SHA-1 MACs |
| User keys | `ssh-ed25519` (made by `ssh-keygen`, or the owner's own copied from the PC) | RSA/ECDSA user keys (signing with them costs minutes on a 68020, 7), later only if asked |
| Compression | `none` | zlib (a decompressor fed by the network is attack surface; SLIP's speed is the limit anyway) |

**TLS 1.2** (for `wget` and later the e-mail client): ECDHE only (X25519, then P-256, P-384 for the server's
choice), suites `ECDHE_{ECDSA,RSA}_WITH_CHACHA20_POLY1305_SHA256`, then `..._AES_128_GCM_SHA256`,
`..._AES_256_GCM_SHA384`; certificates signed with RSA (2048-4096, PKCS#1 v1.5 and PSS) or ECDSA (P-256, P-384); SNI
always; secure renegotiation refused (`BR_OPT_NO_RENEGOTIATION`); no RSA key transport, no CBC suites, no TLS 1.0/1.1.

**Package signatures:** Ed25519 (RFC 8032, pure, SHA-512), section 6.

### 3.4 Post-quantum key exchange for ssh (a later step, cheap)

OpenSSH 10.0 made `mlkem768x25519-sha256` its default; OpenSSH 10.1's *client* warns when a connection does not use
a post-quantum exchange (the server never refuses `curve25519-sha256`). Recorded traffic stays safe from a future
quantum computer only with it. ML-KEM-768 is cheap on a 68020 (16-bit coefficients, `mulu.w`; SHAKE on 32-bit halves):
roughly 0.3-1 s on the stock A1200 (*estimate*), against several seconds for X25519. So: `curve25519-sha256` first
(step 3), the hybrid as its own step (6) from a GPL-2-compatible ML-KEM (mlkem-native, Apache-2.0 OR ISC OR MIT, used
under ISC or MIT; or the CRYSTALS reference under CC0; the LICENSE files checked at that step); then offered first.
`sntrup761x25519-sha512` is not taken (one post-quantum method is enough; ML-KEM is the standard).

### 3.5 Where it lives

- **A static library**, `crypto/` (BearSSL, Monocypher, and `crypto/zc/`: Zenix's glue: random bytes, wiping, the
  X25519 adapter for BearSSL, ssh's ChaCha20-Poly1305 framing, the assembler files), built for Zenix programs, for
  zenixboot (only Ed25519 verification and SHA-512) and for the PC (the host tests). `--gc-sections` keeps only what
  each program calls.
- **Not D13 shared code now:** D13 stage 2 (shared libraries) is not decided, and the users are few and seldom run
  together (`ssh`, `ssh-keygen`, `wget`, `zpkg`). `ssh` and `wget` are built pure (ZEX2), so two ssh sessions share
  one code copy through D13 stage 1. When D13 stage 2 is decided, this library is its second candidate after libz
  (measured then: three programs at once would hold three copies of 50-80 KB).
- **Not a server** (a "crypto daemon" holding keys): with no MMU a server isolates nothing (any program can read its
  memory), and every operation would cost a message round trip. An `ssh-agent` may come later as a convenience
  (section 4.6), not as protection.
- **No kernel change.** The kernel keeps its own ChaCha20/BLAKE2s (`random.c`, 3.7 KB); the library reads
  `/dev/random` (section 5).

### 3.6 The assembler (only where measured to pay)

The golden path: careful code where the user feels it. Here the user feels the handshake. Every public-key operation
spends nearly all its time in one function: the curve25519 field multiply (X25519, Ed25519), BearSSL's `i15`
Montgomery multiply (RSA) and the P-256/P-384 field multiply (ECDSA). The plan, in order, each measured on the stock
A1200 before the next (section 10 step 2):

1. **Plain C first**, measured. *Measured* here: Monocypher's field multiply compiles to 100 calls of libgcc's
   `__muldi3` (its 64-bit products; `-m68020-60` forbids the 68020's own 32x32->64 `mulu.l`), so it is slow on every
   68k; BearSSL's `m15` code compiles to `muls.l` 32x32->32 (1,317 of them in the TLS test).
2. **Portable assembler** (`mulu.w` 16x16->32, legal on every CPU from the 68020 to the 68060) for the curve25519
   field multiply and square, then BearSSL's `i15` Montgomery loop: no library call, no 64-bit emulation. Checked
   against the C on the PC's vectors in FS-UAE (as netd's `m68k.S` was, D19 step 4 choice 7).
3. **Only if (2) is still too slow on the stock A1200:** a second version using the 68020-68040's 32x32->64 `mulu.l`
   (43 clocks for four times the work of a 27-clock `mulu.w`, MC68020UM table 8), chosen at run time on CPUs that
   have it. That needs a way for a program to learn its CPU, which no call gives today: a field appended to an
   existing size-passing struct (the frozen-interface rule), the main session's call at that time, and a named
   exception in `check-68020-60.sh` for that one file.

Constant-time rules for every assembler file: no branch and no memory index that depends on a secret; reviewed with
the C it replaces.

## 4. `ssh`

### 4.1 Dropbear's `dbclient` ported, or a client of our own on the library?

| | **Port `dbclient`** (Dropbear, MIT; libtomcrypt/libtommath) | **Own client on `crypto/`** (chosen) |
|---|---|---|
| Correctness | mature, interop-tested for 20 years | new code: needs the tests and fuzzing of section 11 and a review; borrows OpenSSH's and Dropbear's behaviour where the RFCs are vague (both readable, BSD/MIT) |
| Size on a 2 MB machine | dbclient plus libtomcrypt and libtommath: *estimate* 120-200 KB of code (CTerm's libssh2 alone was ~180 KB, knowledge base `amiga/ssh-libssh2-amissl.md`) | *estimate* 25-35 KB for the client's core plus 45-50 KB of crypto (*measured* below) |
| Constant time | libtommath is not; Dropbear's curve25519 is TweetNaCl's (slow) | BearSSL `ct`/`i15`, Monocypher |
| Fitting Zenix | needs a termios layer, `select`, `fork` for proxy commands and `-f`, its own `malloc` patterns; local patches to keep in step with upstream security releases (Dropbear had CVEs in 2025) | one `poll` loop like telnet; D20's key translation and Unix-mode filter shared, not copied; a system-call-free core in `net/ssh/` beside `net/telnet/`, host-tested and fuzzed the same way |
| Algorithms | all, including the post-quantum ones (sntrup761, ML-KEM through libcrux) | section 3.3's list |

**Choice: our own client**, small and in Zenix's shape, because the client side of ssh is a small part of the
protocol (one transport, one key exchange, one channel), and because size, constant time and the shared terminal
code all point that way. Dropbear stays the reference for edge cases and its server is a second interop target in
the tests. If the client's review or interop tests go badly, porting dbclient is the fallback (owner not asked: a
technical call).

### 4.2 The protocol (RFC 4250-4254 and OpenSSH's PROTOCOL files)

- **Transport** (RFC 4253): the version line, KEXINIT, the key exchange (RFC 8731 for curve25519-sha256), NEWKEYS
  with strict kex, the packet layer (sequence numbers, padding, the cipher and MAC of 3.3; the
  `chacha20-poly1305@openssh.com` construction: two ChaCha20 keys, the length encrypted separately, the Poly1305 tag
  over the whole packet, checked before anything is decrypted). Packets up to 35,000 bytes accepted, as RFC 4253
  6.1 requires. **Rekeying** when the server asks (OpenSSH does after an amount of data; the session pauses for the
  key exchange's few seconds on a stock A1200) and by the client after 1 GB or one hour.
- **Server authentication:** the host key's signature over the exchange hash checked, then the key looked up in
  `known_hosts` (4.4).
- **User authentication** (RFC 4252): `publickey` with Ed25519 (the server's `server-sig-algs` from `ext-info`
  read), then `keyboard-interactive` (RFC 4256: Linux's PAM asks this way) and `password`, in that order, as OpenSSH's
  client. Answers are read with echo off and wiped after sending.
- **Connection** (RFC 4254): one `session` channel; `pty-req` (TERM `xterm-16color`, D14's name; the size from
  `SYS_ttyinfo`, 80x32 on PAL; the modes VERASE 127, VINTR 3 and UTF-8 on) and `shell`, or `exec COMMAND` when a
  command is given (no pty unless `-t`, so `ssh pc ls | grep x` works in scripts); the windows (a 64 KB receive
  window, maximum packet 16 KB, adjusted as the screen consumes), EOF, close, `exit-status` becomes ssh's exit status.
- **Keepalive:** `-o ServerAliveInterval` later; an idle session needs nothing.
- **Not in revision 1:** port forwarding (`-L`/`-R`), agent forwarding, X11, multiplexing, certificates, GSSAPI,
  jump hosts. Each is a later step if asked.

### 4.3 The terminal (shared with D20)

- **One process, one loop:** `poll` on the socket and standard input (D16), as telnet. Bytes from the channel go
  through telnet's Unix-mode filter (`tf_*`: UTF-8 shown as Latin-1) to standard output, 256 bytes a write; keys go
  through `tk_keys` in the Unix mode, then into channel data. `net/ssh/` calls `net/telnet/`'s functions; nothing is
  copied (CLAUDE.md: one fact in one place).
- **The tty:** `TTY_RAW | TTY_NOSIG` while connected (Ctrl-C and Ctrl-Z go to the PC), restored at every exit path,
  including a signal (as telnet).
- **The escape: OpenSSH's** `~.` (close), `~^Z` (suspend: the tty restored, SIGTSTP to itself, raw again on
  SIGCONT), `~?` (help), `~~` (a tilde), only after Return, `-e CHAR` / `-e none` as in OpenSSH. D20 chose Ctrl-] for
  telnet because boards treat `~` as a key; anyone who uses ssh expects `~.`, so ssh keeps ssh's own.
- **nice 1**, as telnet: netd (nice 0) must drain SLIP's ring while ssh draws, and the seconds of public-key work at
  the start must not make the other consoles' shells (nice 0) wait (the golden path: the machine stays snappy).
- **Window size:** told once in `pty-req`. Zenix's consoles do not change size; when pseudo-terminals and SIGWINCH
  come (D20 section 7), ssh sends `window-change` on SIGWINCH. Nothing in this step blocks that.
- **The PC side:** a plain OpenSSH server, nothing special: `sshd` switched on, the Amiga's `id_ed25519.pub` in
  `~/.ssh/authorized_keys`. Over SLIP the connection arrives through `zenix-slirp` as a local one (10.0.2.2 is the
  PC's 127.0.0.1), so the PC's firewall is not involved; over the PiStorm's Ethernet it comes from the LAN, and the
  PC's firewall must let the A1200's address in on port 22 (the owner's ufw today allows it only on ports
  2390-2399: knowledge base `amiga/ssh-libssh2-amissl.md`; the owner's one-line rule, in `ssh`'s page).

### 4.4 Keys and known hosts (OpenSSH's file formats, so files move between the PC and Zenix)

- **`ssh-keygen`** (`-t ed25519` only; `-f FILE`, `-C COMMENT`, `-N PASS`, `-a ROUNDS`, `-l -f FILE` the
  fingerprint, `-y` the public half, `-R HOST` removes a known host): writes `~/.ssh/id_ed25519` (OpenSSH's
  `openssh-key-v1` format, mode 600) and `id_ed25519.pub`; makes `~/.ssh` mode 700. Randomness from `/dev/random`
  (section 5).
- **Passphrases** (`bcrypt_pbkdf` + `aes256-ctr`, OpenSSH's format, read and written): OpenSSH's default 16 rounds
  take *about two minutes* on a stock 14 MHz A1200 (*estimate*: ~2 million Blowfish block operations) and a few
  seconds on the 060. So the default on every machine is no passphrase with the file at mode 600, `-N` makes one, and
  `ssh` reads a passphrase-protected key copied from the PC (owner question 3). What a passphrase protects without an
  MMU: the key on a stolen card, not the key in memory (9).
- **The key file is refused when others may read it** (OpenSSH's "UNPROTECTED PRIVATE KEY FILE" rule; as `ifup`
  refuses an interfaces file others may write).
- **`~/.ssh/known_hosts`**, OpenSSH's format: `host[,host] keytype base64`, `[host]:port` for other ports, hashed
  names (`|1|salt|hash`, HMAC-SHA1, which Debian and Ubuntu write by default) read; ssh writes plain names (owner can
  copy entries from the PC). Also `/etc/ssh/ssh_known_hosts` (root's, read only).
- **Trust on first use:** an unknown host shows its SHA-256 fingerprint in OpenSSH's form (`SHA256:` + base64) and
  asks `yes/no/[fingerprint]` (typing the fingerprint from the PC's `ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub`
  is the careful way; the page says how). A known host with a **different key: refused**, with the file and line to
  remove; never a question. A host known only by another key type: the known one is asked for first.
- **`~/.ssh/config`**, a subset: `Host` (patterns with `*`), `HostName`, `User`, `Port`, `IdentityFile`,
  `ServerAliveInterval`; unknown words reported and ignored. It gives the "phone book" for free.
- **Agent:** later, if wanted (with unencrypted keys and no MMU it adds nothing but convenience). **scp/sftp:**
  later (owner question 8): an `sftp` client and an `scp` over the SFTP protocol (as OpenSSH's own `scp` since 9.0).

### 4.5 Command line and exit status

`ssh [-p PORT] [-l USER] [-i KEY] [-e CHAR] [-t|-T] [-v] [user@]host [command...]`; exit status: the remote command's,
255 for ssh's own errors (OpenSSH's convention). `-v` prints each step with its time (the handshake's seconds are
visible on a slow machine). A progress line during the key exchange on a slow machine ("ssh: key exchange..."), so
seconds of silence are never mistaken for a hang. ^C before the session starts ends ssh (the tty is still normal).

### 4.6 The server (`sshd`): not now

Logging in *to* Zenix needs D20's pseudo-terminals (D20 section 7) and a host key; it is a later step with its own
note. Nothing here blocks it: the transport and the crypto are the same.

## 5. Random numbers

- **The library reads only `/dev/random`** (32 bytes at a time, more for key generation), which is Linux's
  `getrandom()` meaning since D19: it waits until the pool was seeded once, then never again. `/dev/urandom` is never
  used for keys: on a fresh machine it answers before the pool is seeded.
- **When it would wait** (opened with `O_NONBLOCK`, -EAGAIN), the program says why and how to help, then waits in
  `poll`: "ssh-keygen: waiting for the random pool (type on the keyboard)". On a machine with a credited seed file or
  the PiStorm's RNG200 this never happens after the first boot.
- **Uses:** ephemeral X25519 and ML-KEM keys, ssh's padding and cookie, `ssh-keygen`, `bcrypt_pbkdf`'s salt, TLS's
  client random (BearSSL's HMAC-DRBG seeded with 32 bytes through `br_ssl_engine_inject_entropy`). Ed25519 signing
  is deterministic (RFC 8032) and needs none.
- **Nothing new in the kernel.** A forked-DRBG problem cannot happen (no `fork`); each process seeds its own.

## 6. Package signatures (D15 step 7, and `zpkg`)

- **The algorithm:** Ed25519 (RFC 8032, pure). **What is signed:** the bundle's existing header digest (the SHA-256
  over header bytes 0-63 and the table of contents, bytes 64-95), which already covers every part through the parts'
  SHA-256 in the table. The signed message is the 32-byte digest after a fixed label, `"Zenix bundle signature v1"` and
  a NUL, so a signature can never be replayed as one over some other Zenix object. Verifying costs one Ed25519
  verification over 58 bytes: the hashing of the parts is done anyway.
- **Where the signature lives: a detached file**, `zenix-update.zup.sig` beside the bundle (and `NAME.sig` for every
  signed file on the repository). D15's bundle header is frozen; a detached file changes no frozen byte, older
  zenixboots simply ignore it, and the same signature file format serves the bundle and `zpkg`'s list. **Format**
  (frozen once shipped): 80 bytes: `ZSG1`, a format byte 1, the algorithm byte 1 (Ed25519), two zero bytes, the key's
  id (the first 8 bytes of SHA-256 of the 32-byte public key), the 64-byte signature.
- **Public keys:** two built into zenixboot and `zpkg` (current and next, D15), from `keys/zenix-release-*.pub` in
  the repository (public halves only). A bundle signed by "next" is accepted; the new build then carries next as
  current and a new next. A lost or stolen private key: D15's manual path (a new zenixboot through Wasabi).
- **Test keys:** a test key pair under `tools/testdata/sign/` (its private half is a test fixture, public on purpose);
  test builds trust only it, release builds only the release keys, `mkzup.py` refuses to sign a release bundle with
  the test key, and zenixboot names the key that verified ("signed by release key 1a2b...").
- **Signing on the PC:** `tools/zsign.py` calls the PC's OpenSSL (`openssl pkeyutl -sign -rawin`, Ed25519 since
  OpenSSL 1.1.1) with the key in `~/.config/zenix-signing/` (D15: mode 600, outside every repository, with an offline
  backup); the build never sees the key, only the signature. The host tests verify every signature with both OpenSSL
  and the library.
- **The update path:** zenixboot checks the `.sig` before it checks anything else of the bundle (a bad or missing
  signature: the bundle renamed `.rejected`, D15's existing name, with the reason in the update note). Once signing
  has shipped, **an unsigned bundle is refused** (owner question 7; a build option `UNSIGNED_OK` exists only in test
  builds).
- **`zpkg` over the network:** the repository's package list (`index`: each package's name, version, size and
  SHA-256, a serial number and a date) with `index.sig`; packages are covered through their SHA-256. Rollback and
  freeze: `zpkg` refuses a list whose serial is lower than the last one it accepted (`/var/lib/zpkg/serial`) and
  warns when the list is older than 30 days. **So `zpkg` needs no TLS:** plain HTTP from the NAS is safe for
  integrity (owner question 6); HTTPS adds only privacy about which packages are fetched.
- **Cost on the stock A1200:** one Ed25519 verification, *estimate* 6-9 s in plain C, 2-4 s with the assembler,
  once per update (zenixboot runs under AmigaOS there). zenixboot grows by about 20 KB (*measured*: a program that
  only verifies Ed25519 links to 19,432 bytes of code, SHA-512 included).

## 7. Size, memory and time

### 7.1 Code (*measured* 2026-10-27, m68k-elf-gcc 16.1, `-m68020-60 -Os`, linked with `--gc-sections`, plain C)

| Linked test program | Code | Data + BSS |
|---|---|---|
| X25519 only (Monocypher) | 12,292 | 96 |
| Ed25519 verify only (Monocypher, SHA-512 included): zenixboot's and `zpkg`'s share | 19,432 | 160 |
| ssh's primitives: X25519, Ed25519 sign and verify, ChaCha20, Poly1305 (Monocypher); SHA-256, RSA `i15` and ECDSA P-256 verify (BearSSL) | 48,576 | 288 |
| A TLS 1.2 client (BearSSL): X25519/P-256/P-384/P-521 ECDHE, RSA `i15` and ECDSA verify, X.509, ChaCha20-Poly1305, AES-GCM (`ct`), SHA-256/384/512 | 77,888 | 39,572 (33 KB of it the two-way record buffer) |

The rest is *estimate*: ssh's own core 25-35 KB, the program with libz 15 KB, D20's keys and filter 5 KB already
counted by telnet: **`ssh` about 95-110 KB**; `ssh-keygen` 40 KB; `wget` with TLS 100-110 KB. The TLS client can
shrink: P-521 dropped (*estimate* -3 KB), a one-way 16.7 KB record buffer for HTTP (BearSSL's "mono" mode). None of
these go on the RAM floppy; the demo floppy (roadmap block 2's end) counts them in its own step.

### 7.2 Memory on the stock A1200 (2 MB chip, SLIP up: 1,159 KB free, D19 *measured*)

- **ssh:** code ~100 KB (one copy for all sessions, pure), the 35 KB receive buffer RFC 4253 requires, an 8 KB send
  buffer, stack 8 KB, the rest small: **about 155-170 KB for the first session, 55-70 KB for each further one**, so
  about 990 KB stay free during a session.
- **wget with HTTPS:** ~105 KB code, 17 KB record buffer, 7 KB contexts, one trust anchor read at a time (8.1):
  about 140 KB.

### 7.3 Time (all *estimates*; measured on the real machines in step 2)

Scaled from BearSSL's published figures on a 48 MHz ARM Cortex-M0+ (*read*, bearssl.org/speed.html: c25519_m15 4.42
operations a second, p256_m15 1.72, RSA-2048 i15 public 2.2; ChaCha20 551 KB/s, Poly1305 289 KB/s), with the stock
A1200 taken as **6-8 times slower** per operation for this multiply-heavy code (a 27-43 clock multiply against the
M0+'s 1, at 14.18 MHz against 48, slowed a little more by chip-RAM-only memory), and from Monocypher's 100 `__muldi3`
calls per field multiply (*measured* above). The 68060 at 80 MHz with fast RAM: about 30-50 times the stock A1200.
The PiStorm (Emu68's JIT): milliseconds (AmiSSL's TLS handshake there took 50-70 ms, *measured*, knowledge base).

| Operation | Stock A1200, plain C | Stock A1200, portable asm (3.6 item 2) | 68060 80 MHz |
|---|---|---|---|
| X25519 (one scalar multiply) | 4-6 s | 2-3 s | 0.1-0.2 s |
| Ed25519 verify | 6-9 s | 3-5 s | 0.15-0.3 s |
| Ed25519 sign | 2-4 s | 1-2 s | 0.05-0.1 s |
| RSA-2048 verify | 5-10 s | 2-4 s | 0.1-0.3 s |
| RSA-4096 verify | 20-40 s | 8-15 s | 0.5-1 s |
| ECDSA P-256 verify | 15-25 s | 6-10 s | 0.3-0.8 s |
| ECDSA P-384 verify | 40-70 s | 15-25 s | 1-2 s |
| ML-KEM-768 (step 6) | 0.3-1 s | | < 0.05 s |
| ChaCha20-Poly1305 | 40-70 KB/s | | 2-3 MB/s |

- **An ssh login to the PC** (two X25519, one Ed25519 verify, one Ed25519 sign): **about 16-25 s on the stock A1200
  in C, 7-12 s with the assembler, under a second on the 060, instant on the PiStorm.** Typing afterwards: SLIP's
  11.5 KB/s is the limit, not ChaCha20.
- **An HTTPS connection** (X25519, the server's signature, two certificate signatures, often one an RSA-4096 or a
  P-384 root's): **30 s to over a minute on the stock A1200 in C, 15-40 s with the assembler; a second or two on
  the 060.** A server may close a handshake that slow (many wait 60 s, some 10-30 s): HTTPS on the unexpanded A1200 is
  "works with patience, may fail on some sites", said in `wget`'s page (owner question 5). Saving handshakes:
  HTTP/1.1 keep-alive (`zpkg` and `wget` fetch many files over one connection), and BearSSL's session-ID resumption
  within one process (no tickets in BearSSL; no session data written to disk).

## 8. TLS for `wget` (the HTTPS half; `wget` itself is its own step)

### 8.1 Trust anchors (the root certificates)

- **The source:** Mozilla's list (the one curl publishes as `cacert.pem`, about 145 roots; MPL-2.0 data, compatible:
  MPL-2.0 names GPL-2.0 among its secondary licences, and it is shipped as a separate data file with its notice in
  `/usr/share/licenses/`).
- **The file:** `tools/mkanchors.py` turns it at build time into `/etc/ssl/anchors`, BearSSL's form (each root's name
  and public key, not the whole certificate): *estimate* 50-70 KB for the full list. **Format** (frozen once shipped):
  `ZTA1`, a count, an index of (the first 8 bytes of SHA-256 of the encoded name, offset, length), then the entries.
  `wget` reads the index and only the anchor the chain names: one anchor in memory, never the whole list.
- **The owner's own:** PEM files in `/etc/ssl/local/` (root's) are added at run time (the NAS's self-signed
  certificate, a test CA).
- **Updates:** a package `ca-certificates` (version = Mozilla's date), updated through `zpkg` and D15 bundles, signed
  like every other. The demo floppy may carry a smaller list (its step decides).

### 8.2 The checks

BearSSL's `x509_minimal`: the chain as the server sends it, each signature, the server name against the
subjectAltName (wildcards in the first label only), basic constraints, key usage, the validity dates (8.3). Refused:
an unknown critical extension (BearSSL rejects these; rare name-constrained intermediates fail, said in the page),
RSA under 2048 bits, SHA-1 signatures. **Not checked: revocation** (no OCSP, no CRL: like most small clients; said
in the page). SNI always sent; ALPN `http/1.1`.

### 8.3 The time (certificates need a date)

- **Sources, in order:** the battery clock at boot (the owner's A1200 has one); **SNTP** once the network is up (a
  small root command `sntp HOST`, run by init after `ifup -a` when `/etc/sntp` names a server, default
  `pool.ntp.org`; owner question 4); **a floor** that time never goes below: the newest of the library's build date
  and the file `/var/lib/clock` (the time written at `reboot`, at shutdown and after each SNTP answer, as systemd
  keeps its clock file). The kernel's `SYS_settime` already exists (root).
- **Without a trusted time** (no clock, no SNTP answer: `SYS_time` below the floor): BearSSL's time callback
  (`br_x509_minimal_set_time_callback`) checks "not after" against the floor and skips "not before", and `wget` says
  once "the clock is not set: certificate dates checked only against <date>". A certificate that expired before the
  floor is still refused.
- **SNTP is unauthenticated:** someone on the path could set the clock back to accept an expired certificate; the
  floor limits that to certificates that expired after the last good time. Accepted for revision 1 (NTS, the
  authenticated kind, needs TLS 1.3).
- **2038:** times are handled as unsigned seconds (good to 2106) and converted to BearSSL's days-and-seconds;
  certificates valid past 2038 (most roots) are checked correctly.

## 9. Security

### 9.1 What a machine without an MMU means for keys (said plainly, as D19 9.2)

- **Any program can read any memory and the disk's hardware.** A key in `ssh`'s memory, a session key, a typed
  password: a hostile program running on the Amiga at that moment can read them. Users are a convenience, not
  protection (D8). So: **never run programs you do not trust while logged in to something that matters.** This is
  said in `ssh`'s and `ssh-keygen`'s pages.
- **Keys at rest:** mode 600 keeps them from honest programs and other users' ordinary tools; a passphrase keeps
  them from someone who takes the card away; neither stops hostile code on the machine.
- **Lifetimes:** secrets are kept as short as possible: the passphrase and password wiped after use, the private key
  wiped after authentication (a rekey needs no user key), ephemeral keys wiped after the exchange; wiping goes through
  a `volatile` pointer (gcc removes a plain `memset` before `free`). No core dumps exist, no swap.
- **What still holds:** against the network. The server is authenticated, the session is encrypted and
  authenticated, an attacker on the wire or a fake access point learns nothing and changes nothing.

### 9.2 Side channels

- **Remote timing** is what matters on this machine (local attackers can read memory anyway). The library's code has
  no secret-dependent branches or table indices (BearSSL `ct`/`i15`/`m15`, Monocypher); the assembler keeps the rule.
- **The multiplier:** BearSSL's list of CPUs with variable-time multiplies does not mention the 68k. The 68000's
  `mulu` is data-dependent (38 + 2n clocks); the MC68020 manual gives `MUL.W` 25-28 and `MUL.L` 41-44 clocks and calls
  them maximums. **Step 2 measures it on the stock A1200** (the E-clock over 100,000 multiplies with operands 0, 1,
  0xFFFF..., random); if the time depends on the data, it is written down, and the remote attacker still has to see
  a few clocks through a 115,200-baud line and a network.
- **Caches:** the 68EC020 has no data cache (256 bytes of instruction cache only); the 030, 040 and 060 do, so the
  no-secret-index rule matters there (AES table code is never used: BearSSL's `ct` AES only).
- Power and electromagnetic analysis: out of scope.

### 9.3 Protocol rules (each a past attack)

Strict kex (Terrapin); the Poly1305 tag checked before decrypting (no padding oracle); no CBC, no MAC-then-encrypt,
no compression; SHA-1 nowhere except HMAC-SHA1 for reading hashed `known_hosts` names; RSA keys at least 2048 bits;
host key changes refused; every length from the network checked against the buffer before use (CLAUDE.md); a
packet longer than 35,000 bytes ends the connection. TLS: 1.2 only, ECDHE only, AEAD only, no renegotiation, SNI.
Whether BearSSL implements the extended master secret (RFC 7627) is checked at step 7; without client certificates
and renegotiation its absence has no known attack here.

### 9.4 The review plan

Crypto code gets more than the usual feature review: (1) the library imports reviewed against upstream (unchanged
but for the listed patches); (2) `net/ssh/`'s core and the assembler reviewed by an independent reviewer before their
merge, against this note and the RFCs; (3) the fuzzers of 11.1 run before each merge; (4) the security audit at
roadmap block 2's end covers the whole: ssh, TLS, signatures, the random source, file permissions.

## 10. Build order (each step small, merged with its tests; the full suite before each merge)

Steps that touch `Makefile` (`UAPPS`, images), `user/usys.h`, libz, `kernel/*`, the root images or `docs/` counts are
**the main session's** (CLAUDE.md "Sessions"); a side session takes only the host-only parts marked *side*, with this
note's names and formats.

0. *side:* **the library on the PC.** `crypto/bearssl/` and `crypto/monocypher/` imported at the pinned versions
   with their licences, `crypto/zc/` (random bytes through a callback, wiping, the X25519 adapter for BearSSL, ssh's
   ChaCha20-Poly1305 framing), built for the PC and, as objects only, for m68k (the size table of 7.1 re-measured).
   Host tests (`tools/host/crypto-test.sh`): RFC 7748 (X25519, incl. the 1,000-iteration check), RFC 8032 (Ed25519),
   RFC 8439 (ChaCha20-Poly1305), NIST's SHA vectors, BearSSL's own `test_crypto`, and differential runs against the
   PC's OpenSSL for random inputs (signatures both ways). Wycheproof's vectors (Apache-2.0 data) are fetched into a
   cache by the test script, never committed, as lrzsz is (`WYCHEPROOF_DIR`).
1. *side:* **`net/ssh/`**, the system-call-free ssh core (transport, kex, packets, channel, authentication, and the
   parsers of `known_hosts`, key files and `~/.ssh/config`), a host client (`tools/host/ssh-client.c`, as
   `telnet-client.c`) logging in to a real OpenSSH `sshd` and a Dropbear server started in `unshare -rnm` on the PC
   (test host keys made per run; forced rekey with `RekeyLimit 1M`; password, keyboard-interactive and key logins; a
   changed host key; a wrong signature), fuzz harnesses for every parser and the packet layer (one run at a time,
   with a memory cap). Also `tools/zsign.py`, the `.sig` format and its verifier with vectors, and
   `tools/mkanchors.py` with a test CA.
2. *main:* **`cryptotest`** (in `/tests`: the vectors on the real 68k code; timings by the E-clock; the multiplier's
   timing check of 9.2), the library's build rules, `check-68020-60` over it. FS-UAE runs the vectors; **the owner's
   A1200 and the 060 give the time table** (emulator figures are never used for speed). Then the portable assembler
   (3.6 item 2) if the table asks for it, checked by `cryptotest` against the C.
3. *main:* **`ssh-keygen` and `ssh`** (curve25519-sha256), their pages, `commands.md`, `features.md`; FS-UAE tests
   over the A2065 and over SLIP to the namespaced `sshd` (login with a key and with a password, `ssh host command`'s
   exit status, `~.`, ^C to the remote, a changed host key refused, a forced rekey, a 1 MB `cat` checked byte for
   byte; memory with `free` on the 2 MB machine). **First thing the owner can use:** ssh to the PC.
4. *owner:* the real machines: ssh from the PiStorm (GENET) and the stock A1200 (SLIP) to the PC; the handshake's
   time on each, labelled.
5. *main:* **signatures** (D15 step 7): zenixboot and `mkzup.py`, the detached `.sig`, the two keys, the refusal of
   unsigned bundles, the test key rules; FS-UAE: a good, a bad, a missing and a test-key signature on a release
   build.
6. *main:* **`mlkem768x25519-sha256`**, offered first; interop with OpenSSH 10.
7. *main, with `wget`'s own step:* TLS: the anchors file and package, `/etc/ssl/local/`, `sntp`, `/var/lib/clock` and
   the floor, the time callback; tests against a local HTTPS server with a test CA (good, expired, not yet valid,
   wrong name, unknown CA, missing intermediate, RSA-4096 and P-384 chains, a TLS 1.3-only server's clear message,
   keep-alive with several files, resumption counted).
8. *later, each when asked:* `sftp`/`scp`, `ssh-agent`, port forwarding, `sshd` (after D20's ptys), a TLS 1.3
   client (3.2).

## 11. Tests (summary; each step lists its own)

### 11.1 On the PC

Vectors (RFC 7748, 8032, 8439, NIST, BearSSL's, Wycheproof from a cache), differential tests against OpenSSL, the ssh
core against OpenSSH and Dropbear servers in a user and network namespace (no root, nothing leaves the PC, as
`slip-host.py` and the BBS simulator), `.sig` signing and verification both ways, the anchors tool against a test
CA. **Fuzzing** with libFuzzer, one run at a time with a memory cap (this machine's rule): the ssh packet layer and
every message parser, `known_hosts`, key files, `~/.ssh/config`, the `.sig` and anchors files, the `zpkg` list, and
BearSSL's engine fed records (upstream fuzzes it too).

### 11.2 In FS-UAE

`cryptotest`'s vectors on the m68k build (every CPU the matrix has); ssh and later wget over the A2065 and over SLIP
through `zenix-slirp` to the namespaced servers; zenixboot's signature checks; memory on the 2 MB and 1 MB machines.
No speed figure from the emulator.

### 11.3 On the real machines (the owner's)

`cryptotest`'s time table and the multiplier check on the stock A1200 and the 060; an ssh login to the PC from the
PiStorm and from the stock A1200; later an HTTPS fetch from each.

## 12. Frozen once built (CLAUDE.md's list gets them in their steps)

The `.sig` format and the signed message's label (6); the anchors file format (8.1); `/var/lib/clock`'s format and
the floor rule; `/etc/sntp`'s format; the `zpkg` list's format and serial rule (with `zpkg`'s own step). OpenSSH's
formats (`openssh-key-v1`, `known_hosts`, `authorized_keys`, the subset of `ssh_config`) are followed, not ours to
freeze. **Nothing here changes an existing frozen meaning:** no system call, no message type, no struct, no `/etc`
format of base programs; D15's bundle header is untouched (the signature is detached).

## 13. Open questions for the owner (one line each, the recommendation in brackets)

1. TLS 1.2 only for now, TLS 1.3 later only if a site you use needs it? [Yes]
2. The Amiga gets its own ssh key, made on Zenix, instead of a copy of the PC's? [Yes: its own, added to the PC's authorized_keys]
3. A passphrase on that key, which takes about two minutes to unlock on the unexpanded A1200? [No by default; optional]
4. Set the clock from the internet (pool.ntp.org) when the network comes up? [Yes; your RTC module first]
5. HTTPS on the unexpanded A1200 may take a minute and fail on some sites: acceptable? [Yes; ssh and updates stay usable there]
6. `zpkg` fetches from the NAS over plain HTTP, the packages signed? [Yes; HTTPS optional]
7. Refuse unsigned update bundles once signing ships? [Yes; test builds excepted]
8. File copying (`scp`/`sftp`) after ssh is in daily use? [Yes]

## 14. For the main session (not owner questions)

- **The feature halt:** the roadmap (approved order) names the crypto library, ssh and HTTPS; this note adds no
  kernel interface. The owner's approval of this note is their approval of its programs.
- **Review depth:** a feature, not a core change, so one review would do by the rule; crypto and the update path
  (signatures) get two (section 15).
- **A later CPU report** (3.6 item 3) is a frozen-interface addition; only if the A1200's numbers ask for it.

## 15. Reviews

(To come: one review on cryptography, protocol correctness and licences; one on architecture fit, frozen
interfaces, size and the stock A1200.)

Last checked against the code: 2026-10-27 (main at f10de79).
