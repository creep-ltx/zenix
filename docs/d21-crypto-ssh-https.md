# D21: the crypto library, `ssh` and HTTPS (design note, revision 2)

Status: **design, revision 2 after two independent reviews (section 15); steps 0-3 built ("As built" at the end: steps 0-1 by the side helper, 2-3 on branch m4, 2026-10-27); step 2's time table and the X25519 choice wait for the owner's A1200 and 060.** Roadmap block 2 (owner
2026-10-26: "I need to be able to SSH into my Linux machine, properly"): "one small crypto library: modern, small and
constant-time, e.g. BearSSL (MIT) for TLS and Dropbear's client (MIT) for SSH, with Ed25519/X25519/ChaCha20 and a real
random source; it also signs `zpkg`'s packages; then `ssh` (a proper client, keys, known hosts) and HTTPS for
`wget`". D19 section 10 step 11 puts it after telnet. The short version is DESIGN.md D21. Facts about today's code are
*read* from main at f10de79 (2026-10-27); sizes marked *measured* were compiled here with the project's m68k-elf-gcc
16.1 (`-m68020-60 -Os`, linked with `--gc-sections`, 2026-10-27; re-measured by the architecture review); every time
on a 68020 is an *estimate* and shows its arithmetic (7.3). Outside facts (licences, the libraries' code, OpenSSH's
rules) are *read* from the projects' sources and pages on 2026-10-27.

## 1. What the owner gets, in plain words

- `ssh user@pc` from any console logs in to the Linux PC with a key made on the Amiga: the PC's shell in colour, the
  Swedish keys working, Ctrl-C going to the PC, `~.` to leave, as on any Linux machine. The first time it shows the
  PC's fingerprint and asks; a changed key later is refused loudly. Password logins work too.
