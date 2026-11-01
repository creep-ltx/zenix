#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Corruption lab for ZenixFS: build a volume (many commits), damage COPIES of it in many ways, and see what a user can get
back. Nothing here touches the original. Everything follows from the seed and the options: a failure prints the line that
replays it, and the tree is generated, never taken from the working directory (the bytes of a tree decide the shape of the
B+tree; an earlier version read the source tree, and its "one inner node" case hit the root until the tree grew a level).

usage: corrupt-lab.py                   how each damage looks to the plain tools
       corrupt-lab.py --recover         the same plus what salvage and repair bring back
       corrupt-lab.py --check           what must always work (make test): a volume of three levels and one of two
       corrupt-lab.py --sweep N         N trees of different shapes: every inner node in turn, leaves, commit records
       options: --seed S --files N --bshift B   one tree;  --tree DIR  a real tree instead of a generated one
                --only NAME  one damage;  --block N  that block for the damages that take one;  --keep  keep the images"""
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


def opt(name, default=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


CHECK = "--check" in sys.argv
RECOVER = "--recover" in sys.argv or CHECK or "--sweep" in sys.argv
KEEP = "--keep" in sys.argv
rnd = random.Random(0)                                   # seeded for each damage from its name (see reseed): no order matters


def reseed(seed, name):
    rnd.seed(f"{seed}/{name}")


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


def gen_tree(root, seed, nfiles):
    """a tree of nfiles files as a system disk has them: many small files, some inline, some bigger, links, nested directories"""
    r = random.Random(f"tree/{seed}/{nfiles}")
    dirs = [root]
    for i in range(max(3, nfiles // 25)):
        d = os.path.join(r.choice(dirs), f"d{i}")
        os.mkdir(d)
        dirs.append(d)
    made = []
    for i in range(nfiles):
        name = "f%d%s" % (i, "x" * r.randrange(0, 40))
        p = os.path.join(r.choice(dirs), name)
        k = r.random()
        n = r.randrange(0, 180) if k < 0.3 else r.randrange(200, 5000) if k < 0.9 else r.randrange(5000, 60000)
        with open(p, "wb") as f:
            f.write(r.randbytes(n))
        made.append(p)
    for i in range(nfiles // 50):
        os.symlink(os.path.relpath(r.choice(made), root), os.path.join(r.choice(dirs), f"s{i}"))


def build_base(path, seed, nfiles, bshift, tree=None):
    """a volume of 160 MB (or what the files need), filled in separate runs so that the commit ring has wrapped around"""
    mb = max(64, 16 + nfiles // 8)
    run(TOOL, "mkfs", path, str(mb), "-l", "lab", "-b", str(bshift))
    tmp = tempfile.mkdtemp(prefix="zsrc")
    if tree:
        for p in ["docs", "kernel", "user", "tools", "rootfs", "archive", "zenixfs"]:
            src = os.path.join(tree, p)
            if os.path.isdir(src):
                shutil.copytree(src, os.path.join(tmp, p), symlinks=True, ignore=shutil.ignore_patterns("build", ".git", "*.hdf", "*.adf", "*.zenixfs", "*.img"))
    else:
        gen_tree(tmp, seed, nfiles)
    r = run(TOOL, "put", path, "-r", tmp, "/")
    assert r.returncode == 0, r.stdout + r.stderr
    shutil.rmtree(tmp, ignore_errors=True)
    for i in range(5):                                   # a few bigger binary files, so there are long extents
        f = tempfile.NamedTemporaryFile(delete=False)
        f.write(random.Random(f"{seed}/{i}").randbytes(300000 + i * 200000))
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
    """name -> damage. Those that take one block use the --block option if given, else one chosen from the seed"""
    block = opt("--block")
    live = img.live_tree_blocks()
    leaves = [x for x in live if x[1] == 4]
    recs = img.commit_records()
    root = recs[0][2]
    nodes = [x for x in live if x[1] == 5 and x[0] != root]
    bmcur = []                                           # current copy of bitmap pages: the commit record says which
    rec = img.block(recs[0][1])
    for i in range(img.nb):
        sel = (rec[60 + i // 8] >> (i % 8)) & 1
        bmcur.append(9 + 2 * i + sel)

    def one(lst):
        return int(block) if block else rnd.choice(lst)[0]
    sc = []
    sc.append(("one leaf: 3 bit flips", lambda c: flip_bits(c, one(leaves))))
    sc.append(("one leaf: zeroed", lambda c: zero_block(c, one(leaves))))
    sc.append(("one leaf: garbage", lambda c: garbage(c, one(leaves))))
    sc.append(("10 leaves: garbage", lambda c: [garbage(c, x[0]) for x in rnd.sample(leaves, min(10, len(leaves)))]))
    if nodes:
        sc.append(("one inner node: garbage", lambda c: garbage(c, one(nodes))))
        sc.append(("one inner node: zeroed", lambda c: zero_block(c, one(nodes))))
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
    """the repair tool on a copy, then read the volume with the plain tools. A repair that refuses must leave the volume as it
    was (bit for bit); one that says clean must leave a volume that takes a new file and stays clean.
    Returns (files intact, files wrong, clean, why it is not acceptable or "", the result image's name)"""
    q = path + ".repaired"
    shutil.copy(path, q)
    before = hashlib.sha256(open(q, "rb").read()).hexdigest()
    r = run(TOOL, "repair", q)
    why = ""
    if r.returncode != 0 and "nothing was changed" in r.stdout and hashlib.sha256(open(q, "rb").read()).hexdigest() != before:
        why = "repair said nothing was changed, but the volume changed"
    out = tempfile.mkdtemp(prefix="zrep")
    info = run(TOOL, "info", q)
    if info.returncode == 0:
        run(TOOL, "get", q, "-r", "/", out)
    got = sha_tree(out)
    ok = sum(1 for k, v in refhash.items() if got.get(k) == v)
    wrong = sum(1 for k, v in refhash.items() if k in got and got[k] != v)
    shutil.rmtree(out, ignore_errors=True)
    fs = run(TOOL, "fsck", q).returncode == 0 if info.returncode == 0 else False
    if r.returncode == 0 and not why:
        with tempfile.NamedTemporaryFile(delete=False) as f:
            f.write(b"after the repair\n" * 2000)
        a = run(TOOL, "put", q, f.name, "/after-repair.txt")
        b = run(TOOL, "fsck", q)
        os.unlink(f.name)
        if a.returncode or b.returncode:
            why = "the repaired volume does not take a new file: " + (a.stderr + b.stdout)[:200]
    return ok, wrong, fs, why, q, r.returncode


# what the recovery tools MUST achieve (--check): name -> (files salvage must bring back at least, relative to all; files a
# repair that says clean must leave, relative to all; whether repair must succeed)
EXPECT = {
    "newest commit record: bit flips": (-1, -1, True),
    "newest commit record: zeroed": (-1, -1, True),
    "two newest commit records: garbage": (-2, -2, False),   # past the one older commit nothing is guaranteed: repair refuses
    "all commit records: garbage": (0, 0, False),
    "the superblock: garbage": (0, 0, True),
    "a bitmap page (current copy): garbage": (0, 0, True),
    "both copies of a bitmap page: garbage": (0, 0, True),
    "the root block: garbage": (-1, -1, True),
    "one inner node: garbage": (0, 0, True),                 # the leaves are all there: new nodes, nothing lost
    "one inner node: zeroed": (0, 0, True),
    "one leaf: garbage": (-100000, -1, False),               # may refuse; never a wrong file, a refusal changes nothing, salvage keeps its files
    "one leaf: zeroed": (-100000, -1, False),
    "one leaf: 3 bit flips": (-100000, -1, False),
}


class Lab:
    """one volume and its reference copy"""
    def __init__(self, seed, files, bshift, tree=None):
        self.seed, self.files, self.bshift = seed, files, bshift
        self.tree = tree
        self.work = tempfile.mkdtemp(prefix="zlab")
        self.base = os.path.join(self.work, "base.zenixfs")
        build_base(self.base, seed, files, bshift, tree)
        self.ref = reference(self.base)
        self.refhash = sha_tree(self.ref)
        self.img = Img(self.base)
        self.bad = 0
        self.total = len(self.refhash)
        rec = self.img.commit_records()[0]
        self.height = struct.unpack(">I", self.img.block(rec[1])[36:40])[0]

    def replay(self, name, block=None):
        t = f" --tree {self.tree}" if self.tree else ""
        return (f"tests/corrupt-lab.py --check --seed {self.seed} --files {self.files} --bshift {self.bshift}{t} --only '{name}'" +
                (f" --block {block}" if block is not None else "") + " --keep")

    def fail(self, name, block, msg):
        print(f"  CHECK FAILED: {name}: {msg}\n    replay: {self.replay(name, block)}", flush=True)
        self.bad += 1

    def one(self, name, fn, block=None, check=True, quiet=False):
        """apply one damage to a copy and judge what comes back"""
        reseed(self.seed, name)
        c = Img(self.base)
        fn(c)
        p = os.path.join(self.work, "damaged.zenixfs")
        c.save(p)
        s = survey(p, self.ref, self.refhash)
        mount = ("ok" if s["mount"] else "FAIL") + ("+warn" if s["warn"] else "")
        fsck = "clean" if s["fsck"] else f"{s['problems']} prob"
        plain = f"{s['ok']}/{s['total']} ok" + (f", {s['wrong']} WRONG" if s["wrong"] else "")
        line = f"{name:<46} {mount:<7} {fsck:<8} {plain:<20}"
        if check and s["wrong"]:
            self.fail(name, block, f"the plain tools left {s['wrong']} file(s) with wrong contents under their names")
        if RECOVER:
            ok, anywhere, flagged, wrong = recover(p, self.ref, self.refhash)
            line += f"  salvage {ok}/{anywhere}" + (f" ({flagged} flagged)" if flagged else "") + (f" {wrong}SILENT-WRONG" if wrong else "")
            ok2, anywhere2, flagged2, wrong2 = recover(p, self.ref, self.refhash, "--old-copies")
            if (ok2, anywhere2) != (ok, anywhere) or wrong2 != wrong:
                line += f" [+old copies {ok2}/{anywhere2}" + (f" {wrong2}SILENT-WRONG" if wrong2 else "") + "]"
            rok, rwrong, rfs, why, q, rc = repair_then_read(p, self.ref, self.refhash)
            line += f"  | repair {rok}/{s['total']}" + (f" +{rwrong}WRONG" if rwrong else "") + (" clean" if rfs else " not-clean") + ("" if rc == 0 else " (refused)")
            if check and name in EXPECT:
                need, rneed, must = EXPECT[name]
                if anywhere < s["total"] + need or wrong:
                    self.fail(name, block, f"salvage got {anywhere} of {s['total']} (need at least {s['total'] + need}), {wrong} silent errors")
                if must and (not rfs or rok < s["total"] + rneed):
                    self.fail(name, block, f"repair left {rok} of {s['total']} readable, clean={rfs}")
                if rc == 0 and rok < s["total"] + rneed:
                    self.fail(name, block, f"repair said clean, but only {rok} of {s['total']} files are there")
                if name.startswith("two newest") and rc == 0:
                    self.fail(name, block, "repair built on a state two commits back without being asked")
                if why:
                    self.fail(name, block, why)
            if check and rwrong:
                self.fail(name, block, f"after repair {rwrong} file(s) have wrong contents")
            if rc != 0 and check and name in EXPECT and not why:   # after a repair that refused, salvage must still have what it had
                ok3, anywhere3, _, wrong3 = recover(q, self.ref, self.refhash)
                if anywhere3 < anywhere or wrong3:
                    self.fail(name, block, f"salvage after a refused repair brought back {anywhere3}, before it {anywhere}")
            if KEEP:
                shutil.copy(p, "/tmp/zlab-damaged.zenixfs")
                print("  kept: /tmp/zlab-damaged.zenixfs")
            os.unlink(q)
        if not quiet:
            print(line, flush=True)
        return line

    def close(self):
        shutil.rmtree(self.work, ignore_errors=True)
        shutil.rmtree(self.ref, ignore_errors=True)


def header(lab):
    print(f"(damage is applied to copies; the total is {lab.total} files)")
    print(f"base image: seed {lab.seed}, {lab.total} files, {lab.img.nblocks} blocks of {lab.img.B} bytes, {lab.height} tree levels, "
          f"commit generations {[r[0] for r in lab.img.commit_records()]}")


def every_node(lab):
    """each inner node but the root in turn, and each kind of damage on a few leaves"""
    live = lab.img.live_tree_blocks()
    root = lab.img.commit_records()[0][2]
    nodes = [x[0] for x in live if x[1] == 5 and x[0] != root]
    leaves = [x[0] for x in live if x[1] == 4]
    r = random.Random(f"{lab.seed}/nodes")
    for n in nodes:
        lab.one("one inner node: garbage", lambda c, n=n: garbage(c, n), block=n, quiet=True)
    for n in r.sample(leaves, min(6, len(leaves))):
        lab.one("one leaf: garbage", lambda c, n=n: garbage(c, n), block=n, quiet=True)
    return len(nodes)


def main():
    seed = int(opt("--seed", "2026"))
    tree = opt("--tree")
    only = opt("--only")
    if "--sweep" in sys.argv:
        total, nodes = 0, 0
        n = int(opt("--sweep"))
        r = random.Random(f"sweep/{seed}")
        for i in range(n):
            bs = r.choice([10, 11, 12])
            files = r.choice([60, 300, 900, 1000, 1100, 1300, 2000, 3000, 6000])
            lab = Lab(seed + i, files, bs)
            k = every_node(lab)
            for name, fn in scenarios(lab.img):
                if name in EXPECT and "inner node" not in name and "leaf" not in name:
                    lab.one(name, fn, quiet=True)
            print(f"tree {i}: seed {seed + i}, {lab.total} files, block {lab.img.B}, {lab.height} levels: {k} inner nodes; failures {lab.bad}", flush=True)
            total += lab.bad
            nodes += k
            lab.close()
        print(f"sweep: {n} trees, {nodes} inner nodes damaged in turn, {total} failures")
        sys.exit(1 if total else 0)
    if CHECK and "--files" not in sys.argv:              # the shapes that must work: three levels, and two (where only the root is an inner node)
        bad = 0
        for files, bs, want in [(1300, 11, 3), (300, 11, 2)]:
            lab = Lab(seed, files, bs)
            header(lab)
            if lab.height != want:                       # the check must not quietly stop testing what it says
                print(f"  CHECK FAILED: this volume has {lab.height} tree levels, the check is about {want}")
                bad += 1
            k = every_node(lab)
            print(f"every inner node damaged in turn: {k}, leaves: 6")
            for name, fn in scenarios(lab.img):
                if name in EXPECT:
                    lab.one(name, fn)
            bad += lab.bad
            lab.close()
        print("recovery check:", "FAILED" if bad else "ok")
        sys.exit(1 if bad else 0)
    lab = Lab(seed, int(opt("--files", "1300")), int(opt("--bshift", "11")), tree)
    header(lab)
    print(f"{'damage':<46} {'mount':<7} {'fsck':<8} {'plain tools':<20}" + ("  recovered at its place/anywhere, of the files  |  after repair" if RECOVER else ""))
    for name, fn in scenarios(lab.img):
        if (only and name != only) or (CHECK and name not in EXPECT):
            continue
        lab.one(name, fn, check=CHECK)
    bad = lab.bad
    lab.close()
    if CHECK:
        print("recovery check:", "FAILED" if bad else "ok")
        sys.exit(1 if bad else 0)


main()
