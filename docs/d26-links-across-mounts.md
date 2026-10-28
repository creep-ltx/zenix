# D26. Symbolic links across mounts (audit 2026-10-26 F12)

Revision 1, 2026-10-28. A fix, not a feature: a symbolic link into another mount answers ENOENT today, Linux follows
it. Asked for by D25 (the demo floppy's links from the RAM root into `/usr`, d25-demo-floppy.md 7.3), but needed by
every Zenix that mounts anything. Nothing here is built. *Read* from the source at main 77d43f2 unless it says
*measured*.

## 1. The problem

The kernel picks a mount from a path's **text**; the filesystem server follows symbolic links **inside its own
volume** (`ext2_namei_ex`, `kernel/ext2.c` 567-617; zfsd's `namei`, `user/zenixfsd.c` 48-86). So, once a second
filesystem is mounted:

- a link on the root to `/usr/bin/ls` finds the root's own (empty) `/usr` directory and answers ENOENT;
- a link on `/usr` whose target is absolute, `/usr/lib/x`, is resolved from `/usr`'s own root: `/usr/usr/lib/x`
  (wrong file or ENOENT);
- a relative link that climbs out of a volume (`../etc/motd` at `/usr`'s root) stays at that volume's root (ext2's
  root `..` is itself);
- a relative link on the root that passes a mount point (`/bin/ls -> ../usr/bin/ls`) walks the root's mount point
  directory, not the mounted volume.

## 2. How a path is looked up today (end to end)

1. A system call (`open`, `stat`, `chdir`, `unlink`, ...) reaches `kernel/vfs.c` with the program's path (at most 63
   bytes).
2. `route()` (vfs.c 290-313): with **only the root mounted** (`nmnt <= 1`) the path goes to the root's server
   untouched, with the working directory's inode. Otherwise `path_abs` makes it absolute and normalised **as text**
   (the working directory's text + the path, `.` and `..` removed, at most `FS_PATHMAX - 1`, 159 bytes, longer is
   -ENAMETOOLONG); the longest mount point that prefixes it wins; its server gets the rest (always starting with `/`,
   never a `..`) in a kernel buffer of `FS_PATHMAX` bytes, with its root inode as the working directory.
3. One message (`fs_call_on`, `ipc_kcall`: `kflags = UMSG_KERNEL`, the caller's uid, gid, umask) goes to that
   server. The server walks the path itself: every directory searched needs x (`ext2_access`, fsperm.h), a link met
   in the middle (or at the end, when the call follows the last name) is read and resolved recursively from the
   link's directory, an absolute target from **its volume's** root, more than 8 nested links -ELOOP (`depth > 8`; links
   in a row are not counted together). The two-path calls (`FS_RENAME`, `FS_LINK`) and the ones that create or remove
   walk the parent (`ext2_namei_parent`, which copies the path and cuts its last name) and do not follow the last
   name; `FS_LSTAT` and `FS_READLINK` do not follow the last name; `FS_OPEN` with `O_CREAT` on a link to nothing
   answers -ENOENT (architecture.md 999: no second entry of that name).
4. The answer goes back to the program, except: `vfs_chdir` stores the **logical** text (step 2's `path_abs`) as the
   new working directory's text; `vfs_open` tries `/dev/...` names first (`open_device`, the kernel's devices are
   names, not inodes); `fs_exec_open` and D13's checks (`exec_busy`, `name_ino`) key what is loaded by (mount, inode).
5. Nothing else in the message protocol knows about links: `fsproto.h` has no reply but "a value or -errno".

The documented caveat (architecture.md 919): `..` is taken on the text, so a working directory reached through a
link behaves as its text says once a second filesystem is mounted.

## 3. The choice: the kernel follows every link when something is mounted

**When more than the root is mounted, a server does not follow a symbolic link at all: it stops at the first one it
meets and answers "this path continues at TEXT". The kernel normalises TEXT, picks the mount again and sends the
same request again.** The kernel counts these restarts (40, Linux's `MAXSYMLINKS`, then -ELOOP).

Why every link and not only "the ones that leave the volume" (D25 7.3's first idea): a server does not know where
the mount points are, so it cannot tell that a relative link passes one (case 4 of section 1). The kernel knows the
mount table, and its text is link-free up to the link (the server stopped at the first one), so the kernel's textual
`..` is the physical `..` there, and at a mount point it is Linux's (`..` of a mounted root is the mount point's
parent). The server needs no "above my root" test and no recursion; the kernel gets one small loop.

Weighed and not taken:
- **The server answers only for absolute targets and `..` above its root** (D25 7.3 as first written): misses case 4
  and any nested mount; the server would also have to keep its own recursion for the rest.
- **The kernel tells each server its mount points** (a new message): more state in every server, kept in step with
  every mount and umount, for one saved round trip per link.
- **The kernel walks every path name by name** (a lookup message per name, as Linux's VFS): a new message type for
  every path call, many round trips for every path, much more kernel code; far too slow for the stock A1200.
- **Only for the root's server:** a link on `/usr` to `/etc/...` would still be wrong.

With **only the root mounted** nothing changes: no mount can be crossed, the server follows links itself as today
(and its `..` is physical).

## 4. The protocol (append-only additions to `fsproto.h` and `struct umsg`)

- **`UMSG_LINKS` 8**, a bit of `struct umsg`'s `kflags` (1 is `UMSG_KERNEL`, 2 the internal `UMSG_NETREPLY`, 4 D23's
  proposed `UMSG_CANCEL`), meaning "the sender takes the `FS_LINKED` answer". Set only by the kernel (`call()` sets
  `kflags` from its argument; a program's `SYS_call` never carries it), on every path request **while `nmnt > 1`**.
  Without the bit a server does exactly what it does today, so a root program's raw request (`mount`'s `FS_ROOT`, ...)
  and an old kernel see no change.
- **The answer `FS_LINKED`:** the result `-(FS_LINKED + bits)`, `FS_LINKED` 1024, bits:
  `FSL_ABS` 1 (TEXT is absolute, from Zenix's `/`; else relative to the mount point of the server's volume),
  `FSL_A1` 2 (the path in `a[1]` was rewritten; else the one in `a[0]`), `FSL_LAST` 4 (the link was the path's last
  name). So the range -1024 .. -1031, far from every errno (kernel/errno.h ends at 115). **Never returned to a
  program:** the kernel turns any value in that range it did not ask for, or cannot use, into -EIO.
- **TEXT is written into that path argument's own buffer**, NUL-terminated, at most `FS_PATHMAX` bytes (a longer
  continuation is -ENAMETOOLONG from the server). The buffers are the kernel's (route's `FS_PATHMAX` buffers, always
  when `nmnt > 1`), so the server may write there; nothing else of the request is changed.
- **How the server builds TEXT** from the request's **original** path string P (not from a copy: `ext2_namei_parent`
  works on a copy with the last name cut off, which would lose it): the walk records where the link's name starts
  (S) and ends (E) in P and reads the link's target T (at most 255 bytes). Then TEXT = `T` + `P[E..]` with `FSL_ABS`
  when T starts with `/`; else TEXT = `P[1..S)` + `T` + `P[E..]` (the link's directory inside the volume, without the
  leading `/`). TEXT is not normalised by the server.
- **When the server answers it:** in every request that walks a path, at the first link the walk would follow (in
  the middle always; at the end when the request follows its last name: `FS_OPEN`, `FS_STAT`, `FS_CHDIR`,
  `FS_CHMOD`, `FS_CHOWN`, `FS_UTIMES`, `FS_LINK`'s existing name). Before anything is changed: every walk comes first
  in fsd and zfsd today (fsd's clean mark may already be set: harmless, it is taken back 1 s later). The rest of each
  request is unchanged, so a restarted request is a new, whole request.
- Messages, their numbers and their argument meanings are unchanged; the answer is new only for a sender that asked.

## 5. The kernel's part (`kernel/vfs.c`)

One helper that every path call goes through, around `fs_call_on`:

1. Route as today. Set `UMSG_LINKS` when `nmnt > 1`.
2. Answer in `FS_LINKED`'s range: count one (more than 40 in this system call: -ELOOP); force a NUL at the buffer's
   end (a server's text is untrusted, CLAUDE.md); route the rewritten argument again: `FSL_ABS` with `/` as the
   working directory text, else with **the mount point's text** (`mnt[m].path`), so `path_abs` joins and normalises it
   (no new buffer: route's own `tmp` and the argument's buffer, which `route` may share as input and output). Its
   new mount and that mount's root inode replace the old ones. Then the same request again.
3. Two-path calls (`rename`, `link`): the rewritten one (`FSL_A1`) is routed again; if the two now differ in mount,
   -EXDEV, as today.
4. `open` with `FSL_LAST`: **`O_CREAT` is dropped** for the restart (`O_EXCL` never gets there: the server answers
   -EEXIST for any name that exists, a link included, before following it). So a link to nothing stays -ENOENT and a
   link to a file opens it: today's rule, not Linux's (which creates the target). Before each restart of an `open`,
   a text that now starts with `/dev/` tries `open_device` first, so a link to `/dev/null` works.
5. **The resolved text.** After the last answer the kernel knows the link-free text (the mount point + the server's
   path). `chdir` stores **that** as the working directory's text (Linux's `getcwd`: `cd /bin` where `/bin ->
   /usr/bin` shows `/usr/bin`); then a later relative `..` is the physical one, and `umount`'s busy check, which looks
   at working directory texts, sees a working directory reached through a link. `mount` records its mount point by its
   resolved text and `umount` resolves its argument the same way (Linux's).
6. D13 and the loader: `fs_exec_open`, `vfs_open`'s `-ETXTBSY` check and `name_ino` use the helper, so the mount
   they key by is the one that holds the file.
7. The kernel stack (4 KB per process): no new path buffer; one counter and the bits.

## 6. What it does, next to Linux

- **Followed:** every link, absolute or relative, into any mount and back, as Linux. Absolute targets are from
  Zenix's `/` (today: from the volume's root, wrong on every mount but the root).
- **`..`:** at a mount point it leads to the mount point's parent (Linux's). One deviation stays, as today: a `..`
  that a program writes **directly after a link in the same path** (`/bin/../etc` with `/bin -> /usr/bin`) is taken
  on the text before the link is followed (`/etc`; Linux: `/usr/etc`). The same for a `..` after a link inside a
  link's own target. The working directory's text no longer adds to this (5.5). Said in architecture.md section 9.
- **The last name:** `lstat`, `readlink`, `unlink`, `rmdir`, `rename`, `symlink`'s new name and `O_CREAT|O_EXCL` never
  follow it, as today; links in the middle of their paths are followed across mounts. Zenix has no `O_NOFOLLOW` and
  none is added (feature halt); `lstat` answers the link itself on any mount.
- **Permissions:** the first server checks search (x) on every directory up to the link before reading it (the link's
  own mode does not matter, as in Linux); each restart is a whole new walk with the caller's ids (`call()` sets them
  for every message), so the target's directories are checked by the server that holds them. Nothing is checked
  twice wrongly or skipped: a restart only re-walks text the caller could have typed itself.
- **Limits:** 40 links followed per path (Linux's), across all restarts; a continuation longer than 159 bytes is
  -ENAMETOOLONG (the one path limit), also where today a long target inside one volume worked. With only the root
  mounted the server's own rule stays (8 nested).
- **Changing links meanwhile:** another process may change a link while a walk waits for a server. Each restart reads
  the text as it is then; the result is a path the caller could have named, checked with the caller's ids, as in
  Linux (which also gives no atomic walk). A loop made on the fly ends at the 40th restart. No privileged server
  acts on a text it did not check.
- **Not done (named so nobody thinks it is):** Linux's `protected_symlinks` (refusing to follow a link in a sticky,
  world-writable directory owned by someone else). Today's following inside one volume lacks it too; `mktemp` and
  here-documents use `O_EXCL` (architecture.md 1555), so they are not exposed. A candidate for the ideas list.

## 7. Old and new together (the frozen-interface rule)

- **New kernel, old server** (an older zfsd or nfscd after `zpkg --back`, a server of a later kernel's rollback): the
  server ignores the bit and follows links inside its volume: today's answers, never worse.
- **Old kernel, new server** (D15's fallback to the previous kernel): the kernel never sets the bit; the server
  follows links itself, as today. So **both servers keep their own link following** for requests without the bit.
- **Raw requests by root programs:** no bit, no change.

## 8. What else assumed that links stay on one volume

- **architecture.md section 9** (the caveat) and 999 (fsd and zfsd answering the same): rewritten in the build step.
- **D23's nfscd** (not built): its `OPEN` row resolves absolute targets from the export's root and never climbs
  above it. With the bit it answers `FS_LINKED` like fsd (absolute targets from Zenix's `/`, as a Linux NFS client);
  without the bit it keeps D23's rule. Its targets come from the NAS: the bounds of section 4 apply. D23's row is
  amended when nfscd is built (main's).
- **D19 6.5** (netd calls only the root's server): a link on the root into a mount would let netd's files reach
  another server, and an NFS mount would then wait for netd while netd waits for it. Only root can make links in
  `/etc`, `/sbin` and `/bin`, so this is root's configuration, as D23's refusal of NFS mounts there is; the rule in
  D19 6.5 and `mount`'s page get the sentence "nor a link that leads from netd's files into a mount".
- **D11's service programs** (`@zfsd` is `/sbin/zfsd`): a server program found through a link into the mount it
  serves cannot be loaded (its own server is the one being started); root's configuration again, and D11's start
  deadline ends the wait. Said in d11-service-manager.md's mounts section with the same sentence.
- **D25 7.3** points here instead of its own sketch.
- Checked and unchanged: `tools/mkroot.sh`'s links (relative, inside the root: one extra round trip each when
  something is mounted, 9 names: `more`, `gunzip`, `zcat`, `chgrp`, `pkill`, `bootamigaos`, `ifup`, `ifdown`, `sz`),
  `fsck.ext2` and the host tools (offline), `cp`, `tar`, `find`, `ls -l` (they use `lstat` and `readlink`), the D13
  segment keys (5.6), the boot's `sys_stat("/sbin/netd")` (only the root is mounted then).

## 9. Size and speed

**Measured on main 77d43f2 (`make -s all`, 2026-10-28):** `kernel.bin` 155,944 of 179,200 bytes: **23,256 left on the
RAM floppy, 6,872 on the mount tests' floppy**. fsd (`fsd.zx` 25,228 bytes) is inside `kernel.bin`; zfsd
(`zfsd.zx` 33,564) is a file.

*Estimates* at -Os: the kernel's loop, the bits and `O_CREAT`, the device check and the resolved text 250-400 bytes;
fsd (ext2.c's walk recording S, E and T, the TEXT builder, the bit) 200-300 bytes; zfsd the same. So `kernel.bin`
grows 0.5-0.7 KB of the 6,872 (the tightest budget); the step measures it and says so.

Speed: nothing changes with only the root mounted. With mounts, every link followed costs one more round trip to a
server and a new walk of the text before it (cached blocks). Not measured; the build step times `ls` through a link
on the A1200 (stock and 060) when the owner is home, against a direct path.

## 10. Tests (each fails today unless it says so)

On the mount tests' image (`tools/mounttest.sh`, a second ext2 and a ZenixFS volume mounted), new lines in
`tools/tests/mount-test.txt` and a new expected output, for **both** an ext2 and a ZenixFS mount:
1. A link on the root to a file on the mount: `cat` reads it; `ls -l` and `readlink` show the link.
2. A link on the mount with an absolute target on the root (`/etc/motd`), and one with an absolute target on the
   same mount: both read the right file.
3. A relative link at the mount's root climbing out (`../etc/motd`); one on the root passing the mount point
   (`../mnt/f` from `/tmp`).
4. A directory link into the mount: `cat L/f`, `cd L`, `pwd` shows the resolved text, `cd ..; pwd` shows the
   mount's parent; `umount` is -EBUSY while a shell is there.
5. Creating through it: `echo x > L/new` lands on the mount; `O_CREAT` on a link to nothing on another mount is
   -ENOENT; `O_CREAT|O_EXCL` on any link -EEXIST (passes today, kept).
6. `rm L` removes the link, not the target; `mv` and `ln` through a link onto another mount -EXDEV.
7. Loops: two links pointing at each other across mounts -ELOOP; a chain of 40 across the two mounts works, 41
   -ELOOP.
8. Permissions: a user cannot follow a link into a mount directory without x (-EACCES), nor read a link in a
   directory without x.
9. A program run through a link into the mount (`hello`), twice (D13 shares it); `umount` while it runs -EBUSY.
10. A link to `/dev/null` on the mount: writing to it works.
11. A continuation longer than 159 bytes -ENAMETOOLONG; nothing crashes.
12. `mount` on a path through a link: `mount -l` shows the resolved mount point (passes the umount test after it).

Host tests (`tools/ext2-host-test.sh`, `zenixfs/tests`): with and without the bit, for each request kind: the text,
the bits, S and E with the last name cut by the parent walk, a 255-byte target, a target that makes TEXT too long,
nothing changed on the volume before the answer; without the bit today's answers byte for byte.

## 11. Build order (one branch, the full suite before the merge; a kernel change: the reviews of this note count)

0. Claims in CLAUDE.md (main): `UMSG_LINKS` 8, the `FS_LINKED` answer and its bits in `fsproto.h`.
1. ext2.c and fsd: the walk without following when the bit is set, TEXT, the host tests.
2. zfsd and the ZenixFS host tests.
3. The kernel: the helper, every path call through it, 5.2-5.6; the FS-UAE tests of section 10; the size measured.
4. The documents: architecture.md section 9 and 999, features.md, limits.md (40 links), `man ln`'s NOTES, D19 6.5,
   D11's mounts section, D25 7.3, F12 marked fixed in audit-2026-10-26.md.

Steps 1-3 may land in any order (section 7: each side alone gives today's answers), but are merged together.

## 12. For the main session

- Claim `UMSG_LINKS` 8 (after D23's proposed 4) and the answer range -1024 .. -1031 (`FS_LINKED`, `FSL_*`) in the
  CLAUDE.md table; both join the frozen list (message meanings).
- Approve the visible changes, both Linux's: the working directory's text after `cd` through a link is the resolved
  one (`pwd`), and mount points are recorded resolved.
