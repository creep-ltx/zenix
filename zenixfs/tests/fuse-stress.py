#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Stress test of a mounted ZenixFS (zenixfs-fuse). Everything written is read back and compared.
usage: fuse-stress.py MOUNTPOINT IMAGE [--quick]
Phases: A many small files, B random reads/writes/truncates against an in-memory copy (like fsx), C four processes at once,
D one directory with thousands of entries, E renaming whole trees, F big sequential file, G unmount + fsck + remount + verify
everything again, H fill a small volume to ENOSPC and recover. The mount is unmounted and mounted again in G and H."""
import hashlib
import multiprocessing
import os
import random
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "..", "build", "zenixfs-tool")
FUSE = os.path.join(HERE, "..", "build", "zenixfs-fuse")
MNT, IMG = sys.argv[1], sys.argv[2]
QUICK = "--quick" in sys.argv
HEAVY = "--heavy" in sys.argv
ROUNDS = int(sys.argv[sys.argv.index("--rounds") + 1]) if "--rounds" in sys.argv else 1
SEED = 0                                  # changes every round
ROOT = os.path.join(MNT, "stress")
fails = []
manifest = {}      # relative path -> (size, sha256) of files that must survive the remount


def sha(data):
    return hashlib.sha256(data).hexdigest()


def blob(seed, n):
    r = random.Random(seed)
    return r.randbytes(n)


def check(cond, msg):
    if not cond:
        fails.append(msg)
        print("  FAIL:", msg, flush=True)
    return cond


def phase(name):
    print(f"[{time.strftime('%H:%M:%S')}] {name}", flush=True)
    return time.time()


def done(t0, extra=""):
    print(f"    done in {time.time() - t0:.1f} s {extra}", flush=True)


def write_file(path, data):
    with open(path, "wb") as f:
        f.write(data)


def read_file(path):
    with open(path, "rb") as f:
        return f.read()


# ---------------- A: many small files ----------------
def phase_a():
    t0 = phase("A: small files")
    nd, nf = (10, 20) if QUICK else (100, 80) if HEAVY else (60, 50)
    base = os.path.join(ROOT, "a")
    for d in range(nd):
        os.makedirs(os.path.join(base, f"d{d:03d}"))
    for d in range(nd):
        for i in range(nf):
            n = (d * 131 + i * 17) % 3000          # sizes 0..3000: inline files and one-block files
            data = blob(SEED * 1000003 + d * 1000 + i, n)
            p = os.path.join(base, f"d{d:03d}", f"f{i:03d}")
            write_file(p, data)
            manifest[os.path.relpath(p, MNT)] = (n, sha(data))
    bad = 0
    for rel, (n, h) in list(manifest.items()):
        if rel.startswith("stress/a/") and sha(read_file(os.path.join(MNT, rel))) != h:
            bad += 1
    check(bad == 0, f"A: {bad} small files read back wrong")
    # delete every second file, the others must stay
    for rel in list(manifest):
        if rel.startswith("stress/a/") and int(rel[-3:]) % 2 == 0:
            os.unlink(os.path.join(MNT, rel))
            del manifest[rel]
    bad = sum(1 for rel, (n, h) in manifest.items() if rel.startswith("stress/a/") and sha(read_file(os.path.join(MNT, rel))) != h)
    check(bad == 0, f"A: {bad} files damaged after deleting their neighbours")
    for d in range(nd):
        names = sorted(os.listdir(os.path.join(base, f"d{d:03d}")))
        check(names == [f"f{i:03d}" for i in range(nf) if i % 2], f"A: directory d{d:03d} lists the wrong names")
    done(t0, f"({nd * nf} files created, half deleted)")


# ---------------- B: random operations against a shadow copy ----------------
def phase_b():
    t0 = phase("B: random pwrite/pread/truncate against an in-memory copy")
    path = os.path.join(ROOT, "b.bin")
    rng = random.Random(4242 + SEED)
    shadow = bytearray()
    fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o644)
    ops = 600 if QUICK else 12000 if HEAVY else 3000
    limit = 6 * 1024 * 1024
    for i in range(ops):
        op = rng.random()
        if op < 0.55:                                    # write somewhere, maybe past the end (leaves a hole)
            off = rng.randrange(0, len(shadow) + 70000) if len(shadow) < limit else rng.randrange(0, limit)
            n = rng.choice([1, 7, 100, 511, 512, 513, 1000, 4095, 4096, 4097, 20000, 65536])
            data = rng.randbytes(n)
            os.pwrite(fd, data, off)
            if off > len(shadow):
                shadow.extend(bytes(off - len(shadow)))
            shadow[off:off + n] = data
        elif op < 0.80 and shadow:                       # read a range and compare
            off = rng.randrange(0, len(shadow))
            n = rng.choice([1, 100, 4096, 30000])
            got = os.pread(fd, n, off)
            if not check(got == bytes(shadow[off:off + n]), f"B: op {i}: read of {n} bytes at {off} differs"):
                break
        elif op < 0.92:                                  # shrink or grow with truncate
            n = rng.randrange(0, min(limit, len(shadow) + 50000))
            os.ftruncate(fd, n)
            if n < len(shadow):
                del shadow[n:]
            else:
                shadow.extend(bytes(n - len(shadow)))
        else:                                            # whole-file compare, also through a second descriptor
            check(os.fstat(fd).st_size == len(shadow), f"B: op {i}: size {os.fstat(fd).st_size} != {len(shadow)}")
            check(read_file(path) == bytes(shadow), f"B: op {i}: whole file differs")
    os.close(fd)
    check(read_file(path) == bytes(shadow), "B: whole file differs at the end")
    manifest[os.path.relpath(path, MNT)] = (len(shadow), sha(bytes(shadow)))
    done(t0, f"({ops} operations, final size {len(shadow)})")


# ---------------- C: several processes at once ----------------
def worker(args):
    wid, iters = args
    base = os.path.join(ROOT, f"c{wid}")
    os.makedirs(base, exist_ok=True)
    rng = random.Random(wid + 100 * SEED)
    errs = []
    alive = {}                                          # name -> (data, group): names in one group are hard links of one file
    links = {}                                          # symbolic links: name -> target text (they may dangle)
    groups = 0
    for i in range(iters):
        name = f"x{i}"
        data = rng.randbytes(rng.choice([0, 10, 200, 3000, 9000, 40000]))
        write_file(os.path.join(base, name), data)
        groups += 1
        alive[name] = (data, groups)
        act = rng.random()
        if act < 0.3 and alive:                          # rename
            old = rng.choice(list(alive))
            new = old + "r"
            if new not in alive:
                os.rename(os.path.join(base, old), os.path.join(base, new))
                alive[new] = alive.pop(old)
        elif act < 0.5 and alive:                        # hard link
            old = rng.choice(list(alive))
            new = old + "h"
            if new not in alive:
                os.link(os.path.join(base, old), os.path.join(base, new))
                alive[new] = alive[old]
        elif act < 0.65 and alive:                       # symlink
            old = rng.choice(list(alive))
            new = old + "s"
            if new not in alive and new not in links:
                os.symlink(old, os.path.join(base, new))
                links[new] = old
        elif act < 0.85 and len(alive) > 5:              # delete
            old = rng.choice(list(alive))
            os.unlink(os.path.join(base, old))
            del alive[old]
    for name, (data, grp) in alive.items():
        if read_file(os.path.join(base, name)) != data:
            errs.append(f"worker {wid}: {name} differs")
        want = sum(1 for d, g in alive.values() if g == grp)
        got = os.stat(os.path.join(base, name)).st_nlink
        if got != want:
            errs.append(f"worker {wid}: {name} has link count {got}, expected {want}")
    for name, target in links.items():
        if os.readlink(os.path.join(base, name)) != target:
            errs.append(f"worker {wid}: link {name} points somewhere else")
    listing = set(os.listdir(base))
    if listing != set(alive) | set(links):
        errs.append(f"worker {wid}: listing differs ({len(listing)} vs {len(alive) + len(links)})")
    return errs, {os.path.relpath(os.path.join(base, n), MNT): (len(d), sha(d)) for n, (d, g) in alive.items()}


def phase_c():
    t0 = phase("C: four processes working at once")
    iters = 40 if QUICK else 600 if HEAVY else 150
    with multiprocessing.Pool(4) as pool:
        results = pool.map(worker, [(w, iters) for w in range(4)])
    for errs, man in results:
        for e in errs:
            check(False, "C: " + e)
        manifest.update(man)
    done(t0, f"({4 * iters} files created by 4 workers)")


# ---------------- D: one big directory ----------------
def phase_d():
    t0 = phase("D: one directory with many entries")
    n = 800 if QUICK else 15000 if HEAVY else 4000
    base = os.path.join(ROOT, "d")
    os.makedirs(base)
    for i in range(n):
        with open(os.path.join(base, f"entry-{i:05d}"), "wb") as f:
            f.write(b"x" * (i % 50))
    names = os.listdir(base)
    check(len(names) == n and set(names) == {f"entry-{i:05d}" for i in range(n)}, f"D: listing has {len(names)} of {n} entries")
    bad = sum(1 for i in range(0, n, 7) if os.stat(os.path.join(base, f"entry-{i:05d}")).st_size != i % 50)
    check(bad == 0, f"D: {bad} entries with the wrong size")
    for i in range(0, n, 2):
        os.unlink(os.path.join(base, f"entry-{i:05d}"))
    left = set(os.listdir(base))
    check(left == {f"entry-{i:05d}" for i in range(1, n, 2)}, "D: wrong names left after deleting half")
    for name in left:
        manifest[os.path.relpath(os.path.join(base, name), MNT)] = (int(name[-5:]) % 50, sha(b"x" * (int(name[-5:]) % 50)))
    done(t0, f"({n} entries)")


# ---------------- E: renaming whole trees ----------------
def phase_e():
    t0 = phase("E: moving directory trees")
    base = os.path.join(ROOT, "e")
    os.makedirs(os.path.join(base, "src", "x", "y", "z"))
    files = {}
    for sub in ["", "x", "x/y", "x/y/z"]:
        for i in range(5):
            data = blob(hash(sub) % 1000 + i, 500 + i * 900)
            p = os.path.join(base, "src", sub, f"file{i}")
            write_file(p, data)
            files[os.path.join(sub, f"file{i}")] = data
    os.makedirs(os.path.join(base, "dst"))
    os.rename(os.path.join(base, "src"), os.path.join(base, "dst", "moved"))
    os.rename(os.path.join(base, "dst", "moved", "x"), os.path.join(base, "dst", "x2"))
    ok = True
    for rel, data in files.items():
        parts = rel.split("/")
        if parts[0] == "x" and len(parts) > 1:
            p = os.path.join(base, "dst", "x2", *parts[1:])
        else:
            p = os.path.join(base, "dst", "moved", rel)
        ok &= os.path.exists(p) and read_file(p) == data
        if os.path.exists(p):
            manifest[os.path.relpath(p, MNT)] = (len(data), sha(data))
    check(ok, "E: files lost or changed after moving the trees")
    check(not os.path.exists(os.path.join(base, "src")), "E: the old tree is still there")
    try:
        os.rename(os.path.join(base, "dst"), os.path.join(base, "dst", "moved", "inside"))
        check(False, "E: moving a directory into itself was allowed")
    except OSError:
        pass
    done(t0)


# ---------------- F: a big file ----------------
def phase_f():
    t0 = phase("F: big sequential file")
    mb = 16 if QUICK else 120 if HEAVY else 80
    path = os.path.join(ROOT, "big.bin")
    h = hashlib.sha256()
    with open(path, "wb") as f:
        for i in range(mb):
            chunk = blob(SEED * 7919 + i + 77, 1 << 20)
            f.write(chunk)
            h.update(chunk)
    check(os.stat(path).st_size == mb << 20, "F: wrong size")
    h2 = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            c = f.read(1 << 20)
            if not c:
                break
            h2.update(c)
    check(h.hexdigest() == h2.hexdigest(), "F: big file differs")
    manifest[os.path.relpath(path, MNT)] = (mb << 20, h.hexdigest())
    done(t0, f"({mb} MB)")


# ---------------- I: four processes doing random reads/writes/truncates on their own files at the same time ----------------
def fsx_worker(args):
    wid, ops = args
    path = os.path.join(ROOT, f"i{wid}.bin")
    rng = random.Random(900 + wid + 100 * SEED)
    shadow = bytearray()
    errs = []
    fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o644)
    for i in range(ops):
        op = rng.random()
        if op < 0.5:
            off = rng.randrange(0, len(shadow) + 40000) if len(shadow) < 3 << 20 else rng.randrange(0, 3 << 20)
            n = rng.choice([1, 100, 511, 4096, 4097, 9000, 33000])
            data = rng.randbytes(n)
            os.pwrite(fd, data, off)
            if off > len(shadow):
                shadow.extend(bytes(off - len(shadow)))
            shadow[off:off + n] = data
        elif op < 0.85 and shadow:
            off = rng.randrange(0, len(shadow))
            if os.pread(fd, 20000, off) != bytes(shadow[off:off + 20000]):
                errs.append(f"fsx {wid}: op {i}: read differs at {off}")
                break
        else:
            n = rng.randrange(0, min(3 << 20, len(shadow) + 20000))
            os.ftruncate(fd, n)
            if n < len(shadow):
                del shadow[n:]
            else:
                shadow.extend(bytes(n - len(shadow)))
    os.close(fd)
    if read_file(path) != bytes(shadow):
        errs.append(f"fsx {wid}: file differs at the end")
    return errs, {os.path.relpath(path, MNT): (len(shadow), sha(bytes(shadow)))}


def phase_i():
    t0 = phase("I: four processes doing random writes/reads/truncates on their own files at once")
    ops = 300 if QUICK else 2500 if HEAVY else 1000
    with multiprocessing.Pool(4) as pool:
        results = pool.map(fsx_worker, [(w, ops) for w in range(4)])
    for errs, man in results:
        for e in errs:
            check(False, "I: " + e)
        manifest.update(man)
    done(t0, f"({4 * ops} operations)")


# ---------------- J: kill the filesystem while four processes write; the volume must still be clean ----------------
def crash_writer(wid):
    base = os.path.join(MNT, "crash", f"w{wid}")
    os.makedirs(base, exist_ok=True)
    rng = random.Random(wid)
    try:
        for i in range(100000):
            write_file(os.path.join(base, f"f{i}"), rng.randbytes(rng.choice([100, 5000, 70000])))
            if i % 3 == 0:
                os.rename(os.path.join(base, f"f{i}"), os.path.join(base, f"g{i}"))
            if i % 5 == 0 and i:
                os.unlink(os.path.join(base, f"f{i - 1}")) if os.path.exists(os.path.join(base, f"f{i - 1}")) else None
    except OSError:
        pass                                              # the mount disappears under us: expected


def phase_j():
    t0 = phase("J: kill the filesystem process while four processes are writing")
    for r in range(3 if not QUICK else 1):
        umount()
        mount(IMG, MNT)
        procs = [multiprocessing.Process(target=crash_writer, args=(w,)) for w in range(4)]
        for p in procs:
            p.start()
        time.sleep(random.Random(r + SEED).uniform(0.4, 2.0))
        subprocess.run(["pkill", "-9", "-f", f"zenixfs-fuse {IMG}"])
        for p in procs:
            p.join(20)
            if p.is_alive():
                p.kill()
        subprocess.run(["fusermount3", "-uz", MNT], capture_output=True)
        time.sleep(0.5)
        rr = subprocess.run([TOOL, "fsck", IMG], capture_output=True, text=True)
        check(rr.returncode == 0, f"J: fsck after kill #{r + 1}: " + rr.stdout.strip()[:300])
        mount(IMG, MNT)
        shutil.rmtree(os.path.join(MNT, "crash"), ignore_errors=True)
        # what was in the manifest before must still be intact after a crash (it was committed long before)
        bad = 0
        for rel, (n, h) in list(manifest.items())[:2000]:
            try:
                d = read_file(os.path.join(MNT, rel))
            except OSError:
                bad += 1
                continue
            if len(d) != n or sha(d) != h:
                bad += 1
        check(bad == 0, f"J: {bad} older files damaged by crash #{r + 1}")
    done(t0)
# ---------------- G: unmount, fsck, mount again, verify ----------------
def umount():
    for _ in range(10):
        r = subprocess.run(["fusermount3", "-u", MNT], capture_output=True)
        if r.returncode == 0:
            return
        time.sleep(1)
    raise SystemExit("cannot unmount " + MNT)


def mount(img, mnt):
    subprocess.run([FUSE, img, mnt], check=True)
    time.sleep(0.5)


def phase_g():
    t0 = phase("G: unmount, fsck, mount again, verify everything")
    umount()
    r = subprocess.run([TOOL, "fsck", IMG], capture_output=True, text=True)
    check(r.returncode == 0, "G: fsck after the stress: " + r.stdout.strip()[:300])
    mount(IMG, MNT)
    bad = 0
    for rel, (n, h) in manifest.items():
        p = os.path.join(MNT, rel)
        try:
            d = read_file(p)
        except OSError:
            bad += 1
            continue
        if len(d) != n or sha(d) != h:
            bad += 1
    check(bad == 0, f"G: {bad} of {len(manifest)} files missing or changed after the remount")
    done(t0, f"({len(manifest)} files verified)")


# ---------------- H: fill a small volume ----------------
def phase_h():
    t0 = phase("H: fill a small volume until it is full, then recover")
    img = IMG + ".full"
    mnt = MNT + "-full"
    os.makedirs(mnt, exist_ok=True)
    subprocess.run([TOOL, "mkfs", img, "24"], check=True, capture_output=True)
    mount(img, mnt)
    chunk = blob(5, 1 << 20)
    names = []
    err = None
    for i in range(200):
        p = os.path.join(mnt, f"fill{i}")
        try:
            with open(p, "wb") as f:
                f.write(chunk)
            names.append(p)
        except OSError as e:
            err = e
            try:
                os.unlink(p)
            except OSError:
                pass
            break
    check(err is not None and err.errno == 28, f"H: expected ENOSPC when full, got {err}")
    check(len(names) >= 18, f"H: only {len(names)} MB fitted in a 24 MB volume")
    bad = sum(1 for p in names if read_file(p) != chunk)
    check(bad == 0, f"H: {bad} files damaged when the volume filled up")
    for p in names[: len(names) // 2]:
        os.unlink(p)                                      # removing must work on a full volume
    try:
        with open(os.path.join(mnt, "after"), "wb") as f:
            f.write(chunk)
    except OSError as e:
        check(False, f"H: cannot write after freeing space: {e}")
    subprocess.run(["fusermount3", "-u", mnt], check=True)
    r = subprocess.run([TOOL, "fsck", img], capture_output=True, text=True)
    check(r.returncode == 0, "H: fsck after filling: " + r.stdout.strip()[:300])
    os.unlink(img)
    os.rmdir(mnt)
    done(t0, f"({len(names)} MB written before ENOSPC)")


def main():
    global SEED
    start = time.time()
    for rnd in range(ROUNDS):
        SEED = rnd
        print(f"=== round {rnd + 1} of {ROUNDS}{' (heavy)' if HEAVY else ''}", flush=True)
        run_round()
    print(f"\n{len(fails)} failure(s) in {time.time() - start:.0f} s")
    for f in fails:
        print("  -", f)
    return 1 if fails else 0


def run_round():
    manifest.clear()
    shutil.rmtree(ROOT, ignore_errors=True)
    os.makedirs(ROOT)
    for ph in (phase_a, phase_b, phase_c, phase_d, phase_e, phase_f, phase_i, phase_j, phase_g, phase_h):
        ph()


if __name__ == "__main__":
    sys.exit(main())
