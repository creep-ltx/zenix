#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Make the install floppies (DESIGN D35, docs/d35-installer.md section 5): zenix-install-1.adf boots a RAM disk with zinstall;
behind the RAM disk, and on the other disks, lies the set: one stream of package bundles (D22's format), core programs first.
Each disk's part starts after a header sector: "ZSET", the set's id, the disk's number, how many disks (big-endian longs).
The RAM disk's /etc/zinstall.set says what is where, with SHA-256s, and the kernel on disk 1 is the one the set installs.
usage: mkinstall.py BUILD OUTDIR     (ZENIX_INSTALL_CHANNEL: the bundles' channel, default test; a release build: testing)
Also OUTDIR/from-amigaos/: no floppy at all: zenixboot, the kernel and zinstall-ram.img (BUILD/root-install-hd.img with the install set (CORE and SETX) as files
in /install), to copy to an AmigaOS disk and start with
    zenixboot DIR/zenix RAM=DIR/zinstall-ram.img        (docs/d35-installer.md section 5)
Not a frozen format: the program that reads it comes from the same build, on disk 1."""
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
FLOPPY = 901120
SEC = 512
# The install set (DESIGN D35 section 5, decided 2026-11-18 for FA-3): the base system that boots, logs in, uses the network
# and runs zpkg. CORE first, in this order on the disks (what a machine cannot do without first; base first of all so that
# its folders exist); then SETX, the rest of the set (the network's programs, the desktop and its servers, the manual
# pages of the base), on the floppies' last disk, which may be skipped ("extra"; zpkg -S adds them later). The start from
# AmigaOS carries both (its RAM disk must load on the 4 MB minimum). Every other package is optional, on no disk: zpkg -S
# NAME after installing (the compiler, games, Lua, make, the editors and viewers of ports/, irc, mail, sound, ZINE, ...).
CORE = ["zup", "base", "zpkg", "coreutils", "textutils", "shellutils", "sysinfo", "procps", "sysutils", "pager", "grep", "sed",
        "findutils", "shadow", "disktools", "archive", "edit", "zinstall", "nettools", "hashutils", "diffutils"]
SETX = ["awk", "base-man", "telnet", "ssh", "wget", "sntp", "zmodem", "cu", "usb", "desktop"]
SLACK_K = 64                                  # what the start from AmigaOS's RAM disk keeps free after the set (/tmp, logs)


def ram_free(img):
    """-> (free 1 KB blocks, free inodes) of an ext2 image, from dumpe2fs"""
    out = subprocess.run(["dumpe2fs", "-h", img], capture_output=True, text=True).stdout
    get = lambda k: int([l for l in out.split("\n") if l.startswith(k + ":")][0].split()[-1])
    if get("Block size") != 1024:
        sys.exit("mkinstall: %s: not 1 KB blocks" % img)
    return get("Free blocks"), get("Free inodes")


def blocks_k(size):
    """1 KB blocks of an ext2 file of SIZE bytes: data, then the indirect blocks (256 numbers each)"""
    d = (size + 1023) // 1024
    ind = 0 if d <= 12 else 1 + (0 if d <= 12 + 256 else 1 + (d - 12 - 256 + 255) // 256)
    return d + ind


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    B, out = sys.argv[1:]
    pk = os.path.join(B, "pkg")
    os.makedirs(out, exist_ok=True)
    have = sorted(f[:-4] for f in os.listdir(pk) if f.endswith(".tgz"))
    extra = SETX
    for n in CORE + SETX:
        if n not in have:
            sys.exit("mkinstall: the build has no package %s" % n)
    kernel = open(os.path.join(B, "kernel.bin"), "rb").read()
    chan = os.environ.get("ZENIX_INSTALL_CHANNEL", "testing" if os.environ.get("ZENIX_RELEASE") else "test")
    mkzup = [sys.executable, os.path.join(HERE, "mkzup.py")]
    with tempfile.TemporaryDirectory() as t:
        items = []
        for name, tier in [(n, "core") for n in CORE] + [("kernel-zfs", "core")] + [(n, "extra") for n in extra]:
            bundle = os.path.join(t, name + ".zup")
            pkdir = pk
            if name == "kernel-zfs":
                # the kernel for a ZenixFS root, packed: only ever unpacked to be put on the boot partition, never in a package database
                pkdir = os.path.join(t, "kz")
                os.makedirs(pkdir)
                line = open(os.path.join(pk, "base.manifest")).read().split("\n")
                v = [l for l in line if l.startswith("version ")][0].split()
                subprocess.run(mkzup + ["pkg", pkdir, "kernel-zfs", v[1], v[2],
                                        "d:755:0:0:/lib", "d:755:0:0:/lib/zinstall",
                                        "f:644:0:0:%s:/lib/zinstall/zenix" % os.path.join(B, "kernel-zfs.bin")],
                               check=True, stdout=subprocess.DEVNULL)
            subprocess.run(mkzup + ["bundle", bundle, "--no-kernel", "--channel", chan, pkdir, name], check=True, stdout=subprocess.DEVNULL)
            items.append((name, tier, open(bundle, "rb").read()))
        ram = os.path.join(t, "ram.img")
        shutil.copy(os.path.join(B, "root-install.img"), ram)
        ksize = (len(kernel) + 511) & ~511
        rsize = (os.path.getsize(ram) + 511) & ~511
        data1 = 1024 + ksize + rsize + SEC
        if data1 + SEC >= FLOPPY:
            sys.exit("mkinstall: the kernel and the RAM disk fill disk 1")
        # the stream
        stream = bytearray()
        lines = []
        cap1, cap = FLOPPY - data1, FLOPPY - SEC
        for name, tier, data in items:
            if tier == "extra":
                # never across two disks: then "skip" at a disk leaves nothing half done; and the first one on a disk of
                # its own, so a machine with fewer drives may skip the whole rest of the set at once
                used = len(stream)
                room = cap1 - used if used < cap1 else cap - (used - cap1) % cap
                first = not any(l.split()[5] == "extra" for l in lines)
                if len(data) > room or (first and room != cap):
                    stream += bytes(room)
            lines.append("item %s %d %d %s %s\n" % (name, len(stream), len(data), hashlib.sha256(data).hexdigest(), tier))
            stream += data
        ndisks = 1 + (max(len(stream) - cap1, 0) + cap - 1) // cap
        setid = int.from_bytes(hashlib.sha256(bytes(stream) + kernel).digest()[:4], "big")
        text = "zset 1\nid %08x\ndisks %d\ndata1 %d\nkernel 1024 %d %s\n" % (setid, ndisks, data1, len(kernel), hashlib.sha256(kernel).hexdigest())
        text += "".join(lines)
        # into the RAM disk: the list, and words at the prompt
        setf, motd = os.path.join(t, "set"), os.path.join(t, "motd")
        open(setf, "w").write(text)
        open(motd, "w").write("\n Zenix - the install floppy\n Type zinstall and press Return to put Zenix on a disk.\n"
                              " (zinstall --list only shows what is there.)\n\n")
        cmds = "rm /etc/motd\nwrite %s /etc/motd\nsif /etc/motd uid 0\nsif /etc/motd gid 0\nsif /etc/motd mode 0100644\n" \
               "write %s /etc/zinstall.set\nsif /etc/zinstall.set uid 0\nsif /etc/zinstall.set gid 0\nsif /etc/zinstall.set mode 0100644\n" % (motd, setf)
        r = subprocess.run(["debugfs", "-w", "-f", "-", ram], input=cmds, text=True, capture_output=True)
        chk = subprocess.run(["debugfs", "-R", "cat /etc/zinstall.set", ram], capture_output=True, text=True)
        if r.returncode or text not in chk.stdout:
            sys.exit("mkinstall: /etc/zinstall.set did not arrive in the RAM disk: " + r.stderr)
        fs = subprocess.run(["e2fsck", "-fn", ram], capture_output=True, text=True)
        if fs.returncode:
            sys.exit("mkinstall: the RAM disk is not clean: " + fs.stdout)
        # the disks
        for f in os.listdir(out):
            if f.startswith("zenix-install-") and f.endswith(".adf"):
                os.unlink(os.path.join(out, f))
        d1 = os.path.join(out, "zenix-install-1.adf")
        subprocess.run([sys.executable, os.path.join(HERE, "mkadf.py"), os.path.join(B, "bootblock.bin"), os.path.join(B, "kernel.bin"), d1, ram], check=True, stdout=subprocess.DEVNULL)

        def header(n):
            return (b"ZSET" + struct.pack(">III", setid, n, ndisks)).ljust(SEC, b"\0")

        img = bytearray(open(d1, "rb").read())
        if len(img) != FLOPPY:
            img = img.ljust(FLOPPY, b"\0")
        img[data1 - SEC:data1] = header(1)
        part = stream[:cap1]
        img[data1:data1 + len(part)] = part
        open(d1, "wb").write(img)
        pos = cap1
        for n in range(2, ndisks + 1):
            part = stream[pos:pos + cap]
            pos += cap
            img = bytearray(header(n) + part).ljust(FLOPPY, b"\0")
            open(os.path.join(out, "zenix-install-%d.adf" % n), "wb").write(img)
    # the same set as files, for a start from AmigaOS with no floppy
    hd = os.path.join(out, "from-amigaos")
    os.makedirs(hd, exist_ok=True)
    with tempfile.TemporaryDirectory() as t:
        img = os.path.join(t, "ram.img")
        shutil.copy(os.path.join(B, "root-install-hd.img"), img)
        # the size check (FA-3): the set must fit the RAM disk with SLACK_K to spare, said with its numbers, never found by debugfs
        free, inodes = ram_free(img)
        need = 1 + blocks_k(len(kernel)) + sum(blocks_k(len(d)) for n, tier, d in items)
        nfiles = 4 + len(items)
        if need + SLACK_K > free or nfiles > inodes:
            sys.exit("mkinstall: the install set (%d KB, %d files) outgrew the AmigaOS start's RAM disk (%d KB and %d files free, %d KB "
                     "kept for /tmp): take a package out of SETX in tools/mkinstall.py (zpkg -S brings it later) or raise "
                     "RAMDISK_INSTALL_HD_K in the Makefile only as far as the 4 MB minimum loads it (DESIGN D35 section 5)"
                     % (need, nfiles, free, inodes, SLACK_K))
        cmds = "mkdir /install\n"
        text = "zset 1\nfiles 1\nkernelfile /install/zenix %d %s\n" % (len(kernel), hashlib.sha256(kernel).hexdigest())
        kf = os.path.join(t, "zenix")
        open(kf, "wb").write(kernel)
        cmds += "write %s /install/zenix\n" % kf
        for name, tier, data in items:
            f = os.path.join(t, name + ".zup")
            open(f, "wb").write(data)
            cmds += "write %s /install/%s.zup\n" % (f, name)
            text += "fitem %s /install/%s.zup %d %s %s\n" % (name, name, len(data), hashlib.sha256(data).hexdigest(), tier)
        setf, motd = os.path.join(t, "set"), os.path.join(t, "motd")
        open(setf, "w").write(text)
        open(motd, "w").write("\n Zenix - the installer, started from AmigaOS\n Type zinstall --list to see the disk, zinstall to install.\n"
                              " The optional programs (the compiler, games, Lua, irc, mail, sound, ...) are not here:\n"
                              " after installing, zpkg -S NAME brings them from the package server.\n\n")
        cmds += "rm /etc/motd\nwrite %s /etc/motd\nsif /etc/motd mode 0100644\nwrite %s /etc/zinstall.set\nsif /etc/zinstall.set mode 0100644\n" % (motd, setf)
        r = subprocess.run(["debugfs", "-w", "-f", "-", img], input=cmds, text=True, capture_output=True)
        chk = subprocess.run(["debugfs", "-R", "cat /etc/zinstall.set", img], capture_output=True, text=True)
        if r.returncode or text not in chk.stdout or "Could not allocate" in r.stderr:
            sys.exit("mkinstall: the AmigaOS start's RAM disk is too small or did not take the set: " + r.stderr[-300:])
        fs = subprocess.run(["e2fsck", "-fn", img], capture_output=True, text=True)
        if fs.returncode:
            sys.exit("mkinstall: the AmigaOS start's RAM disk is not clean: " + fs.stdout)
        shutil.copy(img, os.path.join(hd, "zinstall-ram.img"))
        shutil.copy(os.path.join(B, "kernel.bin"), os.path.join(hd, "zenix"))
        shutil.copy(os.path.join(B, "zenixboot"), os.path.join(hd, "zenixboot"))
    print("%s: no floppy: zenixboot, zenix and zinstall-ram.img (%d KB; the install set %d KB, %d KB free)"
          % (hd, os.path.getsize(os.path.join(hd, "zinstall-ram.img")) // 1024, need, free - need))
    print("%s: %d disks, %d KB of bundles (%d on disk 1), RAM disk %d KB" % (out, ndisks, len(stream) // 1024, cap1 // 1024, rsize // 1024))


main()
