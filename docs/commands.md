# Command reference

Manual pages for every Zenix program, in `docs/man/NAME.txt`. They use the exact format that Zenix's own `man`
reads (plain text, under 80 columns, `NAME(section)` header, `NAME`, `SYNOPSIS`, `DESCRIPTION`, `OPTIONS`,
`EXAMPLES`, `EXIT STATUS`, `SEE ALSO`), so they can be copied straight to `/usr/man/` on the system disk. `man -k`
reads the line after `NAME` ("name - summary").

The shell, the pager and the editor have longer hand-written pages in `rootfs/usr/man/` (`sh.txt`, `less.txt`,
`edit.txt`); those are the ones installed today.

**Written 2026-10-22 by reading each program's source** (`user/*.c`): options, defaults, limits and exit codes
are what the code does, not what the usual Unix tool does. Nothing here was run; each "NOTES" section lists
where a command is smaller than its Unix namesake.

## Files and directories
| Command | Summary |
|---|---|
| [ls](man/ls.txt) | list files (`-aAldRFhtrS1`) |
| [cat](man/cat.txt) | copy files to output (`-nbsE`) |
| [cp](man/cp.txt), [mv](man/mv.txt), [rm](man/rm.txt) | copy, move, remove |
| [mkdir](man/mkdir.txt), [rmdir](man/rmdir.txt), [touch](man/touch.txt) | make/remove directories, create files |
| [ln](man/ln.txt), [readlink](man/readlink.txt) | hard and symbolic links |
| [chmod](man/chmod.txt), [chown](man/chown.txt) ([chgrp](man/chgrp.txt)), [stat](man/stat.txt) | permission bits, owner and group, file information |
| [find](man/find.txt), [xargs](man/xargs.txt), [du](man/du.txt), [df](man/df.txt) | search a tree, space used, space free |
| [pwd](man/pwd.txt), [basename](man/basename.txt), [dirname](man/dirname.txt) | paths |
| [tar](man/tar.txt), [gzip](man/gzip.txt) ([gunzip](man/gunzip.txt), [zcat](man/zcat.txt)) | archives and compression |

## Text
| Command | Summary |
|---|---|
| [grep](man/grep.txt) | search (POSIX extended expressions) |
| [sed](man/sed.txt) | stream editor |
| [sort](man/sort.txt), [uniq](man/uniq.txt), [cut](man/cut.txt), [tr](man/tr.txt), [tee](man/tee.txt) | filters |
| [head](man/head.txt), [tail](man/tail.txt), [wc](man/wc.txt) | parts and counts |
| [diff](man/diff.txt), [cmp](man/cmp.txt), [hexdump](man/hexdump.txt) | compare, dump |
| [echo](man/echo.txt), [seq](man/seq.txt), [yes](man/yes.txt), [expr](man/expr.txt) | generate and calculate |
| [less](man/less.txt) / [more](man/more.txt), [edit](man/edit.txt) | pager and editor |
| [man](man/man.txt) | these manuals |

## The system
| Command | Summary |
|---|---|
| [sh](man/sh.txt) | the shell |
| [ps](man/ps.txt), [dmesg](man/dmesg.txt), [beep](man/beep.txt), [input](man/input.txt), [free](man/free.txt), [uptime](man/uptime.txt), [uname](man/uname.txt), [date](man/date.txt), [sleep](man/sleep.txt) | system state and time |
| [env](man/env.txt), [printenv](man/printenv.txt), [which](man/which.txt), [whoami](man/whoami.txt), [id](man/id.txt), [hostname](man/hostname.txt), [clear](man/clear.txt), [true](man/true.txt), [false](man/false.txt) | small utilities |
| [login](man/login.txt), [su](man/su.txt), [passwd](man/passwd.txt), [sha256sum](man/sha256sum.txt) | users: sessions, switching user, passwords, checksums |
| [dd](man/dd.txt), [fdisk](man/fdisk.txt), [mkfs.ext2](man/mkfs.ext2.txt), [fsck.ext2](man/fsck.ext2.txt), [mkfs.zenixfs](man/mkfs.zenixfs.txt), [fsck.zenixfs](man/fsck.zenixfs.txt), [sync](man/sync.txt) | disks |
| [mount](man/mount.txt), [umount](man/umount.txt), [mount.ext2](man/mount.ext2.txt), [mount.zenixfs](man/mount.zenixfs.txt) | mounting more than the root filesystem |

## Internals and test programs
[init](man/init.txt), [boot](man/boot.txt), [diskd](man/diskd.txt) (the built-in start-up programs);
[hello](man/hello.txt), [spin](man/spin.txt), [crash](man/crash.txt), [badptr](man/badptr.txt),
[ping](man/ping.txt), [pingd](man/pingd.txt), [bench](man/bench.txt), [termtest](man/termtest.txt).

## How the pages get onto the system
`docs/man/` is the one place for manual pages. `tools/mkroot.sh` copies every `docs/man/*.txt` to `/usr/man` on the
root disk it builds; for a program that has no page there, `tools/mkman.py` still makes a minimal one from the
comment at the top of the program's source. The small RAM-disk image (`zenix-ram.adf`, the install/test floppy) is
built with `NOMAN=1` and carries no manual pages. `man -k` reads only the first 2 KB of each page.

## Keeping this in step with the code
Adding or changing a command means changing its page in the same commit. The rule and the check
(`tools/docs-check.sh`) are described in `../CLAUDE.md`.
