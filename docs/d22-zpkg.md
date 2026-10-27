# D22: `zpkg`, the package manager (design note, revision 1)

Status: **design, revision 1, nothing built; to be reviewed twice before any code.** The owner's wish of 2026-10-24
(future-ideas.md, "A pacman-like package system"): `zpkg -S`, `-Ss`, `-Syu`, `-R`, `-Rns`, a record of which package
owns which file, and **one mechanism with the self-updates** (D15), never a second way to update. Built on the
versioning model the owner approved on 2026-10-25 (ROADMAP, "Versions and updates": a package per part, the group
"base" moving as one, the channels testing and stable, dated snapshots, always a way back, 0.x until the owner calls
1.0), on D15 (the bundle, `zup`, its journal, `prev-V`, the three-way compare, the database `tools/mkzup.py` already
writes), on D21 (detached `.sig` files, `zc_sig_verify`, the label `Zenix package index v1`, test and release keys,
the channels `test`, `testing`, `stable`; owner decision 6: plain HTTP from the NAS with signed packages, HTTPS
optional) and on D19 (sockets, `getaddrinfo` through netd). The short version is DESIGN.md D22.

Facts about today's code are *read* from main at 4f21bc2 (2026-10-27). Sizes marked *measured* were taken from the
build of that day (`build/pkg/`, `build/zup.zx`) or from D21's measured table. Facts about GitHub are *verified* with
`curl` and `openssl s_client` on 2026-10-27. Every time on a 68020 is an *estimate* and shows its arithmetic.

## 1. What the owner gets, in plain words

- `zpkg -Syu` brings the whole machine up to date: programs at once (a running program keeps running, D13), and the
  system itself (the kernel, `init`, `sh`, the servers) through the D15 path at the next restart, with "Zenix
  (previous)" in the boot menu as before. One command, one installer underneath, one way back for the system.
- `zpkg -S telnet` installs a program and what it needs; `zpkg -Ss word` searches; `zpkg -R` removes, `-Rns` also its
  saved settings and the helpers nothing else needs; `zpkg -Qo /bin/awk` says which package owns a file.
- **The repository is plain files on a web server:** the owner's NAS (the testing channel, home only, plain HTTP) and
  GitHub Pages (the stable channel, public, HTTPS). Everything is signed with the owner's key; a package that was
  changed on the way is refused before it touches the disk.
- **A wrong program can be taken back:** the previous version of each package stays in the download cache, and one
  command puts it back.
- **Honest about the stock A1200:** checking that nothing is new takes about two seconds; a new package list about
  12 s (most of it the signature check); one changed program about 10 s; a big update after a library change several
  minutes over the serial cable (estimates, section 7).

## 2. Today (read)

- **D15 is built** (steps 1-5, branch d15, merged): `tools/mkzup.py` makes four packages from the build, `zup`
  (group base), `base` (group base: init, login, sh, mount, the servers on the disk, the network drivers, the
  licences, the console font, the config files), `utils` (every other command, 121 files) and `man`, each a
  manifest (`zpkg 1` text: name, version, group, `depends NAME MINVERSION`, one line per file, link and directory,
  files with mode, owner, SHA-256 and size; `c` marks a config file) and a tar.gz; and a bundle (`ZUP1`: header,
  table of contents with SHA-256 per part, `boot/zenix` and `pkg/NAME.manifest`/`.tgz` parts).
- *Measured* (build of 2026-10-27): `zup.tgz` 26,049 bytes, `base.tgz` 157,687 (307,200 unpacked), `utils.tgz`
  480,716 (1,495,040 unpacked), `man.tgz` 85,184; manifests 157, 2,123, 11,278 and 13,859 bytes; the kernel 154,944;
  the whole bundle 932,864; `/sbin/zup` 48,104 bytes.
- **zup** (user/zup.c) is the only program that changes installed files: one transaction = the journal
  (`/var/lib/zpkg/journal`, synced first), the root marked "needs a check" (`FS_MARK`), every replaced or removed file
  hard-linked into `/var/cache/zpkg/prev-V/f/`, new files as `PATH.zup-new` checked against their SHA-256, owner then
  mode, renamed over; the undo log `prev-V/log.T`, the commit record, older `prev-*` sets deleted. After a cut the
  next zup finishes or rolls back. The installed database is `/var/lib/zpkg/NAME.manifest`, one per package, written
  inside the transaction. Its limits: 16 packages a bundle (`MAXPKG`), 1,024 changes a transaction (`MAXOPS`), paths
  of at most 55 bytes in a package (63 with `.zup-new`). It reads the bundle only from memory (zenixboot's hand-over,
  `SYS_update INFO`) and refuses a bundle older than the installed base.
- **The way back for the system:** `prev-V` stays until the next system update; "Zenix (previous)" makes zenixboot
  put `zenix.old` back and zup replay `prev-V`'s undo logs.
- **zenixboot** reads bundles only from `BOOT:Zenix/zenix-update.zup` (AmigaDOS FFS), which Zenix cannot write; D15
  section 10 left "how a bundle from the network reaches BOOT:" for this note.
- **D21 steps 0-1 are built on the PC side:** `zc_sig_verify` (19,334 bytes of m68k code for Ed25519 verification,
  *measured*), `tools/zsign.py` with the kinds `bundle` and `index`; the TLS client with the X.509 wrapper is 82,554
  bytes (*measured*); nothing runs on Zenix yet. There is no HTTP client and no `wget` in Zenix today.
