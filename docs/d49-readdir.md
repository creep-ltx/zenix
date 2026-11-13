# D49: reading a directory in batches (`SYS_readdirs`, `FS_READDIRS`), and the device in `stat`

Asked by the main session 2026-11-13 after the file view (D48) found a folder of 500 files slow to list: "folder
reading is slow, and the user feels it" (the golden path, CLAUDE.md "Speed and size": reading files and the shell's
answers are in the fast lane). A deep core change (the kernel's VFS, libz and every filesystem server): revision 1
had two independent design reviews (section 12); this is revision 2, built on branch `readdir` 2026-11-13 and checked
in FS-UAE and on the PC, not on real hardware. The second part (section 8) is `cp a a` emptying the file, which needs
the device number this note adds to `stat`.

## 1. In plain words

Listing a folder of 500 files took about 6 seconds in FS-UAE's stock A1200 (68EC020, 4 MB fast), for `ls` and for the
file view alike. The guess was "one system call and one server round trip per entry". Measured (section 2), that is
a tenth of it: each entry's record costs about 1 ms, but `ls` (always) and the file view (on an ext2 volume of
revision 0, the root image's, whose records carry no file type) then ask `lstat` of every name, and the server finds a
name by walking the directory from its start: about 3 ms for the first name and 14 ms for the last, 9-10 ms on
average.

So the fix is the one Linux and NFS made, in one request: **one call fills the caller's buffer with as many entries as
fit, and each entry carries what `lstat` would answer for its name** (NFS calls this READDIRPLUS). A program that
reads a folder makes one call per 40 or so entries, and one that needs the sizes and dates (`ls`, `ls -l`, the file
view) asks nothing more. Every filesystem server answers the new request; one that does not (an older server, an
older kernel) answers "not known" and libz reads one entry at a time as before, so every program works on every mix.

Measured after (emulator figures, the same machine and folder): `ls` 5.66 -> 1.05 s (5.4 times), `ls -l` 6.83 ->
2.19 s (3.1 times: the rest is `ls`'s own formatting), the file view's folder (section 11).

| Part | What changes | Where |
|---|---|---|
| kernel | `SYS_readdirs` 99: the open directory's next records into the caller's buffer; `stat`'s device | `kernel/syscall.c`, `kernel/vfs.c`, `kernel/kernel.h` |
| protocol | `FS_READDIRS` 27, `struct fsrdx`, the record `struct udirx` | `kernel/fsproto.h`, `kernel/udirx.h` |
| servers | each answers `FS_READDIRS` with its own readdir loop and its own `FS_LSTAT` filling | `user/fsd.c` (+ `kernel/ext2.c`), `zenixfsd.c`, `fatd.c`, `ffsd.c`, `pfs3d.c`, `iso9660d.c`, `exfatd.c`, `net/nfs/ops.c` |
| libz | `zdir_open`/`zdir_next`/`zdir_close` (a 2 KB buffer, the fallback, mount points), `dir_list_some` on it, `dir_list_stat`, `same_file` | `user/zhelp.c`, `user/libz.h`, `user/usys.h` |
| C library | `readdir` on the same records, `d_type` filled | `libc/zenix/dirent.c` |
| programs | `ls`, `files` and `sh`'s globbing and completion take the records' stats | `user/ls.c`, `display/files/files.c`, `user/sh.c` |
| `cp`, `mv` | "are the same file" by device and inode | `user/cp.c`, `user/mv.c` |

## 2. Where the time goes (measured in FS-UAE, emulator figures only)

`bench dir PATH` (new, `user/bench.c`) on the stock A1200 with 4 MB fast, the ext2 root (revision 0, 1 KB blocks),
`/tmp/big` holding 500 empty files (502 records with `.` and `..`), E-clock timed, FS-UAE's cycle-exact 68EC020
(fast mode changes only the wall clock):

| What | Each |
|---|---|
| a null system call (`getpid`) | 42 us |
| a message round trip to a server that does nothing (`msgpingd`) | 486 us |
| `fstat` of the open directory (the kernel's VFS, fsd's `ext2_iget`, the trip) | 913 us |
| one `readdir` record (the same plus `ext2_readdir`'s step) | 1,019 us (the last 50: 1,259) |
| `lstat` of the directory's first name | 2,849 us |
| `lstat` of its last name | 14,639 us |
| `ls /tmp/big > /dev/null` | 5.66 s for 500 names |
| `ls -l /tmp/big > /dev/null` | 6.83 s |

So per name `ls` paid about 1 ms for the record and about 9 ms for its `lstat`: the server's lookup walks the
directory's entries in order (ext2 has no index; 24 us an entry passed), and the path's other names and the trip add
2-3 ms. The file view paid the same, because `ext2_readdir` gives type 0 (unknown) on a volume without the
file-type feature (the root image is made with `mke2fs -E revision=0`, `tools/mkroot.sh`) and the view then asks
`lstat` (D48 "Read lean"). A batch of records alone would have saved 0.5 s of the 6; the stats in the records save
the 4.7 s. The message itself (486 us) and the kernel's side of a call (the rest of the 913 us) are a cost of every
request, left for another note (section 10).

After (the same run, `tools/readdir-test.sh`): a record one a call 1.04 ms, in batches through libz 0.58 ms, the
system call alone 0.49 ms. The batch is now the server's own work: a probe in fsd (taken out again) measured per
record about 150 us for `ext2_readdir`'s step, 100 us to make the record and, with the full `ext2_iget`, 430 us for
the inode; `ext2_istat` (the seven numbers a stat shows, read in place) and the inode table read ahead brought the
call to 0.49 ms a record. This 68020 at 14 MHz runs about one instruction a microsecond here.

## 3. The interface

### 3.1 `SYS_readdirs` 99 (fd, buffer, size)

- `fd` an open directory (-ENOTDIR for any other file, -EBADF for no descriptor); `size` 104 (`UDIRX_MAX`, the longest
  record) to 65,535, else -EINVAL: bits 16-31 are reserved for request flags later (a "names only" form, a longer
  record), so they must be 0 today; `buffer` 4-aligned (-EINVAL) and the caller's own memory for `size` bytes
  (`user_ok_wr`, else -EFAULT).
- Answers the bytes of whole records written (each starting at a multiple of 4 from the buffer), 0 at the end, or the
  server's negative errno. The descriptor's position moves past the last record (also at the end: the server's
  position); `SYS_readdir` shares it, so the two may be mixed.
- -ENOSYS: the directory's server does not know `FS_READDIRS`; nothing is written and the position is unchanged. An
  older kernel answers -ENOSYS for the call itself. Either way the caller reads with `SYS_readdir` (libz does).
- The kernel refuses (-EIO) an answer longer than the buffer, or records that do not move the position (a server's
  bug that would make a reader loop for ever).

### 3.2 The record, `struct udirx` (`kernel/udirx.h`, frozen)

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | `size`: this record's bytes, a multiple of 4 (44-104 today) |
| 2 | 1 | `type`: `udirent`'s type numbers (ext2's: 1 file, 2 directory, 7 symbolic link, 3-6 devices, fifos, sockets; 0 not known) |
| 3 | 1 | `flags`: `UDX_STAT` 1 (below); other bits 0 |
| 4 | 4 | `ino`, as `udirent`'s |
| 8 | 4 | `next`: the directory's position after this record (what the descriptor holds once it is read) |
| 12 | 28 | `st`: a `struct ustat` (its first size, the servers' one; zeros without `UDX_STAT`) |
| 40 | 1-64 | `name`: 1-63 bytes and a 0, then zeros to `size` |

**The growth rule** (review 2, 2): a reader steps from record to record by `size` (at least 44, a multiple of 4,
inside the answer) and ignores flags it does not know and any bytes after the name's 0; a longer record comes only
when a request's flags (`size`'s bits 16-31) ask for it. A record that cannot be stepped over ends that answer; one
whose name is not 1-63 bytes ended by a 0 is skipped by libz (the rest is still listed).

The same names `SYS_readdir` gives, in the same order (`.` and `..` too, where the volume has them), with the same
`ino` and the same `type` or, where the old record said 0, a type the server now knows (ext2 of revision 0 reads the
inode for the stat). **`UDX_STAT`** (reviews 1, 2 and 10; 2, 6): `st` is what the server's `FS_LSTAT` gives for this
entry, as Linux's `fstatat(dirfd, NAME, AT_SYMLINK_NOFOLLOW)` on the open directory: the caller's search (x) right
on this directory is checked; the directories above were checked when it was opened, as for every call on an open
descriptor (a later `chmod` above it does not stop the batch; it stops a new `lstat`). Never on `.` and `..` (their
`lstat` is the path's text, D26: `/mnt/a/..` is `/mnt`) or on a name the server cut to 63 bytes (whose `lstat` would
miss it). On a name that is a mount point the server's record describes the directory underneath (as its `ino`
always did, Linux's `d_ino` too); libz takes that stat out (section 5). Where a filesystem lists two entries under
one name (a FAT or exFAT name with characters a Unix name cannot hold put as `_`, review 1, 2), the record's stat is
its own entry's, while an `lstat` of that name finds the first: the record is the more right one.

### 3.3 `FS_READDIRS` 27 (kernel to server)

`a0` the directory's inode, `a1` the position, `a2` the buffer (the caller's memory, written in place as every server
writes), `a3` a `struct fsrdx *` in the kernel (`size`: the buffer's bytes, at least 104; `next`: written by the
server, the position after the last record). The answer: the bytes written, 0 at the end (`next` the end's position),
or -errno; -ENOSYS from a server without it (every server answers an unknown request so, audit E). A server writes
whole records only and stops before one that does not fit; on an error after one or more records it answers those
(the error comes again at the next call, from where they stopped), so nothing read is lost; an empty name (damage) is
left out; a killed client (`UMSG_CANCEL`) gets what was written. Like every `FS_*` request it is served only for the
kernel (`UMSG_KERNEL`) or root.

## 4. The kernel

`SYS_readdirs` is one case calling `vfs_readdirs(f, buffer, size)`: the checks of 3.1, one `FS_READDIRS` with
`f->off`, the answer's checks, `f->off = next`. -ENOSYS is passed up. No loop, no copy, no buffer, no record walk in
the kernel (revision 1 walked the records to take a mount point's stat out; review 1, 3 showed that its numbers
(the parent mount and the mount point's inode) went wrong with a mount put under an existing one or a renamed mount
point, and the walk cost about 200 bytes: it moved to libz, which reads the mount table by name).

`stat`, `lstat` and `fstat` (8.1) write the device when the caller's buffer has room.

## 5. libz and the C library

`user/zhelp.c` (libz and the C library share it, D27):

```
struct zdir *zdir_open(const char *path);            /* NULL and zerrno: cannot open */
const struct udirx *zdir_next(struct zdir *d);       /* the next record, NULL at the end or on an error */
int zdir_err(const struct zdir *d);                  /* 0, or the errno that ended the reading */
int zdir_fd(const struct zdir *d);                   /* its descriptor: its position runs ahead of the records given */
void zdir_rewind(struct zdir *d);                    /* forget the buffered records (the descriptor put back at 0) */
void zdir_close(struct zdir *d);
```

A `struct zdir` is one `malloc` of 2,400 bytes: the descriptor, the state, the mount points' last names (7 of 48
bytes, read once a listing with `SYS_mountinfo`, which asks no server, when a record first has a stat) and a
2,048-byte buffer (about 42 records of short names; 500 names in 12 calls). The first `SYS_readdirs` that answers
-ENOSYS puts that directory in the old mode for good: each `zdir_next` is then one `SYS_readdir`, its `udirent` made
into a `udirx` (no `UDX_STAT`, `next` 0). A record with `UDX_STAT` whose name is the last part of a mount point loses
it (and its stat); by name, as the kernel picks mounts by text: a name that only looks like one loses it too, which
costs that name one `lstat`.

- `dir_list_some` and `dir_list` (every program that walks trees: `find`, `rm -r`, `cp -r`, `du`, `tar`, `chown -R`,
  `diff -r`, `zpkg`, `zup`, ...) read through it, unchanged for their callers.
- `dir_list_stat(path, &names, &stats, &err)`: `dir_list_some` and an array of `struct ustat`, one a name, `mode` 0
  where the record had no stat (a real file's mode is never 0). For `ls`.
- The C library's `opendir`/`readdir` (`libc/zenix/dirent.c`) read through `zdir`; `d_type` is set from `type` (ext2's
  numbers to `DT_*`) or from `st.mode`, `DT_UNKNOWN` when neither says. `rewinddir` opens the directory again.

Programs:

- `ls`: `dir_list_stat`; a name with a stat is not asked again and not joined to its folder's path (a symbolic link's
  target is still read by `readlink`, and a path over 255 bytes goes the old way, so its "file name too long" stays).
  `.` and `..` (`-a`) are asked as before. The output is byte for byte the same (section 9).
- `files` (D48): the type, size, date, mode and owner from the record (`F_STAT`), so neither the folder nor the rows on
  view ask `lstat` (review 1, 5: the 12-byte entries already had the room).
- `sh`: its globbing and its Tab completion read through `zdir`; completion takes "is a folder" and "is a file" from
  the record's stat where it is not a link. Found on the way and fixed: a pattern's match was checked again with
  `stat`, which follows links, so a symbolic link that leads nowhere never matched (bash matches it) and each match
  cost a path walk: a name the listing gave is now taken as it is, and a pattern without wildcards in its last part is
  checked with `lstat`.
- Unchanged, one record a call as before: `man`, `wasabid`, `zine`, and `bench`'s own old-path timing.

## 6. The servers

Each server gets one function: its readdir-one loop from the position, a record a name (the name, the type, the inode
as `FS_READDIR` gives them), `next` after each, the stat filled by the very code its `FS_LSTAT` uses for that object,
stopped when the next record does not fit (its position then is the one before that entry). The search right on the
directory is checked once, with the request's ids, by the server's own permission function, and decides `UDX_STAT`
for the batch.

| Server | Where the stat comes from |
|---|---|
| fsd (ext2, in the kernel image) | `ext2_istat`: the seven numbers `ext2_iget` reads, read in place in the cache (no 128-byte copy, no block list), with `ext2_iget_ahead` set: an inode table block not cached comes with the next ones in one disk request (`bfill_run`, ext2_read's read-ahead); the type from the mode on a volume without file types; none on a name of 63 bytes (it may have been cut) |
| zfsd (ZenixFS) | `zenixfs_getattr` and `fill_stat`; names over 63 bytes left out, as `FS_READDIR` does. Its types are now ext2's numbers in both requests (review 1, 1: it passed the library's 4, 8, 10, so the file view showed ZenixFS folders as files) |
| fatd | `dir_next`'s entry, `fill_node` (the open slot's size and attributes, the entry's date: as `FS_LSTAT`) |
| exfatd | the entry set, `fill_ent` (as `FS_LSTAT`; nothing changes on a read-only volume) |
| ffsd | `do_readdir`'s walk itself (its position is the entry's header block), then `node_load` and `fill_node` of that block: a hard link its object's count and date, as the walk's lookup. A batch that stops early starts the next call's chain walk afresh (found by the host test: the entry read again tripped the walk's loop detection, which skipped the rest of that hash chain) |
| pfs3d | `do_readdir`'s walk, `node_of` of the entry it read (a hard link its object), `fill_node` |
| iso9660d | a file from its record, a directory from its own `.` (as `FS_LSTAT`); none for a Rock Ridge link (its size is the link text's, read only by a walk) |
| nfscd | none: the listing chunk's names, inodes and types, as many as fit (NFS's attributes keep their cache rules; `lstat` asks them) |

