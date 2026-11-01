#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Make the install floppies (DESIGN D35, docs/d35-installer.md section 5): zenix-install-1.adf boots a RAM disk with zinstall;
behind the RAM disk, and on the other disks, lies the set: one stream of package bundles (D22's format), core programs first.
Each disk's part starts after a header sector: "ZSET", the set's id, the disk's number, how many disks (big-endian longs).
The RAM disk's /etc/zinstall.set says what is where, with SHA-256s, and the kernel on disk 1 is the one the set installs.
usage: mkinstall.py BUILD OUTDIR     (ZENIX_INSTALL_CHANNEL: the bundles' channel, default test; a release build: testing)
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
# the order on the disks: what a machine cannot do without first; base first of all so that its folders exist
CORE = ["zup", "base", "zpkg", "coreutils", "textutils", "shellutils", "sysinfo", "procps", "sysutils", "pager", "grep", "sed",
        "findutils", "shadow", "disktools", "archive", "edit", "zinstall", "nettools", "hashutils", "diffutils"]
LEAVE_OUT = {"zenix-tests", "zpkg-new"}       # test programs; the package manager's bootstrap for machines older than it


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    B, out = sys.argv[1:]
    pk = os.path.join(B, "pkg")
    os.makedirs(out, exist_ok=True)
    have = sorted(f[:-4] for f in os.listdir(pk) if f.endswith(".tgz"))
    extra = [n for n in have if n not in CORE and n not in LEAVE_OUT]
    for n in CORE:
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
                                        "d:755:0:0:/usr", "d:755:0:0:/usr/lib", "d:755:0:0:/usr/lib/zinstall",
                                        "f:644:0:0:%s:/usr/lib/zinstall/zenix" % os.path.join(B, "kernel-zfs.bin")],
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
                # never across two disks: then "skip" at a disk leaves nothing half done
                used = len(stream)
                room = cap1 - used if used < cap1 else cap - (used - cap1) % cap
                if len(data) > room:
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
    print("%s: %d disks, %d KB of bundles (%d on disk 1), RAM disk %d KB" % (out, ndisks, len(stream) // 1024, cap1 // 1024, rsize // 1024))


main()