- **GitHub serves only HTTPS** (*verified* 2026-10-27): `http://raw.githubusercontent.com/...` and
  `http://pages.github.com/` answer `301 Moved Permanently`. GitHub Pages answers `Range` requests (`206`,
  `Accept-Ranges: bytes`) and is cached for 10 minutes (`Cache-Control: max-age=600`); its certificate chain is RSA
  (`*.github.io` RSA-2048, Let's Encrypt YR1 RSA-2048, Root YR RSA-4096, cross-signed by ISRG Root X1), the cheap kind
  for BearSSL on a 68020 (D21 7.3).

## 3. The repository on a static web server

### 3.1 The options for the package files

| | (a) a new archive format | (b) manifest + tgz, two files (mkzup's `pkg/` pair) | (c) **a package bundle**: D15's `ZUP1` with only `pkg/` parts |
|---|---|---|---|
| New parser on the Amiga | yes | no (zup has both) | **no** (zup's `bundle_parse`) |
| Self-describing (version, channel, minimum base in a header) | as designed | no | **yes** (D15's header) |
| A signature for a single file (`zpkg -U`, the cable) | new label | two files to sign | **D21's bundle label, unchanged** |
| Files per package on the server | 1 | 2 | 1 (+ its `.sig`) |

**Choice: (c).** A package file is a D15 bundle without a kernel: header (version = the package's version, channel,
minimum base = its base dependency), table of contents, `pkg/NAME.manifest`, `pkg/NAME.tgz`. No new format reaches the
Amiga; the signature over one file is D21's bundle signature (header digest); zenixboot refuses one if someone copies
it to BOOT: ("it has no kernel"), which is right. The overhead is 256 bytes a package. Compression stays gzip (miniz's
`tinfl`, already in zup; mkzup writes level 9 with mtime 0, so the same files give the same bytes).

### 3.2 The layout

One tree, served as it is by any web server:

```
ROOT/                        (e.g. http://nas:PORT/zenix  or  https://OWNER.github.io/zenix-packages)
  pool/NAME-VNUM.zup         a package bundle, never changed once published (its name is its content)
  pool/NAME-VNUM.zup.sig     its D21 signature (80 bytes)
  pool/zenix-VNUM.zup        a system bundle: the kernel and the base group, exactly D15's bundle
  pool/zenix-VNUM.zup.sig
  testing/latest             one line: "zpkglatest 1 SERIAL" (unsigned: it only says which list to fetch)
  testing/index-SERIAL       the package list (3.3), never changed once published
  testing/index-SERIAL.sig   its D21 signature, label "Zenix package index v1"
  stable/...                 the same three
  test/...                   the test channel: test builds, the test key (D21 choice 7)
  snapshots/2026.11/         a dated snapshot: a copy of one stable index and its .sig (the files stay in pool/)
```

- **Channels are directories; the files live once in `pool/`.** Promoting a version from testing to stable is a new
  stable list naming the same pool files: nothing is copied or rebuilt, and what was tested is exactly what ships.
- **Every published file is immutable, and only `latest` changes.** GitHub Pages caches each file for 10 minutes;
  a list and its signature fetched a minute apart could come from two generations, and a pair that does not match
  would look like an attack. With the serial in the names a list and its `.sig` always match; a stale `latest` only
  delays the news by up to 10 minutes. `latest` needs no signature: it only chooses which signed list to fetch, and
  the serial rule (5.2) refuses an older one.
- **Mirrors** are other roots with the same tree (`rsync` of the whole directory). `/etc/zpkg.conf` lists them in
  order; zpkg uses the first that answers and goes to the next on any refusal (a missing file, a wrong size, a wrong
  SHA-256, a list older than the one it has). A mirror needs no key: every file is checked against the signed list.
- **The pool keeps what any list or snapshot names, plus the previous version of every package**, so `zpkg -U` of an
  older version can always be fetched; the release tool prunes the rest (6.2).

### 3.3 The package list (`index-SERIAL`)

Text in the manifests' grammar (a word, its arguments, one line each; unknown words ignored, so it only grows): a
header, then one block per package.

```
zpkgindex 1
channel testing
serial 2026110301
date 20261103
expires 20260102
system 2026110301 0.3b7 zenix-2026110301.zup 412160 SHA256HEX min 0
pkg awk
version 2026110301 0.3b7
file awk-2026110301.zup 31744 SHA256HEX
size 86016
depends base 2026102601
desc pattern scanning and text processing (awk)
pkg telnet
...
```

- `serial` is `YYYYMMDDNN` like the versions, larger in every new list of a channel; `date` is the signing day;
  `expires` the day after which the list is refused (5.3). `channel` must equal the directory's (so a signed testing
  list cannot be served as stable: both are signed with the release key).
- `system` names the system bundle of this list (the kernel and the base group, one version for the group).
- A `pkg` block: `version` (number and string, as the manifest), `file` (pool name, size, SHA-256 of the whole
  file), `size` (installed bytes, for the space check), `depends` (zero or more, name and minimum version, as the
  manifest), `group` (only `base`), `desc` (one line, at most 72 characters), `takes OTHER` (8.4.4).
- The base-group packages (`zup`, `zpkg`, `base`) are listed for `-Ss`, `-Si` and `-Q`, but are only ever installed
  through the `system` bundle (4.4).
- **Size for 150 packages:** about 200 bytes a block (two SHA-256s in hex are 128 of them, `desc` 40-70) and a
  100-byte header: **about 30 KB**; 3 s over SLIP at 11.5 KB/s, under a second over Ethernet. Not compressed: it is
  verified before it is parsed, and 30 KB do not need it. zpkg refuses a list over 256 KB and a `latest` over 64 bytes.
- **What the signature covers:** SHA-256 of the whole file (D21 6.1's table, unchanged), verified **before** a single
  line is parsed.

## 4. The local database and the operations

### 4.1 Where things live

| Path | What | Written by | How it survives a cut |
|---|---|---|---|
| `/var/lib/zpkg/NAME.manifest` | an installed package: its files, owners, SHA-256s (D15, unchanged) | zup only, inside a transaction | the journal |
| `/var/lib/zpkg/journal`, `result` | zup's (D15, unchanged; `result` only by the boot run) | zup | D15 |
| `/var/lib/zpkg/sync/CHANNEL.index`, `.sig` | the last verified list of a channel | zpkg | written as `.new`, renamed |
| `/var/lib/zpkg/deps` | names of the packages installed only as dependencies, one a line | zpkg, after a commit | `.new`, renamed; missing name = explicit |
| `/var/lib/zpkg/lock` | the pid of the zpkg that changes things | zpkg | a lock of a dead pid is stale |
| `/var/cache/zpkg/prev-V/` | the previous system (D15, unchanged) | zup | D15 |
| `/var/cache/zpkg/live/` | the hard links of the one package transaction under way | zup -f / -r | the journal; emptied at the commit |
| `/var/cache/zpkg/pkg/` | downloaded package files and their `.sig` | zpkg | `NAME-V.zup.part`, renamed after its SHA-256 matched |
| `/etc/zpkg.conf` | channel, servers, held packages, cache limit | the owner | a base config file (`c`, three-way compare) |

**The record of which package owns which file is D15's manifests, nothing new.** One fact in one place: a second
"file list" index would have to be kept in step with the manifests inside the same crash-safe transaction. `-Qo` and
the conflict check read the manifests (4.3). **No migration:** a D15 machine's database is already zpkg's; zpkg only
adds `sync/`, `deps`, `lock` and the cache. `/etc/zpkg.conf` is shipped in `base` (stable channel, the GitHub URL,
nothing private); the owner's own (the NAS, testing) is a changed config file and survives every update (D15's
three-way compare).

**Why `deps` lives outside the transaction:** "installed as a dependency" only decides what `-Rns` may remove.
A cut between the commit and the `deps` rewrite leaves the package counted as explicit, which is the safe side (it is
never removed by itself). Putting it inside zup's transaction would need a new kind of database file in zup for a
fact zup never uses.

### 4.2 One installer: zpkg fetches and decides, zup installs

| | (a) zpkg links zup's code | (b) **zpkg runs `/sbin/zup`** |
|---|---|---|
| Who writes a journal | two programs | **one** |
| Who recovers it after a cut | zup at boot, maybe another version of the code | **the same program that wrote it** |
| One mechanism with D15 | the same source | **the same program, the same tests** |
| Cost | none | a spawn per call (zup is 48 KB, one load: about 0.3-0.5 s on the stock A1200, *estimate*) |

**Choice: (b).** zup at boot recovers whatever journal it finds, so the journal must be written by that same program;
zpkg never writes one. zup gains two modes, inside its existing engine (plan, journal, prev links, rename-over,
three-way compare, undo, recovery):

- **`zup -f FILE`**: install one package bundle from a file (read into memory, at most 256 KB; zup's own buffer, so
  the D15 rule "never pass a bundle pointer to a system call" does not arise). One file = one transaction. Checks:
  the bundle's own SHA-256s (zup trusts no caller), **no base-group package** (base is never installed live, D15
  section 2), the package newer than the installed one (or `-d`, a downgrade zpkg asked for), its dependencies
  installed (zpkg orders the calls), its header's minimum base not above the running kernel and the installed base.
- **`zup -r NAME`**: remove one package as one transaction (every file a `D`, every link a `u`, the manifest a `D`;
  config files as 4.3 says).
- **Both:** first `recover()` as at boot (a journal from a cut is finished or rolled back before anything new); the
  prev links go to `/var/cache/zpkg/live/` (not `prev-*`: zup's commit deletes every `prev-*` set but the current
  one, which must keep the system's way back; old zups never look at `live/`), emptied at the commit; the journal
  gains a line `live 1` (growth, ignored by the old reader's `parse_ops`; written and read only by this zup);
  **never** `SYS_update NOTE`, never `result` (those belong to the boot run and the D15 trial); SIGINT, SIGHUP and
  SIGTERM ignored while the transaction runs (as SIGPIPE is today).
- zup stays a v1 ZEXE using only D15-era calls (D15 section 4); the modes are new code, not new calls.

### 4.3 The operations

| Command | Who | What |
|---|---|---|
| `-Sy` | root | fetch `latest`; if its serial is the stored one, "up to date" (one 20-byte file); else the list and its `.sig`, verify, check (5.2), store |
| `-Syu` | root | `-Sy`, then every installed package with a newer version in the list (4.4), the system bundle if newer |
| `-Syuu` | root | as `-Syu`, also down to the list's version where the installed one is newer (after a channel change) |
| `-S NAME...` | root | the packages and their missing or too old dependencies, dependencies first; already current: said, skipped |
| `-Sw` | root | download and verify only (to the cache) |
| `-Ss WORD` | anyone | names and `desc` lines of the stored list containing WORD (no case), installed ones marked |
| `-Si NAME` | anyone | the list's block for NAME, in words |
| `-Sc` | root | the cache down to the installed version and the one before of each package |
| `-Q`, `-Qi NAME`, `-Ql NAME`, `-Qo PATH`, `-Qu` | anyone | installed packages; one's manifest in words; its files; the owner of a path; those with a newer version in the stored list |
| `-R NAME...` | root | remove; refused if another installed package depends on it (named) |
| `-Rs`, `-Rn`, `-Rns` | root | `s`: also the dependencies installed only as dependencies that nothing left needs (recursively); `n`: also the changed config files |
| `-U FILE...` | root | install a package bundle or a system bundle from a file (the cable, a cache file), its `.sig` verified (5.1) |
| `--back NAME` | root | the newest cached version older than the installed one, by `-U` (asks first) |

- **Dependencies:** a name and a minimum version, nothing else (no "provides", no "conflicts", no version ranges);
  resolved recursively from the list, a loop or a missing one refused before anything is fetched, installed in
  dependency order. **Partial updates are safe in Zenix**, unlike Arch: every program is linked statically and the
  interfaces only grow (CLAUDE.md, frozen interfaces), so `-S foo` without `-u` never breaks another program. The only
  coupling is a package's minimum base.
- **File conflicts:** before any download, every path of the new packages is looked up in the installed manifests:
  owned by another package, refused (named), unless the new package `takes` that one (8.4.4); present but owned by no
  package, refused ("exists and belongs to no package: move it away or use --overwrite"), except a config file (D15's
  rule: kept, the new one as `PATH.new`).
- **Config files on removal** (pacman's rule, D15's compare): one the owner never changed is deleted; a changed one is
  renamed `PATH.save` (`-R`) or deleted (`-Rn`). Empty directories that only this package listed are removed after the
  commit (`rmdir`; nothing to undo, a rollback happens before the commit).
- **Holds:** `hold NAME` in `/etc/zpkg.conf` (pacman's IgnorePkg); `-Syu` names what it skipped. `hold base` holds
  the system.
- **`-Qo` and the conflict check** read all installed manifests into one table (about 40 manifests today, 150 later;
  1-3 s on the stock A1200, *estimate*: a file open and read through fsd is about 10-20 ms there).
- **No install scripts:** a package is files, links and directories. A package that needs a step at install (a user,
  a service line) waits for a design note of its own. (Owner question 6.)
- **A running program keeps running** (D13); zpkg says which updated programs are running (`procinfo`), as D15
  section 10 asks for live installs.
- **One zpkg at a time:** `/var/lib/zpkg/lock`. The read-only commands need no lock.

### 4.4 The system update goes only through D15

The base group (the kernel, `zup`, `zpkg`, `base`) is **never** installed live (D15's order rule: the kernel is never
older than the root's base). So `-Syu`, when the list's `system` version is newer than the installed base:

1. fetches `pool/zenix-V.zup` and its `.sig` into the cache, checks size and SHA-256 against the list (the bundle's
   own header version must equal V);
2. **puts it where zenixboot looks, through the update slot (4.5)**, and says "Zenix V is ready: it is installed at
   the next restart (reboot)";
3. installs every other package now, except those whose minimum base is above the running system: those are
   downloaded and verified into the cache and named ("12 packages wait for Zenix V: after the restart, zpkg -Syu
   again"; nothing is downloaded twice).

At the restart **nothing is new**: zenixboot checks the signature (D21 6.4), installs the kernel, keeps `zenix.old`,
hands the root half to zup, the trial, the confirmation, "Zenix (previous)": D15 as built. `zpkg -U zenix-V.zup` (a
system bundle from the cable) goes the same way. **There is no other way for the kernel, `init` or `sh` to change.**

**After "Zenix (previous)":** the system goes back; packages installed later that need the newer base stay, and
`zpkg -Q` marks them ("needs base V, base V-1 runs"); they work again when the system update is installed again, or
the owner takes each back with `--back`. Said plainly in `man zpkg`: the system's way back is the boot menu, a
package's is the cache.

### 4.5 The update slot: how a bundle from Zenix reaches BOOT:

Zenix cannot write FFS, and zenixboot cannot read ext2 (D15 section 10). The options:

| | How | Against it |
|---|---|---|
| (a) An FFS writer in Zenix | create a file on BOOT: | bitmap, headers, hash chains written by Zenix on the owner's AmigaOS partition: the riskiest write in the system, for one file |
| (b) zenixboot reads ext2 | find the root on the card, read a file | MBR/RDB parsing and Emu68's whole-card unit in a program that runs before Zenix (D15 rejected it) |
| (c) The bundle in memory across a warm reset | Zenix leaves it in RAM, reboots, zenixboot picks it up | lost at every power-off (the normal way an Amiga session ends); AmigaOS's own boot may use that memory on a chip-only machine |
| (d) The owner copies it | from AmigaOS, or Wasabi | not "installs itself" |
| (e) **A slot file** | a file made once by AmigaOS on BOOT:, whose data blocks Zenix overwrites in place | Zenix writes into an AmigaOS partition, but never its metadata |

**Choice: (e),** the pattern GRUB uses for its environment block (a file made by the system that owns the
filesystem, whose data the boot loader overwrites in place without a filesystem driver). FFS (unlike OFS) keeps no
header or checksum in its data blocks, and overwriting a file's data changes no bitmap, header, hash chain or date;
AmigaOS sees an ordinary file with new contents. **It needs the owner's yes** (CLAUDE.md: no writes to a disk with
AmigaOS partitions without a plan the owner agreed to): owner question 1. If the answer is no, (d) is the fallback:
zpkg leaves the bundle in `/var/cache/zpkg/` and says how to copy it.

- **Made once, by AmigaOS:** `zenixboot MKSLOT` (run by the owner's card package, the same one that brings D21's
  signing zenixboot: zenixboot is only ever installed by hand) creates `BOOT:Zenix/zenix-update.slot`, 1,280 KB
  (D15's ceiling for a bundle on a 2 MB machine; a base-only system bundle today is about 400 KB: the kernel 154,944,
  zup 26,049, base 157,687, zpkg about 60 KB, *measured* and *estimate*), with an empty control block, and flushes.
- **The slot's format** (frozen once shipped; zenixboot changes only by hand): block 0 is the control block (512
  bytes: `ZSL1`, a format word 1, state 0 empty / 1 ready, a sequence number, the bundle's size, its header digest,
  the 80-byte `.sig`, a SHA-256 over the block's bytes before it); the bundle from block 1.
- **Zenix's side** (`zpkg`, through diskd's raw calls on a partition that is not mounted, `DISK_RAW`): finds the FFS
  partitions in the partition tables diskd already reads; walks each read-only (the root block, the `Zenix` directory,
  the file `zenix-update.slot`: hash chains, the international name hash for `DOS\3`, header and extension blocks,
  every checksum and block type checked, every pointer inside the partition, no block twice, none equal to a
  metadata block it read); accepts `DOS\1`, `DOS\3` and `DOS\5` (FFS, international, directory cache: none keeps
  anything about a file's data outside its data blocks), refuses OFS, `DOS\6`/`DOS\7` (long names: refused until a
  test shows their file headers) and anything else; needs the root block's "bitmap valid" flag (a volume AmigaOS
  left validated) and exactly one slot on the machine. Then: control block written empty; the bundle into the data
  blocks; `DISK_FLUSH`; the control block written ready; `DISK_FLUSH`. A cut at any point leaves the slot empty or
  not ready: nothing happens at boot, and the next `zpkg -Syu` writes it again.
- **zenixboot's side** (under AmigaOS, plain `Read`): a ready slot with a good control block: writes the `.sig` as
  `zenix-update.zup.sig.part` and the bundle as `zenix-update.zup.part` (D15 and D21's copy rule), checks the copy,
  renames (`.sig` first), flushes, writes the slot's control block empty, flushes; then D15 as built. A cut anywhere:
  the slot is still ready and the copy is repeated (a bundle already there with the same digest: only the slot is
  emptied). The copy costs one bundle of space on BOOT: and a few seconds; in exchange every later step is D15's
  unchanged, file-name-based code.
- **Space on BOOT:** the slot (1,280 KB) plus D15's peak (`zenix`, `zenix.old`, `zenix.new`, the bundle, the state
  file): about 2.3 MB for a 400 KB bundle. The owner's BOOT: size is read in step 6, before anything is written.

## 5. Trust

### 5.1 What is verified, when, by whom

- **The list:** `zc_sig_verify` with D21's label and keys over the bytes fetched, **before** they are parsed or
  stored; then `channel`, `serial`, `expires` (5.2, 5.3). The verified list is stored root-owned and used without a
  second verification by `-Ss`, `-Si`, `-Qu` (as the installed files themselves are trusted).
- **A package or system bundle from the list:** its size (the download stops at the list's size; a server sending
  more is cut off) and its SHA-256 against the list, computed while it streams to `NAME-V.zup.part`; renamed only
  when it matches; then name, version and channel inside must equal the list's (a builder's mistake caught). No
  signature check: the signed list's SHA-256 covers it, and saves 6-9 s per package on the stock A1200.
- **A file by `-U`** (the cable, a USB stick later, the cache): its `.sig` (D21's bundle label) verified, once, before
  anything else; no `.sig`, refused. `--back` uses the `.sig` kept beside each cached file.
- **A system bundle** is verified again by zenixboot at the restart (D21 6.4): two checks, two transports, as D21
  says ("once per transport").
- **Nothing unverified is stored under a final name, nothing unverified is parsed beyond the size check, and nothing
  is installed that zup did not check again** (zup checks every part's SHA-256 and every file against its manifest).
- **Keys:** release builds trust the release keys (current and next) for `testing` and `stable`; test builds trust
  only the test key and only for `test` (D21 6.3 and choice 7). A list whose `channel` does not fit the key is refused.
  `zpkg`'s keys travel in its package (D21 6.3): a system bundle signed by "next" brings a zpkg that has "next" as
  current.

### 5.2 Replay and downgrade

- **The serial:** a list is accepted only if its serial is larger than the stored list's for that channel (equal: only
  the same bytes, which costs nothing). An attacker on the NAS's network who replays an older signed list is refused.
  The first list of a machine is checked against the serial built into zpkg (the release's own), so even a fresh
  machine never takes a list older than its software.
- **Mix and match** is impossible: the list binds every file to its SHA-256.
- **Versions never go back by themselves:** `-Syu` only moves up; down only by `-Syuu` or `-U -d`, both asked for.
  System bundles never go back at all (zenixboot renames an older one `.bad`; the way back is "previous").
- **Changing the channel** (`channel stable` after testing): the stable list's serial is compared with the stored
  stable list's, not testing's; packages newer than stable stay until stable catches up, or `-Syuu`.

### 5.3 A frozen repository, and a wrong clock

- **Expiry** (D21 6.5's recommendation): a list past its `expires` day is refused, which stops a "freeze" (someone
  serving the last list for ever so that a fixed hole is never fixed). The release tool re-signs each channel's list
  with a new serial and the same files every week, and `expires` is 60 days after signing (owner question 3).
- **The time:** "today" is the newer of the clock and D21's floor (`/var/lib/clock`, the build date). A clock that
  is behind (a flat battery, 1978) never makes a list look valid for longer than the floor allows; the floor is never
  far behind the real date.
- **A clock that is ahead** (a wrong battery clock, 2099) makes every list look expired: zpkg says "the package list
  expired on 2026-12-31, and the clock says 2099-01-01: check the date", and `--ignore-expiry` (root) goes on once.
  The serial still protects against replay then.
- **A clock that is behind the signed `date`** by more than a day: zpkg says so ("the clock is behind"), and proposes
  (for the main session: it adds a writer to D21 8.3's floor) to raise the floor to the list's signed `date`, an
  authenticated time from the owner's key.

### 5.4 Said plainly

Plain HTTP from the NAS hides nothing (anyone on the home network sees what is installed) and lets nothing be
changed. Without an MMU any root program can replace any file anyway; the signatures guard the transport, not the
machine (D21 9.1). A stolen release key lets the thief sign packages until zpkg and zenixboot carry a new key (zpkg by
a bundle signed with "next", zenixboot by hand).

## 6. Building the repository on the PC

### 6.1 The tools

- **`tools/mkzup.py`** grows (its formats only grow): `bundle --no-kernel` for a package bundle (the header's version
  the package's, `--min` its base dependency); `desc` and `takes` lines in manifests; the per-family package lists
  (8.4.4) instead of one `utils`.
- **`tools/mkrepo.py`** (new): `mkrepo.py add REPO CHANNEL BUILD` takes a build's packages and system bundle,
  compares each package's manifest **file list and SHA-256s** with the channel's current one, gives a new version only
  to a changed package (so `-Syu` fetches only what changed), writes the pool files and a new list (serial = the
  larger of the old one plus one and today's `YYYYMMDD00`), signs every new file and the list through `zsign.py`,
  writes `latest` last. `promote REPO testing stable` (a stable list naming testing's files), `resign REPO CHANNEL`
  (the weekly re-sign: same files, new serial and expiry), `snapshot REPO stable 2026.11`, `prune REPO` (3.2), `check
  REPO` (every list, signature, size and SHA-256, as zpkg would).
- **`tools/zsign.py`** (built, D21): unchanged; the kinds `bundle` and `index` exist.
- **Reproducible packages:** mkzup already writes tar and gzip with mtime 0. A step-1 test builds twice and compares
  the bytes; if the compiler's output differs between two builds of the same source, the version rule uses the
  files' SHA-256s anyway, and the test says which file differs.

### 6.2 The release process the owner can run (or ask a session to run)

1. `make check` passes (the testing channel's rule: "every version that passes `make check`").
2. `make release` (RELEASE=1 build, `mkrepo.py add` to the testing channel, the signatures with the release key) and
   `tools/publish.sh nas` (an `rsync` of the tree to the NAS's web folder; the NAS address lives in the owner's
   `~/.config/zenix-repo.conf`, never in the repository).
3. The owner's A1200 runs `zpkg -Syu` from the NAS.
4. "Daily driven" (the owner's word): `mkrepo.py promote testing stable` and `tools/publish.sh github` (the tree to
   the Pages branch of a public `zenix-packages` repository, one orphan commit each time, so binaries never pile up in
   git history).
5. A user timer on the PC runs `mkrepo.py resign` for both channels weekly and publishes (owner question 3).

**The signing key** stays where D15 and D21 put it: `~/.config/zenix-signing/zenix-release.key`, mode 600, outside
every repository, with an offline backup; the build never sees it, only `zsign.py` reads it. The test key is generated
per build tree (D21 as built). **GitHub Releases are not used:** their downloads redirect to another host, which costs
a second TLS handshake per file (20-30 s each on the stock A1200, D21 7.3); Pages is one host with keep-alive.

## 7. Size, memory and time on the stock A1200

### 7.1 Code (*estimates* unless marked)

| Part | Size |
|---|---|
| zpkg's own logic (config, list parser, resolver, database reader, commands, cache) | 15-20 KB |
| the HTTP/1.1 client (`user/http.c`, 7.4; shared with `wget`) | 5-8 KB |
| Ed25519 verification (`zc_sig_verify`) | 19,334 bytes (*measured*, D21) |
| the slot writer (read-only FFS walk) | 3-5 KB |
| libz, SHA-256 | as every program |
| **zpkg without TLS** | **about 55-65 KB** |
| the TLS client and X.509 wrapper, from D21 step 7 | +82,554 bytes (*measured*, D21) |
| **zpkg with HTTPS** | **about 140-150 KB** (on the disk; loaded only while zpkg runs) |
| zup's two new modes | 2-3 KB on today's 48,104 bytes |

zpkg goes in the base group's package `zpkg`, not on the RAM floppy.

### 7.2 Memory during a `-Syu` (stock A1200: 1,132 KB free with one login, *measured* 2026-10-26)

zpkg's code 60-150 KB; the list 30 KB and its table (150 entries, about 6 KB); the installed manifests' table (40-150
small manifests, 20-60 KB); a 4 KB download buffer (downloads stream to the disk, so a package's size costs no memory
in zpkg); with TLS BearSSL's 16.7 KB one-way buffer and 7 KB of contexts. While zpkg waits for zup: zup's 48 KB, its
change table (1,024 entries, about 85 KB), miniz's 32 KB window and the package (at most 256 KB). **Peak: about
350-650 KB**, so a `-Syu` runs beside a login session on the 2 MB machine. The limit of 256 KB a package is the
release tool's rule (the per-family split keeps every package far below it; today's `utils` at 480 KB is the reason
for the split).

### 7.3 Time on the stock A1200 (all *estimates*)

The basis: SHA-256 about 1 ms per 64-byte block, 60 KB/s (D15 section 9); Ed25519 verification 6-9 s (D21 7.3);
SLIP 11.5 KB/s (D19); inflating with miniz about 150 KB/s (an estimate for a 14 MHz 68020); a zup transaction's fixed
cost (journal, syncs, marks, prev directory, log, commit) 1-2 s; a spawn of zup 0.3-0.5 s.

| Case | Stock A1200 over SLIP | 68060 or PiStorm, Ethernet |
|---|---|---|
| `-Sy`, nothing new (`latest` only) | about 1-2 s | under a second |
| `-Sy`, a new list (30 KB, one signature) | about 10-13 s | about 1 s |
| `-Syu` with one changed 30 KB program (75 KB unpacked) | about 10-15 s more | 1-2 s |
| `-Syu` after a libz change: all 40 packages, 1 MB packed, 2.5 MB unpacked | **about 5-7 minutes** (download 90 s, SHA-256 of the download and zup's part check 35 s, inflating 17 s, zup's checks of 2.5 MB written and read back 85 s, 40 transactions 60-100 s) | about 20-40 s |
| a system bundle of 400 KB to the slot | about 45 s (download 35 s, SHA-256 7 s, writing) | a few seconds |
| HTTPS (GitHub) instead of the NAS | + one handshake per run, about 20-30 s (RSA chain, *verified* key types, D21's estimate) | + 1-2 s |

The slow parts are where D15 put its safety (every file read back against its SHA-256) and the serial line; neither
is cut. The common case (nothing new, or one program) stays short. To be measured on the owner's machines (step 7).

### 7.4 The HTTP client (`user/http.c`)

One small module, linked by `zpkg` and later `wget` (static, as libz; not D13 shared code while stage 2 is not
decided): HTTP/1.1 `GET` only, `Host`, `Connection: keep-alive` (many files over one connection, which is what makes
HTTPS bearable), `Range: bytes=N-` for resuming, `Content-Length` or chunked bodies, at most 3 redirects (never
from https to http), a 4 KB limit on the response header, timeouts (connect 15 s, between bytes 60 s, 120 s for a TLS
handshake on a slow CPU); names through `getaddrinfo` (netd's resolver, D19); `poll` for the timeouts (D16). No
proxies, no cookies, no authentication. TLS through D21's wrapper when the URL is `https:`. It is fuzzed (8.1).

### 7.5 Downloads: resume and the cache

- **Resume:** a cut download stays as `NAME-V.zup.part`; the next run asks for the rest (`Range`); a `206` with the
  right `Content-Range` continues (the SHA-256 is computed again over the whole file at the end: a wrong prefix is
  found, the file deleted and fetched once from the start); a `200` restarts it. Immutable pool names make this safe:
  the same name is always the same bytes.
- **The cache** is on the disk, not in memory: the installed version and the one before of each package with their
  `.sig` (the way back), system bundles only while they wait for the slot. About 2 x 1 MB for today's set; `cache
  4096` (KB) in `/etc/zpkg.conf` caps it on small cards (the oldest versions that are not installed go first); `-Sc`
  empties it down to the installed and the previous versions.

### 7.6 Transports

- **The NAS:** plain HTTP (owner decision 6), the testing channel, home only.
- **GitHub Pages:** HTTPS only (*verified*): the stable channel, public, after D21 step 7.
- **The serial cable:** the same `zpkg` over SLIP (D19: the cable is a network link to the PC through `zenix-slirp`,
  and `python3 -m http.server` in a copy of the tree, or the NAS through the PC). Without SLIP: `rz` (D20) receives
  the files, `zpkg -U` installs them. No separate cable transport.

## 8. Tests

### 8.1 On the PC

- **The repository tools:** `mkrepo.py` against a scratch tree: versions only for changed packages, serials growing,
  `latest` written last, promote and snapshot naming the same pool files, prune keeping what a list names and the
  previous version, `check` finding every kind of damage; two builds compared byte for byte (6.1).
- **zpkg's core built for the PC** (as zup's host build, `tools/host/`): the list parser (damaged, oversized, wrong
  channel, older and equal serials with other bytes, expired, the clock ahead, behind and missing, a test-key list on
  a release build and the reverse), the resolver (loops, missing, minimum versions, dependency order, `-Rs` keeping
  what something still needs), conflicts (owned, unowned, `takes`), holds, the `deps` file after a cut.
- **The HTTP client against a test server** (`tools/host/http-test-server.py`, in `unshare -rnm` as the network tests,
  nothing leaves the PC): keep-alive, chunked, `Range` honoured and ignored, a wrong `Content-Range`, a body longer
  and shorter than `Content-Length`, an endless body, a header over 4 KB, redirect loops, a server that stalls, a cut
  connection then a resume, a mirror that is behind or serves a damaged file (the next mirror used).
- **The slot writer** against FFS images from `tools/mkffs.py` (DOS\1 and DOS\3, files before and after the slot in
  the same hash chain, extension blocks, a slot too small, two slots, damaged checksums and pointers): after a write,
  every block of the image except the slot's data blocks is byte for byte what it was; images with OFS, `DOS\7` and an
  invalid bitmap refused untouched.
- **zup's new modes** in `tools/zup-host-test.sh`: a power cut before each change of a `-f` install, a `-r` removal
  and a `takes` hand-over (D15's cut machinery), each finished or rolled back; `prev-V` untouched by every live
  transaction; the D15 rollback after live transactions; on the real ext2 server with a cut after each block write
  (`tools/e2safe.py`, as D15/F5).
- **Fuzzing** (libFuzzer, one run at a time with a memory cap, this PC's rule): the list parser, `latest`, the HTTP
  response parser, the FFS walker, zup's `-f` path (bundle and manifest parsers fed files).

### 8.2 In FS-UAE

- A test repository (test channel, test key) served from a namespace on the PC, reached through the A2065 (eth-test's
  setup) and through SLIP (`zenix-slirp`): `-Sy`, `-S` with dependencies, `-Ss`, `-Si`, `-Q*`, `-R`, `-Rns`, `-Syu`
  replacing a program another console is running, `--back`, a tampered package and a replayed old list refused, an
  expired list, a download cut and resumed, the emulator killed during a live install (the next boot's zup finishes
  or rolls back; `e2fsck` after).
- **The whole system path:** `-Syu` with a newer system bundle writes the slot on the zenixboot-test disk; at the
  restart zenixboot copies it (`.sig` first), and update-test's D15 checks follow (kernel, trial, confirmation,
  "previous"); a cut during the slot write (nothing happens at boot); the FFS volume checked afterwards by AmigaOS
  itself (the test's own Kickstart run lists the directory and reads every file).
- Memory: `free` during a `-Syu` on the 2 MB and 1 MB chip-only machines.
- No speed figure from the emulator.

### 8.3 On the owner's machines

The times of 7.3 (stock A1200 over SLIP, the 060, the PiStorm over GENET), the slot on the owner's real BOOT: (its
dostype and size read first, step 6), the first system update through zpkg.

### 8.4 Notes for the build (not tests)

1. zup's `-f` reads the file into its own memory; nothing of the D15 boot path changes.
2. The installed base for the dependency rules is the smaller of the base manifest's version and the running
   kernel's (`SYS_update INFO`), which is right after "previous".
3. A package's minimum base is the base version it was built with (the conservative rule; finer rules would need to
   know which system calls a program uses). So after each system update most packages wait for the restart once
   (4.4 step 3).
4. **The split of `utils`** into families (the shell's helpers, `coreutils`-style small commands, the text tools,
   the editor, the disk tools, the network tools, the user tools, ...; each program's manual page in its package,
   `man` dissolves) uses a manifest line `takes OTHER`: zup, planning a package that takes OTHER, treats a path that
   OTHER's installed manifest owns as its own (an `R` with a prev link, never a gap without `/bin/ls`) and rewrites
   OTHER's manifest without those entries in the same transaction (an `R` of `/var/lib/zpkg/OTHER.manifest` with text
   zup makes); OTHER's manifest is removed (a `D`) when no entry is left. Never from or to a base-group package.
   Old zups ignore the line (unknown words); the release that splits `utils` needs the zup that knows `takes`, so a
   system update comes first and the split packages depend on it.

## 9. Build order (small steps; the full suite before each merge)

Side work is host-only and uses this note's names and formats (CLAUDE.md, "Coherence first"); everything touching the
`Makefile`, `UAPPS`, libz, `user/usys.h`, images, `tools/check.sh` or the counts in `docs/` is the main session's.

0. *both:* this note reviewed twice (zup and the system path are the update core), the owner's answers.
1. *side:* `mkzup.py bundle --no-kernel`, `desc`/`takes` lines; `tools/mkrepo.py` with its host tests (6.1, 8.1);
   the list format and `latest`; a test repository the later tests serve.
2. *side:* zpkg's core for the PC (list, resolver, database reader, trust rules, the FFS slot walker against
   `mkffs.py` images), `user/http.c` against the test server in `unshare -rnm`, the fuzz harnesses.
3. *main:* zup's `-f` and `-r`, the `live/` prev links, the journal's `live` line, the signals; `zup-host-test.sh`
   extended (8.1). Two reviews (the update engine).
4. *main:* `zpkg` in the base group (`UAPPS`, the `zpkg` package, `/etc/zpkg.conf`, `man zpkg`, `commands.md`,
   `features.md`), plain HTTP, the FS-UAE tests over the A2065 and SLIP (8.2) without the system path. **First thing
   the owner can use** (with the NAS testing channel and a test-key build, or a release build after D21 step 5).
5. *main:* `takes` and the split of `utils` into families (8.4.4); `mkroot.sh`'s `installdb` with the new packages.
6. *main, with the owner:* the slot: `zenixboot MKSLOT` and the copy at start, zpkg's slot writer; FS-UAE end to end;
   together with D21 step 5's card package (the signing zenixboot). Before anything is written to the owner's card:
   its BOOT: dostype and free space read from AmigaOS.
7. *owner:* the NAS's testing channel; `zpkg -Syu` on the stock A1200, the 060 and the PiStorm; the times.
8. *main, after D21 step 7:* HTTPS; the stable channel on GitHub Pages; the weekly re-sign timer; snapshots.

## 10. Frozen once built (CLAUDE.md's list gets them in their steps)

The list's format (`zpkgindex 1`, its words and their meanings, growth only), `latest`'s line, the pool's names and
the channel directories (3.2-3.3); the slot file's place, size rule and control block (4.5: zenixboot changes only by
hand, so every later zpkg must write what the first slot-reading zenixboot reads); `/etc/zpkg.conf`'s format (a
base config file a base program reads); the manifest lines `desc` and `takes` and the journal line `live` (growth of
D15's frozen formats, which old readers ignore); `/var/lib/zpkg/deps` and `sync/` (read by later zpkgs). zup's
`-f`/`-r` command line is between two programs of the same base version and is not frozen. **Nothing here changes an
existing frozen meaning:** no system call, message, struct or kernel change; D15's bundle, manifest, journal, undo
logs, prev sets and update note keep their meaning; D21's `.sig` and labels are used as they are.

## 11. Open questions for the owner (one line each, the recommendation in brackets)

1. May Zenix write system updates into one file AmigaOS made on the boot partition (`Zenix/zenix-update.slot`), never anything else there? [Yes; otherwise each system update is copied by hand from AmigaOS]
2. Packages by family (about 40 now: the shell, the small commands, the text tools, the network tools, ...) rather than one per command? [Yes]
3. The package list stops working if it is not re-signed for 60 days, and your PC re-signs it every week by itself? [Yes]
4. Stable on GitHub Pages (HTTPS: about 30 s more per update on the unexpanded A1200), testing on your NAS (plain HTTP)? [Yes]
5. After a system update, the packages that wait for it are installed by typing `zpkg -Syu` again after the restart? [Yes for now; automatic later if it annoys you]
6. Packages are only files (no install scripts) for now? [Yes]
7. Keep the previous version of every package in the cache (about 1 MB more on the disk) for `zpkg --back`? [Yes]

## 12. For the main session (not owner questions)

- **The feature halt:** the roadmap's approved order names `zpkg` with its repository on the NAS (block 2); the
  owner's approval of this note is the approval of `zpkg`, zup's two modes, `mkrepo.py`, `zenixboot MKSLOT` and the
  slot.
- **D21 8.3** would gain a third writer of the clock floor (5.3, the list's signed date), if the main session agrees.
- **D15's "Over the serial cable" (section 10)** becomes 7.6 here (SLIP or `rz` plus `-U`): its text points here when
  this note is approved.
- **Review depth:** two reviews (update core); a second look at revision 2 before step 3 is the main session's call.

Last checked against the code: 2026-10-27 (main at 4f21bc2).