## 7. Costs (measured, `make`, 2026-11-13)

- **kernel.bin 197,364 -> 198,780 bytes (+1,416).** The kernel's own code +360: `vfs.o` +296 (`vfs_readdirs` 176, the
  device in `vfs_stat` and `vfs_fstat`), `syscall.o` +64. The built-in fsd +1,044 (`embed.o`): `readdirs` 560 (most of it
  the record and the stat in line), `ext2_istat` and `iraw`, the read-ahead flag and `bfill_run`. Budgets raised in
  `tools/ksize-budget` with the reasons; the mount tests' RAM disk 685 -> 684 KB (the floppy's room 900 bytes, at
  least 800 wanted).
- **The other servers:** zfsd 35,306 -> 35,834 (+528), fatd, ffsd, pfs3d, iso9660d, exfatd 300-500 bytes each, the
  NFS core a few hundred.
- **libz** (in every program that lists; `zdir` at -Os): about 1.2-1.5 KB a program that lists (`cp` 10,448 -> 11,944
  bytes with `same_file`, `ls` 23,168 -> 24,492 with its own part, `sh` +1.2 KB, `files` +1.3 KB).
- **Memory:** 2,400 bytes of heap while a directory is open through `zdir` (`dir_list_some` frees it before it
  returns, so a tree walk holds one at a time); `ls` 28 bytes a name more for the stats while it lists.

