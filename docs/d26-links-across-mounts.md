# D26. Symbolic links across mounts (audit 2026-10-26 F12)

Revision 2, 2026-10-28, after four independent reviews (section 13). A fix, not a feature: a symbolic link into
another mount answers ENOENT today; Linux follows it. D25 asked for it (the demo floppy's links from the RAM root into
`/usr`, d25-demo-floppy.md 7.3), but every Zenix that mounts anything needs it. **Built on branch d26, 2026-10-28:
section 14 says how, and what differs.** Statements are *read* from the source at main 77d43f2 unless they say
*measured*.

## 1. The problem

The kernel picks a mount from a path's **text**. The filesystem server follows symbolic links **inside its own
volume** (`ext2_namei_ex`, `kernel/ext2.c` 567-617; zfsd's `namei`, `user/zenixfsd.c` 48-86; nfscd's walk,
`net/nfs/ops.c` 85-240). So once a second filesystem is mounted:

- a link on the root to `/usr/bin/ls` finds the root's own empty `/usr` directory and answers ENOENT;
- a link on `/usr` with an absolute target, `/usr/lib/x`, is resolved from `/usr`'s own root (`/usr/usr/lib/x`);
- a relative link that climbs out of a volume (`../etc/motd` at `/usr`'s root) stays at that volume's root;
- a relative link on the root that passes a mount point (`/bin/ls -> ../usr/bin/ls`) walks the root's mount point
  directory, not the mounted volume.

## 2. How a path is looked up today (end to end)

1. A system call (`open`, `stat`, `chdir`, `unlink`, ...) reaches `kernel/vfs.c` with the program's path. The path
   is copied into the system call's own 64-byte buffer (`syscall.c` 253, 259, 266, 421).
2. `route()` (vfs.c 292-313) handles two cases:
   - **Only the root is mounted** (`nmnt <= 1`): the path goes to the root's server **untouched**, in that 64-byte
     buffer. It may be relative, and the working directory's inode goes with it.
   - **Something else is mounted:** `path_abs` makes the path absolute and normalised as text. It joins the working
     directory's text and the path and removes `.` and `..`. The result is at most `FS_PATHMAX - 1` (159 bytes then, 255 since 2026-10-31, architecture.md section 7) bytes;
     longer is -ENAMETOOLONG. The longest mount point that prefixes the text wins. Its server gets the rest, which
     always starts with `/` and never holds a `..`, in the caller's `FS_PATHMAX` buffer, with that volume's root
     inode as the working directory.
3. `fs_call_on` sends one message to that server, through `ipc_kcall`, with `kflags = UMSG_KERNEL` and the caller's
   uid, gid and umask. The server walks the path itself:
   - Every directory searched needs x.
   - A link met in the middle of the path, or at the end when the request follows its last name, is read and
     resolved recursively from the link's directory. An absolute target is resolved from **its volume's** root.
   - More than 8 nested links is -ELOOP. Links in a row are not counted together.
   - Calls that create or remove, and the two-path calls, walk the parent directory (`ext2_namei_parent`, which
     copies the path and cuts its last name). `FS_LSTAT` and `FS_READLINK` do not follow the last name.
     `FS_LINK` follows its existing name's last link.
   - fsd refuses requests that change something with -EROFS on a read-only volume, and sets its clean mark,
     **before** the walk (fsd.c 367-370).
4. The answer goes back, with some side steps:
   - `vfs_chdir` stores the **logical** text as the working directory's text.
   - `vfs_open` tries `/dev/...` names first (`open_device`).
   - Several entry points make two calls on the same buffer: `vfs_open`'s `FS_STAT` before an `O_TRUNC` open;
     `name_ino`'s `FS_LSTAT` before an unlink, a rename, a link or a symlink.
   - D13's checks (`exec_on`, `exec_busy`, `name_ino`) key what is loaded by (mount, inode) of the **textual** mount.
   - `mount` and `umount` work on the text (vfs.c 850-920).
5. `fsproto.h` has no answer but "a value or -errno". No interface knows about links.

## 3. The choice: the kernel follows every link

**When a request asks for it, a server does not follow a symbolic link at all. It stops at the first one it meets and
answers "this path continues at TEXT". The kernel normalises TEXT, picks the mount again, and sends the same request
again from there.** At most 40 links per path (Linux's `MAXSYMLINKS`), then -ELOOP.

**Why every link,** and not only the ones that leave the volume (D25 7.3's first idea):
- A server does not know where the mount points are, so it cannot tell that a relative link passes one (case 4 of
  section 1).
- The kernel knows the mount table. Its text is free of links up to the link, because the server stopped at the first
  one. So the kernel's textual `..` is the physical `..` there. At a mount point it is Linux's: `..` of a mounted root
  is the mount point's parent.
- The server needs no "above my root" test and no recursion.

**When it asks** (the reviews' blocker): the kernel sets the bit **only on a request whose path argument is in a
kernel `FS_PATHMAX` buffer holding an absolute, normalised path**. That is the case whenever `route` copied the path
(`nmnt > 1`), and always for `chdir` and `mount` (section 5.6). It never depends on `nmnt` at send time: with only the
root mounted, a request may carry the program's own 64-byte buffer.

**Weighed and not taken:**
- The server answers only for absolute targets and `..` above its root: misses case 4 and nested mounts.
- The kernel tells each server its mount points: more state, kept in step with every mount, to save one round trip per
  link.
- The kernel walks every path name by name, as Linux's VFS does: a lookup message per name; far too slow for the stock
  A1200.
- Only for the root's server: a link on `/usr` to `/etc` would stay wrong.
- Always copying every path, with only the root mounted too: this would make the answers identical in both cases
  (section 6, last bullet). But it costs every call a normalisation and every link a round trip on systems that mount
  nothing, and it changes `..` with only the root mounted from physical to textual. Not taken.

## 4. The protocol (append-only)

- **`UMSG_LINKS` 8**, a bit of `struct umsg`'s `kflags`. The other bits: 1 `UMSG_KERNEL`, 2 `UMSG_NETREPLY`
  (internal), 4 D23's `UMSG_CANCEL`. It means "the sender takes the `FS_LINKED` answer, and its path arguments are
  kernel `FS_PATHMAX` buffers holding absolute, normalised paths". Only the kernel sets it. `call()` takes the kflags
  as an argument and ORs `UMSG_CANCEL` into them, so the two never clash. A program's `SYS_call` never carries it.
  The bit is defined in a header both the kernel and the servers include (`kernel/fsproto.h`, mirrored in
  `user/usys.h`).
- **The answer `FS_LINKED`:** the result is `-(FS_LINKED + bits)`, with `FS_LINKED` 1024.
  - `FSL_A1` 1: the path in `a[1]` was rewritten; otherwise the one in `a[0]`.
  - `FSL_LAST` 2: the link was the last name of the request's **original** path. It is meaningful only for `FS_OPEN`
    and ignored for the others.
  - Used values: -1024 .. -1027. **Reserved: -1024 .. -1279**, with a comment in `kernel/errno.h` that no errno goes
    there.
  - Whether TEXT is absolute is TEXT's first byte, `/`. A relative TEXT never starts with `/`.
- **Which argument may be rewritten** is a table, the same in fsproto.h, the servers and the kernel's helper:

  | Request | Path arguments | `FS_LINKED` allowed |
  |---|---|---|
  | `FS_OPEN`, `FS_STAT`, `FS_LSTAT`, `FS_CHDIR`, `FS_MKDIR`, `FS_UNLINK`, `FS_RMDIR`, `FS_CHMOD`, `FS_CHOWN`, `FS_UTIMES`, `FS_READLINK` | a0 | a0 only (`FSL_LAST` only with `FS_OPEN`) |
  | `FS_RENAME`, `FS_LINK` | a0, a1 | a0 or a1 (`FSL_A1`) |
  | `FS_SYMLINK` | a1 (a0 is the target text, the program's 64-byte buffer) | a1 only: `FSL_A1` must be set |
  | every other request | none | never |

  Anything outside the table is -EIO.
- **`fs_call_on` itself** turns any value in the reserved range into -EIO when the call did not carry the bit. So
  `FS_READ`, `FS_FSTAT`, `FS_READDIR` and every direct call can never pass -1024.. to a program. Only the helper
  (section 5) calls with the bit and handles the answer.
- **TEXT** is NUL-terminated, written into that argument's buffer, at most `FS_PATHMAX` bytes; a longer continuation
  is -ENAMETOOLONG from the server. The server builds TEXT in a scratch buffer of its own, then copies it over, so
  input and output never overlap. It builds it from the request's **original** path P, never from a copy: the parent
  walks work on a copy with the last name cut.
  - The walk records where the link's name starts (S) and ends (E) in P, and reads the target T.
  - **Absolute T:** TEXT = `T` + `P[E..]`.
  - **Relative T:** TEXT = `P[1..S)` + `T` + `P[E..]`, that is, the link's directory inside the volume without its
    leading `/`.
  - The server does not normalise TEXT.
  - An empty target is -ENOENT. A target with a NUL inside is -EIO; nfscd already does both (ops.c 229-230).
  - A target longer than 255 bytes is -ENAMETOOLONG. Today it is silently cut (audit F15, the main line fixes it).
  - The server answers `FS_LINKED` only when the bit is set **and** P starts with `/`.
- **When the server answers it:** in every request of the table, at the first link its walk would follow. That is
  always in the middle of the path. At the end it is only when the request follows its last name: `FS_OPEN`,
  `FS_STAT`, `FS_CHDIR`, `FS_CHMOD`, `FS_CHOWN`, `FS_UTIMES`, and `FS_LINK`'s existing name.
  - **The decision comes before anything else** in the request. With the bit set, fsd walks first and only then
    applies the read-only refusal and its clean mark (fsd.c 367-370). So a `chmod` through a link from D25's
    read-only `/usr` into the RAM root works, and a bounced request writes nothing.
  - zfsd and nfscd walk first already.
  - The handlers' mapping of "a negative answer after a permission failure" to -EACCES (fsd.c 375, zenixfsd.c 374)
    leaves the `FS_LINKED` range alone. One parent walk may have set the "denied" flag before another answers
    `FS_LINKED`, as in `rename`'s two walks.
- **nfscd answers asynchronously** (D23). It writes TEXT into the client's buffer only at reply time, never after a
  cancel or a kill (D23's rule that the link to the message is cut at the reply). Its bound is inside nfscd.
- Message numbers and argument meanings are unchanged. The answer is new only for a sender that asked.

## 5. The kernel's part (`kernel/vfs.c`)

**A walk state and one helper.** Each path argument of a system call gets a small state on the caller's stack:
- the mount `m`;
- that mount's **instance id** `inst`, a new number on every `vfs_mount`, unlike the generation `gen`, which also
  changes when a server restarts;
- the inode to give as working directory (`cwd`);
- the buffer with the server's path (`rel`);
- a link counter, **local** to this state, so nested service loads (D11's `svc_depth`) never share it.

The helper takes the working directory's **text and inode as arguments**, because `fs_exec_open` uses the parent's.

1. **Resolve once.** `walk_start` routes as today and fills the state. `walk_call(state, request)` sends the request
   with the bit when the state's buffer is a kernel copy (section 3). On an `FS_LINKED` answer it does the following:
   1. If `current->killed`, it stops with -EINTR.
   2. It checks the table of section 4 for the request and the bits; anything else is -EIO.
   3. It counts one link; more than 40 in this state is -ELOOP.
   4. It forces a NUL at the buffer's end, because a server's text is untrusted.
   5. It checks that `mnt[m]` is still used with the same `inst`. If not, the answer is -EIO: an `umount` may have run
      while the client waited (it does not count an answered request).
   6. It normalises: `path_abs(tmp, "/", TEXT)` for an absolute TEXT, else `path_abs(tmp, mnt[m].path, TEXT)`.
      This is the helper's own normalisation, independent of `nmnt`, so a mount count that dropped meanwhile cannot
      turn TEXT into a relative path with the caller's inode.
   7. It picks the mount again, and stores the new `m`, `inst`, `cwd` and `rel` in the state.
   8. It sends the same request again.
2. **Two calls on one path use the same state.** The first call resolves the path. The second starts from the
   resolved state: the `FS_STAT` of an `O_TRUNC` open and then the `FS_OPEN`; `name_ino`'s `FS_LSTAT` and then the
   unlink, rename, link or symlink. If the second call still answers `FS_LINKED` (a link changed meanwhile), both are
   redone from that point, counting on the same counter. So both calls reach the same file, and the 40 is per path,
   not per system call.
3. **D13's checks move after the resolution.** `name_ino`'s gate `exec_on(m)` and `vfs_open`'s ETXTBSY pre-check run
   on the resolved mount. Then `rm /bin/L/prog` unlists the segment of the mount that holds `prog`, as audit3 B2 asks.
   For an unlink, a rename over a name, a link or a symlink, the gate becomes "anything loaded on any mount"
   (`seg_on` of any mount, or `loading` not empty). Only then does `name_ino` resolve with `FS_LSTAT`. Otherwise the
   request resolves itself.
4. **Two-path calls** (`rename`, `link`): each argument has its own state. A rewritten `a1` (`FSL_A1`) moves only
   that one. If the two then differ in mount, the answer is -EXDEV.
5. **`open`.** After an `FSL_LAST` answer, **`O_CREAT` is dropped for every later restart of this system call**.
   `O_EXCL` never gets there: the server answers -EEXIST for any existing name, a link included, before following
   it. So a link to nothing stays -ENOENT and a link to a file opens it: today's rule, not Linux's (Linux creates the
   target). Before each restart of an `open`, a normalised text that starts with `/dev/` tries `open_device` first.
6. **The resolved text; `chdir` and `mount` always copy.**
   - `chdir` always normalises into its kernel buffer and sets the bit, with only the root mounted too. It stores the
     resolved, link-free text as the working directory's text: Linux's `getcwd`. `cd /bin` where `/bin -> /usr/bin`
     shows `/usr/bin`. A text over 159 bytes is -ENAMETOOLONG; the text is kept in `cwd[FS_PATHMAX]`. Afterwards a
     relative `..` is the physical one, and `umount`'s busy check, which reads working directory texts, sees a working
     directory reached through a link.
   - `mount` resolves its mount point the same way (the mount point must be a directory, so its last link is
     followed). The refusal of `/` (`abs[1] == 0`), the 47-byte limit and the "already mounted
     there" check all run on the **resolved** text. A link to `/` is -EBUSY, as `/` is.
7. **`umount` asks no server first.** An exact match of the normalised text against the mount table comes first and
   calls no server, so `umount` of a hung NFS mount or a stopped mount service works as today. Only when nothing
   matches does it resolve the **directory part** (`FS_STAT` of the parent, following links) and join the last name,
   with lstat semantics on the last name. It never walks into the mount it removes. So `umount` works by both names
   after a mount through a link.
8. **Kernel stack (4 KB per process).** A state is about 16 bytes; the normalisation uses `route`'s `tmp`. The deepest
   case is `vfs_rename`: two states, its two buffers, `route`'s `tmp` and the message. Step 3 measures the stack use
   with the existing stack guard. It is not assumed.
9. `ipc_kcall` gets a kflags argument. That changes `kernel.h` 570 (main's) and the same lines D23 changes. Their
   shape is agreed with D23 in step 0.

## 6. What it does, next to Linux

- **Followed:** every link, absolute or relative, into any mount and back, as in Linux. Absolute targets are resolved
  from Zenix's `/`. Today they are resolved from the volume's root, which is wrong on every mount but the root.
- **`..`:** at a mount point, `..` leads to the mount point's parent (Linux's). One deviation stays, as today: a `..`
  written **directly after a link in the same path** is taken on the text before the link is followed. With `/bin ->
  /usr/bin`, `/bin/../etc` is `/etc`; Linux gives `/usr/etc`. The same holds for a `..` after a link inside a link's
  own target. Since 5.6, the working directory adds nothing to this. A test pins it.
- **The last name:** `lstat`, `readlink`, `unlink`, `rmdir`, `rename`, `symlink`'s new name and `O_CREAT|O_EXCL` never
  follow it, as today. Links in the middle of their paths are followed across mounts.
  - Zenix has no `O_NOFOLLOW`, and none is added (feature halt).
  - Kept differences from Linux: `link` follows its existing name's last link (Linux does not; across mounts -EXDEV).
    `path_abs` drops a trailing slash, so `lstat L/` and `rm L/` do not follow `L`. `O_CREAT` through a link to a
    directory opens the directory (Linux: -EISDIR).
- **`rename` and `link` across textual mounts:** -EXDEV is decided by the textual mounts when they differ. `mv
  /usr/bin/a /bin/b` with `/bin -> /usr/bin` gives -EXDEV, although both resolve to `/usr`, and `mv` copies. This is
  named as a deviation and tested both ways.
- **Permissions:**
  - The first server checks search (x) on every directory up to the link before it reads the link; the link's own
    mode does not matter, as in Linux.
  - Each restart is a whole new walk with the caller's ids, which `call()` sets for every message, so the target's
    directories are checked by the server that holds them.
  - **Search permission above a mount point** is not checked today: no server walks the mount point's directories
    above it. That is audit F13, which the main line fixes in `route`. D26 depends on it for a link into a mount
    below a directory without x. That test is written once F13 is in.
- **Limits:** 40 links per path argument (Linux's). A continuation over 159 bytes is -ENAMETOOLONG (the one path
  limit), also where a long target inside one volume works today. `realpath(1)` follows 8 links (realpath.c 24); it is
  aligned to 40 in the build. With only the root mounted, the server's own rule stays: 8 nested.
- **Links changed meanwhile:** another process may change a link while a walk waits for a server. Each restart reads
  the text as it is then. The result is a path the caller could have named, checked with the caller's ids, as in
  Linux, which gives no atomic walk either. A loop made on the fly ends at the 40th link. Two calls on one path stay
  on one resolution (5.2), so a check and its action cannot split across two files.
- **A new reach (security):** a link on one volume can now point at another volume's files. A hostile removable
  volume, or a NAS export, can point root, or a set-user-id program, at local files: `/mnt/stick/x ->
  /etc/shadow`. The permission checks are the caller's, as above, so this gives nobody rights they lack. But a root
  program that trusts a volume's names can be steered.
  - Named for D23: NFS mounts are already `nosuid`.
  - Proposed as a later item, not part of this fix: a per-mount "links stay inside" option, Linux's `nosymfollow`, for
    NFS and removable volumes.
  - Linux's `protected_symlinks` (no following of a stranger's link in a sticky, world-writable directory) is not
    done. The following inside one volume today lacks it too. `mktemp` and here-documents use `O_EXCL`
    (architecture.md 1555). It is also a candidate for the ideas list.
- **Root only versus mounts:** with only the root mounted, everything but `chdir` and `mount` goes to the root's
  server untouched. Its links are followed physically inside the volume, as today. So a few answers depend on whether
  anything is mounted: a link to `/dev/null` works only when something is mounted (the root volume has no device
  nodes); `..` after a link is physical with only the root, textual with mounts. Said in architecture.md section 9.

## 7. Old and new together (the frozen-interface rule)

- **New kernel, old server:** for example an older zfsd after `zpkg --back`, or an old nfscd. The server ignores the
  bit and follows links inside its volume: today's answers, never worse.
- **Old kernel, new server:** for example D15's fallback to the previous kernel. The bit is never set, and the server
  follows links itself, as today. So **fsd, zfsd and nfscd all keep their own link following** for requests without
  the bit.
- **Raw requests by root programs:** no bit, no change. A root test program that answers `FS_LINKED` without being
  asked reaches no program: `fs_call_on` makes it -EIO (4).

## 8. What else assumed that links stay on one volume

- **architecture.md section 9** (the caveat at 919) and 999 (the two servers' answers): rewritten in the same commits
  as the code (section 11).
- **D23's nfscd:** its core is on main (`net/nfs/ops.c` 85-240, its own 8-deep walk from the export's root). It
  splices targets into `w->buf`, so S and E must be recorded before the first splice. With the bit it answers
  `FS_LINKED` like fsd: absolute targets from Zenix's `/`, as a Linux NFS client resolves them. Without the bit it
  keeps its walk. The build changes `ops.c`, its host tests and `nfs-fuzz`. D23's `OPEN` row is amended in the same
  commit.
- **D19 6.5** (netd calls only the root's server): D25 ships `/sbin/netd -> /usr/sbin/netd` and `/etc/ssl/anchors ->
  /usr/share/...`, so "no link from netd's files into a mount" would be wrong. The real hazard is only a mount whose
  server calls `net` (NFS): netd would wait for nfscd, which waits for netd. The sentence added to D19 6.5 and to
  `mount`'s page: "netd's files may lead through links into a disk mount, never into a network mount". D23 already
  refuses NFS mounts on `/`, `/bin`, `/sbin` and `/etc`, but a link can still lead there, and it is root's to keep.
- **D11** (4 and 5.1: "a path in `/etc/svctab` is read from the root filesystem, never from a mount"): no longer true
  once a service program may be a link into `/usr`. Amended in the build. A server program found through a link into
  the mount it serves would make its loader wait for itself. **That is audit F14** (a load in `SV_LOADING` can wait
  for itself; D11's start deadline covers only `SV_STARTING`), fixed by the main line. D26 relies on that fix, not on
  the deadline.
- **D25:** 7.3 points here instead of its own sketch.
  - Its `net` service starts from the demo's own `/etc/svctab` line: boot's `sys_stat("/sbin/netd")` runs before init
    mounts `/usr`, so it never sees the link.
  - Its every command is `/bin/NAME -> /usr/bin/NAME`, which costs one extra round trip per start and per `PATH`
    lookup through `/bin`. The demo's `PATH` puts `/usr/bin` first.
- **sh:** `cwd_now()`'s buffer is 128 bytes (sh.c 2236), and resolved texts can be up to 159. Fixed in the build
  (fix when found) to `FS_PATHMAX`.
- **`realpath(1)`:** its own 8-link limit is aligned to 40 (6).
- Checked and unchanged:
  - `tools/mkroot.sh`'s links are relative and inside the root. They cost one extra round trip each while something
    is mounted. There are 9 names: `more`, `gunzip`, `zcat`, `chgrp`, `pkill`, `bootamigaos`, `ifup`, `ifdown`, `sz`.
  - `fsck.ext2` and the host tools work offline.
  - `cp`, `tar`, `find` and `ls -l` use `lstat` and `readlink`.
  - The D13 segment keys are by the resolved mount (5.3).

## 9. Size and speed

**Measured on main 77d43f2 (`make -s all`, 2026-10-28):**
- `kernel.bin` is 155,944 of 179,200 bytes: **23,256 left on the RAM floppy, 6,872 on the mount tests' floppy**.
- `fsd.zx` is 28,676 bytes with its relocations (25,228 of image), and it counts **inside** `kernel.bin`.
- `zfsd.zx` is 36,764 bytes (33,564 of image). It is a file on the mount tests' floppy's RAM disk: a separate budget,
  82 of 720 blocks free.

*Estimates* at -Os:
- the kernel's states, helper, table, instance id, `/dev/` check, resolved texts, `umount`'s two ways and the
  `fs_call_on` guard: 400-800 bytes;
- fsd's walk recording S, E and T, TEXT, the order before EROFS, and the table: 200-400 bytes;
- zfsd the same, from its own budget.

So `kernel.bin` grows **under about 1.2 KB of the 6,872**. Other planned growth against the same budget: D23's cancel
mark, about 150 bytes. The step measures the growth and says so.

Speed: nothing changes with only the root mounted, except `chdir`, which now normalises. With mounts, every link
followed costs one more round trip to a server, plus a new walk of the text before it (cached blocks). This is not
measured. The build times `ls` through a link against a direct path on the A1200, stock and 060, when the owner is
home. The target: a link adds less than one tenth to a command's start on the stock A1200.

## 10. Tests (each fails today unless it says so)

**Where the tests go:**
- **The ext2-mount cases go into `tools/tests/mount-test.txt`** (`tools/mounttest.sh`).
- **The ZenixFS cases, and ext2 next to ZenixFS, go into `tools/zenixfstest.sh` / `tools/tests/zenixfs-test.txt`**,
  with ext2 on `/ext` and ZenixFS on `/mnt`. mount-test's only ZenixFS line is a negative one.
- About 24 new cases come near `run-test.sh`'s 230 s limit. If they pass it, a second script runs on the same image.

**The cases, each on both kinds of volume where it applies:**
1. A link on the root to a file on the mount: `cat` reads it; `ls -l` and `readlink` show the link.
2. A link on the mount with an absolute target on the root (`/etc/motd`); one with an absolute target on the same
   mount.
3. A relative link at the mount's root climbing out (`../etc/motd`); one on the root passing the mount point
   (`../mnt/f` from `/tmp`).
4. A directory link into the mount:
   - `cat L/f`; `cd L`; `pwd` shows the resolved text; `cd ..; pwd` shows the mount's parent;
   - `umount` is -EBUSY while a shell is there;
   - a `chdir` through a link made **before** the first mount, and its `umount` check.
5. Creating through it:
   - `echo x > L/new` lands on the mount;
   - `O_CREAT` on a link to nothing on another mount is -ENOENT;
   - `O_CREAT|O_EXCL` on any link is -EEXIST (passes today, kept);
   - a 25-link chain with `O_TRUNC`.
6. `rm L` removes the link; `mv` and `ln` through a link onto another mount are -EXDEV; `mv` both ways between textual
   mounts that resolve to one (the deviation of 6).
7. Loops:
   - two links pointing at each other across mounts: -ELOOP;
   - a chain of 40 across the two mounts works, 41 is -ELOOP;
   - an `O_TRUNC` open and an `unlink` (two calls on one path) of a 40-chain still work.
8. Permissions: a user cannot read a link in a directory without x. A link into a mount below a directory without x
   gives -EACCES (after F13).
9. Shared programs and the D13 checks:
   - a program run through a link into the mount, twice (D13 shares it); `umount` while it runs is -EBUSY;
   - unlink, rename over it and `O_TRUNC` through a link while a shared program from another mount runs: -ETXTBSY
     where due, the segment unlisted on the right mount.
10. A link to `/dev/null` on the mount: writing to it works. With only the root mounted it is unchanged (6).
11. A continuation over 159 bytes is -ENAMETOOLONG; a target of 256 bytes is -ENAMETOOLONG; an empty target is
    -ENOENT.
12. `mount` on a path through a link:
    - `mount -l` shows the resolved mount point, and `umount` works by both names;
    - a link to `/` is -EBUSY; a link to an existing mount point is -EBUSY; a resolved text over 47 bytes is
      -ENAMETOOLONG.
13. `umount` of a stopped mount service, and of a server that does not answer, by its exact text (no server call).
14. `chmod`, `mkdir` and `rm` through a link from a read-only ext2 mount into a writable one work, and nothing is
    written to the read-only one.
15. The `/bin/../etc` deviation is pinned.
16. **A ZenixFS root (kernel-zfs) with an ext2 mount:** cases 1-3.
17. **D25's shape:** svctab's `net` -> `/sbin/netd`, a link into `/usr`, starts netd (after F14).
18. **Only the root mounted:** the whole existing FS-UAE suite is unchanged byte for byte (`make check`), apart from
    `pwd` after a `cd` through a link.

**Host tests** (`tools/ext2-host-test.sh`, `zenixfs/tests`, nfscd's host tests and `nfs-fuzz`):
- with and without the bit, for each request in the table: TEXT, the bits, S and E when the parent walk cuts the last
  name;
- a 255-byte target, a 256-byte one, an empty one, one with a NUL, a TEXT too long;
- nothing changed on the volume before the answer, and no EROFS before it;
- `FS_SYMLINK` never rewrites a0;
- the -EACCES mapping leaves `FS_LINKED` alone;
- without the bit, or with a relative P, today's answers byte for byte;
- an **old zfsd binary** against the new kernel: today's answers.

## 11. Build order

Each step goes on one branch with its own documents in the same commit (CLAUDE.md), and the full suite runs before
the merge. It is a kernel change, so these reviews count as its two.

0. **Main line, the shared files:**
   - the claims in CLAUDE.md: a kflags row ("1 KERNEL, 2 NETREPLY internal, 4 CANCEL (D23), 8 LINKS (D26)") and an
     fsproto answers row ("-1024 .. -1279 reserved, `FS_LINKED` + `FSL_*`");
   - `UMSG_LINKS` and `FSL_*` in `fsproto.h` and `user/usys.h`, with the table of section 4 and the stale header
     ("result = value or -1") corrected;
   - errno.h's reserved-range comment;
   - `ipc_kcall`'s kflags argument (`kernel.h` 570), agreed with D23's `call()` change.

   Audit F13, F14 and F15 are fixed by the main line before step 3's tests that need them.
1. **ext2.c and fsd:** the walk without following when the bit is set, TEXT, the order before EROFS and the mark, the
   -EACCES mapping, the host tests; architecture.md 999.
2. **zfsd** and the ZenixFS host tests; **nfscd** (`ops.c`, its tests, `nfs-fuzz`) and D23's `OPEN` row.
3. **The kernel:**
   - the states and the helper, every path call through them, 5.1-5.9, and `fs_call_on`'s guard;
   - sh's `cwd_now()` buffer and `realpath`'s 40;
   - the FS-UAE tests of section 10, the size and the stack measured;
   - in the same commit: architecture.md section 9, limits.md (40 links), features.md, the pages of `ln`, `pwd`,
     `mount` and `realpath`, D19 6.5, D11 4 and 5.1, D25 7.3, and F12 marked fixed in audit-2026-10-26.md.

Steps 1 and 2 may land before 3, because each side alone gives today's answers (section 7).

## 12. For the main session

- Claim `UMSG_LINKS` 8 and the answer range in step 0 (main-session decision 2026-10-28: claimed when building).
- **Accepted by the main session (2026-10-28):** the visible changes, both Linux's. After `cd` through a link, the
  working directory's text is the resolved one (`pwd`, as `/bin/pwd` or `cd -P`). Mount points are recorded resolved.
- Audit F13 (search permission above a mount point), F14 (a service load waiting for itself) and F15 (long link
  targets cut) are the main line's. D26's tests 8 and 17 wait for F13 and F14.
- A later item, not this fix: a per-mount `nosymfollow` option (6).

## 13. Reviews

Four independent reviewers read revision 1 (2026-10-28) and were told not to edit. A and D looked at correctness and
security, B and C at architecture fit, frozen interfaces and size. What they found, and what revision 2 did:

**A, correctness and security:**
1. *Blocker:* setting the bit from `nmnt > 1` at send time lets a server write 160 bytes into a 64-byte system call
   buffer: a mount made while a first call blocked. A relative P would also be joined to the mount point. -> The bit
   is tied to "a kernel `FS_PATHMAX` buffer with an absolute path" (3, 4); servers answer only for a P starting with
   `/`.
2. *Major:* fsd refuses EROFS and sets its mark before the walk, so requests through a link out of a read-only volume
   fail and bounced requests write. -> The walk and the answer come first (4).
3. *Major:* an `umount` between the answer and the restart leaves a stale `mnt[m].path`. -> A per-mount instance id is
   checked before the text is used, -EIO (5.1).
4. *Major:* 40 per system call gives a false -ELOOP across two calls. -> 40 per path argument; resolve once (5.2).
5. *Major:* two resolutions in one call can reach different files (D13 unlisting, ETXTBSY). -> Resolve once; the
   second call starts from the resolved state; both are redone if it still answers `FS_LINKED` (5.2, 5.3).
6. *Major:* `mount` and `umount` through links were under-specified; resolving in `umount` would call the server it
   removes. -> Mount's checks run on the resolved text; umount matches the text exactly first and resolves only the
   directory part (5.6, 5.7).
7. *Major:* which argument may be rewritten must be checked per request (`FS_SYMLINK`'s a0 is user text). -> The
   table (4).
8. Minor, all taken:
   - an empty target is -ENOENT; a NUL in a target is -EIO;
   - TEXT is built in a scratch buffer;
   - `FSL_LAST` is defined against the original P; `FSL_ABS` is dropped (TEXT's first byte);
   - the stack is measured, not assumed (5.8); chdir's text length is checked;
   - -EINTR when killed; `UMSG_CANCEL` is ORed in;
   - section 6's kept differences (`link` follows, trailing slash, `O_CREAT` on a directory);
   - nfscd's code on main (8).

**B, architecture, frozen interfaces, size:**
1. *High:* an answer rewrites a buffer that the next call reuses on the old mount (a symlink created on the wrong
   volume); D13 gates on the textual mount. -> The walk state (5.1-5.3).
2. *High:* `umount` would call its own server. -> 5.7.
3. *Medium:* no resolved text with only the root mounted (`chdir` and the first `mount`). -> Both always copy and set
   the bit (5.6).
4. *Medium:* the race with `umount`, and `nmnt` dropping to 1. -> The instance id and the helper's own normalisation
   (5.1).
5. *Medium:* 40 per system call. -> As A4; the `O_TRUNC` case is in test 7.
6. *Medium:* EROFS before the walk. -> As A2; test 14.
7. *Medium:* the D19 6.5 sentence contradicted D25; D11's svctab invariant breaks. -> Narrowed to network mounts; D11
   4 and 5.1 amended (8).
8. *Medium:* -EXDEV decided on the text. -> Named as a deviation, tested both ways (6, test 6).
9. Low-medium: no registry rows for kflags and the answer range; fsproto.h's stale header. -> Step 0.
10. Low-medium: the shared-file edits (`ipc_kcall`, `usys.h`) and D23's `call()`. -> Step 0, agreed with D23.
11. Low-medium: documents in a separate step broke "same commit". -> In steps 1-3.
12. Low: answers depend on whether anything is mounted. -> Said in 6.
13. Low: the resolved mount point's checks. -> 5.6, test 12.
14. Low: sh's 128-byte cwd buffer. -> Fixed in the build (8).
15. Low: a dying process keeps restarting. -> -EINTR (5.1).
16. Low: `FS_SYMLINK` may rewrite only a1. -> The table.
17. Low: the -EACCES mapping can swallow `FS_LINKED`. -> 4, a host test.
18. Low: the size estimate, other growth, speed on D25. -> 9, D25's `PATH` (8), a target.
19. Low: test gaps (a)-(l). -> Tests 4, 5, 7, 9, 12-18 and the host list.
20. The note's facts match the source.

**C, architecture (a second reader):** agreed with B, and added:
1. The ZenixFS cases belong in zenixfstest, and the 230 s limit applies. -> 10.
2. The size labels (fsd.zx 28,676, zfsd.zx 36,764 with relocations; zfsd's own budget) and larger estimates. -> 9.
3. Reserve -1024..-1279 with an errno.h comment; define the bit where both sides see it. -> 4, step 0.
4. The helper takes the working directory's text and inode as arguments; the counter is local. -> 5.
5. `realpath`'s 8 links. -> Aligned to 40.
6. sh's `cwd_now()`. -> Fixed in the build.
7. The -EACCES mapping. -> 4.
8. Tests: `umount` by both names, a ZenixFS root with an ext2 mount, root-only unchanged byte for byte. -> Tests 12,
   16, 18.

**D, correctness and security (a second reader):**
1. "Nothing skipped" was wrong: search permission above a mount point is never checked. -> Audit F13 (main line); D26
   names the dependency, test 8 after F13.
2. The D11 deadline claim was wrong (`SV_LOADING` can wait for itself). -> Audit F14 (main line); 8 relies on its fix.
3. Link targets over 255 bytes are cut today. -> Audit F15; under the bit -ENAMETOOLONG (4).
4. The unasked-answer check belongs in `fs_call_on`. -> 4.
5. `O_CREAT` stays dropped for every later restart. -> 5.5.
6. nfscd answers asynchronously. -> TEXT is written only at reply time, never after a cancel (4).
7. The new reach of a hostile volume. -> Said in 6; `nosymfollow` proposed as a later item.
8. boot's `sys_stat("/sbin/netd")` runs before `/usr` is mounted. -> Said in 8 (D25's svctab line).

## 14. As built (branch d26, 2026-10-28)

Built in the order of section 11, one commit a step: step 0 (the claims, `fsproto.h`'s `UMSG_LINKS`, `FS_LINKED`,
`FSL_*`, `fsl_ok`, the servers' `fsl_text`, errno.h's reserved range, `ipc_kcall`'s kflags), step 1 (fsd), step 2
(zfsd, nfscd), step 3 (the kernel, sh, realpath, the FS-UAE tests). What differs from the text above, and why:

- **fsd walks twice for a changing request with the bit.** `prewalk` makes the request's walks (stopping at the first
  link) before the read-only refusal and the mark; when it meets no link, `handle1` walks again as before. One more
  walk through cached blocks for a `chmod`, `mkdir`, `rm`, `mv`, `ln` or a creating open while something is mounted;
  nothing else changed in `handle1`.
- **nfscd's read-only mount** (`o`) refused every changing request at its start; with the bit that refusal waits for
  the walk (`OPF_RO`), so its answer comes before EROFS as fsd's does (5.6 of the table's rule "the decision comes
  before anything else").
- **`O_TRUNC`'s `FS_STAT` runs on a copy of the walk** (5.2): adopted when it succeeds (then the `FS_OPEN` starts from
  it, and both reach one file), dropped when it fails (nothing is there to be busy). Without the copy a failed
  `FS_STAT` through a last-name link would have moved the walk past the link, and the `FS_OPEN` that follows would have
  created the link's target (5.5).
- **D13's gate** is `seg_any()` (any segment of any mount) or a load in progress, as 5.3 says; `seg_on(m)` and
  `exec_on(m)` had no other user and are gone.
- **`mount`** resolves its mount point with `FS_STAT` first, then runs the `/`, 47-byte and "already mounted" checks on
  the resolved text: a mount on a missing directory now answers -ENOENT before "all entries in use".
- **`realpath`** follows 40 links without recursion (static buffers): 40 recursive levels of its old 1.4 KB frames
  would have overflowed a program's 16 KB stack. **`pwd`** had a 128-byte buffer (a longer working directory answered
  1 with no word) and **sh's prompt** (`\w`) a 130-byte one that a 159-byte working directory overran: both are
  `FS_PATHMAX` now, and pwd says why it failed (found while fixing, CLAUDE.md).
- **Test 9's "umount while it runs is -EBUSY"** does not hold, before or after D26: D13 lets a mount go while a shared
  program from it runs (it holds no file open, `tools/pure-test.sh` m, "umount=0"). The test checks the rest through a
  link: the program shared (D13), `>>` and `cp` over it "text file busy", `rm` through a directory link, and a new start
  after the file is replaced loading a new copy.
- **Test 13** covers a stopped mount service (its server killed, `umount` by the exact text starts no server). A
  server that does not answer at all still holds `umount`'s final `FS_QUIT`, as before D26; only the new resolution
  step must not call it, and it does not when the text matches.
- **Test 5's `O_CREAT|O_EXCL` on a link** is checked on the host (`tools/ext2-host-test.sh`, `zenixfs/tests/zfsd-test.sh`,
  nfs-sim-test `links`): no command on the mount tests' floppy opens with `O_EXCL` by itself.
- **A wrong server** is tested with `msgpingd -fslinked` (`tools/mounttest.sh`): an `FS_LINKED` with bits outside the
  table and one to a request that never asks (`FS_READDIR`) are -EIO; a valid one goes on at its text.
- **The old zfsd** (built from main before D26) under the new kernel, by hand in FS-UAE (2026-10-28): a link inside its
  volume is followed by zfsd itself, a link on the root into it works (fsd answers, the kernel goes on), a link out of
  it answers -ENOENT, `cd` through a link inside it keeps the textual working directory: today's answers, as section 7
  says.

**Sizes (measured, `make -s all`):** `kernel.bin` 157,716 bytes on main (22a6d5f) -> 159,596: +16 step 0, +584 step 1
(fsd is inside it), +1,280 step 3 (vfs.c's walks and `seg_any`); **3,220 bytes left on the mount tests' floppy**
(5,100 before; no RAM disk shrink needed), 19,604 on the RAM floppy. More than section 9's estimate (under 1.2 KB):
the walk state, the copy for `O_TRUNC`, `umount`'s second way and the table each cost more than guessed. `zfsd.zx`
36,788 -> 37,308 (+520, a file on the mount tests' RAM disk). nfscd's core +658 bytes of code (`tools/host/nfs-test.sh`
size).

**Kernel stack (`-fstack-usage` sums, 2026-10-28):** the deepest chain is a rename whose `FS_LSTAT` makes a service
load whose program is reached through a link into a mount whose own server must be loaded too: `syscall_c` 272,
`vfs_link` 392, `two_paths` 48, `wstep` 40, `fs_call_k` 80, `call` 44, the load (`load` 156, `proc_load` 144),
`fs_exec_open` 224, `wstep`, `walk_follow` 180, `pick` 40, `mount_search` 116, `fs_call_k`, `call`, the second load,
`schedule`: about 2.8 KB of the 4 KB, about 0.2 KB more than the same chain before D26 (`route`'s 200 and its callers'
frames). The stack guard did not fire in any test.

**Speed (FS-UAE's 68020: an emulator's figure, not hardware, 2026-10-28):** 200 `test -e` through a directory link on
the root into an ext2 mount 1.516 s, the same path directly 1.008 s: about 2.5 ms more for each link followed (one more
round trip to the root's server, a new normalisation and a walk of the text before the link), `tools/mounttest.sh`'s
INFO line. With only the root mounted nothing changes but `chdir`, which now normalises. The real machines' figures
(the stock A1200, the 060) are measured by the owner: [weekend-2026-10-30.md](weekend-2026-10-30.md) (e) 7.
