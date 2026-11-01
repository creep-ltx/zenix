#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Zenix update bundles and packages (DESIGN D15, docs/d15-self-updates.md; the formats in docs/architecture.md
section 3, "Updates"). The bundle and the manifest are frozen formats (CLAUDE.md): they only grow, at the end.

  mkzup.py packages BUILD "UAPPS"        the standard packages from a build: BUILD/pkg/NAME.manifest + NAME.tgz
                                         (zup and base, the base group; the families of FAMILIES and base-man,
                                         DESIGN D22 8.4.4; the version from kernel/version.h, or ZVNUM/ZVSTR)
  mkzup.py pkg OUTDIR NAME VNUM VSTR [--group G] [--dep NAME:VNUM]... [--desc TEXT] [--takes NAME]... ENTRY...
        one package from explicit entries (the tests): f:MODE:UID:GID:SRC:/PATH (a file), c:... (a config file the
        owner may edit), l:TARGET:/PATH (a symbolic link), d:MODE:UID:GID:/PATH (a directory)
  mkzup.py bundle OUT --kernel FILE [--version V --vstr S --date YYYYMMDD --channel C (default test; release channels: testing, stable) --min V] PKGDIR NAME...
        a bundle: the kernel as boot/zenix and the named packages (each pkg/NAME.manifest + pkg/NAME.tgz); the
        version defaults to the packages' and must be the one the kernel image carries (its "ZVER" record);
        --base-only refuses a package that is not in the group "base" (DESIGN D22 3.1: a system bundle carries the
        base group only; tools/mkrepo.py always asks for it)
  mkzup.py bundle OUT --no-kernel [--channel C --min V --date YYYYMMDD] PKGDIR NAME
        a package bundle (DESIGN D22 3.1): exactly one package, no kernel; the version and its string are the
        package's, the minimum base its "depends base" line's (else 0) unless --min says
  mkzup.py repack PKGDIR NAME /PATH=HOSTFILE...   rebuild package NAME with these files' contents (a new path is
                                         added as a file, mode 755) - for test bundles that change something
  mkzup.py check FILE                    every range and SHA-256 of a bundle; exit 1 with the reason if one is wrong
  mkzup.py list FILE                     the header and the parts
  mkzup.py installdb STAGE PKGDIR DEST   the manifests of the packages whose every entry is in the staging tree STAGE
                                         as the manifest says, into DEST (/var/lib/zpkg of a root image; mkroot.sh)

Manifest lines added by DESIGN D22 (growth; older readers ignore them): "desc TEXT" (one line, at most 72
printable characters) and "takes OTHER" (8.4.4), after the dependencies.

