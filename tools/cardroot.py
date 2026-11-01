#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Carry the owner's settings from the Zenix root a card has now into a new root image, before the new one is written
(tools/write-root.sh; the owner's weekend package, docs/card-package.md). The old root is only read.

usage: cardroot.py carry OLD NEW      OLD: the root now on the card (an ext2 image file, read only); NEW: the new root
                                      image, changed in place (debugfs -w), checked with e2fsck -fn afterwards

What is carried (the rules of zup's config files, docs/man/zup.txt, applied once by hand because the card's root is
older than zup, DESIGN D15 section 8, "the first D15 update"):
  /etc/passwd   the old line of every user the old file has (passwords, shells, homes), the new file's other users
                after them
  /etc/group    every group with the old password and number and both member lists; the new file's other groups
  /etc/fstab    the old file when it has a line that is not a comment (the card's /home); else the new one
  /etc/autologin  the old one, or none when the old root had none (an update never brings an autologin back)
  /var/lib/random-seed  the machine's own seed, when the new image has none (images never carry one)
  any other file under /etc the new image does not have: copied; one both have whose contents differ: the new one
                stays and the old one is kept next to it as NAME.old (nothing the owner wrote is lost)
Prints one line per change. Needs debugfs and e2fsck (e2fsprogs)."""
import os
import shutil
import subprocess
import sys
import tempfile

SPECIAL = {"passwd", "group", "fstab", "autologin"}


def die(msg):
    sys.exit("cardroot: " + msg)


def dbg(img, cmd, write=False):
    args = ["debugfs"] + (["-w"] if write else []) + ["-R", cmd, img]
    return subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)


ZTOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "zenixfs", "build", "zenixfs-tool")


def is_zenixfs(img):
    with open(img, "rb") as f:
        return f.read(36)[32:36] == b"ZNFS"


def ztool(*a):
    return subprocess.run([ZTOOL, *a], stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)


def dump_etc(img, dest):
    """the image's /etc into dest/etc (debugfs rdump: contents and modes)"""
    os.makedirs(dest, exist_ok=True)
    if is_zenixfs(img):                                  # the owner's trial of a ZenixFS root (DESIGN D34)
        r = ztool("get", img, "-r", "/etc", os.path.join(dest, "etc"))
    else:
        r = dbg(img, "rdump /etc " + dest)
    if not os.path.isdir(os.path.join(dest, "etc")):
        die("cannot read /etc of %s: %s" % (img, r.stderr.decode(errors="replace").strip()))
    return os.path.join(dest, "etc")


def lines(path):
    if not os.path.exists(path):
        return []
    with open(path, encoding="latin-1") as f:
        return [l.rstrip("\n") for l in f if l.strip()]


def merge_passwd(old, new):
    o = {l.split(":")[0]: l for l in old if ":" in l}
    out, seen = [], set()
    for l in new:
        name = l.split(":")[0]
        out.append(o.get(name, l))
        seen.add(name)
    out += [l for l in old if ":" in l and l.split(":")[0] not in seen]
    return out


def merge_group(old, new):
    o = {l.split(":")[0]: l.split(":") for l in old if l.count(":") >= 3}
    out, seen = [], set()
    for l in new:
        f = l.split(":")
        if len(f) < 4:
            out.append(l)
            continue
        seen.add(f[0])
        if f[0] in o:
            of = o[f[0]]
            mem = [m for m in of[3].split(",") if m]
            mem += [m for m in f[3].split(",") if m and m not in mem]
            out.append(":".join([f[0], of[1], of[2], ",".join(mem)]))
        else:
            out.append(l)
    out += [l for l in old if l.count(":") >= 3 and l.split(":")[0] not in seen]
    return out


def has_entry(etc):
    return any(not l.lstrip().startswith("#") for l in lines(os.path.join(etc, "fstab")))


class Plan:
    def __init__(self, tmp):
        self.cmds, self.said, self.tmp, self.n = [], [], tmp, 0

    def put(self, path, data, mode, why):
        self.n += 1
        src = os.path.join(self.tmp, "f%d" % self.n)
        with open(src, "wb") as f:
            f.write(data)
        self.cmds += ["rm %s" % path, "write %s %s" % (src, path), "sif %s uid 0" % path, "sif %s gid 0" % path,
                      "sif %s mode 0%o" % (path, 0o100000 | mode)]
        self.said.append("%s: %s" % (path, why))

    def mkdir(self, path):
        self.cmds += ["mkdir %s" % path, "sif %s uid 0" % path, "sif %s gid 0" % path, "sif %s mode 040755" % path]

    def remove(self, path, why):
        self.cmds.append("rm %s" % path)
        self.said.append("%s: %s" % (path, why))


def carry(old, new):
    for p in (old, new):
        if not os.path.exists(p):
            die("%s does not exist" % p)
    tmp = tempfile.mkdtemp(prefix="cardroot.")
    try:
        oe, ne = dump_etc(old, os.path.join(tmp, "old")), dump_etc(new, os.path.join(tmp, "new"))
        plan = Plan(tmp)
        text = lambda ls: ("\n".join(ls) + "\n").encode("latin-1")
        op, np_ = lines(os.path.join(oe, "passwd")), lines(os.path.join(ne, "passwd"))
        if op:
            m = merge_passwd(op, np_)
            if m != np_:
                plan.put("/etc/passwd", text(m), 0o644, "the old users' lines kept (%d users)" % len(m))
        og, ng = lines(os.path.join(oe, "group")), lines(os.path.join(ne, "group"))
        if og:
            m = merge_group(og, ng)
            if m != ng:
                plan.put("/etc/group", text(m), 0o644, "the old groups and members kept, new groups added")
        if has_entry(oe):
            d = open(os.path.join(oe, "fstab"), "rb").read()
            if d != open(os.path.join(ne, "fstab"), "rb").read():
                plan.put("/etc/fstab", d, 0o644, "the card's own mounts kept")
        oa, na = os.path.join(oe, "autologin"), os.path.join(ne, "autologin")
        if os.path.exists(oa):
            d = open(oa, "rb").read()
            if not os.path.exists(na) or d != open(na, "rb").read():
                plan.put("/etc/autologin", d, 0o644, "kept as the card had it")
        elif os.path.exists(na):
            plan.remove("/etc/autologin", "removed: the card had no autologin")
        for dirpath, dirs, files in os.walk(oe):
            rel = os.path.relpath(dirpath, oe)
            for dname in sorted(dirs):
                r = os.path.normpath(os.path.join(rel, dname))
                if not os.path.isdir(os.path.join(ne, r)):
                    plan.mkdir("/etc/" + r)
            for fname in sorted(files):
                r = os.path.normpath(os.path.join(rel, fname))
                if r in SPECIAL:
                    continue
                src, dst = os.path.join(oe, r), os.path.join(ne, r)
                if os.path.islink(src) or not os.path.isfile(src):
                    continue
                mode = os.stat(src).st_mode & 0o7777
                d = open(src, "rb").read()
                if not os.path.exists(dst):
                    plan.put("/etc/" + r, d, mode, "copied from the card (the new root has none)")
                elif d != open(dst, "rb").read() and not os.path.exists(dst + ".old"):
                    plan.put("/etc/" + r + ".old", d, mode, "the card's version kept next to the new one")
        seed = os.path.join(tmp, "seed")
        znew = is_zenixfs(new)
        have = (lambda p: ztool("stat", new, p).returncode == 0) if znew else (lambda p: dbg(new, "stat " + p).stdout.startswith(b"Inode:"))
        if dbg(old, "dump /var/lib/random-seed " + seed).returncode == 0 and os.path.exists(seed) and \
                os.path.getsize(seed) > 0 and not have("/var/lib/random-seed"):
            if not have("/var/lib"):
                plan.mkdir("/var/lib")
            plan.put("/var/lib/random-seed", open(seed, "rb").read(), 0o600, "the machine's own seed kept")
        if znew:
            for c in plan.cmds:
                w = c.split()
                if w[0] == "rm":
                    ztool("rm", new, w[1])
                elif w[0] == "write":
                    if ztool("put", new, w[1], w[2]).returncode:
                        die("%s: cannot write it to the new root" % w[2])
                elif w[0] == "mkdir":
                    ztool("mkdir", new, w[1])
                elif w[0] == "sif" and w[2] == "mode":
                    ztool("chmod", new, "%o" % (int(w[3], 8) & 0o7777), w[1])
            r = ztool("fsck", new)
            if r.returncode != 0:
                die("the new root is not clean after the changes:\n" + r.stdout.decode(errors="replace"))
            for s in plan.said or ["nothing to carry: the old root has the shipped settings"]:
                print("cardroot: " + s)
            for c in plan.cmds:
                if c.startswith("write "):
                    _, src, path = c.split()
                    if ztool("cat", new, path).stdout != open(src, "rb").read():
                        die("%s did not arrive in the new root" % path)
            return
        if plan.cmds:
            script = os.path.join(tmp, "debugfs.cmd")
            with open(script, "w") as f:
                f.write("\n".join(plan.cmds) + "\n")
            subprocess.run(["debugfs", "-w", "-f", script, new], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           check=False)
        r = subprocess.run(["e2fsck", "-fn", new], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
        if r.returncode != 0:
            die("the new root is not clean after the changes:\n" + r.stdout.decode(errors="replace"))
        for s in plan.said or ["nothing to carry: the old root has the shipped settings"]:
            print("cardroot: " + s)
        # read back what was written: every file the plan put must hold its bytes
        for c in plan.cmds:
            if c.startswith("write "):
                _, src, path = c.split()
                chk = os.path.join(tmp, "check")
                if os.path.exists(chk):
                    os.unlink(chk)
                dbg(new, "dump %s %s" % (path, chk))
                if not os.path.exists(chk) or open(chk, "rb").read() != open(src, "rb").read():
                    die("%s did not arrive in the new root" % path)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    if len(sys.argv) != 4 or sys.argv[1] != "carry":
        sys.exit(__doc__)
    carry(sys.argv[2], sys.argv[3])


main()
