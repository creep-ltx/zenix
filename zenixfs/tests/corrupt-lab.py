#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Corruption lab for ZenixFS: build a realistic image (many commits), damage COPIES of it in many ways, and see what a user
can get back. Nothing here touches the original. usage: corrupt-lab.py [--recover | --check]
Without --recover it only reports how the damage looks (mount, fsck, how many files can still be read with the plain tools).
With --recover it also runs the recovery tools (zenixfs-tool salvage / repair) and reports what they bring back."""
import hashlib
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.environ.get("ZTOOL") or os.path.join(HERE, "..", "build", "zenixfs-tool")
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
CHECK = "--check" in sys.argv
RECOVER = "--recover" in sys.argv or CHECK
rnd = random.Random(2026)


def run(*a, **k):
    return subprocess.run(a, capture_output=True, text=True, **k)


def sha_tree(root):
    """relative path -> sha256 of every regular file under root"""
    out = {}
    for d, _, files in os.walk(root):
        for f in files:
            p = os.path.join(d, f)
            if os.path.islink(p):
                continue
            with open(p, "rb") as fh:
                out[os.path.relpath(p, root)] = hashlib.sha256(fh.read()).hexdigest()
    return out


class Img:
    def __init__(self, path):
        self.path = path
        self.data = bytearray(open(path, "rb").read())
        b = self.data
        self.bshift = struct.unpack(">I", b[40:44])[0]
        self.B = 1 << self.bshift
        self.nblocks = struct.unpack(">I", b[44:48])[0]
        self.nb = struct.unpack(">I", b[48:52])[0]

    def block(self, n):
        return self.data[n * self.B:(n + 1) * self.B]

    def save(self, path=None):
        open(path or self.path, "wb").write(self.data)

    def hdr(self, n):
        b = self.block(n)
        magic, typ, level, selfn = struct.unpack(">HBBI", b[4:12])
        gen = struct.unpack(">Q", b[16:24])[0]
        return magic, typ, level, selfn, gen

    def commit_records(self):
        """valid-looking commit slots (type 2) newest first: (gen, slot, root)"""
        recs = []
        for s in range(1, 9):
            m, t, l, sn, g = self.hdr(s)
            if m == 0x5A42 and t == 2:
                recs.append((g, s, struct.unpack(">I", self.block(s)[32:36])[0]))
        return sorted(recs, reverse=True)

    def tree_blocks(self):
        """block numbers whose header says tree leaf (4) or node (5), with their generation"""
        out = []
        for n in range(9 + 2 * self.nb, self.nblocks):
            m, t, l, sn, g = self.hdr(n)
            if m == 0x5A42 and t in (4, 5) and sn == n:
                out.append((n, t, g))
        return out

    def live_tree_blocks(self):
        """tree blocks reachable from the newest commit record"""
        recs = self.commit_records()
        if not recs:
            return []
        seen = []
        stack = [recs[0][2]]
        while stack:
            n = stack.pop()
            m, t, l, sn, g = self.hdr(n)
            if m != 0x5A42 or t not in (4, 5):
                continue
            seen.append((n, t, g))
            if t == 5:
                b = self.block(n)
                cnt = struct.unpack(">H", b[24:26])[0]
                for i in range(cnt):
                    stack.append(struct.unpack(">I", b[32 + 16 * i + 12:32 + 16 * i + 16])[0])
        return seen


def build_base(path):
    """a 160 MB image with the Zenix sources in it, written in a dozen separate runs (so there are a dozen commits)"""
    run(TOOL, "mkfs", path, "160", "-l", "lab")
    parts = ["docs", "kernel", "user", "tools", "rootfs", "archive", "zenixfs"]
    for p in parts:
        src = os.path.join(REPO, p)
        if os.path.isdir(src):
            tmp = tempfile.mkdtemp(prefix="zsrc")
            shutil.copytree(src, os.path.join(tmp, p), symlinks=True, ignore=shutil.ignore_patterns("build", ".git", "*.hdf", "*.adf", "*.zenixfs", "*.img"))
            run(TOOL, "put", path, "-r", os.path.join(tmp, p), "/" + p)
            shutil.rmtree(tmp, ignore_errors=True)
    for i in range(5):                                   # a few bigger binary files, so there are extents
        f = tempfile.NamedTemporaryFile(delete=False)
        f.write(random.Random(i).randbytes(300000 + i * 200000))
        f.close()
        run(TOOL, "put", path, f.name, f"/big{i}.bin")
        os.unlink(f.name)
    run(TOOL, "mkdir", path, "/work")
    for i in range(30):                                  # many small commits: the ring wraps around
        f = tempfile.NamedTemporaryFile(delete=False)
        f.write(f"note {i}\n".encode() * 20)
        f.close()
        run(TOOL, "put", path, f.name, f"/work/n{i}.txt")
        os.unlink(f.name)


def reference(path):
    out = tempfile.mkdtemp(prefix="zref")
    r = run(TOOL, "get", path, "-r", "/", out)
    assert r.returncode == 0, r.stdout + r.stderr
    return out


# -------- the damage --------
def flip_bits(img, n, count=3):
    b = img.block(n)
    for _ in range(count):
        i = rnd.randrange(32, img.B)
        b[i] ^= 1 << rnd.randrange(8)
    img.data[n * img.B:(n + 1) * img.B] = b


def zero_block(img, n):
    img.data[n * img.B:(n + 1) * img.B] = bytes(img.B)


def garbage(img, n):
    img.data[n * img.B:(n + 1) * img.B] = rnd.randbytes(img.B)


def scenarios(img):
    live = img.live_tree_blocks()
    leaves = [x for x in live if x[1] == 4]
    nodes = [x for x in live if x[1] == 5]
    recs = img.commit_records()
    root = recs[0][2]
    bmcur = []                                           # current copy of bitmap pages: the commit record says which
    rec = img.block(recs[0][1])
    for i in range(img.nb):
        sel = (rec[60 + i // 8] >> (i % 8)) & 1
        bmcur.append(9 + 2 * i + sel)
    sc = []
    sc.append(("one leaf: 3 bit flips", lambda c: flip_bits(c, rnd.choice(leaves)[0])))
    sc.append(("one leaf: zeroed", lambda c: zero_block(c, rnd.choice(leaves)[0])))
    sc.append(("one leaf: garbage", lambda c: garbage(c, rnd.choice(leaves)[0])))
    sc.append(("10 leaves: garbage", lambda c: [garbage(c, x[0]) for x in rnd.sample(leaves, min(10, len(leaves)))]))
    if nodes:
        sc.append(("one inner node: garbage", lambda c: garbage(c, rnd.choice([x for x in nodes if x[0] != root] or nodes)[0])))
    sc.append(("the root block: garbage", lambda c: garbage(c, root)))
    sc.append(("newest commit record: bit flips", lambda c: flip_bits(c, recs[0][1], 2)))
    sc.append(("newest commit record: zeroed", lambda c: zero_block(c, recs[0][1])))
    sc.append(("two newest commit records: garbage", lambda c: [garbage(c, r[1]) for r in recs[:2]]))
    sc.append(("all commit records: garbage", lambda c: [garbage(c, s) for s in range(1, 9)]))
    sc.append(("the superblock: garbage", lambda c: garbage(c, 0)))
    sc.append(("a bitmap page (current copy): garbage", lambda c: garbage(c, bmcur[0])))
    sc.append(("both copies of a bitmap page: garbage", lambda c: [garbage(c, 9), garbage(c, 10)]))
    sc.append(("a 1 MB region of garbage in the middle", lambda c: [garbage(c, n) for n in range(c.nblocks // 2, min(c.nblocks, c.nblocks // 2 + (1 << 20) // c.B))]))
    sc.append(("the last 25% of the volume lost (truncated)", lambda c: c.__setattr__("data", c.data[: len(c.data) * 3 // 4])))
    sc.append(("200 random bit flips anywhere", lambda c: [c.data.__setitem__(i, c.data[i] ^ (1 << rnd.randrange(8))) for i in [rnd.randrange(len(c.data)) for _ in range(200)]]))
    sc.append(("64 KB of garbage over the start (RDB-sized hole)", lambda c: c.data.__setitem__(slice(0, 65536), rnd.randbytes(65536))))
    return sc


def survey(path, ref, refhash):
    """what do the plain tools say, and how much of the reference tree can be copied out?"""
    info = run(TOOL, "info", path)
    mount_ok = info.returncode == 0
    warn = "WARNING" in info.stderr
    fsck = run(TOOL, "fsck", path)
    problems = 0
    if fsck.returncode != 0 and "problem" in fsck.stdout:
        for line in fsck.stdout.splitlines():
            if line.strip().endswith("problem(s) found"):
                problems = int(line.split()[0])
    out = tempfile.mkdtemp(prefix="zout")
    if mount_ok:
        run(TOOL, "get", path, "-r", "/", out)
    got = sha_tree(out)
    ok = sum(1 for k, v in refhash.items() if got.get(k) == v)
    wrong = sum(1 for k, v in refhash.items() if k in got and got[k] != v)
    shutil.rmtree(out, ignore_errors=True)
    return dict(mount=mount_ok, warn=warn, fsck=fsck.returncode == 0, problems=problems, ok=ok, wrong=wrong, total=len(refhash))


def recover(path, ref, refhash, *extra):
    """run salvage on the damaged copy. Returns (intact at the right path, intact anywhere incl. lost+found, files with a name that says
    partial/nosize, files that look whole but have wrong content)"""
    out = tempfile.mkdtemp(prefix="zsalv")
    r = run(TOOL, "salvage", path, out, *extra)
    got = sha_tree(out)
    hashes = set(got.values())
    ok = sum(1 for k, v in refhash.items() if got.get(k) == v)
    anywhere = sum(1 for k, v in refhash.items() if v in hashes)
    flagged = sum(1 for k in got if k.endswith(".partial") or k.endswith(".nosize"))
    # wrong = a file under a reference path, not flagged, with different content (a silent error)
    wrong = sum(1 for k, v in refhash.items() if k in got and got[k] != v)
    shutil.rmtree(out, ignore_errors=True)
    return ok, anywhere, flagged, wrong


def repair_then_read(path, ref, refhash):
    """run the repair tool on a copy, then read the volume with the plain tools"""
    q = path + ".repaired"
    shutil.copy(path, q)
    r = run(TOOL, "repair", q)
    out = tempfile.mkdtemp(prefix="zrep")
    info = run(TOOL, "info", q)
    if info.returncode == 0:
        run(TOOL, "get", q, "-r", "/", out)
    got = sha_tree(out)
    ok = sum(1 for k, v in refhash.items() if got.get(k) == v)
    wrong = sum(1 for k, v in refhash.items() if k in got and got[k] != v)
    shutil.rmtree(out, ignore_errors=True)
    fs = run(TOOL, "fsck", q).returncode == 0 if info.returncode == 0 else False
    os.unlink(q)
    return ok, wrong, fs


# what the recovery tools MUST achieve (--check): name -> (files salvage must bring back at least, whether repair must leave a clean volume with everything readable)
EXPECT = {
    "newest commit record: bit flips": (-1, True),
    "newest commit record: zeroed": (-1, True),
    "two newest commit records: garbage": (-2, True),
    "all commit records: garbage": (0, False),
    "the superblock: garbage": (0, True),
    "a bitmap page (current copy): garbage": (0, True),
    "both copies of a bitmap page: garbage": (0, True),
    "the root block: garbage": (-1, True),
    "one inner node: garbage": (-1, True),
}


def main():
    work = tempfile.mkdtemp(prefix="zlab")
    base = os.path.join(work, "base.zenixfs")
    build_base(base)
    ref = reference(base)
    refhash = sha_tree(ref)
    base_img = Img(base)
    print(f"(damage is applied to copies; the total is {len(refhash)} files)")
    print(f"base image: {len(refhash)} files, {base_img.nblocks} blocks of {base_img.B} bytes, "
          f"commit generations {[r[0] for r in base_img.commit_records()]}")
    print(f"{'damage':<46} {'mount':<7} {'fsck':<8} {'plain tools':<20}" + ("  recovered at its place/anywhere, of the files  |  after repair" if RECOVER else ""))
    bad = 0
    for name, fn in scenarios(base_img):
        if CHECK and name not in EXPECT:
            continue
        c = Img(base)
        fn(c)
        p = os.path.join(work, "damaged.zenixfs")
        c.save(p)
        s = survey(p, ref, refhash)
        mount = "ok" if s["mount"] else "FAIL"
        if s["warn"]:
            mount += "+warn"
        fsck = "clean" if s["fsck"] else f"{s['problems']} prob"
        plain = f"{s['ok']}/{s['total']} ok" + (f", {s['wrong']} WRONG" if s["wrong"] else "")
        line = f"{name:<46} {mount:<7} {fsck:<8} {plain:<20}"
        if RECOVER:
            ok, anywhere, flagged, wrong = recover(p, ref, refhash)
            line += f"  salvage {ok}/{anywhere}" + (f" ({flagged} flagged)" if flagged else "") + (f" {wrong}SILENT-WRONG" if wrong else "")
            ok2, anywhere2, flagged2, wrong2 = recover(p, ref, refhash, "--old-copies")
            if (ok2, anywhere2) != (ok, anywhere) or wrong2 != wrong:
                line += f" [+old copies {ok2}/{anywhere2}" + (f" {wrong2}SILENT-WRONG" if wrong2 else "") + "]"
            if CHECK:
                need, must_repair = EXPECT[name]
                if anywhere < s["total"] + need or wrong:
                    print(f"  CHECK FAILED: {name}: salvage got {anywhere} of {s['total']} (need at least {s['total'] + need}), {wrong} silent errors")
                    bad += 1
            rok, rwrong, rfs = repair_then_read(p, ref, refhash)
            line += f"  | repair {rok}/{s['total']}" + (f" +{rwrong}WRONG" if rwrong else "") + (" clean" if rfs else " not-clean")
            if CHECK and EXPECT[name][1] and (not rfs or rok < s["total"] + EXPECT[name][0] or rwrong):
                print(f"  CHECK FAILED: {name}: repair left {rok} of {s['total']} readable, clean={rfs}")
                bad += 1
        print(line, flush=True)
    shutil.rmtree(work, ignore_errors=True)
    shutil.rmtree(ref, ignore_errors=True)
    if CHECK:
        print("recovery check:", "FAILED" if bad else "ok")
        sys.exit(1 if bad else 0)


main()