- `ssh-keygen` makes the Amiga's own key; its one-line public half goes into the PC's `~/.ssh/authorized_keys`.
- `wget https://...` fetches from the web with the certificate checked (in `wget`'s own step).
- **Updates and packages are signed:** zenixboot and `zpkg` refuse a bundle or package list not signed by the owner's
  key (D15 step 7), once the new zenixboot is installed (6.6).
- **Nothing changes for a machine that does not use it:** no kernel change, no new system call, no resident part.
- **Honest about speed:** on the PiStorm and the 060 a login takes a moment; on the stock 14 MHz A1200 the first
  **10-25 s** of a login are public-key arithmetic (*estimate*, 7.3), and an HTTPS connection there takes from about
  half a minute to well over a minute, so some servers may give up. Typing in a running session is not slowed.
- **Honest about safety:** without an MMU any program on the machine can read any other's memory and the disk. The
  key file is protected from honest programs and other users' tools, not from hostile code running on the Amiga (9).

## 2. Today (read)

- **Random numbers** (D19 9.3, step 2 as built): the kernel's pool (BLAKE2s, ChaCha20 with fast key erasure);
  `/dev/urandom` never waits; `/dev/random` waits until 256 bits were credited once, then never again and polls
  readable (Linux 5.6+); `O_NONBLOCK` gives -EAGAIN while unseeded. A fresh machine needs about 128 key presses; the
  seed file `/var/lib/random-seed` credits only on a writable persistent root.
- **Sockets** (D19 steps 3-9): BSD sockets in libz, `poll` over sockets and ttys (D16), `getaddrinfo` through netd,
  SLIP at 115,200 (11.5 KB/s) for every Amiga, the A2065 in FS-UAE, GENET on the PiStorm.
- **The terminal side** (D20): built (step 0) is `net/telnet/`'s system-call-free core, whose key translation
  (`tk_keys`, the Unix mode `TN_UNIX`: Backspace 0x7F, Del `ESC[3~`, Latin-1 keys as UTF-8) and Unix-mode filter
  (`tf_*`: UTF-8 from the host shown as Latin-1) are what an ssh session to Linux needs. **Designed, not built:** the
  console rules (`CON_RULES`, D20 step 1; the Unix mode needs `CONR_LFONLY`, or full-screen programs on the PC draw
  wrongly), telnet's 256-byte screen writes and nice 1 (D20 step 3).
- **Hashes in the tree:** `user/sha256.c` (in `login`, `passwd`, `sha256sum`, zup and zenixboot; "about 1 ms per
  64-byte block on a 68020", the project's estimate).
- **Updates** (D15): a bundle is a 96-byte header (bytes 64-95 a SHA-256 over header bytes 0-63 and the table of
  contents), the table (each part's SHA-256), the parts. zenixboot renames a damaged or unusable bundle `.bad`
  (`bundle_bad()`); `.rejected` is the bundle put aside when the owner chose "previous". Its state file
  (`Zenix/state`, keyed text, unknown keys ignored) remembers what it applied. zenixboot is never updated by a bundle
  (D15 section 6). Signatures are D15 step 7, not built.
- **Time:** `SYS_time` (seconds since 1970) from a battery clock (MSM6242 or RP5C01) read at boot when one ticks; no
  network time. The owner's A1200 has an Individual Computers RTC module (photos, 2026-10-24); the stock A1200 has no
  clock. **Found by the review:** `SYS_time` is `time_base + ticks/100` with a 32-bit `ticks` (it jumps back after 497
  days of uptime) and `syscall.c` returns it through `(s32)` (callers must read it unsigned): section 14.
- **CPUs:** every program is built `-m68020-60`; `tools/check-68020-60.sh` refuses the 64-bit `mulu.l`/`divu.l` forms
  (a 68060 traps on them). No call tells a program its CPU.
- **Licence:** GPL-2.0-only; outside code GPL-2-compatible with its header kept; notices in `/usr/share/licenses/`
  (`mkzup.py`'s `LICENSES` list and `mkroot.sh`; D19 owner decision 6).

## 3. The library

### 3.1 The candidates (licences checked against GPL-2.0-only; confirmed by the review)

| | Licence | GPL-2-only? | What it has | What it lacks | State |
|---|---|---|---|---|---|
| **BearSSL** (Pornin) | MIT | yes | TLS 1.0-1.2 client and server, X.509 validation (`x509_minimal`), RSA (PKCS#1 v1.5), ECDSA P-256/384/521, X25519, AES (`ct`), GCM, ChaCha20-Poly1305, SHA-1/2, HMAC, HMAC-DRBG; no `malloc`; 15-bit-word big-number code (`i15`, `m15`) | **TLS 1.3, Ed25519**, RSA-PSS in certificates, session tickets, the extended master secret (RFC 7627), EKU checks, IP-address names | "0.6, beta" since 2018, maintained for fixes: commits 2026-04-04/06 (git log) |
| **Monocypher 4** | BSD-2-Clause OR CC0-1.0 | yes | X25519, Ed25519 (SHA-512, the optional file), ChaCha20 (also DJB's 64-bit-nonce form), Poly1305, BLAKE2b | RSA, ECDSA, AES, TLS, X.509 | active (commit 2026-10-20) |
| TweetNaCl | public domain | yes | X25519, Ed25519, Salsa20-Poly1305 | ChaCha20; very slow | finished |
| Mbed TLS 3.6 | Apache-2.0 OR GPL-2.0-or-later | **only under its GPL choice** (a few `3rdparty/` files are Apache-only) | TLS 1.2 and 1.3, X.509 | Ed25519; two to three times BearSSL's size; heap-heavy | active |
| libtomcrypt + libtommath (in Dropbear) | Unlicense / public domain | yes | all ssh needs | constant-time big numbers; large | active |
| wolfSSL | GPL-3.0-or-later | **no** | | | |
| OpenSSL 3 / AmiSSL | Apache-2.0 | **no** | | | |

### 3.2 Choice: one library, `crypto/`, made of BearSSL and Monocypher

- **BearSSL** for TLS 1.2, X.509, RSA and ECDSA verification, AES-GCM, SHA-1/2, HMAC and the DRBG, with these
  implementations chosen explicitly: `aes_ct`, `ghash_ctmul32`, `poly1305_ctmul32` (the 32x32->32 ones: BearSSL's
  plain `ctmul` uses 64-bit products, which become libgcc calls), `rsa_i15`, `ec_p256_m15`, `ec_prime_i15`,
  `ec_c25519_m15`, `ecdsa_i15`. Its `i15` code multiplies 15-bit values with a C 32x32->32 multiply (`MUL15`, compiled
  to `muls.l`: 1,317 of them in the TLS test), so it never needs the 64-bit `mulu.l` the 68060 lacks. If step 2 finds
  the 020/030 multiply's time depends on the data, BearSSL's own `BR_CT_MUL15` (bit 31 forced on both operands) is
  the remedy. No global writable data.
- **Monocypher** for Ed25519 (sign and verify; BearSSL has none, and ssh's default keys and the package signatures
  need it), ChaCha20 and Poly1305 for ssh. Used under its **CC0** choice (so zenixboot on the AmigaOS boot partition
  carries no notice duty; the notice ships anyway).
- **X25519: measured before chosen.** Monocypher's field multiply is 100 libgcc `__muldi3` calls under `-m68020-60`
  (*measured*); BearSSL's `c25519_m15` avoids 64-bit products. By the same scaling (7.3) BearSSL's may be two to three
  times faster in plain C. Step 2 times both on the A1200; **the faster one serves ssh and TLS alike** (one curve25519
  in the system: if Monocypher's wins, a ~40-line `br_ec_impl` adapter gives TLS it: the scalar reversed from
  BearSSL's big-endian to Monocypher's little-endian, `order()`/`xoff()` as `ec_c25519_m15`'s, P-256 forwarded to
  `ec_p256_m15`, P-384/521 to `ec_prime_i15`). **Either way an all-zero X25519 result is refused** (RFC 8422 for TLS,
  RFC 8731 for ssh): neither library checks it, so `crypto/zc/` does.
- **Blowfish and `bcrypt_pbkdf`** (OpenSSH key passphrases, 4.4) are in neither: taken from OpenSSH's
  `openbsd-compat/blowfish.c` (BSD-3, no advertising clause) and `bcrypt_pbkdf.c` (ISC), with Monocypher's SHA-512;
  their notices ship too. Only in `ssh` and `ssh-keygen`, only in the step that reads passphrases (10 step 3b).
- **Not taken:** Mbed TLS (TLS 1.3, but two to three times the size on a 2 MB machine and no Ed25519); TweetNaCl
  (slower still); libtomcrypt/libtommath (not constant-time with secrets, the largest); our own primitives (never).
- **TLS 1.3 is the gap, said plainly.** Most servers still accept TLS 1.2 in 2026 (no reliable figure for 1.3-only
  servers was found; step 7 tries the owner's own list: GitHub, Aminet, the NAS, the sites `wget` is used for). A
  server that refuses 1.2 gets a clear message ("this server needs TLS 1.3, which Zenix's wget does not speak yet").
  Also refused: servers in FIPS mode (RHEL 9 FIPS: they demand the extended master secret, and offer no curve25519 or
  ChaCha20). If needed later: a TLS 1.3 client layer on these primitives and BearSSL's X.509 engine (its own design
  step), or Mbed TLS under its GPL choice (owner question 1).
- **Pinned and imported:** BearSSL at 7bea48e (2026-04-06); Monocypher at f5f0a19 (2026-10-20: 4.0.3 plus ten
  commits, among them 55158d3 "Fix timing leak in constant time comparisons" in `neq0`, which 4.0.3 lacks; the sizes
  of 7.1 were measured at this commit). **Only the files used** go into `crypto/bearssl/` and `crypto/monocypher/`
  (BearSSL's tree carries its T0 compiler, samples and tests), each with its licence and a README naming the version,
  the files taken and the patches beside them (as `net/lwip/README`). Upstream security fixes are "fix when found".

### 3.3 Algorithms (offered in this order; nothing weaker is ever accepted)

**ssh**, in two stages (the golden path: the owner's need first, the rest when a server asks for it):

| | Step 3 (the owner's PC: OpenSSH 9.6+) | Step 3b (other servers) | Never |
|---|---|---|---|
| Key exchange | `curve25519-sha256`, `curve25519-sha256@libssh.org`, `ext-info-c`, `kex-strict-c-v00@openssh.com` | `mlkem768x25519-sha256` first (step 6, 3.4) | DH groups, NIST-curve ECDH, SHA-1 |
| Host keys (verify) | `ssh-ed25519` | `ecdsa-sha2-nistp256`, `rsa-sha2-512`, `rsa-sha2-256` (RSA 2048 bits or more) | `ssh-rsa` (SHA-1), DSA |
| Ciphers | `chacha20-poly1305@openssh.com` | `aes256-gcm@openssh.com` | CBC, CTR with "encrypt-and-MAC" or any non-ETM MAC, 3DES, `none` |
| Logins | `publickey` (Ed25519), `keyboard-interactive`, `password` | passphrase-protected keys (bcrypt), hashed `known_hosts` names, `~/.ssh/config` | RSA/ECDSA user keys (signing costs minutes on a 68020) |
| Compression | `none` | | zlib |

**Servers without strict kex are refused** (Terrapin, 9.3): OpenSSH has it since 9.6 and Dropbear since 2024.84.
Servers that offer none of these (FIPS-mode RHEL) are refused with a clear message.

**TLS 1.2** (for `wget`, later the e-mail client): ECDHE only (X25519, P-256, P-384), suites
`ECDHE_{RSA,ECDSA}_WITH_CHACHA20_POLY1305_SHA256`, then `..._AES_128_GCM_SHA256`, `..._AES_256_GCM_SHA384`;
**RSA listed first in `signature_algorithms`** (verifying RSA with exponent 65537 is several times cheaper on a 68020
than ECDSA, 7.3; harmless on fast machines); certificates signed with RSA PKCS#1 v1.5 (2048-4096 bits,
`br_x509_minimal_set_minrsa`) or ECDSA P-256/P-384; **no SHA-1** (the X.509 engine is set up without it, never with
`br_x509_minimal_init_full`); SNI always; `BR_OPT_NO_RENEGOTIATION`; no RSA key transport, no CBC, no TLS 1.0/1.1.

**Package signatures:** Ed25519 (RFC 8032, pure), section 6.

### 3.4 Post-quantum key exchange for ssh (step 6)

OpenSSH 10.0 made `mlkem768x25519-sha256` its default; 10.1's client warns without a post-quantum exchange (the
server never refuses `curve25519-sha256`). It protects recorded sessions from a future quantum computer. Cost on the
stock A1200: about 1-2 s (*estimate*: some 100 Keccak permutations on 64-bit lanes split into 32-bit halves, plus the
number-theoretic transforms; mlkem-native's C assumes a constant-time 32x32->64 multiply, which becomes `__muldi3`
here), on top of X25519. Source: mlkem-native (README badges Apache-2.0, ISC and MIT: its LICENSE checked at step 6
to be a choice, then used under ISC or MIT) or the CRYSTALS reference (CC0). K is SHA-256 of the ML-KEM secret and the
X25519 secret, encoded as a **string** (not an mpint, unlike curve25519-sha256). `sntrup761x25519-sha512` is not taken.

### 3.5 Where it lives

- **A static library**, `crypto/` (the two imports, and `crypto/zc/`: random bytes, wiping, the zero-result check,
  ssh's ChaCha20-Poly1305 packet framing, the X.509 wrapper of 8.1, the assembler files), built for Zenix programs,
  for zenixboot (Ed25519 verification and SHA-512 only) and for the PC (host tests). `--gc-sections` keeps only what a
  program calls.
- **Not D13 shared code now** (stage 2 is not decided; the users are few: `ssh`, `ssh-keygen`, `wget`, `zpkg`).
  **Pure (ZEX2) builds need care** (the review measured): BearSSL's `x509_minimal` does date arithmetic with 64-bit
  division (`__divdi3`/`__moddi3`), which pulls libgcc's `__clz_tab` through an absolute reference, a relocation
  class D13 stage 1 refuses; and under `-msep-data` the vtables move into each process's data (28-56 bytes per
  object, plus the GOT). So: `ssh` pure from the start (it does not link `x509_minimal`); `wget` stays a plain ZEXE
  until a small patch removes the 64-bit division (the date can be computed by the time callback, 8.3) or D13's
  "applied once" class exists. The per-session cost is measured as pure in step 3.
- **Not a server** holding keys: with no MMU a server isolates nothing and every operation would cost a message.
- **No kernel change.** The kernel keeps its own ChaCha20/BLAKE2s (`random.c`); the library reads `/dev/random` (5).

### 3.6 The assembler (only where measured to pay)

Nearly all of a handshake's time is in one function per algorithm: the curve25519 field multiply and square, the
`i15` Montgomery multiply (RSA) and the P-256/P-384 field multiply. In order, each step measured on the stock A1200
before the next (10 step 2):

1. **Plain C first**, measured, at **-O2 for these files** (the golden path: this is the code the user waits on;
   *measured* cost: +2.5 KB for Ed25519 verification, +4 KB for ssh's primitives), -Os for the rest.
2. **Portable assembler** using `mulu.w` (16x16->32, legal on every CPU from the 68020 to the 68060) for the
   multiplies that step 2 shows dominate; checked against the C on the vectors in FS-UAE (as netd's `m68k.S`).
3. **Only if (2) is still too slow:** a version with the 68020-68040's 32x32->64 `mulu.l` (43 clocks for four times
   the work of a 27-clock `mulu.w`, MC68020UM), chosen at run time. That needs the CPU type: the natural carrier is
   `struct uupdate` (`SYS_update` INFO, size-passing, first size 32, readable by anyone) grown at its end, the main
   session's call; plus a named exception in `check-68020-60.sh` for that one file. (A probe that traps on a 68060
   needs a SIGILL handler in every program that uses it: rejected.)

Constant-time rules for every assembler file: no branch and no memory index that depends on a secret; reviewed with
the C it replaces; libgcc's `__muldi3` checked for data-dependent branches while it is still used.

## 4. `ssh`

### 4.1 Dropbear's `dbclient` ported, or a client of our own on the library?

| | **Port `dbclient`** (Dropbear, MIT; libtomcrypt/libtommath) | **Own client on `crypto/`** (chosen) |
|---|---|---|
| Correctness | mature, interop-tested for 20 years | new code: the tests and fuzzing of 11, an independent review; OpenSSH's `PROTOCOL` files and Dropbear's source as references (BSD/MIT, readable) |
| Size on a 2 MB machine | *estimate* 120-200 KB of code (CTerm's libssh2 alone was ~180 KB, knowledge base `amiga/ssh-libssh2-amissl.md`) | *estimate* 20-30 KB of client core plus 49 KB of crypto (*measured*, 7.1) |
| Constant time | libtommath is not; curve25519 from TweetNaCl (slow) | BearSSL `ct`/`i15`/`m15`, Monocypher |
| Fitting Zenix | termios, `select`, `fork` for proxy commands and `-f`; patches to keep in step with upstream security releases | one `poll` loop like telnet; D20's keys and filter shared; a system-call-free core in `net/ssh/` beside `net/telnet/`, host-tested and fuzzed alike |

**Choice: our own client**, because the client side of ssh is a small part of the protocol, and size, constant time
and the shared terminal code all point that way. Dropbear's server is a second interop target in the tests; porting
`dbclient` is the fallback if the review or the interop tests go badly.

### 4.2 The protocol (RFC 4250-4254, RFC 8308, RFC 8731, OpenSSH's `PROTOCOL` and `PROTOCOL.chacha20poly1305`)

- **The version line** may follow other lines from the server (shown, cleaned, 4.3); at most 255 bytes a line.
- **Strict kex** (`PROTOCOL` 1.10): active only if the server lists `kex-strict-s-v00@openssh.com` (and refused if it
  does not, 3.3); the marker counts only in the first KEXINIT; KEXINIT must be the first packet; during the first key
  exchange any other message is fatal, IGNORE, DEBUG and UNIMPLEMENTED included; each direction's sequence number
  resets to 0 after that direction's NEWKEYS, at every key exchange. `first_kex_packet_follows` is handled (a wrong
  guess's packet skipped).
- **curve25519-sha256** (RFC 8731): the shared secret K is the 32 raw bytes read as a big-endian number, encoded as
  an mpint (leading zeros stripped, a zero byte added when the top bit is set: wrong in one connection in 256 if done
  wrong, so the tests force those cases); an all-zero shared secret ends the connection. The exchange hash H and the
  keys by RFC 4253 7.2, **with the key extension** (K1 = HASH(K || H || X || session_id), K2 = HASH(K || H || K1)...)
  for the 64 bytes `chacha20-poly1305` needs.
- **`chacha20-poly1305@openssh.com`**: two 32-byte keys from the 64 bytes (the second for the length, the first for
  the payload); ChaCha20 in DJB's form (Monocypher's `crypto_chacha20_djb`: 64-bit nonce = the sequence number,
  big-endian; 64-bit counter). Receiving: decrypt the 4-byte length with the length key (counter 0), check it only
  against the buffer and the 35,000-byte limit, act on nothing else; wait for the whole packet; compute the Poly1305
  key from block 0 of the main key's stream and **check the tag over the encrypted length and payload** in constant
  time; only then decrypt the payload from block 1. Padding aligns the packet without the length field (as in every
  AEAD and ETM mode). A bad tag ends the connection.
- **Packets:** up to 35,000 bytes accepted (RFC 4253 6.1); a longer length ends the connection.
- **Messages the server may send at any time after the first exchange:** IGNORE, DEBUG (shown cleaned only with
  `-v`), UNIMPLEMENTED; **any global or channel request with want-reply that ssh does not handle is answered with
  REQUEST_FAILURE or CHANNEL_FAILURE** (OpenSSH's `keepalive@openssh.com` with ClientAliveInterval drops a session
  that never answers); `hostkeys-00@openssh.com` ignored; `exit-status`, `exit-signal` and `eow@openssh.com` handled;
  EXT_INFO accepted after NEWKEYS and just before USERAUTH_SUCCESS (RFC 8308 2.4), `server-sig-algs` read.
- **Rekeying** when the server starts one, and by the client after 1 GB or one hour: from our KEXINIT until NEWKEYS
  only key-exchange messages are sent, so typed keys wait in a small buffer (the session pauses for the exchange's
  seconds on a stock A1200).
- **Server authentication:** the host key's signature over H checked, then the key looked up in `known_hosts` (4.4).
- **User authentication** (RFC 4252), OpenSSH's order: `publickey` (first the query without a signature, PK_OK, so a
  key the server will not take costs no 1-4 s signature on a stock A1200), then `keyboard-interactive` (RFC 4256,
  Linux's PAM) and `password`. A passphrase-protected key is unlocked **before connecting** (minutes on a stock
  A1200, longer than sshd's 120-s LoginGraceTime). Answers are read with echo off and wiped after sending.
- **Connection** (RFC 4254): one `session` channel; `pty-req` (TERM `xterm-16color`, D14's name; the size from
  `SYS_ttyinfo`, 80x32 on PAL; modes VERASE 127, VINTR 3, IUTF8 on) and `shell`, or `exec COMMAND` when a command is
  given (no pty unless `-t`, so `ssh pc ls | grep x` works in scripts); a 64 KB receive window, maximum packet 16 KB,
  adjusted as the screen consumes; EOF, close; `exit-status` becomes ssh's exit status.
- **Randomness** for padding and cookies from one per-process ChaCha20 generator seeded once from `/dev/random` (5).
- **Not in this note's steps:** port and agent forwarding, X11, multiplexing, certificates, GSSAPI, jump hosts.

### 4.3 The terminal (shared with D20)

- **One process, one loop:** `poll` on the socket and standard input (D16), as telnet. Bytes from the channel go
  through telnet's Unix-mode filter (`tf_*`) to standard output, 256 bytes a write; keys through `tk_keys` in
  `TN_UNIX`, then into channel data. `net/ssh/` calls `net/telnet/`'s functions; nothing is copied.
- **The console rules:** ssh sets the Unix mode's rules (`CONR_LFONLY`) exactly as `telnet -x` does, through D20's
  `CON_RULES`, with the same fallback when the output is not a console. **So ssh's step 3 comes after D20 steps 1-3.**
- **The tty:** `TTY_RAW | TTY_NOSIG` while connected (Ctrl-C, Ctrl-Z go to the PC), restored on every exit path.
- **Text from the server outside the session** (the banner, keyboard-interactive's name, instruction and prompts,
  disconnect and debug messages, the version line's preamble) is **cleaned before it is shown**: control characters
  other than tab and newline become `?` (as OpenSSH after CVE-2019-6109/6110), so a server cannot drive the console.
- **The escape: OpenSSH's** `~.` (close), `~^Z` (suspend: tty restored, SIGTSTP to itself, raw again on SIGCONT),
  `~?`, `~~`, only after Return; `-e CHAR` / `-e none`. D20 chose Ctrl-] for telnet because boards treat `~` as a key;
  anyone who uses ssh expects `~.`.
- **nice 1**, as telnet (D20 6): netd (nice 0) drains SLIP's ring while ssh draws or computes (serial bytes wake netd
  through the next tick's `kwork`, and netd preempts ssh's arithmetic at that return to user mode), and the shells on
  other consoles stay snappy during the handshake. The price, said in the page: D10 has strict priorities without
  ageing, so a program keeping the CPU busy at nice 0 on another console stalls ssh until it stops.
- **Window size:** told in `pty-req`; `window-change` on SIGWINCH once D20's pseudo-terminals exist.
- **The PC side:** a plain OpenSSH server: `sshd` switched on, the Amiga's `id_ed25519.pub` in
  `~/.ssh/authorized_keys`. Over SLIP the connection arrives through `zenix-slirp` as a local one (10.0.2.2 is the
  PC's 127.0.0.1), so the firewall is not involved; over the PiStorm's Ethernet it comes from the LAN, and the PC's
  firewall must let the A1200 in on port 22 (today the owner's ufw allows it only on ports 2390-2399: knowledge base
  `amiga/ssh-libssh2-amissl.md`; the one-line rule goes in `ssh`'s page).

### 4.4 Keys and known hosts (OpenSSH's formats, so files move between the PC and Zenix)

- **`ssh-keygen`** (`-t ed25519` only; `-f FILE`, `-C COMMENT`, `-N PASS`, `-a ROUNDS`, `-l -f FILE`, `-y`, `-R
  HOST`): writes `~/.ssh/id_ed25519` (`openssh-key-v1`, mode 600) and `id_ed25519.pub`; makes `~/.ssh` mode 700.
- **Passphrases** (step 3b; `bcrypt_pbkdf` + `aes256-ctr`, OpenSSH's default 16 rounds): **about 2-5 minutes** on a
  stock A1200 (*estimate*: some two million Blowfish block operations), seconds on the 060. Default: no passphrase,
  the file at mode 600 (owner question 3); `ssh` can read a protected key copied from the PC. Without an MMU a
  passphrase protects the key on a stolen card, not the key in memory (9).
- **A key file others may read is refused** (OpenSSH's "UNPROTECTED PRIVATE KEY FILE" rule).
- **`~/.ssh/known_hosts`** (and root's `/etc/ssh/ssh_known_hosts`, read only): `host[,host] keytype base64`,
  `[host]:port` for other ports, patterns with `*`, `?` and `!`; `@revoked` lines refuse that key; `@cert-authority`
  lines are ignored (no certificates); RSA keys are stored as `ssh-rsa` whichever `rsa-sha2-*` signed. Hashed names
  (`|1|salt|hash`, HMAC-SHA1 of the name or `[host]:port`) read from step 3b; ssh writes plain names.
- **Trust on first use:** an unknown host shows its `SHA256:` fingerprint and asks `yes/no/[fingerprint]` (typing the
  fingerprint read on the PC with `ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub` is the careful way; the page says
  how). A known host with a **different key: refused**, with the file and line to remove; never a question.
- **`~/.ssh/config`** (step 3b), a subset: `Host` (patterns), `HostName`, `User`, `Port`, `IdentityFile`,
  `ServerAliveInterval`; unknown words reported and ignored. It is the phone book.
- **Agent, `scp`/`sftp`:** later (owner question 8; `scp` over the SFTP protocol, as OpenSSH's own since 9.0).

### 4.5 Command line and exit status

`ssh [-p PORT] [-l USER] [-i KEY] [-e CHAR] [-t|-T] [-v] [user@]host [command...]`; the exit status is the remote
command's, 255 for ssh's own errors. `-v` prints each step with its time. A progress line during the key exchange
("ssh: key exchange...") so seconds of silence are never taken for a hang; ^C before the session ends ssh.

### 4.6 The server (`sshd`): not now

Logging in *to* Zenix needs D20's pseudo-terminals and a host key: a later step with its own note.

## 5. Random numbers

- **The library reads only `/dev/random`** (Linux's `getrandom()` meaning since D19: it waits until the pool was
  seeded once, then never). `/dev/urandom` is never used for keys.
- **When it would wait** (`O_NONBLOCK`, -EAGAIN) the program says why and waits in `poll`: "ssh-keygen: waiting for the
  random pool (type on the keyboard)". With a credited seed file or the PiStorm's RNG200 this never happens after the
  first boot.
- **Uses:** ephemeral X25519 and ML-KEM keys, `ssh-keygen`, `bcrypt_pbkdf`'s salt, ssh's per-process generator (4.2),
  BearSSL's HMAC-DRBG (32 bytes through `br_ssl_engine_inject_entropy` before the handshake). Ed25519 signing is
  deterministic (RFC 8032). No `fork`, so no two processes share a generator's state.

## 6. Package signatures (D15 step 7, and `zpkg`)

### 6.1 What is signed

Ed25519 (RFC 8032, pure) over a **labelled digest**: a fixed label for the kind of object, a NUL, then a 32-byte
SHA-256. The verifier always **computes** the digest from the bytes it will use, never reads it from the file.

| Kind | Label | Digest |
|---|---|---|
| An update bundle | `Zenix bundle signature v1` | SHA-256 over header bytes 0-63 and the table of contents (the value D15's header holds in bytes 64-95, recomputed and compared) |
| `zpkg`'s package list | `Zenix package index v1` | SHA-256 of the whole file |
| Any later kind | a new label, added here first | SHA-256 of the whole file unless its note says otherwise |

The bundle digest covers every part through the table's SHA-256 values, **and** the header's version, minimum
version and channel, so an old signed bundle cannot be passed off as another one. This refines D15's "Ed25519 over the
table of contents" on purpose: the header is covered too.

### 6.2 The signature file (frozen once shipped)

A **detached file**, the signed file's name plus `.sig` (`zenix-update.zup.sig`, `index.sig`). D15's header has no
room (its `flags` word is inside the hashed bytes, and anything after `total` fails zenixboot's size check), so a
detached file changes no frozen byte and older zenixboots ignore it. **80 bytes:** `ZSG1`; a format byte 1; an
algorithm byte 1 (Ed25519); two zero bytes; the key id (the first 8 bytes of SHA-256 of the 32-byte public key; it
only picks which built-in key to try); the 64-byte signature. **Its life follows the bundle's:** copied as
`.sig.part` and renamed before the bundle's `.part` is renamed; renamed with the bundle to `.bad`, `.done` or
`.rejected` (as `zenix-update.bad.sig` and so on).

### 6.3 Keys

- **Public keys built in:** two (current and next) into zenixboot and `zpkg`, from `tools/keys/zenix-release-*.pub`
  (public halves only). **Rotation:** `zpkg`'s keys travel in its package, so a bundle signed by "next" brings a `zpkg`
  that has next as current. **zenixboot is never updated by a bundle** (D15 section 6), so its keys change only with a
  new zenixboot installed by hand with the owner (D15's manual path, which also replaces a lost or stolen key).
- **Test keys:** a pair under `tools/testdata/sign/` (the private half a public test fixture). What decides:
  a **release bundle** is one whose header channel is `stable` or `testing`; a **release build** is one made with
  `make RELEASE=1`. Release builds trust only the release keys, test builds only the test key; `tools/zsign.py`
  refuses to sign a release bundle with the test key; zenixboot names the key that verified ("signed by release key
  1a2b..."). The Makefile's `bundle` rule and `update-test.sh` sign with the test key (main session, step 5).
- **Signing on the PC:** `tools/zsign.py` builds the exact 58-byte message (label, NUL, digest; no newline) and calls
  the PC's OpenSSL (`openssl pkeyutl -sign -rawin`, Ed25519 since 1.1.1) with the key in `~/.config/zenix-signing/`
  (D15: mode 600, outside every repository, an offline backup). The build never sees the key. The host tests verify
  every signature with both OpenSSL and the library.

### 6.4 zenixboot (the BOOT: transport)

1. The header's sizes and the table's bounds checked (as today); 2. the digest **computed** over header bytes 0-63
and the table, and compared with bytes 64-95; 3. the `.sig` read and the signature verified over that computed
digest; 4. then the parts' SHA-256 as today. A bad or missing signature goes through `bundle_bad()`: renamed `.bad`
(D15's name for a bundle that is not used), the reason on the screen and the serial line. **No change to the update
note** (its kinds are frozen and carry no reasons).
- **Once per bundle, not per start:** during D15's trial the bundle stays on BOOT: and is handed over at each start;
  6-9 s of Ed25519 at every start would break D15 section 9 ("checked only when applied"). So zenixboot writes
  `sigdigest HEX` (the verified digest) into its state file when it first verifies, and at later starts of the same
  trial compares the computed digest with it instead of verifying again. The state file is keyed text whose unknown
  keys older zenixboots ignore: an append-compatible growth, frozen once shipped (section 12).
- **Unsigned bundles are refused** once the signing zenixboot is installed (owner question 7); `UNSIGNED_OK` exists
  only in test builds.
- **What it buys, honestly:** whoever can write BOOT: can also replace zenixboot itself, so the check does not stop
  someone with the card; it stops a wrong bundle (a test build's, a damaged or foreign one) and, with `zpkg`, anything
  fetched over the network or the cable.

### 6.5 `zpkg` (the network and the serial cable)

- **Every bundle or package `zpkg` receives**, by network or by the serial cable (D15 section 10, from 2026-10-31),
  has its `.sig` verified by `zpkg` **before it is stored**; zup at boot applies only bundles `zpkg` or zenixboot
  verified. Packages from the network are covered by the signed list's SHA-256 values.
- **The list's format, serial number and expiry rule belong to `zpkg`'s own note**, with this note's recommendation:
  a signed serial (an older list refused) and a signed expiry date refused when passed (judged against the clock floor
  of 8.3, with an override for the owner), which stops a freeze attack over plain HTTP. **So `zpkg` needs no TLS:**
  plain HTTP from the NAS keeps integrity (owner question 6).

### 6.6 Cost and the first install

One Ed25519 verification per update: *estimate* 6-9 s on the stock A1200 in plain C (zenixboot runs under AmigaOS
there). zenixboot grows by about 20 KB of code (*measured*: Ed25519 verification alone links to 19,432 bytes, 21,932
at -O2) and needs about 2 KB more stack at its peak (*measured*: `crypto_eddsa_check_equation`'s frame is 1,136
bytes); it is started from a one-line Startup-Sequence with the boot shell's default stack (about 4 KB), so step 5
measures zenixboot's total and uses `StackSwap` if needed. **Signatures protect nothing until the owner installs the
new zenixboot by hand:** step 5 includes that card package, with the owner.

## 7. Size, memory and time

### 7.1 Code (*measured* 2026-10-27, `-m68020-60 -Os`, `--gc-sections`, plain C; BearSSL 7bea48e, Monocypher f5f0a19)

| Linked test program | Code (-Os) | Code (-O2) | Data + BSS |
|---|---|---|---|
| X25519 only (Monocypher) | 12,292 | 12,684 | 96 |
| Ed25519 verify only (Monocypher, SHA-512 included): zenixboot's and `zpkg`'s share | 19,432 | 21,932 | 160 |
| ssh's primitives: X25519, Ed25519 sign and verify, ChaCha20, Poly1305 (Monocypher); SHA-256, RSA `i15` and ECDSA P-256 verify (BearSSL) | 48,576 | 52,776 | 288 |
| A TLS 1.2 client (BearSSL): X25519/P-256/P-384/P-521 ECDHE, RSA `i15` and ECDSA verify, X.509, ChaCha20-Poly1305, AES-GCM (`ct`), SHA-256/384/512 | 77,888 | | 39,572 (33 KB the two-way record buffer) |

Stack (*measured*, `-fstack-usage`): Ed25519 verification about 2 KB at its peak, BearSSL's largest frame about
2.3 KB (RSA): an 8 KB stack suffices. Everything else is *estimate*: ssh's core 20-30 KB, the program with libz
15 KB: **`ssh` about 85-100 KB** at step 3 (step 3b's RSA/ECDSA host keys, AES-GCM, Blowfish and the config parser add
about 15-25 KB); `ssh-keygen` 40 KB; `wget` with TLS 100-110 KB (P-521 dropped: about -3 KB; BearSSL's one-way
"mono" 16.7 KB buffer for HTTP). None goes on the RAM floppy; the demo floppy counts them in its own step.

### 7.2 Memory on the stock A1200 (2 MB chip, SLIP up: 1,159 KB free, D19 *measured*)

- **ssh:** code ~90 KB (one copy for all sessions, pure); the receive buffer starts at 18 KB (the channel's 16 KB
  maximum packet plus headers) and grows to RFC 4253's 35,000 bytes only when a larger packet actually arrives; an
  8 KB send buffer; stack 8 KB; the pure data and GOT (measured in step 3): **about 130-140 KB for the first session,
  40-50 KB for each further one**, so about 1,020 KB stay free during one session.
- **wget with HTTPS:** ~105 KB code (not pure, 3.5), 17 KB record buffer, 7 KB contexts, the buffered chain and the
  anchors it names (8.1): about 145 KB.

### 7.3 Time (all *estimates*; measured on the real machines in step 2)

**The basis.** BearSSL's figures on a 48 MHz ARM Cortex-M0+ (*read*, bearssl.org/speed.html): c25519_m15 4.42
operations/s, p256_m15 1.72 point multiplications/s (2.55 with a fixed base), RSA-2048 public (`i15`) 2.2/s, ChaCha20
551 KB/s, Poly1305 289 KB/s. **The rule:** the stock A1200 (14.18 MHz, chip RAM only) is taken as **6-8 times slower
per operation** for this multiply-heavy code (a 27-43-clock multiply against the M0+'s 1, a third of the clock rate,
chip-bus waits). So: X25519 0.23 s x 6-8; an ECDSA P-256 verification is one variable and one fixed-base
multiplication plus ~15% (about 1 s on the M0+); RSA-4096 public is about four times RSA-2048; P-384 (`ec_prime_i15`,
generic) about three to four times P-256. Monocypher's X25519 is slower than BearSSL's (100 `__muldi3` calls per field
multiply, *measured*): about 4-6 s. The 060 at 80 MHz with fast RAM: 30-50 times the stock A1200. The PiStorm (Emu68's
JIT): milliseconds (AmiSSL's handshake took 50-70 ms there, *measured*, knowledge base).

| Operation | Stock A1200, plain C | 68060 |
|---|---|---|
| X25519: BearSSL `c25519_m15` / Monocypher | 1.4-1.8 s / 4-6 s | 0.05-0.2 s |
| Ed25519 verify (Monocypher) | 6-9 s | 0.15-0.3 s |
| Ed25519 sign (Monocypher) | 2-4 s | 0.05-0.1 s |
| RSA-2048 verify | 3-4 s | about 0.1 s |
| RSA-4096 verify | 11-15 s | 0.3-0.5 s |
| ECDSA P-256 verify | 6-9 s | 0.2-0.3 s |
| ECDSA P-384 verify | 20-35 s | 0.5-1 s |
| ML-KEM-768 (step 6) | 1-2 s | < 0.1 s |
| ChaCha20-Poly1305 | 20-35 KB/s | 1-2 MB/s |

The portable assembler (3.6) is expected to halve the public-key rows; step 2 decides.

- **An ssh login** (two X25519, one Ed25519 verify, one Ed25519 sign): **about 10-25 s on the stock A1200 in plain
  C**, depending on which X25519 wins step 2; under a second on the 060; instant on the PiStorm. Typing afterwards:
  SLIP's 11.5 KB/s is the limit; a large `cat` keeps the CPU busy decrypting (20-35 KB/s).
- **An HTTPS connection** (two X25519, the server's signature, two certificate signatures): **about 20-30 s** for a
  site with an RSA chain (Let's Encrypt R-series: RSA-2048, RSA-2048, RSA-4096), **about 1-1.5 minutes** for an ECDSA
  chain (P-256 and two P-384: Let's Encrypt E-series under ISRG Root X2); a second or two on the 060. Many servers wait
  60 s for a handshake, some 10-30 s: on the unexpanded A1200 HTTPS works with patience and fails on some sites, said
  in `wget`'s page (owner question 5). Saving handshakes: HTTP/1.1 keep-alive (`zpkg` and `wget` fetch many files over
  one connection) is what counts; BearSSL resumes only by session ID (no tickets) and many servers keep no ID cache
  (nginx's default), so resumption is a bonus, kept in the process only and keyed by host and port.

## 8. TLS for `wget` (the HTTPS half; `wget` itself is its own step)

### 8.1 Trust anchors (the root certificates)

- **The source:** Mozilla's `certdata.txt` (MPL-2.0; compatible: MPL-2.0's secondary licences include GPL-2.0, and
  certdata carries no "incompatible" notice). `tools/mkanchors.py` keeps only roots **trusted for TLS servers**, and
  **drops roots with a "distrust after" date** (Entrust, Chunghwa and any later one: BearSSL cannot apply such a date).
  The exact `certdata.txt` used is kept in the repository beside the tool with its MPL notice (MPL's source-form rule),
  and the notice ships in `/usr/share/licenses/`.
- **The file `/etc/ssl/anchors`** (frozen once shipped; *estimate* 50-70 KB for about 145 roots), big-endian:
  a 16-byte header (`ZTA1`, format u16 1, count u16, the index's offset u32, the data's offset u32); the index, one
  12-byte entry per anchor sorted by hash (the first 8 bytes of SHA-256 of the DER-encoded subject name, the entry's
  offset u32 from the data's start); each entry: flags u8 (bit 0 a CA), key type u8 (1 RSA, 2 EC), the name's length
  u16 and the DER name; then for RSA the modulus's length u16, the modulus, the exponent's length u8, the exponent; for
  EC the curve u8 (BearSSL's ids: 23 P-256, 24 P-384, 25 P-521), the point's length u8 and the uncompressed point.
  **Several roots may share a name** (re-keyed roots): every matching entry is loaded.
- **Loading only what is needed:** BearSSL's `x509_minimal` takes its anchor array before the chain arrives, so
  `crypto/zc/`'s **X.509 wrapper** (a `br_x509_class` around `x509_minimal`) buffers the server's chain (at most 16 KB:
  a longer chain is refused), reads each certificate's issuer name with `br_x509_decoder`, loads the anchors whose
  hash matches (from the index and from `/etc/ssl/local/`), fills the array, then replays the chain into
  `x509_minimal`. It also does the checks BearSSL lacks (8.2).
- **The owner's own:** PEM files in `/etc/ssl/local/` (root's), read by the wrapper at run time (the NAS's self-signed
  certificate, a test CA).
- **Updates:** a package `ca-certificates` (version = Mozilla's date), through `zpkg` and D15 bundles, signed. The
  demo floppy may carry a smaller list (its step decides).

### 8.2 The checks

BearSSL's `x509_minimal`: the chain **in the order the server sends** (no path building, no fetching of missing
intermediates: a misconfigured server fails), each signature (RSA PKCS#1 v1.5 or ECDSA; **RSA-PSS is not supported**:
a PSS-signed certificate fails, rare on the web), the server name against subjectAltName **DNS names** (wildcards in
the first label only; **IP-address names are not matched**: `https://<NAS address>` works only through the NAS's name
or a DNS-name entry), basic constraints, key usage, the dates (8.3); an unknown critical extension refuses the chain
(rare name-constrained intermediates fail). **Added by the wrapper:** the leaf's Extended Key Usage must include
serverAuth when present (BearSSL ignores EKU). **Not checked: revocation** (no OCSP, no CRL). **No extended master
secret** (RFC 7627, absent from BearSSL): with no client certificates and no renegotiation there is no known attack,
but servers requiring it (FIPS mode) refuse. SNI always; ALPN `http/1.1`. An HTTP body without a length that ends
without TLS `close_notify` is reported as truncated.

### 8.3 The time (certificates need a date)

- **Sources, in order:** the battery clock at boot (the owner's A1200 has one); **SNTP**; **a floor** that the time
  used for certificates never goes below: the newer of the library's build date and the file `/var/lib/clock`.
- **`sntp`** (a root command, `user/sntp.c`): **`ifup` starts it** in the background when an `auto` interface has come
  up, if `/etc/sntp` exists (init starts `ifup -a` without waiting, so init cannot order it); it asks up to four
  servers, waits at most 10 s, never delays a login, and sets the clock with `SYS_settime`. The reply is checked
  (its origin timestamp equals the random transmit time we sent, stratum 1-15, leap indicator not 3, a "kiss of death"
  obeyed). **`/etc/sntp`** (frozen once shipped; `server HOST` lines, at most four, `#` comments, unknown words
  reported and ignored) is **shipped** with a vendor zone the project applies for at the NTP pool
  (`zenix.pool.ntp.org`, as the pool asks of operating systems; owner question 4); until the zone exists, it is not
  shipped and the page shows the line to add.
- **`/var/lib/clock`** (frozen once shipped; root's, mode 644): one line `zenix-clock 1 SECONDS`, unsigned decimal.
  **Written by** `reboot` (before the reset, as the seed file) and by `sntp` after an accepted answer; read by the
  library. **Never poisoned by one bad answer:** `sntp` sets the clock but raises the floor file only when the answer
  agrees within 10 minutes with the battery clock, or (with no battery clock) is not more than 400 days past the
  current floor; `sntp -r` (root) resets the floor to the build date. A battery clock's time below the floor is
  reported once at boot.
- **Without a trusted time** (no battery clock and no accepted SNTP answer this boot): BearSSL's time callback
  (`br_x509_minimal_set_time_callback`, which receives the dates as days and seconds) checks "not after" against the
  floor and skips "not before"; `wget` says once "the clock is not set: certificate dates checked only against
  <date>". A certificate that expired before the floor is still refused.
- **Authenticated time later:** Roughtime needs only Ed25519 and SHA-512, which the library has (NTS needs TLS 1.3):
  the step after `sntp` if wanted.
- **2038:** times are read unsigned (good to 2106; section 14's `(s32)` fix) and converted to BearSSL's days and
  seconds by the callback, which also removes `x509_minimal`'s 64-bit division (3.5).

## 9. Security

### 9.1 What a machine without an MMU means for keys (said plainly, as D19 9.2)

- **Any program can read any memory and the disk's hardware.** A key in `ssh`'s memory, a session key, a typed
  password: a hostile program running on the Amiga at that moment can read them. Users are a convenience, not
  protection (D8). So: **never run programs you do not trust while logged in to something that matters**; said in
  `ssh`'s and `ssh-keygen`'s pages.
- **Keys at rest:** mode 600 keeps them from honest programs and other users' tools; a passphrase keeps them from
  someone who takes the card; neither stops hostile code on the machine.
- **Lifetimes:** the passphrase and password wiped after use, the private key wiped after authentication, ephemeral
  keys after the exchange; wiping through a `volatile` pointer (gcc drops a plain `memset` before `free`). No core
  dumps, no swap.
- **What still holds:** against the network. The server is authenticated, the session encrypted and authenticated.

### 9.2 Side channels

- **Remote timing** is what matters here (a local attacker can read memory anyway). No secret-dependent branches or
  table indices (BearSSL `ct`/`i15`/`m15`, Monocypher; the assembler keeps the rule; Blowfish's tables in bcrypt are a
  local matter only).
- **The multiplier:** BearSSL's list of CPUs with variable-time multiplies does not mention the 68k. The 68000's `mulu`
  is data-dependent (38 + 2n clocks); the MC68020 manual gives `MUL.W` 25-28 and `MUL.L` 41-44 clocks and calls the
  `MUL.L` times maximums. **Step 2 measures it on the stock A1200** (the E-clock over 100,000 multiplies with operands
  0, 1, 0xFFFF..., random); if it varies: `BR_CT_MUL15` for BearSSL, the assembler for the rest.
- **Caches:** the 68EC020 has no data cache (256 bytes of instruction cache); the 030, 040 and 060 do, so the
  no-secret-index rule matters there (only BearSSL's `ct` AES).
- Power and electromagnetic analysis: out of scope.

### 9.3 Protocol rules (each a past attack)

ssh: strict kex required (Terrapin, 4.2); the Poly1305 tag checked before decrypting and nothing done with the
decrypted length but the bounds check; no CBC, no non-ETM MACs, no compression; SHA-1 only as HMAC-SHA1 for hashed
`known_hosts` names; RSA 2048 bits or more; all-zero X25519 results refused; host key changes refused; server text
cleaned before display (4.3); every length from the network checked against the buffer before use; packets over
35,000 bytes end the connection. TLS: 1.2 only, ECDHE only, AEAD only, no renegotiation, no SHA-1, SNI, EKU added, no
extended master secret (8.2, said).

### 9.4 The review plan

(1) The imports checked against upstream (unchanged but for the listed patches); (2) `net/ssh/`, `crypto/zc/` and the
assembler reviewed independently before their merges, against this note and the RFCs; (3) the fuzzers of 11.1 run
before each merge; (4) the security audit at roadmap block 2's end covers ssh, TLS, signatures, the random source,
the time, file permissions.

## 10. Build order (each step small, merged with its tests; the full suite before each merge)

Steps that touch `Makefile` (`UAPPS`, images), `user/usys.h`, libz, `kernel/*`, the root images, `tools/check.sh`,
`docs/source-tree.md` or the counts in `docs/` are **the main session's** (CLAUDE.md "Sessions"); a side session takes
only the host-only parts marked *side*, with this note's names and formats. The main session adds `crypto/`,
`net/ssh/` and `tools/keys/` to `docs/source-tree.md` and the host tests to `tools/check.sh` when it merges.

0. *side:* **the library on the PC.** `crypto/bearssl/`, `crypto/monocypher/` (only the files used, the pins of 3.2,
   READMEs, licences), `crypto/zc/` (random bytes through a callback, wiping, the zero-result check, ssh's
   ChaCha20-Poly1305 framing with the key extension), built for the PC and as m68k objects (7.1 re-measured). Host
   tests (`tools/host/crypto-test.sh`): RFC 7748 (X25519, the 1,000-iteration check, the all-zero cases), RFC 8032,
   RFC 8439, NIST's SHA vectors, BearSSL's `test_crypto`, OpenSSH's chacha20-poly1305 packets, differential runs
   against the PC's OpenSSL. Wycheproof's vectors (Apache-2.0 data) are fetched into a cache the first time, never
   committed (`WYCHEPROOF_DIR`, like `LRZSZ_DIR`). A missing tool on the PC is a clear "skipped", never a pass.
1. *side:* **`net/ssh/`**, the system-call-free core (transport, strict kex, packets, channel, authentication, the
   `known_hosts`/key-file/config parsers, the text cleaner); a host client (`tools/host/ssh-client.c`) logging in to a
   real OpenSSH `sshd` and a Dropbear server in `unshare -rnm` (test host keys per run; `RekeyLimit 1M`; key, password
   and keyboard-interactive logins; `ClientAliveInterval`; a changed host key; a server without strict kex refused);
   fuzz harnesses for every parser and the packet layer (one at a time, a memory cap). Also `tools/zsign.py`, the
   `.sig` format with vectors (signed by OpenSSL, verified by the library and back), `tools/mkanchors.py` with a test
   CA and the X.509 wrapper's host tests.
2. *main:* **`cryptotest`** (`/tests`: the vectors on the m68k code; timings by the E-clock; the multiplier check of
   9.2; BearSSL's and Monocypher's X25519 side by side), the library's build rules, `check-68020-60` over it; FS-UAE
   runs the vectors; **the owner's A1200 and 060 give the time table** (never emulator figures). Then the X25519
   choice (3.2) and, if the table asks, the portable assembler (3.6), checked against the C.
3. *main, after D20 steps 1-3:* **`ssh-keygen` and `ssh`** with step 3's algorithm column (3.3), pages,
   `commands.md`, `features.md`; FS-UAE tests over the A2065 and SLIP to the namespaced `sshd` (key and password
   logins, `ssh host command`'s exit status, `~.`, ^C to the remote, a changed key refused, a forced rekey, 1 MB
   checked byte for byte, `free` on the 2 MB machine, the pure per-session cost). **First thing the owner can use.**
3b. *main:* the wider column: RSA/ECDSA host keys, `aes256-gcm`, passphrases (Blowfish, `bcrypt_pbkdf`), hashed
   `known_hosts`, `~/.ssh/config`.
4. *owner:* ssh from the PiStorm (GENET) and the stock A1200 (SLIP) to the PC; the times, labelled.
5. *main:* **signatures** (D15 step 7): zenixboot (section 6.4, `sigdigest`, the stack measured), `zsign.py` in the
   `bundle` rule with the test key, `RELEASE=1`, `zpkg`'s check on the cable; the licences in `mkzup.py`'s `LICENSES`;
   FS-UAE: a good, a bad, a missing and a test-key signature on a release build, a trial's second start not verifying
   again; **the owner's card package with the new zenixboot.**
6. *main:* `mlkem768x25519-sha256`, offered first; interop with OpenSSH 10.
7. *main, with `wget`'s own step:* TLS: the anchors file and package, `/etc/ssl/local/`, the wrapper, `sntp` and
   `/etc/sntp`, `/var/lib/clock` and the floor, the time callback, the patch removing `x509_minimal`'s 64-bit
   division; tests against a local HTTPS server with a test CA (good, expired, not yet valid, wrong name, unknown CA,
   missing intermediate, no serverAuth, RSA-4096 and P-384 chains, a TLS 1.3-only server's message, keep-alive,
   resumption counted, a lying SNTP server not moving the floor).
8. *later, each when asked:* `sftp`/`scp`, `ssh-agent`, forwarding, Roughtime, `sshd` (after D20's ptys), TLS 1.3.

## 11. Tests (summary; each step lists its own)

### 11.1 On the PC

Vectors (RFC 7748, 8032, 8439, NIST, BearSSL's, OpenSSH's, Wycheproof from a cache), differential tests against
OpenSSL, the ssh core against OpenSSH and Dropbear servers in a user and network namespace (no root, nothing leaves
the PC), `.sig` both ways, the anchors tool and wrapper against a test CA. **Fuzzing** with libFuzzer, one run at a
time with a memory cap (this machine's rule): the ssh packet layer and every message parser, `known_hosts`, key files,
the config, the text cleaner, the `.sig` and anchors files, the X.509 wrapper, BearSSL's engine fed records. Which of
these join `make check` (the vectors, the ssh core against `sshd`, the signatures) is named in each step; each skips
clearly when `sshd`, `dropbear` or `openssl` is missing.

### 11.2 In FS-UAE

`cryptotest`'s vectors on every CPU the matrix has; ssh, later wget, over the A2065 and over SLIP through
`zenix-slirp` to the namespaced servers; zenixboot's signature cases; memory on the 2 MB and 1 MB machines. No speed
figure from the emulator.

### 11.3 On the real machines (the owner's)

`cryptotest`'s time table and the multiplier check on the stock A1200 and the 060; ssh to the PC from the PiStorm and
from the stock A1200; later an HTTPS fetch from each.

## 12. Frozen once built (CLAUDE.md's list gets them in their steps)

The `.sig` format, its life beside the bundle, and the labels with their digests (6.1-6.2); zenixboot's state-file
key `sigdigest` (6.4); the anchors file format (8.1); `/var/lib/clock`'s format and `/etc/sntp`'s format (8.3); the
release-channel and `RELEASE=1` rule for which key is trusted (6.3). `zpkg`'s list format is frozen in `zpkg`'s note.
OpenSSH's formats (`openssh-key-v1`, `known_hosts`, `authorized_keys`, the `ssh_config` subset) are followed, not ours
to freeze. **Nothing here changes an existing frozen meaning:** no system call, message type, struct or base `/etc`
format; D15's bundle header and update note are untouched (the signature is detached; the state file only gains a key
older readers ignore).

## 13. Open questions for the owner (one line each, the recommendation in brackets)

1. TLS 1.2 only for now, TLS 1.3 later only if a site you use needs it? [Yes]
2. The Amiga gets its own ssh key, made on Zenix, instead of a copy of the PC's? [Yes: its own, added to the PC's authorized_keys]
3. A passphrase on that key, which takes minutes to unlock on the unexpanded A1200? [No by default; optional]
4. Set the clock from the internet (a Zenix zone at the NTP pool) when the network comes up? [Yes; your RTC module first]
5. HTTPS on the unexpanded A1200 takes half a minute to a minute and fails on some sites: acceptable? [Yes; ssh and updates stay usable there]
6. `zpkg` fetches from the NAS over plain HTTP, the packages signed? [Yes; HTTPS optional]
7. Refuse unsigned update bundles once the new zenixboot is installed? [Yes; test builds excepted]
8. File copying (`scp`/`sftp`) after ssh is in daily use? [Yes]

## 14. For the main session (not owner questions)

- **The feature halt:** the roadmap (approved order) names the crypto library, ssh and HTTPS; this note adds no kernel
  interface. The owner's approval of this note is their approval of its programs (`ssh`, `ssh-keygen`, `sntp`,
  `cryptotest`) and of zenixboot's signature check.
- **Found by the review, "fix when found":** `SYS_time` is `time_base + ticks/100` with a 32-bit tick count (it jumps
  back after 497 days of uptime), and `syscall.c` returns it through `(s32)` (wrong after 2038 for a caller reading it
  signed). Both are kernel fixes, independent of this note.
- **Review depth:** crypto and the update path got two reviews (section 15); a second look at revision 2 before step 0
  is the main session's call.
- **A later CPU report** (3.6 item 3) through `struct uupdate`, only if the A1200's numbers ask for it.

## 15. Reviews

Two independent read-only reviews of revision 1 (2026-10-27); every finding is worked in above.

**Cryptography, protocols, licences** (read against BearSSL 7bea48e, Monocypher f5f0a19, the MC68020 manual,
OpenSSH's `PROTOCOL` files, bearssl.org): no blocker; the licences confirmed (MPL-2.0 data and Wycheproof included).
*Should fix* - no Blowfish/`bcrypt_pbkdf` source was named (OpenSSH's BSD-3/ISC files, 3.2); BearSSL has no RSA-PSS
(3.3, 8.2); "one anchor in memory" cannot work with `x509_minimal`'s up-front array (the buffering wrapper, 8.1); build
the anchors from `certdata.txt` with the server-trust bits and the distrust-after roots dropped (8.1); the all-zero
X25519 check and the adapter's byte order and curve forwarding (3.2); strict kex spelled out and servers without it
refused (4.2, 3.3); the chacha20-poly1305 packet order (length decrypted first, used only for bounds; tag; then the
payload) and the key extension (4.2); want-reply requests answered with FAILURE, EXT_INFO's two places (4.2); server
text cleaned before display (4.3); per-object signature labels and what `index.sig` signs (6.1); zenixboot's key
rotation contradicting D15 (6.3); the SNTP floor poisoned by one false answer (8.3); the HTTPS estimate too low for
ECDSA chains, RSA preferred on slow CPUs (7.3, 3.3). *Minor* - no extended master secret and no EKU check in BearSSL
(8.2); IP-address names, chain order, SHA-1 off, `minrsa` (8.2, 3.3); `i15` uses a 32x32->32 `MUL15`, `BR_CT_MUL15`
(3.2); `poly1305_ctmul32`/`ghash_ctmul32` named; Monocypher's timing fix after 4.0.3 (3.2); mpint and string encodings
of K, keys held during a rekey, PK_OK, unlocking a passphrase before connecting, `known_hosts` markers and patterns
(4.2, 4.4); the AES-CTR+ETM option dropped; a signed expiry for `zpkg`'s list (6.5); SNTP reply checks, a vendor zone,
Roughtime later (8.3); the table's rows recomputed from its own basis, ML-KEM and bcrypt slower (7.3); resumption
worth little (7.3); truncation without `close_notify` (8.2); one per-process generator for ssh (4.2).

**Architecture, frozen interfaces, size, the stock A1200** (with the sizes re-linked and re-measured, all exact):
*blocker* - a bad signature renamed `.rejected` with a reason in the update note: `.rejected` is the owner's "previous"
choice and the update note's kinds are frozen; now `bundle_bad()`'s `.bad` and the reason on screen (6.4). *Should
fix* - the verification order and re-verifying at every trial start (the `sigdigest` key, 6.4); zenixboot's keys
rotate only by hand, signatures protect nothing until the new zenixboot is installed, BOOT: writers can replace
zenixboot (6.3, 6.4, 6.6); the serial-cable transport (6.5); undefined frozen formats: the labels for other files, the
anchors file in full, `/var/lib/clock`, `/etc/sntp`, who writes the clock file (6.1, 8.1, 8.3); `sntp` cannot run
"after" an `ifup -a` init does not wait for (`ifup` starts it, 8.3); `wget` cannot be pure under D13 stage 1
(`x509_minimal`'s 64-bit division pulls `__clz_tab`; vtables in per-process data, 3.5); the dependency on D20's
`CON_RULES`/`LFONLY` and steps 1-3, and D20's nice 1 and 256-byte writes being design, not built (2, 4.3, 10); the
measurements were at Monocypher f5f0a19, not 4.0.3 (3.2); X25519 measured both ways before the adapter (3.2, 7.3);
the login figure made consistent (1, 7.3); who refuses test-key release signatures, and what "release" means (6.3).
*Minor* - signing the header digest is a deliberate refinement of D15 (6.1); the `.sig`'s life with renames (6.2);
the CPU report's carrier (3.6); D10's strict priorities starve ssh behind a busy nice-0 program (4.3); `SYS_time`'s
wrap and `(s32)` return (2, 14); import only the files used, `source-tree.md`, `check.sh`, `tools/keys/` (3.2, 10);
stage ssh's algorithms (3.3); a receive buffer that grows (7.2); -O2 for the hot files (3.6, 7.1); zenixboot's stack
(6.6); Monocypher under CC0, the `LICENSES` list (3.2, 10); clear skips for missing PC tools (11.1).

Revision 2 changes no part of the overall shape (BearSSL and Monocypher in one static library, Zenix's own ssh
client on D20's terminal code, `/dev/random` only, detached Ed25519 signatures, no kernel change); both reviewers
judged that shape sound, the architecture reviewer approving once the D15 joint was fixed.

Last checked against the code: 2026-10-27 (main at f10de79).

## Owner decisions (2026-10-27)

"Yes" to every recommendation of the owner questions: (1) TLS 1.2 now, 1.3 only if a site needs it; (2) the Amiga
gets its own ssh key; (3) no passphrase by default, optional; (4) the clock from the internet when the network comes up,
the battery clock first; (5) HTTPS may take up to a minute on the stock A1200; (6) `zpkg` over plain HTTP with signed
packages, HTTPS optional; (7) unsigned update bundles refused once signing ships, test builds excepted; (8) `scp`/`sftp`
after ssh is in daily use.

## As built: steps 0-1, the PC-side parts (side helper, merged 2026-10-27; reviewed by the main session)

Built: `crypto/bearssl/` (the 60 files used, at 7bea48e, unchanged), `crypto/monocypher/` (4 files at f5f0a19,
unchanged), `crypto/openssh/` (blowfish.c, blf.h, bcrypt_pbkdf.c from OpenSSH V_10_6_P1, unchanged, plus two shim
headers); their licence files are named LICENSE/LICENCE/AUTHORS (docs-check refuses .md/.txt outside docs/), and the
tests compare every imported file with upstream byte for byte. `crypto/zc/`: wiping, the per-process ChaCha20 generator,
X25519 (all-zero result refused; BearSSL by default, Monocypher behind `ZC_X25519_MONOCYPHER` until step 2 times both),
ssh's mpint and key derivation, chacha20-poly1305@openssh.com (tag checked before decrypting), strict Ed25519 and ECDSA
checks in front of the libraries' lenient ones (Monocypher accepts a malformed R, Wycheproof case 151, in every version
since 4.0.2; BearSSL accepts BER and odd-length raw ECDSA encodings: neither is a forgery, both are refused now), the
X.509 wrapper and the anchors reader, `zc_sig_verify` (shared by zenixboot and zpkg). `net/ssh/`: the system-call-free
core (step 3's algorithms, strict kex required, rekeying both ways, OpenSSH's auth order, the key-file incl. bcrypt,
known_hosts, config and text-cleaner parsers). `tools/zsign.py`, `tools/mkanchors.py`, `tools/certdata` (Mozilla's,
pinned NSS_3_132_RTM; 117 roots, an anchors file of 52,231 bytes). Tests: `tools/host/crypto-test.sh` (in `check.sh
host`; 167 RFC/FIPS vectors, BearSSL's test_crypto, 4,000+ Wycheproof cases fetched at test time and never committed,
about 5,000 differential cases against OpenSSL, 28 checks against OpenSSH 10.6's sshd, the signatures, the anchors);
11 fuzz harnesses (`crypto-test.sh fuzz SECONDS`), 22 minutes, nothing found. Sizes (m68k -Os, measured): Ed25519 verify
19,334; zenixboot's .sig check 22,222; ssh primitives 47,778; the ssh core with its parsers 73,650; the TLS client with
the X.509 wrapper 82,554 (all within about 1 KB of section 7.1). No private key is in git: the TEST pair is generated by
`zsign.py testkey` into `$ZSIGN_TEST_DIR` or `build/sign-test/` (PEM, mode 600), and source files split the PEM marker.

Choices where the note was silent (binding unless changed here): (1) the bcrypt files live in `crypto/openssh/` (step 3
may split `ssh_key_read` so plain keys do not link bcrypt's 6.7 KB); (2) the X.509 wrapper is built and tested now in
`crypto/zc/zc_x509.*`; (3) the issuer name is read by a small DER walk of zc's own (BearSSL gives only the subject);
(4) `zc_sig_verify` lives in `crypto/zc/`; (5) Mozilla's file is `tools/certdata`; (6) zsign.py's release key defaults
to `~/.config/zenix-signing/zenix-release.key`, public keys are PEM, the TEST pair is generated, never committed;
(7) **channels (main session, 2026-10-27):** test bundles carry the channel `test` (mkzup's default); `testing` and
`stable` are release channels, and a release-channel bundle is never signed with the TEST key; (8) the ssh version
string is the caller's; (9) the KEXINIT lists `hmac-sha2-256-etm@openssh.com` only as a known name, never used;
(10) the text cleaner turns CR LF into LF, a lone CR, bad UTF-8 and C1 controls (0x9B included) into `?`; (11) a
known_hosts entry with only another key type means "unknown, ask with a warning", a different key of the same type is
refused; (12) limits: bcrypt rounds at most 1024, at most 8 keyboard-interactive prompts, at most 8 certificates a
chain; (13) the X.509 wrapper refuses "no time" itself (BearSSL would quietly use `time()`); (14) sshd in the tests
runs under `unshare --map-root-user --map-auto -nm`; (15) fuzz builds use `-fno-sanitize=function`. For step 3: the
caller keeps calling `ssh_write` until it returns 0 before it polls.

## As built: step 2, `cryptotest` (branch m4, 2026-10-27; the A1200's and the 060's table not yet made)

The library for Zenix programs: the Makefile builds `crypto/` (BearSSL's 60 files, Monocypher, OpenSSH's bcrypt files,
`crypto/zc/`) into `build/libcrypto.a`, linked with `--gc-sections`; -O2 for the files a handshake waits on (3.6
step 1: Monocypher, `ec_c25519_m15`, `i15_montmul`, `ec_p256_m15`, `chacha20_ct`, `poly1305_ctmul32`, SHA-2), -Os for
the rest; the same `BR_*` defines as the PC tests (no unaligned access, no system random source). The programs include
the library's headers through `net/port/shim` (libz is the C library: no newlib header clashes); libz gained `calloc`
(bcrypt's). `check-68020-60` passes over `cryptotest`, `ssh` and `ssh-keygen`.

`user/cryptotest.c` (`docs/man/cryptotest.txt`): the known answers from `tools/host/crypto-vectors.h` (the PC tests'
file, one copy): 83 checks (X25519 both ways with the all-zero cases, RFC 8032, RFC 8439, FIPS 180, RFC 4231, ssh's
packets); `cryptotest time [N]` by the E-clock (its rate measured against the tick); `cryptotest mul` (9.2: 100,000
`muls.l` and `mulu.w` with four operand classes). **Choices:** the 1,000-iteration X25519 check stays on the PC (minutes
on a 68020); no assembler and no X25519 switch yet: the note waits for the real machines' table (the weekend checklist,
`a1200-checklist-network.md` section 10), so `zc_x25519` stays BearSSL's.

**FS-UAE's figures (the emulated 68EC020 of the stock A1200; an emulator's, NOT the machine's):** X25519 BearSSL 4.84 s,
Monocypher 8.17 s; Ed25519 sign 3.37 s, verify 11.18 s; chacha20-poly1305 16 KB/s; SHA-256 31 KB/s; `muls.l` and
`mulu.w` the same time for every operand (spread 0%: FS-UAE does not model data-dependent multiply times, so this says
nothing about the real 020). Size: `cryptotest` 78,368 bytes. **Tests:** `tools/crypto68k-test.sh` (in `make check`):
the known answers on the 020 (stock A1200), 030, 040 and 060 machines, all pass; on the 020 the time and multiplier
runs complete.

## As built: step 3, `ssh-keygen` and `ssh` (branch m4, 2026-10-27; not yet run on the A1200)

`user/ssh.c` (`docs/man/ssh.txt`) over `net/ssh/` and the library, `user/ssh-keygen.c` (`docs/man/ssh-keygen.txt`).
As 4.2-4.5 say: step 3's algorithm column; OpenSSH's files; trust on first use (`yes/no/[fingerprint]`, read on
`/dev/tty`), a changed or revoked key refused with the file, line and `ssh-keygen -R`; `/etc/ssh/ssh_known_hosts` read
when the user's file does not know the host; the Unix mode's console rule `CONR_LFONLY` and telnet's Unix filter and
keys when a terminal is asked for and the output is the console, the bytes untouched otherwise (`ssh host cat f > g`);
`TTY_RAW | TTY_NOSIG` while a terminal session runs; OpenSSH's escapes `~.` `~^Z` `~?` `~~` after Return; nice 1;
256-byte screen writes; `ssh_write` called until it takes nothing before each poll (the step-1 choice); the key wiped
once the session is open; `-v` prints each step with its time since boot. Choices where the note was silent:
1. **The identity:** `-i KEY`, else `~/.ssh/id_ed25519` when it exists; a key file open to group or others is ignored
   with OpenSSH's "UNPROTECTED PRIVATE KEY FILE" warning; a passphrase is asked on the terminal before connecting.
2. **A terminal** when no command is given and standard input is a terminal (`-t` forces, `-T` never), as OpenSSH.
3. **`ssh-keygen`:** `-t ed25519 -f -C -N -a -q`, `-l`, `-y` (`-P` for a protected key), `-R HOST` (known_hosts, the old
   file as `.old`); an existing key file is never overwritten; `~/.ssh` made 700, the key written 600 before its
   secret goes in, `.pub` 644; the comment `$USER@$HOSTNAME`.
4. **`ssh` is a program with shared code** (D13 ZEX2, as 3.5 says: `PUREAPPS`): a pure build of the library, the core
   and telnet's filter through `tools/purecc.sh` (`build/libcrypto-pure.a`); no table form purecc refuses appeared.

**Sizes (*measured*):** `ssh` 103,200 bytes of shared code, 1,716 of data and 30 KB of other memory per process (the
plain build was 102,468 bytes); `ps` during a session: 113 KB own plus 100 KB shared; `ssh-keygen` 54,148 bytes. The
kernel is unchanged by this step. **Memory on the stock A1200** (2 MB chip RAM, FS-UAE, the A2065 with DHCP, one
login, one ssh session): **850 KB free** (the note's estimate was about 1,020 KB: ssh's receive buffer grows to the
35,000-byte packet, its static buffers and the A2065's netd count too). **Times (FS-UAE's emulated 68EC020, an
emulator's):** a key login 26 s from the start to the session (key exchange to host key verified 22 s, the signed
publickey request 4 s); 1 MB through the session in about 140 s (about 7.5 KB/s, the CPU decrypting) with the
server's rekey in the middle.
**Tests:** `tools/ssh-test.sh` (FS-UAE, in `make check`; `tools/ssh-ns.py` runs two OpenSSH sshds in a user, mount and
network namespace, `unshare --map-root-user --map-auto -nm`): `ssh-keygen` makes root's key (mode 600, OpenSSH's public
line); the first `ssh` asks about the unknown host (answered yes), logs in with the password and appends the Amiga's
key to the PC's `authorized_keys`; a key login (sshd's log: "Accepted publickey") runs a command whose exit status 7
comes back; 1 MB from the PC byte for byte (compared on the PC and by `sha256sum` on Zenix) with the server's rekey
(`RekeyLimit 1M`, two key exchanges); a host whose key differs from known_hosts refused (exit 255, no login on that
sshd); an interactive shell (a command, Ctrl-C to a remote `sleep`, `free` and `ps` on console 2), `~.` (exit 255).
SLIP was not tried (the A2065 path covers the protocol; over SLIP the 11.5 KB/s line is the limit): left for the A1200.
