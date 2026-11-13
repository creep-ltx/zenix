# Command reference

Zenix has 153 programs (the `UAPPS` list in the Makefile; servers and test programs included), each with a manual
page in `docs/man/NAME.txt`; a few more pages are for names that start the same program (`gunzip`, `zcat`, `chgrp`,
`more`, `pkill`, `ifup`, `ifdown`, `unlzx`). The pages use the exact format that Zenix's own `man` reads (plain text of at most 78 columns, a
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
| [unlha](man/unlha.txt) ([unlzx](man/unlzx.txt)) | list, test and unpack LhA and LZX archives (DESIGN D33) |

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
| [less](man/less.txt) / [more](man/more.txt), [edit](man/edit.txt), [sumi](man/sumi.txt), [maki](man/maki.txt), [markdown](man/markdown.txt) | pagers and editors (the owner's: sumi the editor with tabs and colours for code, maki the pager with colours, pipes and directories, markdown the Markdown viewer and editor; `ports/`) |
| [zdesk](man/zdesk.txt), [term](man/term.txt) | Zenix Desktop (DESIGN D38): the floating window manager with its bar, workspaces and icons, typed after logging in as startx; the terminal window |
| [files](man/files.txt) | Zenix Desktop's file view (DESIGN D48): a folder in a window, Ahiru's keys (configurable) and the mouse; opens files in a terminal window, copies, moves, renames and deletes after asking |
| [deskprefs](man/deskprefs.txt), [conprefs](man/conprefs.txt) | preferences with Test, Use, Save and Cancel: the desktop's screen mode, font and colours (a window); the text console's colours and Unicode font (a text screen, root's) |
| [ahiru](man/ahiru.txt) | the owner's file manager in three columns: previews, marks, copy, move, delete, archives and disk images as folders, tabs, find (`ports/ahiru`) |
| [zine](man/zine.txt) | runs an AmigaOS program under ZINE, Zenix's own small AmigaOS (DESIGN D29; being built: real demos run in FS-UAE); an executable AmigaOS program also runs by its name |
| [ctop](man/ctop.txt) | the owner's monitor in the way of btop: memory, disks, the network's rates and every process in four boxes (`ports/ctop`) |
| [cu](man/cu.txt) | a terminal on another computer through the serial port (DESIGN D12) |
| [man](man/man.txt) | these manuals |

## The system
| Command | Summary |
|---|---|
| [sh](man/sh.txt) | the shell |
| [ps](man/ps.txt), [dmesg](man/dmesg.txt), [conmirror](man/conmirror.txt), [beep](man/beep.txt), [play](man/play.txt), [modplay](man/modplay.txt), [lsusb](man/lsusb.txt), [input](man/input.txt), [free](man/free.txt), [uptime](man/uptime.txt), [uname](man/uname.txt), [date](man/date.txt), [sleep](man/sleep.txt) | system state and time |
| [nice](man/nice.txt), [renice](man/renice.txt) | run a command at a priority, change a running one's |
| [kill](man/kill.txt), [pgrep](man/pgrep.txt) ([pkill](man/pkill.txt)), [time](man/time.txt), [timeout](man/timeout.txt), [watch](man/watch.txt), [cal](man/cal.txt), [mktemp](man/mktemp.txt), [realpath](man/realpath.txt) | processes, timing, calendar, temporary files, real paths |
| [env](man/env.txt), [printenv](man/printenv.txt), [which](man/which.txt), [whoami](man/whoami.txt), [id](man/id.txt), [hostname](man/hostname.txt), [clear](man/clear.txt), [true](man/true.txt), [false](man/false.txt) | small utilities |
| [login](man/login.txt), [su](man/su.txt), [passwd](man/passwd.txt), [sha256sum](man/sha256sum.txt) | users: logging in on a console, switching user, passwords, checksums |
| [useradd](man/useradd.txt), [userdel](man/userdel.txt), [gpasswd](man/gpasswd.txt) | add and remove users, group members (root) |
| [sudo](man/sudo.txt) | run one command as root: members of the group wheel, with their own password |
| [dd](man/dd.txt), [fdisk](man/fdisk.txt), [mkfs.ext2](man/mkfs.ext2.txt), [fsck.ext2](man/fsck.ext2.txt), [mkfs.zenixfs](man/mkfs.zenixfs.txt), [fsck.zenixfs](man/fsck.zenixfs.txt), [sync](man/sync.txt), [reboot](man/reboot.txt) (bootamigaos, halt, poweroff, shutdown) | disks |
| [zinstall](man/zinstall.txt) | put Zenix on a hard disk, CF or SD card, from the install floppies or an existing Zenix (DESIGN D35) |
| [zup](man/zup.txt) | install the root half of an update (`/sbin/zup`, DESIGN D15); one package at a time for zpkg (`-f`, `-r`, D22) |
| [zpkg](man/zpkg.txt) | the package manager: `-Syu`, `-S`, `-R`, `-Q`, `-U`, signed lists and packages over HTTP; writes a system update onto the boot partition itself (DESIGN D22) |
| [zpkg-new](man/zpkg-new.txt) | zpkg in a live package of its own, for a system whose zpkg cannot write system updates (2026103000): `zpkg-new -Syu` once (D22 4.5) |
| [demo](man/demo.txt) | not a command: the demo floppy (DD and HD): what is on it, how it loads, what it never writes (DESIGN D25) |
| [random](man/random.txt) | not a command: `/dev/random` and `/dev/urandom`, the kernel's random numbers, and the seed file (DESIGN D19) |
| [rescue](man/rescue.txt) | not a command: what to do when the disk check at boot cannot repair the root (the four choices boot offers, step by step) |
| [fatd](man/fatd.txt) | not a command: a FAT mount's server (`/sbin/fatd`, DESIGN D30), started by the kernel for `mount -t fat`; its manual is mount's |
| [ffsd](man/ffsd.txt) | not a command: an OFS or FFS mount's server (`/sbin/ffsd`, DESIGN D31), started by the kernel for `mount -t ffs`; its manual is mount's |
| [pfs3d](man/pfs3d.txt) | not a command: a PFS3 mount's server, read-only (`/sbin/pfs3d`, DESIGN D32), started by the kernel for `mount -t pfs3`; its manual is mount's |
| [exfatd](man/exfatd.txt) | not a command: an exFAT mount's server (a USB stick), read-only (`/sbin/exfatd`, DESIGN D47), started by the kernel for `mount -t exfat`; its manual is mount's |
| [iso9660d](man/iso9660d.txt) | not a command: an ISO 9660 mount's server (a CD-ROM), read-only (`/sbin/iso9660d`, DESIGN D44), started by the kernel for `mount -t iso9660`; its manual is mount's |
| [mount](man/mount.txt), [umount](man/umount.txt) | mounting more than the root filesystem (its server is a service: the kernel's fsd, zfsd, or nfscd for a NAS's folder over NFS, DESIGN D23)|

## The network
| Command | Summary |
|---|---|
| [ifconfig](man/ifconfig.txt) ([ifup](man/ifup.txt), [ifdown](man/ifdown.txt)) | the network's interfaces: show them, bring them up and down, an address; `/etc/network/interfaces` |
| [route](man/route.txt) | show the routes, set or remove the default gateway |
| [ping](man/ping.txt) | ICMP echo, no root needed |
| [netstat](man/netstat.txt) | the network's connections and listening sockets (`-tuwla`) |
| [host](man/host.txt) | look a name up (`/etc/hosts`, then DNS) |
| [telnet](man/telnet.txt) | a terminal to another computer over the network: Amiga and PC boards, Unix hosts, Zmodem, a phone book (DESIGN D20) |
| [rz, sz](man/rz.txt) | receive and send files with Zmodem over standard input and output (DESIGN D20) |
| [ssh](man/ssh.txt) | a secure shell on another computer (OpenSSH's sshd): keys, known hosts, ~. (DESIGN D21) |
| [ssh-keygen](man/ssh-keygen.txt) | make the Amiga's ssh key; fingerprints, public key lines, known_hosts entries removed (DESIGN D21) |
| [wget](man/wget.txt) | fetch files over HTTP and HTTPS: certificates checked, redirects, -c resume (DESIGN D21) |
| [sntp](man/sntp.txt) | set the clock from the network, the certificate time's floor (root; DESIGN D21) |
| [netd](man/netd.txt) | not a command: the network server (`/sbin/netd`, DESIGN D19), started at the first socket call |
| [dlogin](man/dlogin.txt) | not a command: the graphical login (`/sbin/dlogin`, DESIGN D38 revision 3), started by init on console 1 with /etc/console's `login graphical` |
| [sndd](man/sndd.txt) | not a command: the sound server (`/sbin/sndd`, DESIGN D40, port `audio`), started by the first call (play, modplay) |
| [usbd](man/usbd.txt) | not a command: the USB server (`/sbin/usbd`, DESIGN D46, port `usb`): a Pi 4's or CM4's USB port under Emu68, hubs, keyboards and mice; started by `lsusb`, kept running by `lsusb --hold` |
| [displayd](man/displayd.txt) | not a command: the display server of Zenix Desktop (`/sbin/displayd`, DESIGN D38), started by the desktop's first program |
| [a2065](man/a2065.txt) | not a command: the Commodore A2065 Ethernet board's driver (`/sbin/net/a2065`), started by netd |
| [genet](man/genet.txt) | not a command: the Ethernet driver of a Raspberry Pi CM4 or Pi 4 under Emu68 (`/sbin/net/genet`), started by netd |
| [ne2000](man/ne2000.txt) | not a command: the driver of an NE2000 PC Card (the Netgear FA411) in the A600/A1200's slot (`/sbin/net/ne2000`), started by netd |
| [plipbox](man/plipbox.txt) | not a command: the driver of a plipbox, Ethernet through the parallel port (`/sbin/net/plipbox`), started by netd |
| [nfscd](man/nfscd.txt) | not a command: an NFS mount's server (`/sbin/nfscd`, DESIGN D23), started by the kernel for `mount -t nfs`; its manual is mount's |
| [wasabid](man/wasabid.txt) | not a command: the remote control daemon (`/sbin/wasabid`, DESIGN D24), the owner's Wasabi protocol on TCP 1236; off until root writes `/etc/wasabid.conf`, kept running by init through `/etc/daemons` |

## Internals and test programs
[init](man/init.txt), [boot](man/boot.txt), [diskd](man/diskd.txt) (the built-in start-up programs);
[hello](man/hello.txt), [spin](man/spin.txt), [crash](man/crash.txt), [badptr](man/badptr.txt),
[msgping](man/msgping.txt), [msgpingd](man/msgpingd.txt), [bench](man/bench.txt), [termtest](man/termtest.txt),
[priotest](man/priotest.txt), [svctest](man/svctest.txt), [sigtest](man/sigtest.txt), [ptytest](man/ptytest.txt), [gcontest](man/gcontest.txt), [dpytest](man/dpytest.txt),
[socktest](man/socktest.txt), [socktestd](man/socktestd.txt) (the socket layer and its test server, DESIGN D19),
[nettest](man/nettest.txt) (the network over the loopback, D19), [cryptotest](man/cryptotest.txt) (the crypto library's known answers and times, D21).

## Run on AmigaOS, not on Zenix
[zenixboot](man/zenixboot.txt) starts Zenix from files on a hard disk, CF or SD card partition, so that no floppy is
needed (not one of the 153 programs: it is an AmigaOS program on the AmigaDOS boot partition).
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
