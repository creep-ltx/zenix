# D22: `zpkg`, the package manager (design note, revision 2)

Status: **design, revision 2 after two independent reviews (section 13), nothing built.** The owner's wish of
2026-10-24 (future-ideas.md, "A pacman-like package system"): `zpkg -S`, `-Ss`, `-Syu`, `-R`, `-Rns`, a record of
which package owns which file, and **one mechanism with the self-updates** (D15), never a second way to update. Built
on the versioning model the owner approved on 2026-10-25 (ROADMAP, "Versions and updates": a package per part, the
group "base" moving as one, the channels testing and stable, dated snapshots, always a way back, 0.x until the owner
calls 1.0), on D15 (the bundle, `zup`, its journal, `prev-V`, the three-way compare, the database `tools/mkzup.py`
already writes), on D21 (detached `.sig` files, `zc_sig_verify`, the label `Zenix package index v1`, test and release
keys, the channel `test` and the release channels `testing` and `stable`; owner decision 6: plain HTTP from the NAS
with signed packages, HTTPS optional) and on D19 (sockets, `getaddrinfo` through netd). The short version is
DESIGN.md D22.

Facts about today's code are *read* from main at 4f21bc2 (2026-10-27). Sizes marked *measured* were taken from the
build of that day (`build/pkg/`, `build/zup.zx`, `build/zup.uelf`) or from D21's measured table, and re-measured by
the architecture review. Facts about GitHub are *verified* with `curl` and `openssl s_client` on 2026-10-27. Every time
on a 68020 is an *estimate* and shows its arithmetic.

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
  12 s (most of it the signature check); one changed program about 10-15 s; a big update after a library change
  several minutes over the serial cable (estimates, section 7).

## 2. Today (read)

- **D15 is built** (steps 1-5, merged): `tools/mkzup.py` makes four packages from the build, `zup` (group base),
  `base` (group base: init, login, sh, mount, the servers on the disk, the network drivers, the licences, the console
  font, the config files), `utils` (every other command, 121 files) and `man`, each a manifest (`zpkg 1` text: name,
  version, group, `depends NAME MINVERSION`, one line per file, link and directory, files with mode, owner, SHA-256 and
  size; `c` marks a config file) and a tar.gz; and a bundle (`ZUP1`: header, table of contents with SHA-256 per part,
  `boot/zenix` and `pkg/NAME.manifest`/`.tgz` parts) that today carries **all four** packages.
- *Measured* (build of 2026-10-27): `zup.tgz` 26,049 bytes (51,200 unpacked), `base.tgz` 157,687 (307,200),
  `utils.tgz` 480,716 (1,495,040), `man.tgz` 85,184; manifests 157, 2,123, 11,278 and 13,859 bytes; the kernel
  154,944 (it carries its version in a 24-byte `ZVER` record, D15 14.1); the whole bundle 932,864; `/sbin/zup` 48,104
  bytes (text 42,806, bss 97,142, of it the change table 81,920: 1,024 entries of 80 bytes).
