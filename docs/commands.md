# Command reference

Zenix has 117 programs (the `UAPPS` list in the Makefile; servers and test programs included), each with a manual
page in `docs/man/NAME.txt`; a few more pages are for names that start the same program (`gunzip`, `zcat`, `chgrp`,
`more`, `pkill`). The pages use the exact format that Zenix's own `man` reads (plain text of at most 78 columns, a
`NAME(section)` header, `NAME`, `SYNOPSIS`, `DESCRIPTION`, `OPTIONS`, `EXAMPLES`, `EXIT STATUS`, `SEE ALSO`); the
root disk gets them in `/usr/man` (below). `man -k` reads the line after `NAME` ("name - summary").

The pages are written by reading each program's source (`user/*.c`): options, defaults, limits and exit codes are
what the code does, not what the usual Unix tool does. Each NOTES section lists where a command is smaller than its
Unix namesake. Whether a command is tested: features.md (the text tools and the shell are compared with GNU bash and
tools by `tools/cmds-test.sh` and `tools/cmds2-test.sh`). This index was checked against the Makefile on
2026-10-23 (`tools/docs-check.sh` does it on every commit); the pages are kept in step with each change, and were
spot-checked against the code that day.

## Files and directories
| Command | Summary |
|---|---|
| [ls](man/ls.txt) | list files (`-aAldRFhtrS1`) |
| [cat](man/cat.txt) | copy files to output (`-nbsE`) |
| [cp](man/cp.txt), [mv](man/mv.txt), [rm](man/rm.txt) | copy, move, remove |
| [mkdir](man/mkdir.txt), [rmdir](man/rmdir.txt), [touch](man/touch.txt) | make/remove directories, create files |
| [ln](man/ln.txt), [readlink](man/readlink.txt) | hard and symbolic links |
| [chmod](man/chmod.txt), [chown](man/chown.txt) ([chgrp](man/chgrp.txt)), [stat](man/stat.txt) | permission bits, owner and group, file information |
| [tree](man/tree.txt) | a directory as a tree |
| [find](man/find.txt), [xargs](man/xargs.txt), [du](man/du.txt), [df](man/df.txt) | search a tree, space used, space free |
| [pwd](man/pwd.txt), [basename](man/basename.txt), [dirname](man/dirname.txt) | paths |
| [tar](man/tar.txt), [gzip](man/gzip.txt) ([gunzip](man/gunzip.txt), [zcat](man/zcat.txt)) | archives and compression |

## Text
| Command | Summary |
|---|---|
| [grep](man/grep.txt) | search (POSIX extended expressions) |
| [sed](man/sed.txt) | stream editor |
| [awk](man/awk.txt) | pattern scanning and text processing language (POSIX awk) |
| [sort](man/sort.txt), [uniq](man/uniq.txt), [cut](man/cut.txt), [tr](man/tr.txt), [tee](man/tee.txt) | filters |
| [head](man/head.txt), [tail](man/tail.txt), [tac](man/tac.txt), [rev](man/rev.txt), [nl](man/nl.txt), [wc](man/wc.txt) | parts, reversed, numbered, counts |
| [fold](man/fold.txt), [expand](man/expand.txt), [paste](man/paste.txt), [comm](man/comm.txt) | wrap, tabs, merge, compare sorted |
| [base64](man/base64.txt), [md5sum](man/md5sum.txt) | encoding and checksum |
| [diff](man/diff.txt), [cmp](man/cmp.txt), [hexdump](man/hexdump.txt) | compare, dump |
| [echo](man/echo.txt), [printf](man/printf.txt), [seq](man/seq.txt), [yes](man/yes.txt), [expr](man/expr.txt) | generate and calculate |
| [less](man/less.txt) / [more](man/more.txt), [edit](man/edit.txt) | pager and editor |
| [cu](man/cu.txt) | a terminal on another computer through the serial port (DESIGN D12) |
| [man](man/man.txt) | these manuals |