## 8. `cp a a` and the device in `stat`

`cp a a` (or the same file by a hard link, another path, or a link) opened the target with `O_TRUNC` before it read
the source: the file was emptied. `mv` did the same when it copied across mounts (a link to the source's own mount,
decided textually, D26: `-EXDEV`, then `copy_tree` empties the file; the source was then not removed, since the copy
"failed" short, but its data was gone). GNU cp and mv refuse: "'a' and 'b' are the same file". Same file means the
same device and inode; `struct ustat` had no device.

### 8.1 The device

`SYS_stat`, `SYS_lstat` and `SYS_fstat` with a size of 32 or more (`USTAT_SIZE`) write, after the 28 bytes the server
wrote, a 32-bit device at offset 28: the mount's instance number (D26's: the same for every file of one mounted
volume, never reused while the system runs, the root's 1; review 1, 4: a slot number would be reused after an
`umount`); 0 for the kernel's own devices and sockets (`fstat` of a pipe is still -EINVAL). The servers' `struct ustat`
stays 28 bytes (it is in the message protocol, frozen at its size, and the servers write it into the kernel's own
buffers): programs ask with `struct ustat2` (`user/usys.h`: the 28 bytes and `dev`) through `sys_stat2`, `sys_lstat2`,
`sys_fstat2`, which set `dev` to 0 first, so an older kernel (which gives 28 bytes) leaves 0. The C library's `stat`,
`lstat` and `fstat` fill `st_dev` from it (it was always 0). A caller passing 28 (every program built before) gets 28.