- **zup** (user/zup.c) is the only program that changes installed files: one transaction = the journal
  (`/var/lib/zpkg/journal`, synced first), the root marked "needs a check" (`FS_MARK`), every replaced or removed file
  hard-linked into `/var/cache/zpkg/prev-V/f/`, new files as `PATH.zup-new` checked against their SHA-256, owner then
  mode, renamed over; the undo log `prev-V/log.T`, the commit record, then `post_commit` deletes **every `prev-*` set
  but `prev-V`** (zup.c:736-746). `recover()` and `write_log()` derive the directory from the journal's version
  (`set_prevdir`). The installed database is `/var/lib/zpkg/NAME.manifest`, one per package, written inside the
  transaction; `installed()` caches at most 32 manifests and answers "not installed" past that (zup.c:281-291). Limits:
  16 packages a bundle (`MAXPKG`), 1,024 changes a transaction (`MAXOPS`), paths of at most 55 bytes in a package (63
  with `.zup-new`). It reads the bundle only from memory (zenixboot's hand-over) and refuses a bundle whose version is
  below the installed base (zup.c:951). There is no lock: two zups would share one journal.
- **The way back for the system:** `prev-V` stays until the next system update; "Zenix (previous)" makes zenixboot
  put `zenix.old` back and zup replay `prev-V`'s undo logs, which today also cover `utils` and `man`.
- **zenixboot** reads bundles only from `BOOT:Zenix/zenix-update.zup` (AmigaDOS FFS), which Zenix cannot write; D15
  section 10 left "how a bundle from the network reaches BOOT:" for this note. It refuses a bundle without
  `boot/zenix` (zenixboot.c:442), renames a leftover `.part` to `.bad` (zenixboot.c:609-613), and accepts an older
  bundle again after REJECTED (zenixboot.c:655).
- **diskd** allows a raw open of a partition that overlaps nothing mounted (diskd.c:21-24, 302-312), so the boot
  partition of a dual-boot card is reachable while Zenix's root on the same card is mounted. Its `diskinfo` and the
  RDB reader (user/rdb.h) do not carry the FFS geometry (block size, reserved blocks).
- **D21 steps 0-1 are built on the PC side:** `zc_sig_verify` (19,334 bytes of m68k code, *measured*),
  `tools/zsign.py` with the kinds `bundle` and `index` (it checks the channel's key class only for bundles,
  zsign.py:122-125); the TLS client with the X.509 wrapper is 82,554 bytes (*measured*); nothing runs on Zenix yet.
  There is no HTTP client and no `wget` in Zenix today.
- **GitHub serves only HTTPS** (*verified* 2026-10-27): `http://raw.githubusercontent.com/...` and
  `http://pages.github.com/` answer `301 Moved Permanently`. GitHub Pages answers `Range` requests (`206`,
  `Accept-Ranges: bytes`) and is cached for 10 minutes (`Cache-Control: max-age=600`); its chain is RSA (`*.github.io`
  RSA-2048, Let's Encrypt YR1 RSA-2048, Root YR RSA-4096, cross-signed by ISRG Root X1), the cheap kind for BearSSL on
  a 68020 (D21 7.3).

## 3. The repository on a static web server

### 3.1 The options for the package files

| | (a) a new archive format | (b) manifest + tgz, two files (mkzup's `pkg/` pair) | (c) **a package bundle**: D15's `ZUP1` with only `pkg/` parts |
|---|---|---|---|
| New parser on the Amiga | yes | no (zup has both) | **no** (zup's `bundle_parse`) |
| Self-describing (version, channel, minimum base in a header) | as designed | no | **yes** (D15's header) |
| A signature for a single file (`zpkg -U`, the cable) | new label | two files to sign | **D21's bundle label, unchanged** |
| Files per package on the server | 1 | 2 | 1 (+ its `.sig`) |

**Choice: (c).** A package file is a D15 bundle without a kernel holding exactly one package: header (version = the
package's version, channel, minimum base = the package's base dependency), table of contents, `pkg/NAME.manifest`,
`pkg/NAME.tgz`. No new format reaches the Amiga; its signature is D21's bundle signature; zenixboot refuses one copied
to BOOT: ("it has no kernel"). 256 bytes of overhead a package. gzip stays (miniz's `tinfl`, already in zup; mkzup
writes level 9 with mtime 0 and empty names, deterministic, tested at zup-host-test.sh:48).

**A system bundle carries the base group only** (the kernel, `zup`, `zpkg`, `base`; about 400 KB). mkzup and mkrepo
refuse a non-base package in a system bundle. Today's bundles also carry `utils` and `man`; why that matters for the
way back is 4.4.

### 3.2 The layout

```
ROOT/                        (e.g. http://nas:PORT/zenix  or  https://OWNER.github.io/zenix-packages)
  pool/NAME-VNUM.zup         a package bundle, never changed once published (its name is its content)
  pool/NAME-VNUM.zup.sig     its D21 signature (80 bytes)
  pool/zenix-VNUM.zup        a system bundle (base group only)
  pool/zenix-VNUM.zup.sig
  testing/latest             one line: "zpkglatest 1 SERIAL" (24 bytes; unsigned: it only says which list to fetch)
  testing/index-SERIAL       the package list (3.3), never changed once published
  testing/index-SERIAL.sig   its D21 signature, label "Zenix package index v1"
  stable/...                 the same three
  test/...                   the test channel: test builds, the test key (D21 choice 7)
  snapshots/2026.11/         a dated snapshot: one stable index with "snapshot 2026.11" added, signed (amendment 1)
```

- **Channels are directories; the files live once in `pool/`.** Promoting testing to stable is a new stable list
  naming the same pool files: what was tested is exactly what ships. So a stable list names files whose signed header
  says `testing`; the check is the channel's **class** (5.1), not its name.
- **Every published file is immutable, and only `latest` changes.** Pages caches each file for 10 minutes; a list and
  its `.sig` fetched a minute apart could otherwise come from two generations. With the serial in the names a list and
  its `.sig` always match; a stale `latest` only delays the news, and the rules of 5.2-5.3 stop it from hiding an
  expiry.
- **Mirrors** are other roots with the same tree (`rsync`). `/etc/zpkg.conf` lists them in order; zpkg uses the first
  that answers and goes to the next on any refusal (a missing file, a wrong size or SHA-256, a `latest` or list older
  than the stored one: "this mirror is behind"). A mirror needs no key.
- **The pool keeps what any list or snapshot names, plus the previous version of every package**; mkrepo prunes the
  rest (6.2).
- **Snapshots are for installing a known state** (the installer of block 4, release notes), not for `-Syu`: their
  serial is older and their expiry passes. `zpkg --snapshot 2026.11 -S ...` reads one knowingly: the signature, the
  class and the signed `snapshot` line naming 2026.11 are checked, its age and expiry are not (said on the screen),
  and it is stored apart from the channel's list (`sync/snapshot-NAME.index`), never replacing it (amendment 1).

### 3.3 The package list (`index-SERIAL`)

Text in the manifests' grammar (a word, its arguments, one line each; unknown words ignored, so it only grows): a
header, then one block per package.

```
zpkgindex 1
channel testing
serial 2026110301
date 20261103
expires 20270102
system 2026110301 0.3b7 zenix-2026110301.zup 412160 SHA256HEX min 0
pkg awk
version 2026103001 0.3b6
file awk-2026103001.zup 31744 SHA256HEX
size 86016
depends base 2026102601
desc pattern scanning and text processing (awk)
pkg telnet
...
```

- `serial` is `YYYYMMDDNN`, larger in every new list of a channel, never more than the `date`'s own day (`date` x 100
  + 99) and never below any version the list names; `date` is the signing day; `expires` the day after which the list
  is refused (5.3). `channel` must equal the directory's.
- `system` names the system bundle (one version for the base group). A `pkg` block: `version` (number and string, as
  the manifest), `file` (pool name, size, SHA-256 of the whole file), `size` (installed bytes), `depends` (as the
  manifest; zpkg compares them with the manifest after download, 4.3), `group` (only `base`), `desc` (one line, at most
  72 characters), `takes OTHER` (8.4.4). Base-group packages are listed for `-Ss`, `-Si`, `-Q`, and only ever
  installed through the `system` bundle.
- **Size for 150 packages:** about 180 bytes a block (one SHA-256 in hex is 64 of them, `desc` 40-70) and a 100-byte
  header: **about 27 KB**; 3-4 s over SLIP, under a second over Ethernet. Not compressed. zpkg refuses a list over
  256 KB, a `latest` over 64 bytes and a `.sig` that is not exactly 80 bytes; the resolver refuses dependencies more
  than 64 deep (audit 2026-10-28 S21: it recurses a link at a time on a stack without a guard).
- **What the signature covers:** SHA-256 of the whole file (D21 6.1, unchanged), verified **before** a line is parsed.

### 3.4 Versions: who gives them (new in revision 2)

The build gives every package the build's number, and the kernel carries it in `ZVER`; a repository that versions
"only what changed" must therefore assign versions itself. **mkrepo decides, per channel:**

- **A package is changed** when its manifest differs in anything but its own `version` line and its `depends base`
  line: the file list, kinds, modes, owners, SHA-256s, link targets, `desc`, `takes`, other dependencies.
- **The base group is changed** when the kernel differs with its `ZVER` record masked, or any base-group package is
  changed. Then the system gets the build's version (the kernel's own `ZVER` number), and all base-group packages
  carry it (one version for the group, ROADMAP).
- **A changed package gets the build's version; its `depends base` line and its bundle header's minimum are rewritten
  to the channel's current base version** (the system it was built with, which is either the new one in this same
  list or the unchanged older one). mkrepo writes the package bundle after this rewrite, then signs it. An unchanged
  package keeps its old pool file.
- **Images** (`mkroot.sh`'s `installdb`) take their database versions from the repository state the release made
  (mkrepo writes the manifests for installdb), so a fresh image never looks newer or older than its list.
- This is defined before step 1, which builds mkrepo.

## 4. The local database and the operations

### 4.1 Where things live

| Path | What | Written by | How it survives a cut |
|---|---|---|---|
| `/var/lib/zpkg/NAME.manifest` | an installed package (D15, unchanged) | zup only, inside a transaction | the journal |
| `/var/lib/zpkg/journal`, `result` | zup's (D15; `result` only by the boot run) | zup | D15 |
| `/var/lib/zpkg/lock` | the pid of the zpkg or zup that changes things (4.2) | zpkg, zup | made with `O_EXCL`; a dead pid's lock is stale |
| `/var/lib/zpkg/sync/CHANNEL.index`, `.sig` | the last verified list of a channel | zpkg | `.sig` then the list written as `.new`, renamed, the list last |
| `/var/lib/zpkg/deps` | names of the packages installed only as dependencies | zpkg, after a commit | `.new`, renamed; a missing name = explicit |
| `/var/cache/zpkg/prev-V/` | the previous system (D15, unchanged) | zup | D15 |
| `/var/cache/zpkg/live/` | the hard links of the one package transaction under way | zup -f / -r | the journal; emptied at its commit |
| `/var/cache/zpkg/pkg/` | downloaded package files and their `.sig` | zpkg | `.part`, renamed after its SHA-256 matched |
| `/etc/zpkg.conf` | channel, servers, held packages, cache limit | the owner | a base config file (`c`, three-way compare) |

**The record of which package owns which file is D15's manifests, nothing new** (a second index would have to be
kept in step inside the same transaction). **No migration:** a D15 machine's database is already zpkg's; zpkg only
adds `sync/`, `deps`, `lock` and the cache. zpkg works in the cache with **relative names** (a 24-character package
name makes `/var/cache/zpkg/pkg/NAME-VNUM.zup.sig.part` 68 bytes, over the 63-byte system-call limit). Images without a
database (the small ones, `PKGDB` unset): zpkg says "no package database here" and changes nothing.

**Why `deps` lives outside the transaction:** it only decides what `-Rs` may remove; a cut between the commit and the
rewrite leaves the package counted as explicit, the safe side.

### 4.2 One installer: zpkg fetches and decides, zup installs

| | (a) zpkg links zup's code | (b) **zpkg runs `/sbin/zup`** |
|---|---|---|
| Who writes a journal | two programs | **one** |
| Who recovers it after a cut | zup at boot, maybe another version of the code | **the same program that wrote it** |
| Cost | none | a spawn per package (0.3-0.5 s on the stock A1200, *estimate*) |

**Choice: (b).** zpkg never writes a journal. zup gains two modes inside its existing engine:

- **`zup -f FILE`**: install one package bundle from a file; one file = one transaction. **Streamed:** the header,
  table, manifest and the plan stay in memory; the tgz part is read from the file twice (its SHA-256 first, then
  inflated), so a package's size costs no memory and no 256 KB contiguous block is needed. The change table is
  allocated for the plan's size (a live package has tens of changes, not 1,024).
- **`zup -r NAME`**: remove one package as one transaction.
- **zup's own rules for both (it trusts no caller):**
  - **The lock.** zup takes `/var/lib/zpkg/lock` itself with `O_CREAT|O_EXCL` (or finds it held by its parent zpkg,
    whose pid it is given) and runs `recover()` only while it holds it, so a second zup (by hand, or after zpkg died
    while its zup ran) can never take a running transaction's journal for a cut one. The journal of `-f`/`-r` is made
    with `O_EXCL`. A lock whose pid is not alive (`procinfo`) is stale. zup at boot runs before anything else and
    clears a stale lock.
  - **Live transactions never touch `prev-*`.** The journal's head carries `live 1` before the operations (inside
    the synced part); `recover()` reads it **before** choosing a directory: prevdir `live/`, transaction 1, no undo
    log, and a commit path of its own (empty `live/`, unlink the journal, clear the mark) that never runs
    `post_commit`'s deletion of `prev-*`; leftovers in `live/` are emptied at the next start of a transaction; the
    result messages name the package, not a system version; never `SYS_update NOTE`, never `result`. A host test
    checks that `prev-V` is byte-identical after a cut at every step of `-f`, `-r` and `takes`, including just after
    the commit line.
  - **Versions:** `-f` does not use install()'s "not older than the installed base" rule (a package's version is
    often older than base); it needs the package newer than the installed one (or `-d`, a downgrade zpkg asked for)
    and the header's minimum not above the effective base (the smaller of the installed base and the running kernel,
    `SYS_update INFO`).
  - **Never the base group:** refused by its `group` line **and** by name (`zup`, `zpkg`, `base`, and any package
    `prev-V`'s undo logs touch, 4.4); `-r` refuses the same and any package a base-group package depends on.
  - **Paths:** refused if owned by another installed manifest (unless the package `takes` it), under
    `/var/lib/zpkg` or `/var/cache/zpkg`, or with a symbolic link in a directory component (a link from another
    package could point into `/sbin`). `d` entries may be shared (pacman's rule); a `d` where a file is, or the
    reverse, is a conflict.
  - **The installed-manifest table** grows as needed (today's 32 slots fail silently); a limit that is hit is an
    error.
  - SIGINT, SIGHUP and SIGTERM ignored while a transaction runs (as SIGPIPE is today).
- **Config files on removal:** one the owner never changed is deleted; a changed one is renamed `PATH.save` (`-R`) or
  deleted (`-Rn`). The rename is a new journal operation `S` (a hard link to `prev`, then a rename), live-only: only
  the zup that writes a live journal ever reads one. Empty directories that only this package listed are removed
  after the commit.
- **`takes` and config files:** the three-way compare uses the taken package's manifest entry (the owner's change is
  measured against what was installed), not the new package's.
- zup stays a v1 ZEXE using only D15-era calls (D15 section 4); the modes are new code, not new calls.
- **Shared parsers:** the manifest and bundle parsers are static in zup.c today; zpkg and the host tests need the
  same ones. A main-session commit before step 2 moves them to `user/zpkgfmt.c` (+ `.h`), used by zup, zpkg and the
  tests: one parser of each frozen format.

### 4.3 The operations

| Command | Who | What |
|---|---|---|
| `-Sy` | root | fetch `latest`: equal to the stored serial: the stored list is used (its expiry checked); lower: "this mirror is behind", the next mirror; higher: the list and its `.sig`, verified, checked (5.2-5.3), stored |
| `-Syu` | root | `-Sy`, then every installed package with a newer version in the list, and the system bundle if newer (4.4) |
| `-Syuu` | root | as `-Syu`, also down to the list's version where the installed one is newer (after a channel change) |
| `-S NAME...` | root | the packages and their missing or too old dependencies, dependencies first |
| `-Sw` | root | download and verify only |
| `-Ss WORD` | anyone | names and `desc` lines of the stored list containing WORD (no case), installed ones marked |
| `-Si NAME` | anyone | the list's block for NAME, in words |
| `-Sc` | root | the cache down to the installed version and the one before of each package |
| `-Q`, `-Qi`, `-Ql`, `-Qo PATH`, `-Qu` | anyone | installed packages; one in words; its files; the owner of a path; those with a newer version in the stored list (its expiry said) |
| `-R NAME...` | root | remove; refused if an installed package depends on it (named) |
| `-Rs`, `-Rn`, `-Rns` | root | `s`: also the dependencies installed only as dependencies that nothing left needs (recursively); `n`: also the changed config files |
| `-U FILE...` | root | a package or system bundle from a file, its `.sig` verified (5.1); an older system bundle refused |
| `--back NAME` | root | the newest cached (else pool) version older than the installed one, by `-U -d` (asks first) |

- **Order of work:** resolve from the list (a loop or a missing dependency refused before anything is fetched);
  download and verify everything, close the connection; **then** check the downloaded manifests (their `depends`
  equal to the list's, file conflicts as in 4.2), so a problem stops the set before the first change; then one `zup
  -f` per package, dependencies first. zpkg holds no connection and little memory while zup runs.
- **Dependencies:** a name and a minimum version, nothing else. **Downgrades and holds check the reverse:** `-Syuu`,
  `--back`, `-U -d` and `hold NAME` refuse (or name) an installed package that needs the newer version.
- **Partial updates:** a program built against an older base keeps running on a newer one (the frozen interfaces
  cover the kernel's calls and the servers' messages). They do **not** cover what packages expect of each other
  (a command option a script uses, a file one writes and another reads): those are declared with `depends`, and
  `-S foo` installs foo's declared dependencies at their minimum versions. Nothing more is promised.
- **Holds:** `hold NAME` in `/etc/zpkg.conf`; `-Syu` names what it skipped. `hold base` holds the system.
- **`-Qo` and the conflict check** read all installed manifests (40 today, 150 later; 1-3 s on the stock A1200,
  *estimate*).
- **No install scripts:** a package is files, links and directories (owner question 6).
- **A running program keeps running** (D13); zpkg names the updated programs that are running (`procinfo`).
- **During an unconfirmed system trial** (D15 section 8) zpkg installs nothing live and writes no slot: "previous"
  would break what it installs. It knows the trial from a new `SYS_update INFO` flag `UPD_F_TRIAL` that zenixboot
  sets in the bootinfo flags when it hands over during a trial (growth of a frozen field; the main session claims the
  bit); an older zenixboot leaves it 0, and zpkg then only warns.
- **Where zpkg changes nothing:** a RAM-disk root (`RAM=`, live installs would vanish at power-off: refused unless
  `--ram`), a ZenixFS root (test-only until block 4; zenixboot would refuse a system bundle there, `roots_check`), a
  machine booted from the bootblock floppy (no zenixboot, no slot: system updates refused with a message; programs
  install).

### 4.4 The system update goes only through D15

The base group (the kernel, `zup`, `zpkg`, `base`) is **never** installed live (D15's order rule). When the list's
`system` version is newer than the installed base, `-Syu`:

1. fetches `pool/zenix-V.zup` and its `.sig`, checks size and SHA-256 against the list, the header's version (V),
   that it carries only base-group packages, that it fits the slot and that its minimum is not above the installed
   base;
2. **writes it into the update slot (4.5)** (not if the slot already holds this digest) and says "Zenix V is ready:
   it is installed at the next restart (reboot)";
3. installs every other package now, except those whose minimum base is the new V: downloaded and verified into the
   cache and named ("12 packages wait for Zenix V: after the restart, zpkg -Syu again"; nothing is downloaded twice).

At the restart **nothing is new**: zenixboot checks the signature (D21 6.4), installs the kernel, keeps `zenix.old`,
hands the root half to zup, the trial, the confirmation, "Zenix (previous)": D15 as built. `zpkg -U zenix-V.zup` goes
the same way. **There is no other way for the kernel, `init` or `sh` to change.**

**The way back stays clean only if the system's undo logs and live installs never touch the same package.** Today's
bundles carry `utils` and `man`, so `prev-V`'s undo logs on every machine hold their files and manifests; a live update
of `utils` followed by "previous" would put back an old `utils.manifest` beside files it does not list. So:
- zup refuses `-f` and `-r` of any package whose manifest path appears in the newest `prev-V`'s undo logs (4.2), for
  as long as that set exists; the first base-only system update (step 4) ends it for today's machines;
- system bundles carry the base group only (3.1), so after that the two never meet again.

**After "Zenix (previous)":** packages installed later that need the newer base stay, and `zpkg -Q` marks them ("needs
base V, base V-1 runs"); they work again when the system update is installed again, or the owner takes each back with
`--back`. Said plainly in `man zpkg`: the system's way back is the boot menu, a package's is the cache.

### 4.5 The update slot: how a bundle from Zenix reaches BOOT:

| | How | Against it |
|---|---|---|
| (a) An FFS writer in Zenix | create a file on BOOT: | bitmap, headers, hash chains written by Zenix on the owner's AmigaOS partition: the riskiest write in the system, for one file |
| (b) zenixboot reads ext2 | find the root on the card, read a file | MBR/RDB parsing and Emu68's whole-card unit before Zenix runs (D15 rejected it) |
| (c) The bundle in memory across a warm reset | Zenix leaves it in RAM, reboots | lost at every power-off; AmigaOS's own boot may use that memory on a chip-only machine |
| (d) The owner copies it | from AmigaOS, or Wasabi | not "installs itself" |
| (e) **A slot file** | a file made once by AmigaOS on BOOT:, whose data blocks Zenix overwrites in place | Zenix writes into an AmigaOS partition, but never its metadata |

**Choice: (e),** the pattern GRUB uses for its environment block. FFS (unlike OFS) keeps no header or checksum in
its data blocks; overwriting a file's data changes no bitmap, header, hash chain, date or directory-cache entry.
**It needs the owner's yes** (CLAUDE.md: no writes to a disk with AmigaOS partitions without an agreed plan): owner
question 1. If no, (d): zpkg leaves the bundle in the cache and says how to copy it.

- **Made once, by AmigaOS:** `zenixboot MKSLOT [SIZE]` (default 1,280 KB, D15's ceiling for a bundle on a 2 MB
  machine; smaller for a small BOOT:), run by the owner's card package, the same one that brings D21's signing
  zenixboot (zenixboot is only ever installed by hand). Zenix reads the size from the file header.
- **The slot's format** (frozen once shipped; in bytes, not blocks): bytes 0-511 the control block (`ZSL1`, a format
  word 1, state 0 empty / 1 ready, a sequence number, the bundle's size, its header digest, the 80-byte `.sig`, a
  SHA-256 over the block's bytes before it); the bundle from byte 512.
- **Zenix's side** (zpkg, through diskd; *the strictest code in zpkg*):
  - **Geometry:** the RDB partition block's environment (block size, reserved blocks, dostype) read through a
    read-only open of the whole disk (allowed while the root is mounted); refused unless 512-byte blocks and 2
    reserved blocks, until a test covers more.
  - **Dostypes:** `DOS\1` and `DOS\3` only (what `tools/mkffs.py` makes and the tests cover); `DOS\0`, `\2`, `\4`,
    `\5`, `\6`, `\7` and anything else refused until a test image covers them.
  - **The walk, read-only, every time (no block list is ever kept between runs):** the root block, its "bitmap valid"
    flag (a volume AmigaOS left validated), the `Zenix` directory and the file by their hash chains (the
    international hash for `DOS\3`), the file header (secondary type exactly `ST_FILE`, no hard or soft link),
    extension blocks (`T_LIST`, `ST_FILE`), every checksum and type; every data-block pointer inside the partition,
    none twice, none equal to a block it read as metadata, none a bitmap block, **every one marked in use in the
    bitmap**; the byte size exactly the slot's and the block count to match. Exactly one slot on the machine.
  - **The device must flush** (`DISKCAP_FLUSH`), else refused.
  - **The order:** control block written empty; the bundle into the data blocks; `DISK_FLUSH`; the control block
    written ready; `DISK_FLUSH`. A cut leaves the slot empty or not ready (the control block's SHA-256 makes a torn
    write read as empty): nothing happens at boot, the next `-Syu` writes it again.
- **zenixboot's side** (under AmigaOS, plain `Read`): **only** when the slot is ready, no `zenix-update.zup` exists,
  the D15 phase is neither INSTALLING nor TRIAL, PREVIOUS was not chosen, and the start is not CHECK, `RAM=` or an
  explicit KERNEL; and **before** `update_half`'s rule that renames a leftover `.part` to `.bad`: stale `.sig.part`
  and `.part` deleted, the `.sig` written as `zenix-update.zup.sig.part` and the bundle as `zenix-update.zup.part`,
  the copy checked, renamed (`.sig` first, D21 6.2), flushed, the control block written empty, flushed; then D15 as
  built. Otherwise the slot stays ready for a later start. A cut: the slot is still ready and the copy is repeated (a
  bundle already there with the same digest: only the slot is emptied).
- **Space on BOOT:** the slot plus D15's peak (`zenix`, `zenix.old`, `zenix.new`, the bundle, the state file): about
  2.3 MB for a 400 KB bundle and the default slot. The owner's BOOT: size and dostype are read in step 7 before
  anything is written.

## 5. Trust

### 5.1 What is verified, when, by whom

- **The list:** `zc_sig_verify` with D21's label and keys over the bytes fetched, **before** they are parsed or stored;
  then `channel`, `serial`, `date`, `expires` (5.2-5.3). The stored list is used without a second signature check
  (root-owned, as the installed files), but its expiry is checked at every use. (A snapshot's list: its name instead of
  its expiry, amendment 1.)
- **A package or system bundle from the list:** the download stops at the list's size (a server sending more is cut
  off; a total time per file and chunk sizes are limited), its SHA-256 is computed while it streams to `.part`, it is
  renamed only when it matches; then the name and version inside must equal the list's, and the header's channel must
  be **of the key's class** (`test` for the test key; `testing` or `stable` for the release keys). A cached file is
  checked against the list again before use. Its `.sig` is fetched beside it (for `-U` and `--back` later), not
  verified now: the signed list covers it and saves 6-9 s a package.
- **A file by `-U`** (the cable, the cache): its `.sig` (D21's bundle label) verified once before anything else; no
  `.sig`, refused.
- **A system bundle** is verified again by zenixboot at the restart (D21 6.4): one check per transport.
- **zup checks again** every part's SHA-256 and every file against its manifest, and enforces 4.2's rules itself.
- **Keys:** release builds trust the release keys (current and next), test builds only the test key (D21 6.3,
  choice 7); a list or file whose channel class does not fit the key is refused. mkrepo (and zsign.py, a fix for
  the main session: today it checks the class only for bundles) refuses to sign a `testing` or `stable` list with the
  test key. `zpkg`'s keys travel in its package (D21 6.3).

### 5.2 Replay and downgrade

- **The serial:** a list is accepted only if its serial is larger than the stored one for that channel (equal: the
  stored list is used). A replayed older signed list is refused. A fresh machine's floor is zpkg's build date as
  `YYYYMMDD00`. A serial is bounded by its own signed `date` (3.3), so one mistaken huge serial cannot lock a machine
  out for ever; root can still reset the stored serial (`--reset-serial CHANNEL`, said loudly). A snapshot has no
  floor; its serial must equal the stored one (amendment 1).
- **Mix and match** is impossible: the list binds every file to its SHA-256.
- **Versions never go back by themselves:** `-Syu` only moves up; down only by `-Syuu` or `-U -d`. zpkg refuses an
  older system bundle itself (zenixboot alone would accept one after REJECTED); the system's way back is "previous".
- **Changing the channel:** the new channel's serial is compared with its own stored list; packages newer than the
  new channel stay until it catches up, or `-Syuu`.

### 5.3 A frozen repository, and a wrong clock

- **Expiry** (D21 6.5): a list past `expires` is refused, at every use, also when `latest` says nothing is new (a
  stale `latest` cannot hide a frozen repository). The release tool re-signs each channel weekly (same files, new
  serial), `expires` 60 days after signing (owner question 3); a snapshot's list is bound by its name instead
  (amendment 1). Said plainly: if the owner's PC stops re-signing, every
  machine, the public ones too, refuses its list after 60 days until it is re-signed or `--ignore-expiry` is used.
- **The time:** "today" is the newer of the clock and D21's floor (`/var/lib/clock`, the build date). A clock that is
  behind never makes a list look valid for longer than the floor allows.
- **A clock that is ahead** (2099) makes every list look expired: zpkg says "the package list expired on 2027-01-02,
  and the clock says 2099-01-01: check the date"; `--ignore-expiry` (root) goes on once; the serial still protects.
- **A clock behind the signed `date`** by more than a day: zpkg says "the clock is behind". **Proposed, for the main
  session (it adds a writer to D21 8.3):** raise the floor to the list's signed `date` under D21's own sntp guard
  (agreeing within 10 minutes with the battery clock, or at most 400 days past the current floor); mkrepo refuses a
  `date` far from the previous list's (a PC with a wrong clock).

### 5.4 Said plainly

Plain HTTP from the NAS hides nothing and lets nothing be changed. Without an MMU any root program can replace any
file anyway; the signatures guard the transport, not the machine (D21 9.1). A stolen release key lets the thief sign
packages until zpkg and zenixboot carry a new key (zpkg by a system bundle signed with "next", zenixboot by hand).

## 6. Building the repository on the PC

### 6.1 The tools

- **`tools/mkzup.py`** grows (formats only grow): `bundle --no-kernel` for a package bundle (one package; version the
  package's, `--min` its base dependency); refusal of a non-base package in a system bundle; `desc` and `takes` lines;
  the per-family package lists (8.4.4) instead of one `utils`.
- **`tools/mkrepo.py`** (new): `add REPO CHANNEL BUILD` (3.4's version rules, the pool files, a new list with serial
  and expiry, every new file and the list signed through `zsign.py`, `latest` written last; refuses a test key for a
  release channel and a `date` far from the previous list's), `promote REPO testing stable`, `resign REPO CHANNEL`
  (the weekly re-sign), `snapshot REPO stable 2026.11` (signs, amendment 1), `prune REPO` (3.2), `check REPO` (every list, signature, size
  and SHA-256, as zpkg would), `installdb` input for images (3.4).
- **`tools/zsign.py`** (built, D21): the kinds `bundle` and `index` exist; the index class check is added (5.1).

### 6.2 The release process the owner can run (or ask a session to run)

1. `make check` passes (the testing channel's rule).
2. `make release` (a RELEASE=1 build, `mkrepo.py add` to testing, signed with the release key) and `tools/publish.sh
   nas` (an `rsync` to the NAS's web folder; the NAS address lives in the owner's `~/.config/zenix-repo.conf`, never
   in the repository).
3. The owner's A1200 runs `zpkg -Syu` from the NAS.
4. "Daily driven" (the owner's word): `mkrepo.py promote testing stable` and `tools/publish.sh github` (the tree to
   the Pages branch of a public `zenix-packages` repository, one orphan commit each time, so binaries never pile up in
   git history; Pages allows about 1 GB a site and about 100 GB of traffic a month, far above a few MB a release).
5. A user timer on the PC runs `mkrepo.py resign` for both channels weekly and publishes (owner question 3).

**The signing key** stays where D15 and D21 put it (`~/.config/zenix-signing/zenix-release.key`, mode 600, outside
every repository, an offline backup); only `zsign.py` reads it. **People who build Zenix themselves** have the test
key, not the release key: their builds trust only the `test` channel, which they serve themselves; the public stable
channel is for release builds. **GitHub Releases are not used:** their downloads redirect to another host, a second
TLS handshake per file (20-30 s each on the stock A1200); Pages is one host with keep-alive.

## 7. Size, memory and time on the stock A1200

### 7.1 Code (*estimates* unless marked)

| Part | Size |
|---|---|
| zpkg's own logic (config, list parser, resolver, database reader, commands, cache) | 15-20 KB |
| the HTTP/1.1 client (7.4) | 5-8 KB |
| Ed25519 verification (`zc_sig_verify`) | 19,334 bytes (*measured*, D21) |
| the slot writer (read-only FFS walk) | 3-5 KB |
| **zpkg without TLS** | **about 55-65 KB** (sha256sum with libz is 9.6 KB of text, telnet 20 KB, *measured* by the review) |
| the TLS client and X.509 wrapper, from step 9 | +82,554 bytes (*measured*, D21) |
| **zpkg with HTTPS** | **about 140-150 KB** on the disk; loaded only while zpkg runs |
| zup's two modes, the lock, the live paths | 3-5 KB on today's 48,104 bytes |

**TLS linked into zpkg is an accepted cost:** 82 KB on the disk twice (zpkg and later `wget`), nothing in memory when
zpkg is not running; a shared fetch helper would save the disk space but cost a second program, a protocol between
them and one more process during a `-Syu`. If disk space on floppies ever asks for it, the demo floppy's own step
decides.

### 7.2 Memory during a `-Syu` (stock A1200: 1,132 KB free with one login, *measured* 2026-10-26)

- **While zpkg fetches:** its code 60-150 KB, the list (27 KB) and its table (about 6 KB), the installed manifests'
  table (20-60 KB), a 4 KB download buffer (downloads stream to the disk), with TLS BearSSL's 16.7 KB buffer and
  7 KB of contexts, a 16 KB stack: **about 140-280 KB**.
- **While zup installs** (zpkg waits with its connection closed, so its peak stays as above): zup's text 43 KB and
  bss (today 97 KB, of it the 80 KB change table, which `-f` allocates by the plan's size instead: a few KB), miniz's
  window 32 KB and decompressor about 11 KB, buffers 8 KB, a 16 KB stack, the manifests and the journal text:
  **about 120-160 KB** with `-f` streaming (revision 1 had about 460-500 KB with the package in memory, the
  architecture review's measurement).
- **Peak: about 260-440 KB**, beside a login session on the 2 MB machine. **The 1 MB chip-only machine** (about 428 KB
  free with one login, D19 *measured*): plain-HTTP zpkg probably fits once `-f` streams; zpkg checks the free memory
  before starting and says so if it is short. Measured in step 5.

### 7.3 Time on the stock A1200 (all *estimates*)

The basis: SHA-256 about 1 ms per 64-byte block, 60 KB/s (D15 section 9); Ed25519 verification 6-9 s (D21 7.3); SLIP
11.5 KB/s at most (the line; D19 measured 7-8.4 KB/s paced, in the emulator); inflating about 150 KB/s; a zup
transaction's fixed cost 1-2 s; a spawn of zup 0.3-0.5 s.

| Case | Stock A1200 over SLIP | 68060 or PiStorm, Ethernet |
|---|---|---|
| `-Sy`, nothing new (`latest` only) | about 1-2 s | under a second |
| `-Sy`, a new list (27 KB, one signature) | about 10-14 s | about 1 s |
| `-Syu` with one changed 30 KB program (75 KB unpacked) | about 10-15 s more | 1-2 s |
| `-Syu` after a libz change: all 40 packages, 1 MB packed, 2.5 MB unpacked | **about 6-9 minutes** (download 90-140 s, SHA-256 of the download and zup's two passes 50 s, inflating 17 s, zup's checks of 2.5 MB written and read back 85 s, 40 transactions 60-100 s) | about 20-40 s |
| a system bundle of 400 KB to the slot | about 50-70 s (download 35-55 s, SHA-256 7 s, writing) | a few seconds |
| the restart that installs it | about 1-1.5 minutes more than a normal start (zenixboot's copy and check about 20 s, Ed25519 6-9 s, SHA-256 of 400 KB twice 14 s, zup's two stages) | seconds |
| HTTPS (GitHub) instead of the NAS | + one handshake per run, about 20-30 s (RSA chain) | + 1-2 s |

The slow parts are where D15 put its safety and the serial line; neither is cut. The common case stays short. To be
measured on the owner's machines (step 8).

### 7.4 The HTTP client

Following the D20/D21 pattern: **a core without system calls in `net/http/`** (request writing, response parsing,
chunked decoding, the state machine), host-tested and fuzzed, and a small socket layer; linked by `zpkg` and later
`wget` (static, as each program links what it uses). HTTP/1.1 `GET` only, `Host`, keep-alive, `Range: bytes=N-`,
`Content-Length` or chunked bodies (chunk sizes checked against overflow), at most 3 redirects (never from https to
http), a 4 KB limit on the response header, timeouts (connect 15 s, between bytes 60 s, 120 s for a TLS handshake on a
slow CPU, a total per file from the expected size); names through `getaddrinfo` (D19); `poll` for the timeouts (D16).
No proxies, cookies or authentication. TLS through D21's wrapper for `https:`.

### 7.5 Downloads: resume and the cache

- **Resume:** a cut download stays as `.part`; the next run asks for the rest (`Range`); a `206` with the right
  `Content-Range` continues (the SHA-256 is computed over the whole file at the end: a wrong prefix is found, the file
  deleted and fetched once from the start); a `200` restarts it. Immutable pool names make this safe.
- **The cache** is on the disk: the installed version and the one before of each package with their `.sig`, system
  bundles only while they wait. About 2 x 1 MB for today's set; `cache 4096` (KB) in `/etc/zpkg.conf` caps it, but
  **never evicts the installed version or the one before it** (ROADMAP, "always a way back"); `-Sc` empties it down to
  those. `--back` fetches the previous version from the pool when nothing is cached (images have no cache).

### 7.6 Transports and the shipped configuration

- **The NAS:** plain HTTP (owner decision 6), the testing channel, home only.
- **GitHub Pages:** HTTPS only (*verified*): the stable channel, public, from step 9. HTTPS needs D21's trust anchors,
  the package `ca-certificates`: zpkg's manifest depends on it, and `-R` refuses what a base-group package depends on.
- **The shipped `/etc/zpkg.conf`:** until step 9 it names no server (only commented examples; `man zpkg` shows the
  NAS line), so zpkg says "no server configured" instead of failing at GitHub; from step 9 it names the GitHub
  stable channel. The owner's own file survives (three-way compare).
- **The serial cable:** the same `zpkg` over SLIP (D19, through `zenix-slirp`: a copy of the tree served on the PC or
  the NAS through the PC). Without SLIP: `rz` (D20) receives the files, `zpkg -U` installs them.

## 8. Tests

### 8.1 On the PC

- **The repository tools:** `mkrepo.py` against a scratch tree: 3.4's version rules (an unchanged rebuild gives no new
  versions and no system update; the kernel's `ZVER` masked; rewritten `depends base` and header minimum), serials
  growing and bounded by their date, `latest` last, promote and snapshot naming the same pool files, prune, `check`
  finding every kind of damage, the test key refused for release lists; two builds compared byte for byte.
- **zpkg's core built for the PC:** the list parser (damaged, oversized, wrong channel, wrong class for the key, older
  and equal serials, a serial above its date, expired at every use, the clock ahead, behind and missing), the
  resolver (loops, missing, minimum versions, order, `-Rs`, reverse dependencies of downgrades and holds), conflicts
  (owned, unowned, shared directories, `takes`, `depends` differing from the list), snapshots, the `deps` file after a
  cut.
- **The HTTP core against a test server** (`tools/host/http-test-server.py`, in `unshare -rnm`): keep-alive, chunked
  and huge chunk sizes, `Range` honoured and ignored, a wrong `Content-Range`, bodies longer and shorter than
  announced, an endless body, a slow drip, a header over 4 KB, redirect loops, a stalled server, a cut connection then
  a resume, a mirror that is behind or serves a damaged file.
- **The slot writer** against `tools/mkffs.py` images (DOS\1 and DOS\3, files before and after the slot in the same
  hash chain, extension blocks, a slot too small, two slots, a slot block marked free in the bitmap, a slot that is a
  hard link, damaged checksums and pointers): after a write, every block but the slot's data blocks is byte for byte
  what it was; OFS, other dostypes, other block sizes and an invalid bitmap refused untouched.
- **zup's new modes** in `tools/zup-host-test.sh`: a power cut before each change of `-f`, `-r` (with `.save`) and a
  `takes` hand-over, each finished or rolled back; `prev-V` byte-identical after every cut, the commit line included;
  the D15 rollback after live transactions; a second zup refused while one runs, a stale lock taken over; the refusals
  of 4.2 (base by name, `prev-V` packages, owned paths, `/var/lib/zpkg`, symlinked directories); on the real ext2
  server with a cut after each block write (`tools/e2safe.py`, D15/F5).
- **Fuzzing** (libFuzzer, one run at a time with a memory cap, this PC's rule): the list parser, `latest`, the HTTP
  core, the FFS walker, `zpkgfmt.c`'s manifest and bundle parsers fed through `-f`.

### 8.2 In FS-UAE

- A test repository (test channel, test key) served from a namespace on the PC, through the A2065 (eth-test's setup)
  and through SLIP (`zenix-slirp`): `-Sy`, `-S` with dependencies, `-Ss`, `-Si`, `-Q*`, `-R`, `-Rns`, `-Syu` replacing
  a program another console runs, `--back`, a tampered package and a replayed old list refused, an expired list, a
  download cut and resumed, the emulator killed during a live install (the next boot's zup finishes or rolls back;
  `e2fsck` after).
- **The whole system path:** `-Syu` with a newer system bundle writes the slot on the zenixboot-test disk; at the
  restart zenixboot copies it (`.sig` first), and update-test's D15 checks follow (kernel, trial, confirmation,
  "previous"); a cut during the slot write (nothing at boot); a slot ready during a trial left alone; zpkg refusing
  live installs during the trial; the FFS volume read by AmigaOS itself afterwards (every file).
- Memory: `free` during a `-Syu` on the 2 MB and 1 MB chip-only machines. No speed figure from the emulator.

### 8.3 On the owner's machines

The times of 7.3 (stock A1200 over SLIP, the 060, the PiStorm over GENET); the slot on the owner's real BOOT: (its
dostype, block size and free space read from AmigaOS first); the first system update through zpkg.

### 8.4 Notes for the build (not tests)

1. Nothing of the D15 boot path changes; zup's boot run ignores `live/` except to empty leftovers.
2. The effective base is the smaller of the base manifest's version and the running kernel's (`SYS_update INFO`).
3. A package's minimum base is the base it was built with (3.4); so after a system update the packages rebuilt with
   it wait for the restart once (4.4 step 3).
4. **The split of `utils`** into families (the shell's helpers, `coreutils`-style small commands, the text tools, the
   editor, the disk tools, the network tools, the user tools, ...; each program's manual page in its package) uses a
   manifest line `takes OTHER`: zup, planning a package that takes OTHER, treats a path OTHER's installed manifest owns
   as its own (an `R` with a prev link, never a gap without `/bin/ls`) and rewrites OTHER's manifest without those
   entries in the same transaction (an `R` of `/var/lib/zpkg/OTHER.manifest` with text zup makes); OTHER's manifest
   is removed when no entry is left. **Never from or to a base-group package:** the manual pages of the base programs
   (init, login, sh, mount, zup, zpkg) go to a non-base package `base-man`, which takes them from `man`. Old zups
   ignore the line. Order: the system update that brings the `takes`-aware zup and ends today's `utils`/`man` in
   system bundles comes first; the family packages, which depend on it, follow live. `utils` (480 KB) is never
   installed live: the split comes before zpkg is in use.

## 9. Build order (small steps; the full suite before each merge)

Side work is host-only and uses this note's names and formats (CLAUDE.md, "Coherence first"); everything touching the
`Makefile`, `UAPPS`, libz, `user/usys.h`, kernel headers, images, `tools/check.sh` or the counts in `docs/` is the
main session's.

0. *both:* this note reviewed (done twice, section 13), a second look at revision 2 if the main session wants one, the
   owner's answers.
1. *main:* `user/zpkgfmt.c` (the manifest and bundle parsers moved out of zup.c, no behaviour change, zup's tests
   unchanged); the `UPD_F_TRIAL` bit claimed.
2. *side:* `mkzup.py bundle --no-kernel` and the base-only rule, `desc`/`takes` lines; `tools/mkrepo.py` with 3.4's
   version rules and its host tests; the list format and `latest`; a test repository.
3. *side:* zpkg's core for the PC (list, resolver, database reader, trust rules, the FFS slot walker against
   `mkffs.py` images), `net/http/`'s core against the test server, the fuzz harnesses.
4. *main:* zup's `-f` (streamed) and `-r`, the lock, the live paths, `takes`, `.save`, the refusals; `zup-host-test.sh`
   extended. Two reviews (the update engine). Then **a base-only system update through D15 as today** (the copied
   bundle, as before): it brings this zup, ends `utils`/`man` in system bundles and brings the family packages' split
   (8.4.4) with `base-man`.
5. *main:* `zpkg` in the base group (`UAPPS`, the package, `/etc/zpkg.conf`, `man zpkg`, `commands.md`,
   `features.md`), plain HTTP, the FS-UAE tests over the A2065 and SLIP (8.2) without the system path; memory on the
   1 MB machine. **First thing the owner can use** (with the NAS testing channel).
6. *main:* `mkroot.sh`'s `installdb` from the repository state (3.4).
7. *main, with the owner:* the slot: `zenixboot MKSLOT` and the copy at start, zpkg's slot writer, `UPD_F_TRIAL` in
   zenixboot; FS-UAE end to end; together with D21 step 5's card package (the signing zenixboot). Before anything is
   written to the owner's card: its BOOT: dostype, block size and free space read from AmigaOS.
8. *owner:* the NAS's testing channel; `zpkg -Syu` on the stock A1200, the 060 and the PiStorm; the times.
9. *main, after D21 step 7:* HTTPS, `ca-certificates` as zpkg's dependency, the stable channel on GitHub Pages, the
   shipped `/etc/zpkg.conf` naming it, the weekly re-sign timer, snapshots.

## 10. Frozen once built (CLAUDE.md's list gets them in their steps)

The list's format (`zpkgindex 1`, its words and meanings, growth only), `latest`'s line, the pool's names and the
channel directories (3.2-3.3); the slot file's place, the `MKSLOT` size rule and the control block (4.5: zenixboot
changes only by hand, so every later zpkg must write what the first slot-reading zenixboot reads); `UPD_F_TRIAL` (a
bit of D15's frozen bootinfo flags, growth); `/etc/zpkg.conf`'s format (a base config file a base program reads); the
manifest lines `desc` and `takes`, the journal line `live` and the live-only operation `S` (growth of D15's frozen
formats, which old readers ignore and never meet); `/var/lib/zpkg/deps`, `lock` and `sync/` (read by later zpkgs and
zups). zup's `-f`/`-r` command line is between two programs of the same base version and is not frozen. **Nothing here
changes an existing frozen meaning:** no system call, message or struct (one flag bit grows); D15's bundle header keeps
its meaning (a package bundle's version is that package's, its minimum the base it needs: the same words for one
package); D15's manifest, journal, undo logs, prev sets and update note keep theirs; D21's `.sig` and labels are used
as they are.

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
- **To claim or change elsewhere when built:** `UPD_F_TRIAL` (bootinfo flags, `user/usys.h`, `kernel/kernel.h`);
  `zsign.py`'s class check for lists; D21 8.3's floor writer (5.3, if agreed); D15 section 10 "Over the serial cable"
  pointing to 7.6 here.
- **Review depth:** two reviews (update core) done; a second look at revision 2 before step 4 is the main session's
  call.

## 13. Reviews

Two independent read-only reviews of revision 1 (2026-10-27); every finding is worked in above.

**Correctness, crash safety, trust** (read against zup.c, mkzup.py, zsign.py, zenixboot.c, diskd.c, mkffs.py, D15,
D21): *blocker* - a live transaction would have used `prev-V`'s paths: its commit or a recovery after a cut deletes the
system's way back (`post_commit`, `set_prevdir`); now `live 1` in the head, read before any directory is chosen, a
commit path of its own, a host test that `prev-V` is untouched (4.2). *Should fix* - no lock on zup (4.2); today's
bundles carry `utils`/`man`, so "previous" after a live update would revert half a package (4.4: `prev-V` packages
refused live, system bundles base-only); `-f` inheriting install()'s base-version rule (4.2); `utils` above the
package limit before the split (8.4.4, 9); promotion against the channel check (the class rule, 5.1); the conflict
check "before any download" impossible and zup's own refusals (owned paths, `/var/lib/zpkg`, symlinked directories,
base by name, shared directories, `depends` cross-check: 4.2, 4.3); the `-Sy` shortcut skipping expiry and a lower
`latest` (4.3, 5.3); snapshots against the serial and expiry rules (3.2); the clock floor from the list's date under
sntp's guard (5.3); the slot's geometry, bitmap and link checks, untested dostypes, zenixboot's copy against the D15
phases and the `.part` rule, a device that cannot flush (4.5); "partial updates are safe" reworded (4.3); mkrepo's
change rule (3.4). *Minor* - live installs during a trial (`UPD_F_TRIAL`, 4.3); reverse dependencies of downgrades and
holds; the 32-manifest table; `.save` as a live-only operation; `takes` and config files; cache path lengths; the
built-in serial, a serial bounded by its date and a reset; `.sig` size, time and chunk limits; cached files re-checked;
an older system bundle refused by zpkg; zsign.py's class check for lists; the sync pair's order; the example's dates;
the restart's time; slot fit and minimum checked first.

**Architecture fit** (with the sizes re-measured from the build, all matching revision 1): *blocker* - two zups at
once (the same finding, 4.2); the build's single version and the kernel's `ZVER` would make every release a system
update and could make changed packages uninstallable (3.4: mkrepo assigns versions, `ZVER` masked, `depends base`
rewritten, images from the repository). *Should fix* - "exactly D15's bundle" hid the base-only precondition (3.1);
dissolving `man` against the `takes` rule (`base-man`, 8.4.4); shared directories (4.2); the 32-manifest table; `-f`'s
version rules and `-r` on base; the memory peak underestimated (zup's bss 97,142 bytes *measured*; now `-f` streams and
sizes its table, 7.2); the 1 MB machine decided (7.2); RAM, ZenixFS and floppy-booted roots and images without a
database (4.1, 4.3); the stale `latest` (5.3); one shared parser (`zpkgfmt.c`, 4.2, 9); HTTPS's placement, the shipped
configuration and `ca-certificates` (7.1, 7.6). *Minor* - the example's numbers; the serial floor; zenixboot's skip
cases and a slot already ready; `MKSLOT`'s size; the restart's time and SLIP's paced rate (7.3); the cache never
evicting the way back, `--back` from the pool (7.5); the HTTP core in `net/http/` (7.4); `zsign.py`; GitHub's limits,
the re-signing burden and self-builders (5.3, 6.2). *Found sound:* zpkg running `/sbin/zup`; zpkg in the base group;
old zups ignoring every new line; diskd allowing the slot's raw writes; the immutable pool and the unsigned `latest`
(once its expiry rule was fixed); about 40 family packages; the size and time estimates.

Revision 2 keeps the shape of revision 1 (a static signed repository, package bundles, zup as the one installer,
system updates only through D15 by the slot); both reviewers judged that shape sound.

Last checked against the code: 2026-10-27 (main at 4f21bc2).

## Owner decisions (2026-10-27)

The owner delegated section 10's questions ("I let you decide those things, but decide it good and professional"). The
main session took all seven recommendations, each re-weighed: (1) the update slot, with every check of section 13;
(2) packages by family; (3) the 60-day expiry with a weekly re-sign (long enough for a PC away for weeks, short enough
to bound a replayed old list); (4) stable from GitHub Pages over HTTPS, testing from the NAS over plain HTTP, both signed;
(5) `zpkg -Syu` again after a system restart, automatic later; (6) no install scripts; (7) the previous version kept for
`zpkg --back`.

## As built: steps 2-3, the PC-side parts (side helper, merged 2026-10-27; reviewed by the main session)

Built: `tools/zsign.py` refuses to sign a `zpkgindex` list naming `testing` or `stable` with the TEST key, and a list with
no channel line; `tools/mkzup.py` gains `bundle --no-kernel` (a package bundle), `--base-only` (opt-in until step 4
makes it the Makefile's default), and `desc`/`takes` lines; `tools/mkrepo.py` (`add`, `promote`, `resign`, `snapshot`,
`prune`, `check`, `installdb`) with the layout of section 2 (`latest` written last, 24 bytes); the system-call-free
cores `user/zpkg/` (`index.c` the list, `latest` and trust rules; `resolve.c`; `ffsslot.c` the slot's read-only FFS walk
and in-place write) and `net/http/` (URLs, at most 3 redirects and never https to http, GET, a streaming response parser
with a 4 KB header limit, Content-Length or chunked with overflow checks, the resume rule); m68k -Os: 5.5, 4.1, 4.0 and
3.5 KB, no 64-bit multiplies, libz only. Tests: `tools/host/zpkg-test.sh` (in `check.sh host`; core 174, repo 56, a
differential test of the C and Python list parsers on 3,000 damaged lists, slot 71 on DOS\1 and DOS\3 images with 23
kinds of damage refused, net 40 in `unshare -rnm`); fuzzing 15 minutes in two runs (found: the HTTP parser's byte count
on an error, and in review a `-Syuu` upgrade planned before a downgrade that removes what it needs; both fixed), the
second run of about 260 million inputs found nothing.

Choices where the note was silent (binding unless changed here): (1) inside the base group, dependencies on other
base-group packages are masked like `depends base`; (2) **other dependencies (main session, 2026-10-27):** a package's
`depends X BUILD` is rewritten by mkrepo to X's version as published in the pool at that time (the newest version of X
in the channel), so an unchanged X keeps its older version and the dependent package still resolves; mkrepo refuses only
if X is not in the pool at all; (3) a package without `depends base` gets the current base as its header minimum, no
line added; (4) `--base-only` opt-in until step 4; (5) base-group blocks in the list have no `file` line; (6) a package
dropped from a build is dropped from the channel's list, and an unchanged build writes no new serial; (7) `snapshot`
copies `latest` too; (8) `prune` keeps each channel's last two lists (a cached `latest` may name the previous one);
(9) a list date before the previous list's or more than 400 days after it is refused (a parameter); (10) a fresh machine
accepts a serial at or above the floor, a machine with a stored list only a higher one; (11) limits: at most 8
dependencies and 8 `takes` lines a block, version strings 1-15 printable characters, `desc` 1-72; (12) slot: whole
blocks from 1 KB to 16 MB, a bitmap extension block refused until an image covers it, block size and reserved blocks
passed in until `user/rdb.h` carries them (the main line's change), "exactly one slot" a machine-level check, and **the
control block's field layout is defined by the main line in step 7 before it ships**; (13) HTTP: relative and `//host`
Locations are not followed, Content-Length with chunked or any other transfer coding is refused, the per-file time
limit belongs to the socket layer (step 5); (14) `user/zpkgfmt.c` and its fuzz harness are the main line's step 1.

## Amendment 1 (2026-10-28): a snapshot's list names itself (network audit O1)

**The hole** (docs/audit-2026-10-28-network.md O1, *verified* by that audit's reviewer): `mkrepo snapshot` copied a
channel's list and its `.sig` unchanged, and `--snapshot NAME` skipped the serial and expiry checks. Nothing signed
said which snapshot a list was, so anyone between the machine and a plain-HTTP mirror could answer
`snapshots/NAME/` with any older list signed by a key of the right class (an expired `testing` list from months ago),
and zpkg took it (rc 0).

**The fix: the name is in the signed bytes.**

- **A new header word, `snapshot NAME`** (growth of `zpkgindex 1`, 3.3; once, before the first `pkg`; NAME is
  `[A-Za-z0-9][A-Za-z0-9._-]{0,31}` and not ending in `.part`, mkrepo's rule for snapshot names, so it can never
  hold a `/` or start with a dot, and is never a cut run's `NAME.part` folder, which `check` and `prune` skip). A list with the line is that snapshot's list; a channel's list never has one. Inside a package block the word
  is an unknown word, ignored, as any growth.
- **`mkrepo snapshot REPO CHANNEL NAME --key KEY`** writes `snapshots/NAME/index-SERIAL`: the channel's current list
  with `snapshot NAME` after its `channel` line and every other line as it was (serial, date, expiry, every block),
  read back with the parser to be the same list, then **signed anew** (the index label, a release key: a snapshot is
  of `testing` or `stable`, never `test`, and never signed with the test key), then `latest`. A name is never used
  twice (as before). The pool files are the channel list's, so prune keeps them (3.2) as before.
- **zpkg's rules (5.1, after the signature and the class):**
  - `--snapshot NAME` takes only a list whose `snapshot` line is NAME, byte for byte. An older channel list, a list
    of another snapshot or a list without the line is refused ("the package list is not the snapshot asked for").
  - Without `--snapshot`, a list that has a `snapshot` line is refused ("the package list is a snapshot's, not the
    channel's"), so a snapshot can never be passed off as a channel's newer list either. This holds at every use of a
    stored list too (`zp_index_usable`).
  - NAME is checked with the same rule before it goes into a URL or a path.
- **Replay and the serial (5.2, kept consistent):** a snapshot is old on purpose, so it has **no serial floor** and
  **no expiry** (a floor at zpkg's build date would make every newer zpkg refuse every older snapshot, the opposite of
  "install a known state"). The binding replaces both: since the name is signed and a name is published once, the
  only list a mirror can answer `--snapshot NAME` with is that snapshot's own. What stays is the list's own shape
  (3.3): its serial at most its date's x 100 + 99 and not below any version it names, checked for snapshots as well.
  **Once stored, a snapshot does not change:** a later fetch of the same name must have the stored serial (equal:
  taken again), otherwise it is refused, like an older channel list. A stored file that does not itself say NAME
  (damaged, or from before this amendment) is ignored, not used as the serial, so it cannot lock the snapshot out.
  To take a remade snapshot root uses 5.2's reset: `--reset-serial snapshot-NAME` (a repository never remakes one;
  this is only for a repository that broke that rule). **"Published once" holds only per repository:** one
  repository per release key, and a snapshot folder is never deleted (prune never does); a machine that already
  stored a snapshot is protected by the stored serial either way, a fresh one only by that rule.
  Expiry stays as decided (owner decision 3): 60 days for channel lists; for a snapshot it would only bound a
  list the owner asked for by name.
- **Said plainly:** a snapshot names a fixed set of files, so a snapshot of something with a later found hole stays
  installable by name; that is what a snapshot is for, and `-Syu` from the channel moves past it. A stolen release
  key can sign a false snapshot as it can sign anything (5.4).
- **A key change** (D21's next key): snapshots signed with a key zpkg no longer trusts stop verifying; the release
  process makes new snapshots under new names, it never re-signs an old name.
- **`mkrepo check`** refuses a snapshot directory whose list does not say its own name, a channel directory whose
  list says any snapshot, and names a `snapshots/NAME.part` left by a cut run (harmless, zpkg refuses its list, but
  rsync would carry it to the mirrors).

**What this changes elsewhere:** `zpkgindex 1`'s word list (frozen once built, growth only: one header word); the
trust struct grows a `snapshot` field and the `ZP_T_SNAPSHOT` flag is gone (the name is the mode, so the two cannot
disagree); a new refusal `ZP_ESNAPSHOT` (-1006). No other file format, no system call, no Amiga code outside
`user/zpkg/` (zpkg itself is not built on the Amiga yet). Snapshots made before this amendment (none were published)
would now be refused by zpkg and by `check`. A zpkg built before this amendment would ignore the word and could take
a snapshot's list as a channel's; none was built, and it would be no worse than its own channel's older list.

### As built (side helper, branch o1, 2026-10-28)

- `user/zpkg/index.c`, `zpkg.h`: the `snapshot` header word; `zp_snapshot_name_ok`; `struct zp_trust` gains
  `snapshot` (the last field) and loses `ZP_T_SNAPSHOT`; `ZP_ESNAPSHOT`; both checks (`zp_index_check`,
  `zp_index_usable`); for a snapshot the serial's shape is checked, its floor not, and a stored snapshot's serial must
  match; the test client ignores a stored snapshot file that does not say its name. m68k -Os -m68000: 5,624 -> 6,129 bytes of text (*measured*, +505).
- `tools/mkrepo.py`: `parse_index` and `index_text` know the word; `snapshot` signs (`--key`), refuses `test` and the
  test key; `check` as above.
- Tests (`tools/host/zpkg-test.sh`, all passing, *verified*): core 189 (snapshot binding both ways, another
  snapshot's list, the stored serial, a fresh machine's floor not applied, bad and good names); repo 63 (the
  snapshot's bytes are stable's plus one line, the C parser reads the name, test key and test channel refused, four
  `check` findings: a snapshot dir holding the plain channel list, a snapshot under another name, a cut run's
  `.part` folder, a channel dir holding a snapshot's list); differential on 3,000 damaged lists, now with snapshot seeds and words, the parsers
  also agreeing on the snapshot name; net 46, among them **the O1 swap: `--snapshot 2026.11` answered by an older
  signed `testing` list over HTTP is refused and nothing is stored**, another snapshot's list refused, a snapshot's
  list served as `stable`'s refused, a path as the name refused, a wrong stored snapshot file ignored; fuzz 30 s,
  nothing found.
- **For the main line, when zpkg is built (step 5):** pass `--snapshot NAME` as `zp_trust.snapshot` after
  `zp_snapshot_name_ok`, set `zp_trust.stored` from `sync/snapshot-NAME.index` when present and it says NAME (`zp_index_usable`
  not `ZP_ESNAPSHOT`), accept `--reset-serial snapshot-NAME`, and show
  `ZP_ESNAPSHOT`'s text; `docs/man` for zpkg says what `--snapshot` checks.

### Review (one independent read-only review, 2026-10-28)

The reviewer judged that the amendment closes O1, with no blockers. It checked the fetch path, the stored-list path,
a snapshot with no channel asked, latest skipped for snapshots and the line stripped, and it found that the swap test
really runs the attack. It also found that 21 of the lists both parsers accepted in the differential run carry the new
word, and that "no floor" is the right call. Its findings, all taken in above:
- a name ending in `.part` was valid but hidden from `check` and `prune` (now refused in both parsers and in the rule);
- a cut run's `.part` folder went unreported (`check` names it);
- a wrong stored snapshot file could lock the snapshot out (now ignored);
- sections 5.1-5.3 did not mention the exceptions (now one line each);
- the recovery was a new mechanism (now 5.2's `--reset-serial snapshot-NAME`);
- Python's `$` let a trailing newline through (now `\Z`);
- "published once" holds only per repository (now said);
- an old zpkg ignores the word (now said);
- two client paths had no test (now tested).


## As built: step 1 (main line, branch zpkg1, 2026-10-28)

- `user/zpkgfmt.c` + `.h`: zup.c's manifest and bundle parsers moved out unchanged (the same checks, the same
  messages through the program's `zf_say`), with the bundle reader split so a file can be streamed (step 4):
  `zf_bundle_head` (header and table, asks for more bytes while it has too few), `zf_bundle_entry` (one table entry,
  range-checked), `zf_part_kind` (a `pkg/` part's name), `zf_bundle_parse` (all of it in memory, as before);
  `zf_pkg_free`/`zf_bundle_free` (a bundle's package names now live in the bundle itself, so nothing leaks after a
  refusal). Built at -Os: `zup` text 42,806 -> 40,520 bytes, `/sbin/zup` 48,104 -> 45,228 (*measured*).
  `tools/zup-host-test.sh` unchanged in what it checks (48 passed). Fuzz target `fmt` in `tools/host/zpkg-fuzz.c`
  (manifests, and bundles read in pieces as `zup -f` will): 10 million inputs in 120 s, nothing found.
- `UPD_F_TRIAL` 2 claimed (CLAUDE.md; `kernel/kernel.h`, `user/usys.h`): the kernel keeps that one bit after
  `UPD_RELEASE`, so zpkg, which runs after boot gave the bundle back, still sees it. zenixboot sets it from step 7.

## As built: step 4, zup's live transactions (main line, branch zpkg1, 2026-10-28)

- `zup -f FILE [-d]` and `zup -r NAME [-n]` (man/zup.txt) in zup's one engine, every rule of 4.2: the lock (taken by
  zup itself with `O_EXCL`, or held by its parent; a dead holder's lock is stale only while no other zup lives, so a
  zup left running after its zpkg died is never taken for a cut one; made and removed under the "needs a check" mark;
  the boot run takes it only for a journal or a way back without a bundle in memory, since with a bundle nothing else
  runs); `live 1 NAME` before the operations, `live/`, no undo log, no result, no note, a commit and a recovery that
  never touch `prev-*`; versions (`-d`; the header's minimum against the smaller of the installed base and the
  running kernel); never the base group (group and name), never a package in the newest prev set's logs, never during
  a trial (`UPD_F_TRIAL`); paths owned by another package (directories shared, `takes` excepted), under the
  database or the cache, below a symbolic link; `takes` with the taken entry in the three-way compare and the taken
  manifest rewritten (or removed) in the same transaction; `.save` (`S`). The file is streamed: header and table,
  the manifest copied and parsed, the tgz's SHA-256 a piece at a time, then inflated from the file in 4 KB pieces.
  The change table and the installed-manifest table grow as needed: zup's bss 97,142 -> 19,690 bytes.
- **Choices where the note was silent (conservative):** (1) an existing file or link no package owns is a conflict
  (pacman's rule; 4.2 named only owned paths); (2) a removal is refused while any installed package depends on the
  package (zpkg removes dependents first), not only a base-group one; (3) an older `PATH.save` is never replaced: the
  removal is refused until it is moved; (4) two more live-only operations, `M` (a new directory, removed by a
  rollback) and `X` (a removed package's directory, removed if empty, put back by a rollback), so a rollback leaves
  no directory behind and a cut after the commit leaves none either; (5) the lock's parent rule uses the parent's pid
  from `SYS_procinfo` (not an argument); (6) a damaged installed manifest stops every live change ("the package
  database cannot be read"); the boot run keeps D15's behaviour; (7) a base-group package with `takes` is refused in
  a system bundle too; (8) zup -f and -r refuse to run on a kernel without `SYS_update` and on a root without
  `/var/lib/zpkg` ("no package database here").
- **Found and fixed on the way (kernel/ext2.c):** `mkdir` wrote the new directory (with its `..`) before raising the
  parent's count; a cut in between, then an rmdir of the parent before `fsck` (a live rollback removing the directory
  it made), left that `..` naming a free inode, the kind of damage F5 rules out. The count is raised first now.
- Sizes (*measured*): `zup` text 40,520 -> 47,288 bytes at -Os (zup.c is cold code: -Os now, it was -O2), bss
  97,142 -> 19,690; `/sbin/zup` 45,228 -> 52,988 bytes; the kernel +40 bytes.
- Tests (`tools/zup-host-test.sh`, all passing, *verified*): D15's 48 unchanged, plus 44: the base-only system
  update, a first install, "installed already", an upgrade with a kept config, older refused and `-d`, 13 refusals of
  `-f` and 4 of `-r` each checked to change nothing (root, `prev-V` and the lock), a removal refused for a dependent,
  the lock held by a living zup, a stale lock with another zup alive, the parent's lock, a stale lock taken over,
  `takes` (part of a package, the rest of it, a config file), `-r` with `.save`, `-n`, an older `.save`; a power cut
  before each change of four live operations (`-f` new, `-f` upgrade with a kept config, a `takes` hand-over, `-r`
  with `.save`), the commit line included, then the next start: old or new exactly, `prev-V` byte for byte; "Zenix
  (previous)" after live transactions; on the real ext2 server a cut after each block write of `-f` (506 cuts) and
  `-r` (378): at worst leaks, marked, `prev-V` untouched.
- **The base-only system update and the split (8.4.4, the rest of step 4):** `make bundle` now passes `--base-only`
  with the base group (`zup base`; zpkg joins in step 5): the system bundle is 400,784 bytes (was 932,864). `make
  packages` makes the families of `tools/mkzup.py` FAMILIES (26: coreutils, findutils, archive, grep, sed, awk,
  textutils, diffutils, hashutils, shellutils, sysinfo, procps, sysutils, pager, edit, shadow, disktools, cu, zmodem,
  nettools, telnet, ssh, wget, sntp, zenix-tests; each with a `desc`, its programs, links and pages, `depends base`,
  `takes utils` and `takes man`) and base-man (`takes man`); a program in no family stops the build; packages an
  older build left in `build/pkg` are removed. `tools/update-test.sh` builds its own test bundles with zup, base and
  zenix-tests (the D15 path itself still takes any package; 16 packages a bundle is zup's limit). Tested on the host
  (section 2c of `tools/zup-host-test.sh`): this build packed as the D15 bundles shipped it (zup, base, utils, man)
  installed on an empty root, a family refused while the prev set holds utils, the base-only update, then the 26
  families and base-man by `zup -f`: utils and man gone, every file as before, the prev set untouched.
  **Choices (conservative):** `boot` and `diskd` go to zenix-tests (the kernel carries its own copies; the files on
  the disk are not what runs); the man program goes with less (pager); a page that is no program's goes to base-man.

### Reviews of step 4 (two independent read-only reviews, 2026-10-28) and what changed

*Correctness and power cuts:* (blocker) the boot run read base's manifest into zup's table before `recover()`, so a
recovery that put base back left a stale version for the install (it said "installed already"): the table is now
emptied after `recover()`, and a new host test cuts before each change and then starts straight away with the
bundle. (should fix) the lock's marks could clear the "needs a check" mark under another zup's transaction: the
lock file's writes no longer touch zup's mark (one request each, which fsd's own clean mark covers; only the holder
marks); the pid was written after the `O_EXCL` create, so a reader could see an empty lock: now `lock.PID` is written
and linked to `lock`; a stale lock is taken over only while no other zup or zpkg lives; a boot run that cannot take
the lock for another reason than a living holder goes on (nothing else runs at boot); `zup -f`/`-r` refuse to
finish a system update's journal ("restart first": its result is boot's). (minor) mkdir's failure path now frees
the new directory before lowering the parent's count; `X`'s undo sets owner and mode again (a cut between) and
records the directory's own mode; old link targets are read from the disk for `u` (as for `L`) and must be
journal-safe words; `takes` keeps the taken package's directory lines; `M` operations come first in the plan; a
damaged live journal keeps its previous files in `live.damaged`.
*Security and refusals:* (blocker) a package could name `PATH.zup-new` (a link of its own) and aim zup's writes,
`chmod` and `chown` through it at a base file: the names `.zup-new`, `.new` and `.save` are refused in every manifest
(user/zpkgfmt.c, and mkzup), every `.zup-new` is made anew with `O_EXCL` after an unlink, and only a regular file of
zup's own is chowned, chmodded or renamed. (should fix) a path listed twice, a link target `-` or with other
characters than mkzup's, more than 2,048 entries: refused by the parser; a tar member must have its manifest's size
and the archive may not inflate past its manifest's sizes; a package whose installed manifest says `group base` is
refused too; a sibling zup under the same parent is refused; a downgrade (`-d`) is refused while an installed
package needs the newer version, and a `takes` that empties a package something needs; the larger of the header's
minimum and `depends base` is checked against the effective base. (minor) a path that is an ancestor of the database
or the cache, or on another mount, is refused, and removals get the symbolic-link check too; SIGQUIT and SIGTSTP are
ignored as well; a system bundle that would take a path an installed package outside it owns is refused (the boot
run). Not changed: `SYS_procinfo` carries no owner, so a user's program named zup or zpkg can make zup wait (never
go ahead); owners and set-user-id bits are the repository's policy (mkrepo), not zup's.
All tests pass after the fixes (`tools/zup-host-test.sh` 107, `tools/ext2-host-test.sh` 82).

## As built: step 5, zpkg (main line, branch zpkg1, 2026-10-28)

- `user/zpkg.c` (`/bin/zpkg`, man/zpkg.txt) over the cores of steps 2-3, `user/zpkgfmt.c` and Ed25519: `-Sy`,
  `-Syu`, `-Syuu`, `-S`, `-Sw`, `-Ss`, `-Si`, `-Sc`, `-Q`, `-Qi`, `-Ql`, `-Qo`, `-Qu`, `-R`/`-Rs`/`-Rn`, `-U`,
  `--back`, `--snapshot`, `--ignore-expiry`, `--reset-serial`, `--ram`; plain HTTP with time limits (connect 15 s,
  silence 60 s, a total per file of 2 min plus 1 s a KB), keep-alive, redirects (never to https), resume from
  `.part`; the order of 4.3 (resolve; download and check everything; the manifests' dependencies and `takes` against
  the list, file conflicts across the set; then one `zup -f` or `zup -r` per package); the deps file; the lock as
  zup's (linked whole, a stale holder only while no other zup or zpkg lives); the running programs of an updated
  package named; a newer system downloaded and checked, and the owner told to copy it to BOOT: until the slot comes
  (step 7). The package `zpkg` joins the base group (`/bin/zpkg`, `/etc/zpkg.conf` as a config file naming no server,
  D22 7.6); `make bundle` carries zup, base and zpkg. The trusted keys are built in (`tools/zsign.py cheader`: the
  TEST key in a test build, `RELEASE=1` the release keys of `tools/keys/`).
- **Choices (conservative):** `-U` copies the file and its `.sig` into the root-only cache before it checks them
  (nobody can change the file between the check and zup) and takes only a whole path; `--back` looks in the cache
  only (the list names no older versions; the pool fallback of 7.5 needs a list word, left open); zpkg asks for 160
  KB free before a change; the kernel's own processes (boot, diskd) are not named as running old versions.
- **Found and fixed on the way:** the resolver never set its depth counter (`user/zpkg/resolve.c`, the S21 guard):
  on the 68020 the first `-S` said "more than 64 deep"; kernel/ext2.c wrote a deleted inode's dtime from a clock
  never set (near 1970), below the inode count, which e2fsck takes for an orphan-list link: now never below it.
- Sizes (*measured*): `/bin/zpkg` 89,936 bytes (text 82,678, bss 33,520: Ed25519 and SHA-512 about 20 KB, the HTTP
  core, the list parser and resolver, libz's printf); `/sbin/zup` 56,920 (text 50,734); the system bundle 453,504
  bytes (the kernel, zup, base, zpkg). The RAM floppy's kernel: 159,924 of 179,200 bytes (19,276 left; 2,892 on the
  mount tests' floppy).
- **Memory (FS-UAE, *measured*, not hardware):** the stock A1200 (2 MB chip): 1,014 KB free after the run, about 540
  KB free while zpkg and zup install one package; over SLIP 598 KB. **The 1 MB chip-only machine cannot run zpkg with
  the network up:** with SLIP up and one login about 104 KB are free (netd's pools take the rest), so zpkg refuses
  with its memory message and changes nothing (`zpkg-emu-test.sh m` checks exactly that). An owner's question
  (below in the hand-back), not decided here.
- Tests (`tools/zpkg-emu-test.sh`, *verified* in FS-UAE): `e` the A2065, 22 checks (every operation above, a replay
  skipped, a damaged mirror refused and the next used, a cut download resumed, an expired list and
  `--ignore-expiry`, `-U` with a bad `.sig`, the refusals, e2fsck after); `s` SLIP (-Sy, -S with a dependency, -Rns,
  e2fsck); `m` the 1 MB machine (above). In `tools/check.sh` as `zpkg-emu-test` (parts e|s|m). Not yet: the emulator
  killed during a live install (the host tests cut before every change and after every block write instead).