## The system
| Command | Summary |
|---|---|
| [sh](man/sh.txt) | the shell |
| [ps](man/ps.txt), [dmesg](man/dmesg.txt), [beep](man/beep.txt), [input](man/input.txt), [free](man/free.txt), [uptime](man/uptime.txt), [uname](man/uname.txt), [date](man/date.txt), [sleep](man/sleep.txt) | system state and time |
| [nice](man/nice.txt), [renice](man/renice.txt) | run a command at a priority, change a running one's |
| [kill](man/kill.txt), [pgrep](man/pgrep.txt) ([pkill](man/pkill.txt)), [time](man/time.txt), [timeout](man/timeout.txt), [watch](man/watch.txt), [cal](man/cal.txt), [mktemp](man/mktemp.txt), [realpath](man/realpath.txt) | processes, timing, calendar, temporary files, real paths |
| [env](man/env.txt), [printenv](man/printenv.txt), [which](man/which.txt), [whoami](man/whoami.txt), [id](man/id.txt), [hostname](man/hostname.txt), [clear](man/clear.txt), [true](man/true.txt), [false](man/false.txt) | small utilities |
| [login](man/login.txt), [su](man/su.txt), [passwd](man/passwd.txt), [sha256sum](man/sha256sum.txt) | users: logging in on a console, switching user, passwords, checksums |
| [useradd](man/useradd.txt), [userdel](man/userdel.txt), [gpasswd](man/gpasswd.txt) | add and remove users, group members (root) |
| [sudo](man/sudo.txt) | run one command as root: members of the group wheel, with their own password |
| [dd](man/dd.txt), [fdisk](man/fdisk.txt), [mkfs.ext2](man/mkfs.ext2.txt), [fsck.ext2](man/fsck.ext2.txt), [mkfs.zenixfs](man/mkfs.zenixfs.txt), [fsck.zenixfs](man/fsck.zenixfs.txt), [sync](man/sync.txt), [reboot](man/reboot.txt) | disks |
| [zup](man/zup.txt) | install the root half of an update (`/sbin/zup`, DESIGN D15) |
| [rescue](man/rescue.txt) | not a command: what to do when the disk check at boot cannot repair the root (the four choices boot offers, step by step) |
| [mount](man/mount.txt), [umount](man/umount.txt) | mounting more than the root filesystem (its server is a service: the kernel's fsd or zfsd) |

## Internals and test programs
[init](man/init.txt), [boot](man/boot.txt), [diskd](man/diskd.txt) (the built-in start-up programs);
[hello](man/hello.txt), [spin](man/spin.txt), [crash](man/crash.txt), [badptr](man/badptr.txt),
[ping](man/ping.txt), [pingd](man/pingd.txt), [bench](man/bench.txt), [termtest](man/termtest.txt),
[priotest](man/priotest.txt), [svctest](man/svctest.txt), [sigtest](man/sigtest.txt).

## Run on AmigaOS, not on Zenix
[zenixboot](man/zenixboot.txt) starts Zenix from files on a hard disk, CF or SD card partition, so that no floppy is
needed (not one of the 117 programs: it is an AmigaOS program on the AmigaDOS boot partition).
[bootmenu](man/bootmenu.txt) is the boot menu of a disk with AmigaOS and Zenix on it (the SD card of
`tools/mkdualcard.sh`): it boots a chosen AmigaOS partition or runs zenixboot.

## How the pages get onto the system
`docs/man/` is the one place for manual pages. `tools/mkroot.sh` copies every `docs/man/*.txt` to `/usr/man` on the
root disk it builds; for a program that has no page there, `tools/mkman.py` still makes a minimal one from the
comment at the top of the program's source. The small RAM-disk image (`zenix-ram.adf`, the install/test floppy) is
built with `NOMAN=1` and carries no manual pages. `man -k` reads only the first 2 KB of each page.

## Keeping this in step with the code
Adding or changing a command means changing its page in the same commit. The rule and the check
(`tools/docs-check.sh`) are described in `../CLAUDE.md`.