### 8.2 cp and mv

`same_file(fd, path)` (libz): the source is opened first and asked by its descriptor (`fstat2`), the target by
`stat2` (links followed, as the open would): one path walk, not two (review 1, 6). The same device and inode: "cp: 'A'
and 'B' are the same file", status 1, nothing opened for writing; `mv` refuses the same before its copy across
mounts. With an older kernel (`dev` 0 in both) the inode, size, time and mode together decide (two empty files with
one number on two new volumes may look alike: refused, nothing lost; FAT's and exFAT's numbers above 32 GB can
repeat, review 2, 9: also only a refusal). `mv`'s rename onto the same file is left to the server: FAT's `mv readme
README` names one file and must go on.

## 9. Tests

- **`tools/readdir-test.sh`** (new; FS-UAE, the stock A1200 + 4 MB fast, the ext2 root of revision 0): `bench dir` on
  500 files, a folder and a link (504 records, 502 stats: identical both ways, every stat equal to `lstat`'s, the
  call's refusals, the times); on a folder of 40 files with a mount point, a dangling link and a file of mode 750 (46
  records, 43 stats: none on the mount point); on a mount of `msgpingd -fsdir`, a server that knows only
  `FS_READDIR` (libz's old path, 52 records); `ls`, `ls -l` and `ls -laF` of that folder byte for byte the rows `ls`
  makes of the same names given as operands (each by `lstat`, the old way); the counts of `ls`, of the old server's
  names, of `sh`'s `f*`; the times of `ls`, `ls -l`, `find`; the host's `e2fsck -fn`.
- **Host tests** (`tools/host/fsd-host.c`'s `rdx DIR [SIZE]`: the whole tree both ways, every buffer size 104-300 in
  turn and 2,048, guard bytes after the buffer, every stat against `FS_LSTAT`, none on `.` and `..`): ext2 with and
  without file types (300 files, links, a hard link, a 70-byte name listed cut without a stat, a folder another user
  may read but not search: names, no stats), ZenixFS (250 files, links, the same folder; the types), FAT12/16/32 (a
  file open and grown: the slot's size), FFS and OFS (hard links, soft links, 150 files), PFS3 (ten layouts, 300 files),
  ISO 9660 (plain, Joliet, Rock Ridge with its links and deep tree), exFAT (four layouts); NFS: `lsx` in nfs-sim-test
  (300 names in 104-, 300- and 2,048-byte buffers, the same as one by one, no stats) and `FS_READDIRS` in nfs-fuzz.
- **`cmds-test`**: `cp a a`, `cp a hardlink`, `cp a dir/../a`, `cp link-to-a a`: GNU's messages and statuses, the file
  intact. **`memtest`** (`badptr`): `stat` with 31 bytes gets 28, with 32 the device 1 of the root. **`usize-test`**.
- The filesystems' emulator tests (`mounttest`, `zenixfstest`, `fat-test`, `ffs-test`, `pfs3-test`, `exfat-test`,
  `cd-test`), `svc-test` and `files-test` (stock) run unchanged against the new code: their listings go through it.

## 10. Not done (and why)

- The message round trip (486 us) and the kernel's part of a file call (about 400 us more in `fstat`): every request
  pays them; their own measured note.
- ext2's lookup walks the directory (24 us an entry): Linux keeps where the last lookup ended
  (`i_dir_start_lookup`), which would make `find`, `du`, `rm -r`, `cp -r` and `mv`'s `lstat`s in listing order cheap
  too (both reviews); a small fsd change of its own, after this. Those tools could also take `dir_list_stat`'s stats.
- `ls -l`'s own formatting (about 2 ms a row here) is now most of its time.
- NFS stats in the records (review 1, 9): READDIRPLUS's attributes are fetched already; giving them needs the
  attribute cache's age rule in the batch. Later.
- ZINE's `ExNext` and Ahiru (review 2, 11), which still `stat` every name through the C library: they could take
  `zdir`'s stats (`d_type` they get now).

## 11. As built: the measured result (FS-UAE's stock A1200 + 4 MB fast, emulator figures)

| What | Before | After |
|---|---|---|
| `ls /tmp/big > /dev/null` (500 files) | 5.66 s | 1.05 s (5.4 times) |
| `ls -l /tmp/big > /dev/null` | 6.83 s | 2.19 s (3.1 times) |
| the file view, start to the drawn window (`files-test`, stock speed) | 6.5-6.7 s | 1.73 s (3.8 times) |
| ... of it reading and sorting the folder | 6.0 s | 1.08 s (5.6 times) |
| a record, one a call / in batches through libz / the call alone | 1.04 ms | 0.58 / 0.49 ms |
| an lstat of every name (what `ls` did) | 9.4-10 ms a name | not asked |

What is left in the file view is its own sort (about 0.7 s for 500 names: libz's `qsort`, a Shell sort, and the
view's case-blind comparison) and the drawing; in `ls -l` its formatting (about 2 ms a row). `find` (5.6 s) still
lstats every name (section 10).

## 12. Reviews of the design (2026-11-13, two independent reviewers, revision 1)

Both: the approach is sound; fix before freezing the format. What changed for revision 2:

- **ZenixFS's types** (1, must): zfsd passed the library's type numbers; fixed in both requests (section 6).
- **"st equals lstat" too strong** (1 and 2, must): restated as `fstatat` on the open directory (3.2); none on names
  a server cut (ext2's over 63 bytes, both); the per-server table checked against each `FS_LSTAT` (fatd's date,
  exfatd's slots, ffsd's and pfs3d's hard links, iso9660d's directories and links); lossy names stated.
- **Mount points by number** (1, must): fragile with mounts under mounts and renamed mount points; now by name in
  libz, no kernel state (section 4).
- **The record's growth rule** (2, must): stated; the 104-byte cap is today's, readers step by size (3.2).
- **Robustness** (2): empty names skipped by the servers and libz (one bad entry no longer ends the listing);
  records that do not move the position refused; the position kept at the end too; size's high bits reserved and the
  buffer 4-aligned; `UMSG_CANCEL` between records.
- **The device** (1, 4): the mount's instance, never a reused slot. **`same_file`** (1, 6): one path walk.
- **The file view** (1, 5): takes the stats into its entries. **`badptr`** (2, 8): updated for 32 bytes.
- **Estimates** (1, 7 and 8): `ls -l` reaches about 3 times, not 5 (its formatting); the cheaper alternatives (types
  only; ext2's lookup hint) weighed: types alone would have fixed the file view, not `ls`; the hint is section 10.
- Kept as proposed: nfscd without stats (1, 9: later), cp/mv in this note (2, 11 suggested its own change: it is in
  its own commits, and it needs the device this note adds).

## 13. Review of the code (2026-11-13, one independent reviewer)

- **A hang on a damaged FFS volume** (bug): the first build reset ffsd's chain walk at the end of every batch (the fix
  for a skipped chain, section 6), so its loop check (Brent's: a loop of L entries is caught after about 2L steps)
  never caught a looped hash chain longer than about 16 entries, and a listing of such a volume never ended. Now the
  walk's state is saved before each entry and put back when a batch stops before giving it, so the next call redoes
  exactly that step. `ffs-host-test` builds a 40-name chain looped to its start: it ends; with the old code it hangs.
- The C library's `readdir` copied 64 bytes of a record's name however short the record (a read past the buffer near
  its end): it copies the name. `mv`'s same-file refusal no longer says "cannot move" too. `zdir`'s seven mount names
  tied to a named constant. The host harness's `rdx` clamps its buffer size; `bench dir`'s raw loop steps with
  `udirx_step`; `fstat` of a pipe writes nothing.

## 14. Tests run (2026-11-13, branch `readdir`)

In FS-UAE, each with its own build copy (fast mode unless said): `readdir-test` (10 checks), `files-test` (stock
speed, 43), `cmds-test` (782 lines identical to GNU's, with `cp a a` and its kin), `memtest` (45), `mounttest`,
`zenixfstest`, `fat-test` (122), `ffs-test` (37), `pfs3-test` (33), `exfat-test` (27), `cd-test` (30), `svc-test`
(148 of 149 at first: part d's "cmp-ok" line had a stray character from the serial echo glued to it, the copy itself
right; part d run again twice: passes). On the PC: `ext2-host-test` (91), `zfsd-test` (10), `fat-host-test`,
`ffs-host-test` (138, with the looped chain), `pfs3-host-test` (53), `iso9660-host-test` (63), `exfat-host-test`
(117), `nfs-test` (sim 251 checks, libnfs, unfs3, size; fuzz 2 minutes, 418,772 inputs: nothing), `usize-test`.
`make`, `make demo` (the DD demo floppy 29,184 bytes free), `docs-check`.