Bundle (big-endian): a 96-byte header ("ZUP1", format 1, header size 96, version number, version string[16], date,
minimum installed base version, channel[8], TOC offset, TOC entries, total size, flags 0, SHA-256 over header bytes
0-63 and the TOC), the TOC (80 bytes an entry: name[40], offset, size, SHA-256), then the parts, each at a 16-byte
boundary. Package: a manifest (text, "zpkg 1" first) and a tar.gz (ustar) of its files in manifest order.
"""
import gzip
import hashlib
import io
import os
import re
import stat
import struct
import sys
import tarfile
import tempfile
import subprocess

HDR = 96
TOCE = 80
MAXPARTS = 64
MAXSIZE = 16 << 20                       # zenixboot refuses a bigger bundle
PATHMAX = 55                             # a path plus ".zup-new" fits zup's 63 (user/zpkgfmt.h, ZF_PMAX), which older zups have
NAME_RE = re.compile(rb'^(boot|pkg)/[A-Za-z0-9._-]+$')
PATH_RE = re.compile(r'^/[A-Za-z0-9._+/-]+$')
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def die(msg):
    sys.stderr.write("mkzup: %s\n" % msg)
    sys.exit(1)


def version_h():
    """the number and string of kernel/version.h, unless ZVNUM / ZVSTR are set (a test build's own)"""
    t = open(os.path.join(ROOT, "kernel", "version.h")).read()
    n = re.search(r'#define ZENIX_VNUM (\d+)u', t).group(1)
    s = re.search(r'#define ZENIX_VSTR "([^"]*)"', t).group(1)
    return int(os.environ.get("ZVNUM") or n), os.environ.get("ZVSTR") or s


def sha(data):
    return hashlib.sha256(data).hexdigest()


def check_path(p):
    if p == "/var/lib/random-seed":     # DESIGN D19 9.3: a seed shipped in a bundle would be every machine's
        die("refusing to put a random seed (%s) in a package" % p)
    if (not PATH_RE.match(p) or '//' in p or '/./' in p or '/../' in p or p.endswith('/') or p.endswith('/.') or
            p.endswith('/..') or len(p) > PATHMAX):
        die("bad path %r (absolute, [A-Za-z0-9._+/-], at most %d bytes)" % (p, PATHMAX))
    if p.endswith((".zup-new", ".new", ".save")):   # zup's own names beside a path (user/zpkgfmt.c zf_reserved)
        die("%s: .zup-new, .new and .save are names zup keeps for itself" % p)


# ---- packages ----
NAME_PKG_RE = re.compile(r'^[a-z0-9][a-z0-9.+-]{0,23}$')
DESC_RE = re.compile(r'^[\x20-\x7e]{1,72}$')
BASE_GROUP = "base"


def make_package(outdir, name, vnum, vstr, entries, group=None, deps=(), desc=None, takes=()):
    """entries: (kind, mode, uid, gid, src or link target, path); kind f c l d. Writes NAME.manifest and NAME.tgz."""
    if not NAME_PKG_RE.match(name):
        die("bad package name %r" % name)
    lines = ["zpkg 1", "name %s" % name, "version %d %s" % (vnum, vstr)]
    if group:
        lines.append("group %s" % group)
    for d, v in deps:
        lines.append("depends %s %d" % (d, v))
    if desc is not None:
        if not DESC_RE.match(desc) or desc != desc.strip():
            die("%s: bad desc %r (one line, 1-72 printable characters, D22 3.3)" % (name, desc))
        lines.append("desc %s" % desc)
    for t in takes:
        if not NAME_PKG_RE.match(t) or t == name:
            die("%s: bad takes %r" % (name, t))
        if group == BASE_GROUP:
            die("%s: a base-group package never takes another's files (D22 8.4.4)" % name)
        lines.append("takes %s" % t)
    tbuf = io.BytesIO()
    tar = tarfile.open(fileobj=tbuf, mode="w", format=tarfile.USTAR_FORMAT)
    seen = set()
    for kind, mode, uid, gid, src, path in entries:
        check_path(path)
        if path in seen:
            die("%s: %s twice" % (name, path))
        seen.add(path)
        if kind == 'd':
            lines.append("d %o %d %d %s" % (mode, uid, gid, path))
        elif kind == 'l':
            if not re.match(r'^[A-Za-z0-9._+/-]+$', src) or len(src) > PATHMAX - 1 or src == "-":
                die("bad link target %r" % src)
            lines.append("l %s %s" % (src, path))
        elif kind in 'fc':
            data = open(src, "rb").read()
            lines.append("%s %o %d %d %s %d %s" % (kind, mode, uid, gid, sha(data), len(data), path))
            ti = tarfile.TarInfo(path[1:])
            ti.size = len(data)
            ti.mode = mode
            ti.uid, ti.gid = uid, gid
            ti.uname = ti.gname = ""
            ti.mtime = 0
            tar.addfile(ti, io.BytesIO(data))
        else:
            die("unknown entry kind %r" % kind)
    tar.close()
    man = ("\n".join(lines) + "\n").encode()
    os.makedirs(outdir, exist_ok=True)
    with open(os.path.join(outdir, name + ".manifest"), "wb") as f:
        f.write(man)
    with open(os.path.join(outdir, name + ".tgz"), "wb") as f:
        with gzip.GzipFile(fileobj=f, mode="wb", compresslevel=9, mtime=0, filename="") as g:
            g.write(tbuf.getvalue())


def parse_manifest(text):
    """-> dict(name, vnum, vstr, group, deps[(name, v)], entries[(kind, mode, uid, gid, sha, size, path, target)])"""
    lines = text.decode().split("\n")
    if lines[0] != "zpkg 1":
        raise ValueError("not a zpkg 1 manifest")
    m = {"deps": [], "entries": [], "group": None, "desc": None, "takes": []}
    for ln in lines[1:]:
        if not ln:
            continue
        w = ln.split(" ")
        if w[0] == "name":
            m["name"] = w[1]
        elif w[0] == "version":
            m["vnum"], m["vstr"] = int(w[1]), w[2]
        elif w[0] == "group":
            m["group"] = w[1]
        elif w[0] == "depends":
            m["deps"].append((w[1], int(w[2])))
        elif w[0] == "desc":
            m["desc"] = ln[5:]
        elif w[0] == "takes":
            m["takes"].append(w[1])
        elif w[0] in ("f", "c"):
            m["entries"].append((w[0], int(w[1], 8), int(w[2]), int(w[3]), w[4], int(w[5]), w[6], None))
        elif w[0] == "d":
            m["entries"].append(("d", int(w[1], 8), int(w[2]), int(w[3]), None, 0, w[4], None))
        elif w[0] == "l":
            m["entries"].append(("l", 0o777, 0, 0, None, 0, w[2], w[1]))
        # other words: a later format's additions (append-only), ignored
    return m


# The standard packages (section 4 of the design note). Ownership and modes as tools/mkroot.sh gives them on the
# images (root owns everything; su, sudo, passwd and zine set-user-id root; /etc/passwd and /etc/group 644).
BASE_PROGS = ["init", "login", "sh", "mount"]
BASE_CONFIG = ["svctab", "passwd", "group", "fstab", "motd", "console", "hosts", "daemons"]   # /etc/autologin is the test images' own: never shipped
SETUID = {"su", "sudo", "passwd", "zine"}
LINKS = [("less", "/bin/more"), ("gzip", "/bin/gunzip"), ("gzip", "/bin/zcat"), ("chown", "/bin/chgrp"),
         ("pgrep", "/bin/pkill"), ("reboot", "/bin/bootamigaos"), ("ifconfig", "/bin/ifup"), ("ifconfig", "/bin/ifdown"),
         ("rz", "/bin/sz"), ("unlha", "/bin/unlzx")]
SBIN = {"zup"}
OWN_PKG = {"zpkg"}                        # a base-group package of its own (DESIGN D22 step 5): /bin/zpkg, /etc/zpkg.conf
# The bootstrap (DESIGN D22 4.5, as built 2026-10-31): a base whose zpkg cannot write the boot partition (2026103000,
# the first one the owner runs) installs nothing of the base group live, so this build's zpkg also comes as a live
# package, zpkg-new (/bin/zpkg-new), pinned to that older base (tools/mkrepo.py keeps its "depends base"). In the
# repository (not in the build, so an image's database never names a package the image lacks) sysutils depends on it
# (BOOTSTRAP_DEP, added by tools/mkrepo.py add), so the old zpkg's -Syu brings it. Then "zpkg-new -Syu" writes the
# system update; after the restart the base's own /bin/zpkg can. NAME: the oldest base it must install on. Both go
# when no machine runs a base without the writer any more (the owner's word).
BOOTSTRAP = {"zpkg-new": 2026103000}
BOOTSTRAP_DEP = {"sysutils": "zpkg-new"}
# The packages by family (DESIGN D22 8.4.4, owner decision 2): every command outside the base group belongs to exactly
# one family (a program in UAPPS that none names is refused: add it here), with its links and its manual pages. Each
# family "takes" the old one-piece packages utils and man, so a machine installed before the split moves over one
# family at a time, live (zup -f), after the base-only system update that brought the takes-aware zup. The pages of
# the base group's programs and the pages of no program go to base-man (not in the base group: it takes from man).
FAMILIES = [
    ("coreutils", "basic file and directory commands", "ls cat cp mv rm mkdir rmdir touch ln readlink chmod chown stat "
                  "pwd basename dirname realpath mktemp du df tree"),
    ("findutils", "find files and run commands on lists of them", "find xargs"),
    ("archive", "tar and gzip archives, unpacking LhA and LZX", "tar gzip unlha"),
    ("grep", "search text (POSIX extended expressions)", "grep"),
    ("sed", "the stream editor", "sed"),
    ("awk", "pattern scanning and text processing (POSIX awk)", "awk"),
    ("textutils", "filters for text: sort, cut, head, wc and others", "sort uniq cut tr tee head tail tac rev nl wc "
                  "fold expand paste comm"),
    ("diffutils", "compare and dump files", "diff cmp hexdump"),
    ("hashutils", "checksums and encodings", "base64 md5sum sha256sum"),
    ("shellutils", "small commands for the shell and scripts", "echo printf seq yes expr true false env printenv which "
                   "sleep date cal clear time timeout watch"),
    ("sysinfo", "who and what: uname, id, hostname, uptime", "uname whoami id hostname uptime"),
    ("procps", "processes and memory: ps, kill, nice", "ps free kill pgrep nice renice"),
    ("sysutils", "the console, the kernel log, the disks' sync, restart", "dmesg input beep sync reboot"),
    ("pager", "less and man, the pager and the manual reader", "less man"),
    ("edit", "the text editor", "edit"),
    ("sumi", "Sumi, a text editor: tabs, colours for code, undo, find and replace", "sumi"),
    ("maki", "Maki, a pager: colours, search, pipes, directories, line numbers", "maki"),
    ("markdown", "Markdown, a viewer and editor for Markdown files", "markdown"),
    ("ahiru", "Ahiru, a file manager: three columns, previews, archives, copy, move", "ahiru"),
    ("ctop", "ctop, a monitor in the way of btop: memory, disks, network, processes", "ctop"),
    ("shadow", "users and passwords: su, sudo, passwd, useradd", "su sudo passwd useradd userdel gpasswd"),
    ("disktools", "disks and filesystems: fdisk, mkfs, fsck, dd, umount", "dd fdisk mkfs_ext2 fsck_ext2 mkfs_zenixfs "
                  "fsck_zenixfs umount"),
    ("cu", "a terminal on another computer through the serial port", "cu"),
    ("zmodem", "receive and send files with Zmodem (rz, sz)", "rz"),
    ("nettools", "the network's interfaces, routes, ping, netstat, host", "ifconfig route ping netstat host"),
    ("telnet", "a terminal to another computer over the network", "telnet"),
    ("ssh", "the secure shell and its keys", "ssh ssh-keygen"),
    ("wget", "fetch files over HTTP and HTTPS", "wget"),
    ("sntp", "set the clock from the network", "sntp"),
    ("zine", "ZINE: AmigaOS programs on Zenix (zine PROGRAM)", "zine"),
    ("zinstall", "the installer: put Zenix on a disk; the boot partition's programs", "zinstall"),
    ("zenix-tests", "Zenix's test programs and the start-up programs' copies", "hello spin crash badptr msgpingd msgping "
                    "socktestd socktest bench termtest priotest svctest sigtest nettest cryptotest boot diskd"),
]
BASE_SBIN = ["zfsd", "netd", "nfscd"]              # not in UAPPS (not commands): build/NAME.zx, installed as /sbin/NAME
# the licence notices of the code from other projects (owner decision, DESIGN D19 2026-10-27), as tools/mkroot.sh
LICENSES = [("net/lwip/COPYING", "lwIP"), ("user/miniz/LICENSE", "miniz"), ("tools/font_8x8.COPYING", "font_8x8"),
            ("user/xfer/LICENSE", "zmodem"), ("crypto/bearssl/LICENSE", "BearSSL"), ("crypto/monocypher/LICENCE", "Monocypher"),
            ("tools/certdata.COPYING", "certdata"), ("crypto/openssh/LICENCE", "OpenSSH-bcrypt"),
            ("libc/picolibc/COPYING.picolibc", "picolibc")]
# the notices of code from other projects that only one family's programs carry (as tools/mkroot.sh puts them)
# (the text-UI library's notice once a package: sumi's is "tui" since it shipped first, the others' "PROG-tui"; two
# packages may not own one file, zpkg 4.3)
# files beside a family's programs, from the build directory (DESIGN D29: ZINE's blob, the small AmigaOS zine loads)
FAMILY_SBIN = {"disktools": ["fatd", "ffsd", "pfs3d"]}     # DESIGN D30, D31, D32: mount's servers for a FAT volume, an OFS or FFS volume, a PFS3 volume
# (DESIGN D35: the two AmigaOS programs of a boot partition, what zinstall puts there)
FAMILY_FILES = {"zine": [("zine.blob", "/lib/zine/zine.blob")],
                "zinstall": [("bootmenu", "/usr/lib/zinstall/bootmenu"), ("zenixboot", "/usr/lib/zinstall/zenixboot")]}
FAMILY_LICENSES = {"sumi": [("ports/sumi/upstream/LICENSE", "sumi"), ("ports/tui/upstream/LICENSE", "tui")],
                   "maki": [("ports/maki/upstream/LICENSE", "maki"), ("ports/tui/upstream/LICENSE", "maki-tui")],
                   "markdown": [("ports/markdown/upstream/LICENSE", "markdown"),
                                ("ports/tui/upstream/LICENSE", "markdown-tui")],
                   "ahiru": [("ports/ahiru/upstream/LICENSE", "ahiru"), ("ports/tui/upstream/LICENSE", "ahiru-tui")],
                   "ctop": [("ports/ctop/upstream/LICENSE", "ctop"), ("ports/tui/upstream/LICENSE", "ctop-tui")]}
ANCHORS = "anchors"                       # DESIGN D21 8.1: build/anchors (tools/mkanchors.py) as /etc/ssl/anchors
FONT = "cp437-8x8.psf"                    # DESIGN D20 4.2: build/cp437-8x8.psf as /usr/share/consolefonts/cp437-8x8.psf
UFONT = "zenix-8x8.zcf"                   # DESIGN D28 6: the Unicode font init loads at boot (/etc/console's font line)


def binname(p):
    return re.sub(r'^fsck_', 'fsck.', re.sub(r'^mkfs_', 'mkfs.', p))


def std_packages(build, uapps):
    vnum, vstr = version_h()
    out = os.path.join(build, "pkg")
    zx = lambda p: os.path.join(build, p + ".zx")
    progs = uapps.split()
    for p in BASE_PROGS:
        if p not in progs:
            die("%s is not in UAPPS" % p)
    dirs = [("d", 0o755, 0, 0, None, d) for d in ("/var", "/var/lib", "/var/lib/zpkg", "/var/cache", "/var/cache/zpkg")]
    names = ["base"]
    if "zup" in progs:
        make_package(out, "zup", vnum, vstr, [("d", 0o755, 0, 0, None, "/sbin"),
                                              ("f", 0o755, 0, 0, zx("zup"), "/sbin/zup")], group="base")
        names.insert(0, "zup")
    if "zpkg" in progs:
        make_package(out, "zpkg", vnum, vstr, [("f", 0o755, 0, 0, zx("zpkg"), "/bin/zpkg"),
                                               ("c", 0o644, 0, 0, os.path.join(ROOT, "rootfs", "etc", "zpkg.conf"), "/etc/zpkg.conf")],
                     group="base", deps=[("zup", vnum)] if "zup" in progs else [])
    base = dirs + [("d", 0o700, 0, 0, None, "/root")] + [("f", 0o755, 0, 0, zx(p), "/bin/" + p) for p in BASE_PROGS]
    # the servers that live on the disk (DESIGN D19: /sbin/zfsd, which mount starts for a ZenixFS volume; /sbin/netd,
    # which the kernel starts at the first socket call)
    base += [("d", 0o755, 0, 0, None, "/sbin")] + [("f", 0o755, 0, 0, zx(p), "/sbin/" + p) for p in BASE_SBIN]
    # the network's drivers (DESIGN D19 step 7: the Makefile's NETDRVS, built as drv-NAME), /sbin/net/NAME
    drvs = os.environ.get("NETDRVS", "").split()
    if drvs:
        base += [("d", 0o755, 0, 0, None, "/sbin/net")] + [("f", 0o755, 0, 0, zx("drv-" + d), "/sbin/net/" + d) for d in drvs]
    base += [("d", 0o755, 0, 0, None, d) for d in ("/usr", "/usr/share", "/usr/share/licenses")]
    base += [("f", 0o644, 0, 0, os.path.join(ROOT, f), "/usr/share/licenses/" + n) for f, n in LICENSES]
    base += [("d", 0o755, 0, 0, None, "/usr/share/consolefonts"),
             ("f", 0o644, 0, 0, os.path.join(build, FONT), "/usr/share/consolefonts/" + FONT),
             ("f", 0o644, 0, 0, os.path.join(build, UFONT), "/usr/share/consolefonts/" + UFONT)]
    base += [("d", 0o755, 0, 0, None, "/etc/ssl"), ("d", 0o755, 0, 0, None, "/etc/ssl/local"),
             ("f", 0o644, 0, 0, os.path.join(build, ANCHORS), "/etc/ssl/" + ANCHORS)]
    base += [("c", 0o644, 0, 0, os.path.join(ROOT, "rootfs", "etc", c), "/etc/" + c) for c in BASE_CONFIG]
    if os.environ.get("ZENIX_RELEASE"):
        rel = os.path.join(build, "passwd.release")
        with open(rel, "w") as f:
            f.write(release_passwd())
        base = [e[:4] + (rel, e[5]) if e[5] == "/etc/passwd" else e for e in base]
    make_package(out, "base", vnum, vstr, base, group="base", deps=[("zup", vnum)] if "zup" in names else [])
    if "zpkg" in progs:
        names.append("zpkg")
    # the manual pages, as mkroot.sh installs them: docs/man, and a generated page for a program without one
    with tempfile.TemporaryDirectory() as t:
        subprocess.run([sys.executable, os.path.join(HERE, "mkman.py"), os.path.join(t, "gen")] + progs, check=True,
                       stdout=subprocess.DEVNULL)
        pages = {}
        for d in (os.path.join(t, "gen"), os.path.join(ROOT, "docs", "man")):
            for f in os.listdir(d):
                if f.endswith(".txt"):
                    pages[f] = os.path.join(d, f)
        mandirs = [("d", 0o755, 0, 0, None, "/usr"), ("d", 0o755, 0, 0, None, "/usr/man")]
        fam_of = {}
        for fam, _, members in FAMILIES:
            for p in members.split():
                if p in fam_of:
                    die("%s is in two families (%s, %s)" % (p, fam_of[p], fam))
                fam_of[p] = fam
        rest = [p for p in progs if p not in BASE_PROGS and p not in SBIN and p not in OWN_PKG and p not in fam_of]
        if rest:
            die("programs in no family (tools/mkzup.py FAMILIES): %s" % " ".join(rest))
        taken = set()
        for fam, desc, members in FAMILIES:
            mine = [p for p in members.split() if p in progs]
            if not mine:
                continue
            ents = [("f", 0o4755 if p in SETUID else 0o755, 0, 0, zx(p), "/bin/" + binname(p)) for p in mine]
            for p in FAMILY_SBIN.get(fam, []):           # a server beside the family's programs (not a command)
                if os.path.exists(zx(p)):
                    if not any(e[0] == "d" and e[5] == "/sbin" for e in ents):
                        ents.append(("d", 0o755, 0, 0, None, "/sbin"))
                    ents.append(("f", 0o755, 0, 0, zx(p), "/sbin/" + p))
            links = [(t, p) for t, p in LINKS if t in mine]
            ents += [("l", 0o777, 0, 0, t, p) for t, p in links]
            mypages = sorted({binname(p) + ".txt" for p in mine} | {os.path.basename(p) + ".txt" for t, p in links})
            mypages = [f for f in mypages if f in pages]
            taken.update(mypages)
            if mypages:
                ents += mandirs + [("f", 0o644, 0, 0, pages[f], "/usr/man/" + f) for f in mypages]
            if FAMILY_LICENSES.get(fam):
                have = {e[5] for e in ents}
                ents += [("d", 0o755, 0, 0, None, d) for d in ("/usr", "/usr/share", "/usr/share/licenses") if d not in have]
                ents += [("f", 0o644, 0, 0, os.path.join(ROOT, f), "/usr/share/licenses/" + n)
                         for f, n in FAMILY_LICENSES[fam]]
            for f, dst in FAMILY_FILES.get(fam, []):
                have = {e[5] for e in ents}
                parts = dst.split("/")[1:-1]
                ents += [("d", 0o755, 0, 0, None, d) for d in ("/" + "/".join(parts[:i + 1]) for i in range(len(parts)))
                         if d not in have]
                ents += [("f", 0o644, 0, 0, os.path.join(build, f), dst)]
            make_package(out, fam, vnum, vstr, ents, deps=[("base", vnum)], desc=desc, takes=("utils", "man"))
            names.append(fam)
        if "zpkg" in progs:                 # the bootstrap package (BOOTSTRAP above): this build's zpkg, under its own name
            # (its page goes with base-man: an image has the page but not this package, so it may not own it)
            ents = [("f", 0o755, 0, 0, zx("zpkg"), "/bin/zpkg-new")]
            make_package(out, "zpkg-new", vnum, vstr, ents, deps=[("base", BOOTSTRAP["zpkg-new"])],
                         desc="zpkg that writes the system update itself, for a system older than that")
            names.append("zpkg-new")
        # the remote control daemon (DESIGN D24, /sbin/wasabid): a package of its own, off until root writes
        # /etc/wasabid.conf and its line in /etc/daemons; not a command (SBINAPPS), so in no family
        if os.path.exists(zx("wasabid")):
            ents = [("d", 0o755, 0, 0, None, "/sbin"), ("f", 0o755, 0, 0, zx("wasabid"), "/sbin/wasabid")]
            if "wasabid.txt" in pages:
                ents += mandirs + [("f", 0o644, 0, 0, pages["wasabid.txt"], "/usr/man/wasabid.txt")]
                taken.add("wasabid.txt")
            make_package(out, "wasabid", vnum, vstr, ents, deps=[("base", vnum)],
                         desc="the remote control daemon (the Wasabi protocol), off by default",
                         takes=("utils", "man"))      # as every family: the split's rule (D22 8.4.4)
            names.append("wasabid")
        rest = sorted(f for f in pages if f not in taken)
        make_package(out, "base-man", vnum, vstr, mandirs + [("f", 0o644, 0, 0, pages[f], "/usr/man/" + f) for f in rest],
                     deps=[("base", vnum)], desc="the manual pages of the base system and of what is not a command",
                     takes=("man",))
        names.append("base-man")
    for f in os.listdir(out):           # a package an older build made and this one does not (utils, man): gone
        n, ext = os.path.splitext(f)
        if ext in (".manifest", ".tgz") and n not in names:
            os.unlink(os.path.join(out, f))
    open(os.path.join(out, "list"), "w").write(" ".join(names) + "\n")
    print("%s: packages %s, version %d (%s)" % (out, " ".join(names), vnum, vstr))


# ---- bundles ----
def kernel_version(img):
    i = img.find(b"ZVER")
    while i >= 0:
        if i + 24 <= len(img):
            return struct.unpack(">I", img[i + 4:i + 8])[0], img[i + 8:i + 24].split(b"\0")[0].decode()
        i = img.find(b"ZVER", i + 1)
    return None, None


def make_bundle(out, kernel, pkgdir, names, vnum=None, vstr=None, date=None, channel="test", minv=None, nocheck=False,
                base_only=False):
    """kernel None: a package bundle (D22 3.1), exactly one package"""
    parts = []
    kimg = None
    if kernel is not None:
        kimg = open(kernel, "rb").read()
        parts.append((b"boot/zenix", kimg))
    elif len(names) != 1:
        die("a package bundle (--no-kernel) holds exactly one package (D22 3.1)")
    mans = []
    for n in names:
        man = open(os.path.join(pkgdir, n + ".manifest"), "rb").read()
        mans.append(parse_manifest(man))
        if base_only and kimg is not None and mans[-1]["group"] != BASE_GROUP:
            die("%s is not in the group base: a system bundle carries the base group only (D22 3.1)" % n)
        parts.append((("pkg/%s.manifest" % n).encode(), man))
        parts.append((("pkg/%s.tgz" % n).encode(), open(os.path.join(pkgdir, n + ".tgz"), "rb").read()))
    if kimg is None:
        m = mans[0]
        if vnum is None:
            vnum = m["vnum"]
        if vstr is None:
            vstr = m["vstr"]
        if minv is None:
            minv = dict(m["deps"]).get(BASE_GROUP, 0)
    if vnum is None:
        vnum = max([m["vnum"] for m in mans] or [version_h()[0]])
    if vstr is None:
        vstr = ([m["vstr"] for m in mans if m["vnum"] == vnum] or [version_h()[1]])[0]
    if kimg is not None:
        kv, _ = kernel_version(kimg)
        if kv != vnum and not nocheck:
            die("the kernel carries version %s, the bundle is %d (build it with the same version)" % (kv, vnum))
    if date is None:
        date = vnum // 100
    blob = bundle_bytes(parts, vnum, vstr, date, minv or 0, channel)
    with open(out, "wb") as f:
        f.write(blob)
    print("%s: bundle %d (%s), %d parts, %d bytes" % (out, vnum, vstr, len(parts), len(blob)))


def bundle_bytes(parts, vnum, vstr, date, minv, channel):
    """parts [(name bytes, data)] -> the bundle's bytes (the layout in the module's text)"""
    if len(vstr.encode()) > 15 or len(channel.encode()) > 7:
        die("version string %r or channel %r too long" % (vstr, channel))
    toc_size = TOCE * len(parts)
    off = (HDR + toc_size + 15) & ~15
    toc = b""
    body = b""
    for name, data in parts:
        if len(name) >= 40:
            die("part name too long: %s" % name)
        toc += struct.pack(">40sII32s", name, off, len(data), hashlib.sha256(data).digest())
        pad = (-len(data)) & 15
        body += data + b"\0" * pad
        off += len(data) + pad
    total = (HDR + toc_size + 15) & ~15
    total += len(body)
    head = struct.pack(">4sIII16sII8sIIII", b"ZUP1", 1, HDR, vnum, vstr.encode(), date, minv, channel.encode(),
                       HDR, len(parts), total, 0)
    assert len(head) == 64
    digest = hashlib.sha256(head + toc).digest()
    blob = head + digest + toc
    blob += b"\0" * (((HDR + toc_size + 15) & ~15) - len(blob)) + body
    assert len(blob) == total
    if total > MAXSIZE:
        die("the bundle is %d bytes, more than %d" % (total, MAXSIZE))
    return blob


def read_bundle(data):
    """parse and check everything (the same checks as zup and zenixboot); -> (header dict, [(name, data)])"""
    if len(data) < HDR:
        raise ValueError("shorter than the header")
    magic, fmt, hsz, vnum, vstr, date, minv, chan, tocoff, ntoc, total, flags = struct.unpack(">4sIII16sII8sIIII", data[:64])
    if magic != b"ZUP1" or fmt != 1 or hsz != HDR:
        raise ValueError("not a ZUP1 bundle (format 1)")
    if total != len(data) or total > MAXSIZE:
        raise ValueError("the size is %d, the header says %d" % (len(data), total))
    if tocoff != HDR or ntoc < 1 or ntoc > MAXPARTS:
        raise ValueError("a bad table of contents (offset %d, %d entries)" % (tocoff, ntoc))
    tend = HDR + TOCE * ntoc
    if tend > total:
        raise ValueError("the table of contents goes past the end")
    if hashlib.sha256(data[:64] + data[HDR:tend]).digest() != data[64:96]:
        raise ValueError("the table of contents' SHA-256 does not match")
    parts = []
    for i in range(ntoc):
        name, off, size, digest = struct.unpack(">40sII32s", data[HDR + TOCE * i:HDR + TOCE * (i + 1)])
        if b"\0" not in name:
            raise ValueError("part %d: its name is not terminated" % i)
        name = name.split(b"\0")[0]
        if not NAME_RE.match(name) or b".." in name:
            raise ValueError("part %d: a bad name %r" % (i, name))
        if off < tend or off > total or size > total - off:
            raise ValueError("%s: outside the bundle" % name.decode())
        if hashlib.sha256(data[off:off + size]).digest() != digest:
            raise ValueError("%s: SHA-256 does not match" % name.decode())
        parts.append((name.decode(), data[off:off + size]))
    hdr = {"vnum": vnum, "vstr": vstr.split(b"\0")[0].decode(), "date": date, "min": minv,
           "channel": chan.split(b"\0")[0].decode(), "total": total}
    return hdr, parts


# ---- the package database of an image ----
def installdb(stage, pkgdir, dest):
    os.makedirs(dest, exist_ok=True)
    got = []
    for f in sorted(os.listdir(pkgdir)):
        if not f.endswith(".manifest"):
            continue
        man = open(os.path.join(pkgdir, f), "rb").read()
        m = parse_manifest(man)
        ok = True
        for kind, mode, uid, gid, digest, size, path, target in m["entries"]:
            p = stage + path
            if kind == 'd':
                ok = os.path.isdir(p)
            elif kind == 'l':
                ok = os.path.islink(p) and os.readlink(p) == target
            else:
                ok = os.path.isfile(p) and not os.path.islink(p) and sha(open(p, "rb").read()) == digest
            if not ok:
                break
        if ok:
            open(os.path.join(dest, f), "wb").write(man)
            got.append(m["name"])
    return got


def repack(pkgdir, name, repl):
    m = parse_manifest(open(os.path.join(pkgdir, name + ".manifest"), "rb").read())
    tar = tarfile.open(fileobj=io.BytesIO(gzip.decompress(open(os.path.join(pkgdir, name + ".tgz"), "rb").read())))
    with tempfile.TemporaryDirectory() as t:
        ents = []
        for kind, mode, uid, gid, digest, size, path, target in m["entries"]:
            src = target
            if kind in "fc":
                src = repl.pop(path, None)
                if src is None:
                    src = os.path.join(t, str(len(ents)))
                    open(src, "wb").write(tar.extractfile(path[1:]).read())
            ents.append((kind, mode, uid, gid, src, path))
        for path, src in repl.items():
            ents.append(("f", 0o755, 0, 0, src, path))
        make_package(pkgdir, name, m["vnum"], m["vstr"], ents, m["group"], m["deps"], m["desc"], m["takes"])


def release_passwd():
    """/etc/passwd as released: root's home is /root (owner 2026-10-31). rootfs/ keeps / for the test builds, whose
    scripts wait for the prompt zenix:/# (ZENIX_RELEASE, set by the Makefile's RELEASE=1; users-test checks this line)."""
    with open(os.path.join(ROOT, "rootfs", "etc", "passwd")) as f:
        return f.read().replace("root::0:0:root:/:", "root::0:0:root:/root:", 1)

def main(a):
    if a == ["release-passwd"]:
        sys.stdout.write(release_passwd())
    elif len(a) >= 3 and a[0] == "packages":
        std_packages(a[1], a[2])
    elif len(a) >= 5 and a[0] == "pkg":
        out, name, vnum, vstr = a[1], a[2], int(a[3]), a[4]
        group, deps, ents, desc, takes = None, [], [], None, []
        i = 5
        while i < len(a):
            if a[i] == "--group":
                group = a[i + 1]; i += 2; continue
            if a[i] == "--desc":
                desc = a[i + 1]; i += 2; continue
            if a[i] == "--takes":
                takes.append(a[i + 1]); i += 2; continue
            if a[i] == "--dep":
                d, v = a[i + 1].split(":"); deps.append((d, int(v))); i += 2; continue
            w = a[i].split(":")
            if w[0] in ("f", "c"):
                ents.append((w[0], int(w[1], 8), int(w[2]), int(w[3]), w[4], w[5]))
            elif w[0] == "d":
                ents.append(("d", int(w[1], 8), int(w[2]), int(w[3]), None, w[4]))
            elif w[0] == "l":
                ents.append(("l", 0o777, 0, 0, w[1], w[2]))
            else:
                die("bad entry %r" % a[i])
            i += 1
        make_package(out, name, vnum, vstr, ents, group, deps, desc, takes)
    elif len(a) >= 3 and a[0] == "repack":
        repack(a[1], a[2], dict(x.split("=", 1) for x in a[3:]))
    elif len(a) >= 2 and a[0] == "bundle":
        out, kernel, opts, rest = a[1], None, {}, []
        i = 2
        while i < len(a):
            if a[i] in ("--kernel", "--version", "--vstr", "--date", "--channel", "--min"):
                opts[a[i][2:]] = a[i + 1]; i += 2
            elif a[i] in ("--no-version-check", "--no-kernel", "--base-only"):
                opts[a[i][2:]] = 1; i += 1
            else:
                rest.append(a[i]); i += 1
        if ("kernel" in opts) == ("no-kernel" in opts) or not rest:
            die("bundle OUT --kernel FILE PKGDIR NAME...  or  bundle OUT --no-kernel PKGDIR NAME")
        make_bundle(out, opts.get("kernel"), rest[0], rest[1:], int(opts["version"]) if "version" in opts else None,
                    opts.get("vstr"), int(opts["date"]) if "date" in opts else None, opts.get("channel", "test"),
                    int(opts["min"]) if "min" in opts else None, "no-version-check" in opts, "base-only" in opts)
    elif len(a) == 2 and a[0] in ("check", "list"):
        try:
            hdr, parts = read_bundle(open(a[1], "rb").read())
        except ValueError as e:
            print("%s: refused: %s" % (a[1], e))
            sys.exit(1)
        if a[0] == "list":
            print("bundle %d (%s), %d, channel %s, for base %d or newer, %d bytes" %
                  (hdr["vnum"], hdr["vstr"], hdr["date"], hdr["channel"], hdr["min"], hdr["total"]))
            for n, d in parts:
                print("  %-28s %8d %s" % (n, len(d), sha(d)))
        else:
            print("%s: ok" % a[1])
    elif len(a) == 4 and a[0] == "installdb":
        got = installdb(a[1], a[2], a[3])
        print("package database: %s" % (" ".join(got) or "none"))
    else:
        sys.stderr.write(__doc__)
        sys.exit(2)


if __name__ == "__main__":
    main(sys.argv[1:])
