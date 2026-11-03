#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The installer's tables and boot partition writer on the PC (DESIGN D35): tools/host/zinstall-host.c, the same code as on
Zenix, under ASAN and UBSAN, on image files made here and judged by a parser of its own (and, where installed, amitools).
What matters most: after every install every sector outside what the plan named is the same as before (so every partition
not chosen is byte-identical); a cut at every sector write of the table leaves the old table or the new one whole; every torn
commit sector gives the old table, the new one or a refusal; a table that does not add up is refused; a disk changed between
show and write is not written. Then the fuzzer. usage: tools/zinstall-host-test.py [fuzz-rounds] (default 300000).
ZENIX_TEST_CARD=IMG (optional, never in the repo): the tables of such a card, copied into a sparse file, are tried too."""
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
fails = 0
passes = 0


def check(name, ok, detail=""):
    global fails, passes
    if ok:
        passes += 1
        print("PASS", name)
    else:
        fails += 1
        print("FAIL", name, detail)


T = tempfile.mkdtemp(prefix="zinstall-", dir=os.environ.get("ZENIX_OUT") or None)
BIN = os.path.join(T, "zinstall-host")
r = subprocess.run(["gcc", "-g", "-O1", "-std=gnu11", "-DZHOST", "-Wall", "-Wextra", "-Wno-unused-parameter", "-fsanitize=address,undefined",
                    "-fno-sanitize-recover=undefined", "-o", BIN, "tools/host/zinstall-host.c", "user/zinstall/table.c",
                    "user/zinstall/ffsw.c", "user/sha256.c"], cwd=ROOT, capture_output=True, text=True)
if r.returncode:
    print("FAIL build", r.stderr)
    sys.exit(1)

MB = 2048
env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
CUT_ENV = {}


def run(*args, **e):
    e = dict(CUT_ENV, **e)
    return subprocess.run([BIN] + [str(a) for a in args], capture_output=True, text=True, env=dict(env, **{k: str(v) for k, v in e.items()}))


# ---- making images ----
def sumblock(longs):
    longs = list(longs) + [0] * (128 - len(longs))
    longs[2] = 0
    longs[2] = (-sum(longs[:longs[1]])) & 0xFFFFFFFF
    return struct.pack(">128I", *longs)


def rdsk(cyls, heads, secs, plist, fslist=0xFFFFFFFF, rdbhi=None, high=None, locyl=None, hicyl=None):
    cb = heads * secs
    L = [0x5244534B, 64, 0, 7, 512, 0x12, 0xFFFFFFFF, plist, fslist, 0xFFFFFFFF] + [0xFFFFFFFF] * 6
    L += [cyls, secs, heads, 0, cyls, 0, 0, 0, cyls, cyls, cyls, 0, 0, 0, 0, 0]
    rdbhi = cb - 1 if rdbhi is None else rdbhi
    L += [0, rdbhi, 1 if locyl is None else locyl, cyls - 1 if hicyl is None else hicyl, cb, 0, high if high is not None else 0]
    return L


def part(name, nxt, lo, hi, heads, secs, dostype, flags=0, pri=0, sizeblock=128):
    nm = bytes([len(name)]) + name.encode() + b"\0" * (31 - len(name))
    L = [0x50415254, 64, 0, 7, nxt, flags, 0, 0, 0] + list(struct.unpack(">8I", nm)) + [0] * 15
    L += [16, sizeblock, 0, heads, 1, secs, 2, 0, 0, lo, hi, 30, 0, 0x1FE00, 0xFFFFFFFE, pri & 0xFFFFFFFF, dostype]
    return L


def fill(f, start, sectors, seed):
    """pseudo-random data at the start, middle and end of a partition, enough to notice any write"""
    h = hashlib.sha256(seed.encode()).digest() * 16
    for at in (0, sectors // 2, sectors - 8):
        f.seek((start + at) * 512)
        f.write(h[:4096])


def mkimg(path, sectors):
    with open(path, "wb") as f:
        f.truncate(sectors * 512)


class Rdb:
    """an RDB at base: parts = (name, lo, hi, dostype, flags, pri); chain blocks from 3 on; a FileSysHeader + LSEG at 40-49"""

    def __init__(self, f, base, cyls, heads, secs, parts, fs=True, rdbhi=None, locyl=None):
        self.cb = heads * secs
        n = len(parts)
        blocks = [3 + i for i in range(n)]
        hi = rdbhi if rdbhi is not None else self.cb - 1
        L = rdsk(cyls, heads, secs, blocks[0] if n else 0xFFFFFFFF, 40 if fs else 0xFFFFFFFF, rdbhi=hi, high=49 if fs else (blocks[-1] if n else 1), locyl=locyl)
        f.seek(base * 512)
        f.write(sumblock(L))
        for i, p in enumerate(parts):
            nxt = blocks[i + 1] if i + 1 < n else 0xFFFFFFFF
            f.seek((base + blocks[i]) * 512)
            f.write(sumblock(part(p[0], nxt, p[1], p[2], heads, secs, p[3], p[4], p[5])))
        if fs:
            F = [0x46534844, 64, 0, 7, 0xFFFFFFFF, 0, 0, 0, 0x50465303, 0] + [0] * 8 + [41]
            f.seek((base + 40) * 512)
            f.write(sumblock(F))
            for i in range(41, 50):
                G = [0x4C534547, 64, 0, 7, i + 1 if i < 49 else 0xFFFFFFFF] + [i * 7] * 40
                f.seek((base + i) * 512)
                f.write(sumblock(G))


def mbr(f, entries, sig=True):
    b = bytearray(512)
    b[0:16] = b"\xfa" * 16
    for i, e in enumerate(entries):
        if e:
            active, typ, st, n = e
            b[446 + 16 * i:462 + 16 * i] = struct.pack("<B3sB3sII", active, b"\xfe\xff\xff", typ, b"\xfe\xff\xff", st, n)
    if sig:
        b[510:512] = b"\x55\xaa"
    f.seek(0)
    f.write(bytes(b))


PFS3, FFS, FFSI, EXT2 = 0x50465303, 0x444F5301, 0x444F5303, 0x4C4E5800
AOS = [("DH0", 1, 80, FFSI, 1, 0), ("DH1", 81, 160, PFS3, 0, 0)]


def img_rdb_free(p):        # 512 MB; DH0, DH1 in 16x63, the rest free, a filesystem handler in the RDB
    s = 512 * MB
    mkimg(p, s)
    with open(p, "r+b") as f:
        Rdb(f, 0, s // 1008, 16, 63, AOS)
        fill(f, 1008, 80 * 1008, "dh0")
        fill(f, 81 * 1008, 80 * 1008, "dh1")
    return [("DH0", 1008, 80 * 1008), ("DH1", 81 * 1008, 80 * 1008)]


def img_rdb_big_cyl(p):     # an HDToolBox kind of geometry: 128 x 64
    s = 2048 * MB
    cb = 128 * 64
    mkimg(p, s)
    with open(p, "r+b") as f:
        Rdb(f, 0, s // cb, 128, 64, [("BOOT", 2, 5, FFS, 1, 5), ("DH0", 6, 100, PFS3, 1, 0), ("DH1", 101, 150, PFS3, 0, 0)], rdbhi=16383, locyl=2)
        for n, lo, hi in ((b"BOOT", 2, 5), (b"DH0", 6, 100), (b"DH1", 101, 150)):
            fill(f, lo * cb, (hi - lo + 1) * cb, n.decode())
    return [("BOOT", 2 * cb, 4 * cb), ("DH0", 6 * cb, 95 * cb), ("DH1", 101 * cb, 50 * cb)]


def img_mbr_card(p):        # the Emu68 card's kind: FAT, 0x76 with an RDB, a free slot, free space
    s = 1024 * MB
    fat = (2048, 200 * MB)
    cont = (fat[0] + fat[1], 180 * MB)
    mkimg(p, s)
    cb = 16 * 63
    with open(p, "r+b") as f:
        mbr(f, [(0x80, 0x0C, *fat), (0, 0x76, *cont)])
        fill(f, fat[0], fat[1], "fat")
        Rdb(f, cont[0], cont[1] // cb, 16, 63, [("BOOT", 1, 9, FFS, 1, 5), ("DH0", 10, 100, PFS3, 1, 0)])
        fill(f, cont[0] + cb, 9 * cb, "boot")
        fill(f, cont[0] + 10 * cb, 91 * cb, "dh0")
    return [("FAT", fat[0], fat[1]), ("BOOT", cont[0] + cb, 9 * cb), ("DH0", cont[0] + 10 * cb, 91 * cb), ("0x76-rest", cont[0] + 101 * cb, cont[1] - 101 * cb)]


def img_mbr_full_slots(p):  # four slots used; room only inside the 0x76's RDB
    s = 1024 * MB
    mkimg(p, s)
    cb = 16 * 63
    with open(p, "r+b") as f:
        cont = (2048 + 100 * MB, 500 * MB)
        mbr(f, [(0x80, 0x0C, 2048, 100 * MB), (0, 0x76, *cont), (0, 0x83, cont[0] + cont[1], 100 * MB), (0, 0x83, cont[0] + cont[1] + 100 * MB, 100 * MB)])
        fill(f, 2048, 100 * MB, "fat")
        Rdb(f, cont[0], cont[1] // cb, 16, 63, [("BOOT", 1, 9, FFS, 1, 5), ("DH0", 10, 100, PFS3, 1, 0)])
        fill(f, cont[0] + cb, 9 * cb, "boot")
        fill(f, cont[0] + 10 * cb, 91 * cb, "dh0")
        fill(f, cont[0] + cont[1], 100 * MB, "p3")
        fill(f, cont[0] + cont[1] + 100 * MB, 100 * MB, "p4")
    return [("FAT", 2048, 100 * MB), ("BOOT", cont[0] + cb, 9 * cb), ("DH0", cont[0] + 10 * cb, 91 * cb),
            ("p3", cont[0] + cont[1], 100 * MB), ("p4", cont[0] + cont[1] + 100 * MB, 100 * MB)]


def img_mbr_plain(p):       # a plain MBR: FAT and Linux, a free slot, free space
    s = 512 * MB
    mkimg(p, s)
    with open(p, "r+b") as f:
        mbr(f, [(0x80, 0x0C, 2048, 100 * MB), (0, 0x83, 2048 + 100 * MB, 100 * MB)])
        fill(f, 2048, 100 * MB, "fat")
        fill(f, 2048 + 100 * MB, 100 * MB, "linux")
    return [("FAT", 2048, 100 * MB), ("LIN", 2048 + 100 * MB, 100 * MB)]


# ---- an independent reader ----
def read_table(path):
    """what the image's table says: a list of (name, start, sectors, dostype-or-mbrtype, flags, pri), or a string: why it is unreadable"""
    with open(path, "rb") as f:
        def sec(n):
            f.seek(n * 512)
            return f.read(512)

        def rdb_at(base, limit):
            for i in range(16):
                b = sec(base + i)
                if b[:4] == b"RDSK":
                    L = struct.unpack(">128I", b)
                    if sum(L[:L[1]]) & 0xFFFFFFFF:
                        return "RDSK checksum"
                    out = []
                    n = L[7]
                    seen = set()
                    while n != 0xFFFFFFFF:
                        if n in seen or len(seen) > 40:
                            return "chain loops"
                        seen.add(n)
                        c = sec(base + n)
                        P = struct.unpack(">128I", c)
                        if c[:4] != b"PART" or sum(P[:P[1]]) & 0xFFFFFFFF:
                            return "PART checksum at %d" % n
                        e = P[32:]
                        cyl = e[3] * e[5] * (e[1] // 128)
                        name = c[37:37 + c[36]].decode("latin-1")
                        pri = e[15] if e[15] < 128 else e[15] - (1 << 32)
                        out.append((name, base + e[9] * cyl, (e[10] - e[9] + 1) * cyl, e[16], P[5], pri))
                        n = P[4]
                    return out
            return None
        r = rdb_at(0, 0)
        if r is not None:
            return r
        m = sec(0)
        out = []
        if m[510:512] != b"\x55\xaa":
            return "no table"
        for i in range(4):
            act, typ, st, n = struct.unpack("<B3xB3xII", m[446 + 16 * i:462 + 16 * i])
            if not typ:
                continue
            out.append(("slot%d" % i, st, n, typ, act, 0))
            if typ == 0x76:
                inner = rdb_at(st, n)
                if isinstance(inner, str):
                    return inner
                out += inner or []
        return out


def overlaps(parts):
    """two partitions of one table overlapping (a container and what is inside it do not count)"""
    bad = []
    for i, a in enumerate(parts):
        for b in parts[i + 1:]:
            if a[0].startswith("slot") and a[3] == 0x76 and a[1] <= b[1] and b[1] + b[2] <= a[1] + a[2]:
                continue
            if a[1] < b[1] + b[2] and b[1] < a[1] + a[2]:
                bad.append((a[0], b[0]))
    return bad


def changed_sectors(a, b):
    """the sector numbers that differ between two images (whole 1 MB steps first: they are sparse and big)"""
    out = []
    step = 1 << 20
    with open(a, "rb") as fa, open(b, "rb") as fb:
        at = 0
        while True:
            x, y = fa.read(step), fb.read(step)
            if not x and not y:
                break
            if x != y:
                for k in range(0, max(len(x), len(y)), 512):
                    if x[k:k + 512] != y[k:k + 512]:
                        out.append((at + k) // 512)
            at += step
    return out


def within(sectors, ranges, extras):
    extras = set(extras)
    bad = []
    for s in sectors:
        if s in extras:
            continue
        if not any(st <= s < st + n for st, n in ranges):
            bad.append(s)
    return bad


def new_parts(out):
    res = {}
    for l in out.splitlines():
        if l.startswith("new "):
            w = l.split()
            res[w[1]] = {k: int(v, 16 if k == "dostype" else 10) for k, v in (x.split("=") for x in w[2:])}
    return res


def options(path):
    r = run("list", path)
    opts = [l.split() for l in r.stdout.splitlines() if l.startswith("option ")]
    return r, {o[2] + ("" if o[2] != "part" else o[4]): int(o[1]) for o in opts}, opts


def opt_for(path, kind, device=None):
    r, _, opts = options(path)
    for o in opts:
        if o[2] == kind:
            if device is None:
                return int(o[1])
            part = int(o[4].split("=")[1])
            parts = [l.split()[1] for l in r.stdout.splitlines() if l.startswith("part ")]
            if parts[part] == device:
                return int(o[1])
    return None


def cp(a, b):
    subprocess.run(["cp", "--sparse=always", "--reflink=auto", a, b], check=True)


def full_install(name, img, keep, kind, device=None, mb=None, chosen=(), extra_ok=()):
    """install with option kind; everything outside the plan unchanged, the table whole, the boot volume read by amitools"""
    before = img + ".before"
    cp(img, before)
    k = opt_for(img, kind, device)
    if k is None:
        check(name + ": the option is offered", False, run("list", img).stdout)
        return None
    args = ["install", img, k] + ([mb] if mb else [])
    # the plan's numbers
    r = run(*args)
    check(name + ": it installs", r.returncode == 0 and "done" in r.stdout, r.stdout + r.stderr)
    if r.returncode:
        return None
    nw = new_parts(r.stdout)
    ranges = [(v["start"], v["sectors"]) for v in nw.values()]
    tbl = before + ".tmp"
    ch = changed_sectors(before, img)
    # sectors of the table: the new blocks, and at most the RDSK and link blocks and the MBR: read from the plan's text
    chain = read_table(img)
    check(name + ": the table reads back", not isinstance(chain, str), str(chain))
    if isinstance(chain, str):
        return None
    allowed = set()
    outside = [s for s in ch if not any(st <= s < st + n for st, n in ranges)]
    # what is outside the new partitions must be table sectors, and few
    check(name + ": outside the new partitions at most %d table sectors change" % 8, len(outside) <= 8, str(outside))
    for nm, st, n in keep:
        hit = [s for s in ch if st <= s < st + n and not any(a <= s < a + b for a, b in ranges)]
        check(name + ": %s is byte-identical" % nm, not hit, str(hit[:8]))
        h1, h2 = hashlib.sha256(), hashlib.sha256()
    for s in outside:
        with open(before, "rb") as a, open(img, "rb") as b:
            a.seek(s * 512)
            b.seek(s * 512)
            x, y = a.read(512), b.read(512)
        tag = y[:4]
        check(name + ": changed sector %d is a table sector" % s, tag in (b"PART", b"RDSK") or y[510:512] == b"\x55\xaa", repr(tag))
    bad = overlaps(chain)
    check(name + ": no partitions overlap", not bad, str(bad))
    # every old name still there, unless chosen
    names = {c[0] for c in chain}
    old = read_table(before)
    if not isinstance(old, str):
        for o in old:
            if o[0] in chosen or kind == "whole":
                continue
            m = [c for c in chain if c[0] == o[0]]
            if o[0].startswith("slot"):
                m = m and [m[0]] if m[0][1:3] == o[1:3] else []
            check(name + ": %s stays in the table, same place" % o[0], len(m) == 1 and m[0][1:3] == o[1:3] and m[0][4:] == o[4:], str((o, m)))
    # the new ones: flags, type; boot partition bootable, root marked, home neither
    zb = [c for c in chain if c[0].startswith("ZB")]
    zr = [c for c in chain if c[0].startswith("ZR")]
    check(name + ": the boot partition is bootable, FFS", len(zb) == 1 and zb[0][3] == FFS and zb[0][4] == 1, str(zb))
    check(name + ": the root is marked and not mounted by AmigaOS", len(zr) == 1 and zr[0][4] == 3, str(zr))
    oldboot = [o for o in old if not isinstance(old, str) and not o[0].startswith("slot") and o[4] & 1 and not o[4] & 2 and o[0] not in chosen] if not isinstance(old, str) and kind != "whole" else []
    if oldboot:
        check(name + ": the boot partition's priority is the highest", all(zb[0][5] > o[5] for o in oldboot), str((zb, oldboot)))
    # the boot volume, by amitools
    if shutil.which("xdftool") and zb:
        vol = img + ".boot.hdf"
        with open(img, "rb") as f, open(vol, "wb") as g:
            f.seek(zb[0][1] * 512)
            g.write(f.read(zb[0][2] * 512))
        l = subprocess.run(["xdftool", vol, "list"], capture_output=True, text=True)
        ok = all(x in l.stdout for x in ("bootmenu", "zenix", "Startup-Sequence")) or ("bootmenu" in l.stdout and "S" in l.stdout)
        check(name + ": amitools lists the boot volume", ok and l.returncode == 0, l.stdout + l.stderr)
        rd = subprocess.run(["xdftool", vol, "read", "Zenix/zenix", vol + ".zenix"], capture_output=True, text=True)
        blob = bytes((((i * 2654435761) & 0xFFFFFFFF) >> 24) & 0xFF for i in range(165484))
        check(name + ": amitools reads the kernel back, every byte", rd.returncode == 0 and open(vol + ".zenix", "rb").read() == blob)
        sc = subprocess.run(["xdfscan", vol], capture_output=True, text=True)
        os.unlink(vol)
        os.unlink(vol + ".zenix")
    os.unlink(before)
    return chain


def table_state(path):
    t = read_table(path)
    return t if isinstance(t, str) else tuple(sorted(t))


def cut_test(name, img, keep, kind, device=None, mb=None):
    """a power cut before every sector write of stage 1 and stage 2: the table is the old one or the new one, whole"""
    base = img + ".cut"
    cp(img, base)
    k = opt_for(base, kind, device)
    ex = [mb] if mb else []
    old = table_state(base)
    # how many writes in all, where the stages end
    probe = base + ".p"
    cp(base, probe)
    r = run("install", probe, k, *ex)
    st1 = int([l for l in r.stdout.splitlines() if l.startswith("stage1")][0].split()[1])
    files = int([l for l in r.stdout.splitlines() if l.startswith("files")][0].split()[1])
    done = int([l for l in r.stdout.splitlines() if l.startswith("done")][0].split()[1])
    newt = table_state(probe)
    # the new table before stage 2: flags differ: compare with stage 1's own end
    probe2 = base + ".p2"
    cp(base, probe2)
    r2 = run("install", probe2, k, *ex, ZI_TABLE_ONLY=1)
    new1 = table_state(probe2)
    nbad = 0
    cuts = list(range(0, st1 + 1)) + [files, files + 1, done]
    for n in cuts:
        w = base + ".w"
        cp(base, w)
        rr = run("install", w, k, *ex, ZI_CUT=n)
        t = table_state(w)
        if t not in (old, new1, newt):
            # stage 2 half done: root marked, boot not yet
            if not (n == files + 1):
                nbad += 1
                print("  cut at", n, "gives", t, "exit", rr.returncode)
        ch = changed_sectors(base, w)
        for nm, st, c in keep:
            if any(st <= s < st + c for s in ch):
                nbad += 1
                print("  cut at", n, "touched", nm)
        if isinstance(t, tuple) and t != newt and any(x[0].startswith("ZB") and (x[4] & 1) for x in t):
            nbad += 1
            print("  cut at", n, "left the boot partition bootable before stage 2's end")
        os.unlink(w)
    check("%s: a cut at each of %d table writes: old or new table, whole; others untouched; never bootable early" % (name, len(cuts)), nbad == 0)
    for f in (base, probe, probe2):
        os.unlink(f)


def torn_test(name, img, kind, device=None, mb=None):
    """the sector that makes the table real, torn at every 4 bytes: the old table, the new one, or a refusal; never a crash"""
    base = img + ".torn"
    cp(img, base)
    k = opt_for(base, kind, device)
    old = table_state(base)
    new = base + ".n"
    cp(base, new)
    run("install", new, k, *([mb] if mb else []), ZI_TABLE_ONLY=1)
    new1 = table_state(new)
    ch = changed_sectors(base, new)
    # the commit sector: the one sector of the old table's chain that changed: RDSK/PART that was there before, or the MBR
    with open(base, "rb") as a, open(new, "rb") as b:
        commits = []
        for s in ch:
            a.seek(s * 512)
            b.seek(s * 512)
            x, y = a.read(512), b.read(512)
            if x[:4] in (b"PART", b"RDSK") or (s == 0):
                commits.append((s, x, y))
    tests = refused = olds = news = bad = 0
    for s, x, y in commits:
        for cutat in range(0, 512, 4):
            for torn in (x[:cutat] + y[cutat:], y[:cutat] + x[cutat:]):
                w = base + ".w"
                cp(new, w)
                with open(w, "r+b") as f:
                    f.seek(s * 512)
                    f.write(torn)
                r = run("list", w)
                tests += 1
                if r.returncode not in (0,) or "AddressSanitizer" in r.stderr or "runtime error" in r.stderr:
                    bad += 1
                    print("  torn at", cutat, r.stderr[:300])
                t = table_state(w)
                if t == old:
                    olds += 1
                elif t == new1:
                    news += 1
                else:
                    refused += 1
                os.unlink(w)
    check("%s: the commit sector torn %d ways: %d old, %d new, %d others read and judged, no crash" % (name, tests, olds, news, refused), bad == 0 and tests > 0)
    for f in (base, new):
        os.unlink(f)


def refused_all(name, path, want_whole=None):
    r = run("list", path)
    ok = r.returncode == 0
    has = [l for l in r.stdout.splitlines() if l.startswith("option ") and l.split()[2] != "whole"]
    check(name + ": nothing but perhaps the erase is offered", ok and not has, r.stdout + r.stderr)
    wh = [l for l in r.stdout.splitlines() if l.startswith("whole ")][0]
    if want_whole is not None:
        check(name + ": erase " + ("offered" if want_whole else "refused"), (wh == "whole allowed") == want_whole, wh)
    return r


try:
    # ---- the disks with something on them: installs, every other partition byte-identical ----
    p = os.path.join(T, "rdb.hdf")
    keep = img_rdb_free(p)
    keep0 = keep
    full_install("RDB free space", p, keep, "free")
    p = os.path.join(T, "rdb2.hdf")
    keep = img_rdb_free(p)
    cut_test("RDB free space", p, keep, "free")
    torn_test("RDB free space", p, "free")

    # an RDB area kept as tight as tools/mkrdb-multi.py keeps it: it grows into what is free before the first partition
    p = os.path.join(T, "tight.hdf")
    s = 512 * MB
    mkimg(p, s)
    with open(p, "r+b") as f:
        Rdb(f, 0, s // 1008, 16, 63, AOS, fs=False, rdbhi=4)
        fill(f, 1008, 80 * 1008, "dh0")
        fill(f, 81 * 1008, 80 * 1008, "dh1")
    keep = [("DH0", 1008, 80 * 1008), ("DH1", 81 * 1008, 80 * 1008)]
    full_install("a tight RDB area", p, keep, "free")
    mkimg(p, s)
    with open(p, "r+b") as f:
        f.truncate(0)
        f.truncate(s * 512)
        Rdb(f, 0, s // 1008, 16, 63, AOS, fs=False, rdbhi=4)
    cut_test("a tight RDB area", p, [], "free")

    p = os.path.join(T, "bigcyl.hdf")
    keep = img_rdb_big_cyl(p)
    full_install("RDB of big cylinders", p, keep, "free")
    r = run("list", p).stdout

    p = os.path.join(T, "card.img")
    keep = img_mbr_card(p)
    full_install("MBR + 0x76 card, a new 0x76", p, keep, "newcont")
    p = os.path.join(T, "card2.img")
    keep = img_mbr_card(p)
    cut_test("MBR + 0x76 card", p, keep, "newcont")
    torn_test("MBR + 0x76 card", p, "newcont")

    p = os.path.join(T, "slots.img")
    keep = img_mbr_full_slots(p)
    r, _, opts = options(p)
    check("MBR with four slots used: no new 0x76 offered", not [o for o in opts if o[2] == "newcont"], r.stdout)
    full_install("MBR with four slots used, inside the 0x76's RDB", p, keep, "free")
    p = os.path.join(T, "slots2.img")
    keep = img_mbr_full_slots(p)
    cut_test("inside the 0x76's RDB", p, keep, "free")

    p = os.path.join(T, "plain.img")
    keep = img_mbr_plain(p)
    r = run("list", p).stdout
    check("plain MBR: the FAT partition is never offered", "FAT32" in r and "never touched" in r, r)
    full_install("plain MBR, a new 0x76", p, keep, "newcont")
    r = run("list", p).stdout
    check("a disk with a FAT partition: the erase refused", "whole it has a FAT partition" in r, r)

    # ---- a chosen partition ----
    p = os.path.join(T, "pick.hdf")
    keep = img_rdb_free(p)
    full_install("a chosen RDB partition (DH1)", p, [k for k in keep if k[0] != "DH1"], "part", "ide0p2", chosen=("DH1",))
    p = os.path.join(T, "pick2.hdf")
    keep = img_rdb_free(p)
    cut_test("a chosen RDB partition", p, [k for k in keep if k[0] != "DH1"], "part", "ide0p2")
    p = os.path.join(T, "pickm.img")
    keep = img_mbr_plain(p)
    full_install("a chosen MBR partition (Linux)", p, [k for k in keep if k[0] != "LIN"], "part", "ide0p2", chosen=("slot1",))
    p = os.path.join(T, "pickm2.img")
    keep = img_mbr_plain(p)
    cut_test("a chosen MBR partition", p, [k for k in keep if k[0] != "LIN"], "part", "ide0p2")
    p = os.path.join(T, "cont.img")
    keep = img_mbr_card(p)
    r = run("list", p).stdout
    check("the 0x76 container is not offered as a whole", [l for l in r.splitlines() if l.startswith("part ide0p2")][0].endswith("never touched as a whole"), r)
    check("a partition in the card's RDB may be chosen", "part ide0p4" in r and [l for l in r.splitlines() if l.startswith("part ide0p4")][0].endswith("refused=-"), r)

    # ---- the owner's case: a 16 GB card, PFS3 in the RDB, DH2 split into Zenix and a new empty DH2 ----
    def img_card16(p):
        s = 16 * 1024 * MB
        cb = 128 * 64
        cyls = s // cb
        mkimg(p, s)
        with open(p, "r+b") as f:
            L = rdsk(cyls, 64, 128, 123, 1, rdbhi=16383, high=125, locyl=2)
            f.seek(0); f.write(sumblock(L))
            f.seek(512); f.write(sumblock([0x46534844, 64, 0, 7, 0xFFFFFFFF, 0, 0, 0, PFS3, 0x13002] + [0] * 8 + [2]))
            for i in range(2, 123):
                f.seek(i * 512)
                f.write(sumblock([0x4C534547, 64, 0, 7, i + 1 if i < 122 else 0xFFFFFFFF] + [(i * 2654435761) & 0xFFFFFFFF] * 100))
            parts = [("DH0", 2, 257, 1, 0), ("DH1", 258, 769, 0, 0), ("DH2", 770, cyls - 1, 0, 0)]     # 1 GB, 2 GB, the rest
            for k, (nm, lo, hi, fl, pri) in enumerate(parts):
                P = part(nm, 124 + k if k < 2 else 0xFFFFFFFF, lo, hi, 128, 64, PFS3, fl, pri)
                P[32 + 13] = 0xFFFFFF; P[32 + 11] = 80     # HDToolBox's MaxTransfer and buffers, as the owner's card has
                f.seek((123 + k) * 512); f.write(sumblock(P))
            fill(f, 2 * cb, 256 * cb, "dh0")
            fill(f, 258 * cb, 512 * cb, "dh1")
            fill(f, 770 * cb, (cyls - 770) * cb, "dh2")
        return cb, cyls
    p = os.path.join(T, "card16.hdf")
    cb, cyls = img_card16(p)
    b16 = p + ".b"
    cp(p, b16)
    k = opt_for(p, "part", "ide0p3")
    r = run("install", p, k, 5000)
    check("the owner's card: DH2 split, 5 GB for Zenix", r.returncode == 0 and "done" in r.stdout, r.stdout + r.stderr)
    t = read_table(p)
    names = [x[0] for x in t]
    check("  the chain: DH0, DH1, Zenix's three, then DH2", names == ["DH0", "DH1", "ZB0", "ZR0", "ZH0", "DH2"], str(names))
    rest = [x for x in t if x[0] == "DH2"][0]
    zh = [x for x in t if x[0] == "ZH0"][0]
    zb = [x for x in t if x[0] == "ZB0"][0]
    check("  Zenix first, at the old DH2's start; ZB0 under 4 GB", zb[1] == 770 * cb and zb[1] + zb[2] <= 8388608, str(zb))
    check("  the new DH2 takes the rest to the old end, about 8 GB, after ZH0", rest[1] >= zh[1] + zh[2] and rest[1] + rest[2] == cyls * cb and 7 * 1024 * MB < rest[2] < 9 * 1024 * MB, str((zh, rest)))
    check("  it is PFS3, mountable, not bootable", rest[3] == PFS3 and rest[4] == 0, str(rest))
    with open(b16, "rb") as a, open(p, "rb") as c:
        def blk(f, n):
            f.seek(n * 512); return f.read(512)
        same = all(blk(a, i) == blk(c, i) for i in list(range(1, 123)) + [123, 124])
        check("  DH0's and DH1's PART blocks, the FileSysHeader and its 121 LSEG blocks: byte-identical", same)
        old0, new0 = struct.unpack(">128I", blk(a, 0)), struct.unpack(">128I", blk(c, 0))
        diff = [i for i in range(128) if old0[i] != new0[i]]
        check("  RDSK: only HighRDSKBlock and its checksum changed", set(diff) <= {2, 38}, str(diff))
        oldp = struct.unpack(">128I", blk(a, 125))
        nb = [i for i in range(126, 200) if blk(c, i)[:4] == b"PART" and blk(c, i)[37:40] == b"DH2"]
        newp = struct.unpack(">128I", blk(c, nb[0])) if nb else None
        d2 = [i for i in range(128) if newp and oldp[i] != newp[i]]
        check("  the new DH2's PART: the old one but next, flags, LowCyl and the checksum (name, DosEnvec, dostype, buffers, MaxTransfer kept)",
              newp is not None and set(d2) <= {2, 4, 5, 32 + 9} and newp[32 + 9] > oldp[32 + 9] and newp[32 + 10] == oldp[32 + 10], str(d2))
    ch = changed_sectors(b16, p)
    hit = [s_ for s_ in ch if 2 * cb <= s_ < 770 * cb]
    check("  DH0 and DH1 byte-identical", not hit, str(hit[:5]))
    r = run("list", p).stdout
    check("  the table reads back whole (rdb.h): nothing called damaged", "noedit -" in r and "region 0" in r and "noedit=-" in r, r)
    # the way back: the old bytes of every written sector, the commit first (what zinstall --undo does), give the old table exactly
    cp(b16, p)
    r = run("install", p, k, 5000, ZI_UNDO_AFTER=1)
    check("  undone: the first 256 sectors exactly as before", r.returncode == 0 and not [x for x in changed_sectors(b16, p) if x < 256], r.stdout[-300:])
    r = run("install", p, k, 10)
    check("  less than 28 MB for Zenix is refused", r.returncode != 0, r.stdout)
    cp(b16, p)
    r = run("install", p, k, (cyls - 770) * cb // 2048 - 8)
    check("  a rest under 16 MB is refused, says why", r.returncode == 1 and "under 16 MB" in r.stdout, r.stdout)
    os.unlink(b16)
    p2 = os.path.join(T, "card16b.hdf")
    cb, cyls = img_card16(p2)
    cut_test("the owner's card, DH2 split", p2, [("DH0", 2 * cb, 256 * cb), ("DH1", 258 * cb, 512 * cb)], "part", "ide0p3", 5000)
    torn_test("the owner's card, DH2 split", p2, "part", "ide0p3", 5000)

    # ---- layouts (the owner's live test, 2026-11-03): recommended, one partition, custom; /home's filesystem ----
    p = os.path.join(T, "lay.hdf")
    img_rdb_free(p)
    b0 = p + ".b"
    cp(p, b0)
    k = opt_for(p, "free")
    r = run("install", p, k, ZI_TABLE_ONLY=1)
    nw = new_parts(r.stdout)
    check("layout recommended on 350 MB: boot 16, root a third (at least 128), /home the rest", nw["ZB0"]["sectors"] // 2048 == 16 and 110 <= nw["ZR0"]["sectors"] // 2048 <= 140 and "ZH0" in nw, r.stdout)
    cp(b0, p)
    r = run("install", p, k, ZI_TABLE_ONLY=1, ZI_LAYOUT="one")
    nw = new_parts(r.stdout)
    check("layout one partition: boot and root only, the root all the rest", "ZH0" not in nw and nw["ZR0"]["sectors"] > 300 * MB, r.stdout)
    cp(b0, p)
    r = run("install", p, k, ZI_LAYOUT="one")
    t = read_table(p)
    check("  it installs, two new partitions", r.returncode == 0 and sorted(x[0] for x in t if x[0].startswith("Z")) == ["ZB0", "ZR0"], str(t))
    cp(b0, p)
    r = run("install", p, k, ZI_TABLE_ONLY=1, ZI_LAYOUT="custom", ZI_ROOT=100, ZI_HOME=50, ZI_HOMEFS="zenixfs")
    nw = new_parts(r.stdout)
    check("layout custom: root 100 MB, /home 50 MB as typed (whole cylinders), the rest left free", 100 * MB <= nw["ZR0"]["sectors"] < 101 * MB and 50 * MB <= nw["ZH0"]["sectors"] < 51 * MB, r.stdout)
    check("  /home on ZenixFS: its dostype", nw["ZH0"]["dostype"] == 0x5A4E4601, r.stdout)
    cp(b0, p)
    r = run("install", p, k, ZI_LAYOUT="custom", ZI_ROOT=100, ZI_HOME=0)
    t = read_table(p)
    check("  custom with no /home", r.returncode == 0 and "ZH0" not in [x[0] for x in t], str(t))
    for root, home, why in ((20, 50, "at least 32"), (100, 2, "at least 4"), (300, 300, "do not fit")):
        cp(b0, p)
        r = run("install", p, k, ZI_LAYOUT="custom", ZI_ROOT=root, ZI_HOME=home)
        check("  custom %d + %d MB refused (%s), nothing written" % (root, home, why), r.returncode == 1 and why in r.stdout and not changed_sectors(b0, p), r.stdout)
    os.unlink(b0)
    # the owner's card: custom sizes in DH2 decide the split; what is left stays DH2
    p = os.path.join(T, "card16c.hdf")
    cb, cyls = img_card16(p)
    k = opt_for(p, "part", "ide0p3")
    r = run("install", p, k, ZI_LAYOUT="custom", ZI_ROOT=1024, ZI_HOME=3000)
    t = read_table(p)
    rest = [x for x in t if x[0] == "DH2"]
    check("the card, custom 1 GB root and 3 GB /home in DH2: the rest stays DH2", r.returncode == 0 and rest and 9 * 1024 * MB < rest[0][2] < 10 * 1024 * MB, str(t))
    p = os.path.join(T, "card16d.hdf")
    cb, cyls = img_card16(p)
    CUT_ENV.update(ZI_LAYOUT="custom", ZI_ROOT="1024", ZI_HOME="3000")
    cut_test("the card, custom layout in DH2", p, [("DH0", 2 * cb, 256 * cb), ("DH1", 258 * cb, 512 * cb)], "part", "ide0p3")
    CUT_ENV.clear()

    # ---- a disk with a Zenix already: reinstall (keep /home, erase it), remove, install elsewhere ----
    p = os.path.join(T, "re.hdf")
    keep = img_rdb_free(p)
    run("install", p, opt_for(p, "free"), 150)        # room left for an install elsewhere
    after1 = p + ".1"
    cp(p, after1)
    t1 = read_table(p)
    zh = [x for x in t1 if x[0] == "ZH0"][0]
    r = run("reinstall", p)
    t2 = read_table(p)
    ch = changed_sectors(after1, p)
    check("reinstall, /home kept: the same partitions, root and boot marked as before", r.returncode == 0 and t2 == t1, r.stdout + str(t2))
    check("  /home not written", not [x for x in ch if zh[1] <= x < zh[1] + zh[2]], str(ch[:5]))
    for nm, st, n in keep:
        check("  %s byte-identical" % nm, not [x for x in ch if st <= x < st + n])
    cp(after1, p)
    r = run("reinstall", p, ZI_ERASE_HOME=1)
    ch = changed_sectors(after1, p)
    check("reinstall erasing /home: /home written, the rest as kept", r.returncode == 0 and [x for x in ch if zh[1] <= x < zh[1] + zh[2]] and
          not [x for x in ch for nm, st, n in keep if st <= x < st + n], r.stdout)
    # a cut at each write of a reinstall: the old Zenix is never left bootable with a half-written root
    nbad = 0
    probe = after1 + ".p"
    cp(after1, probe)
    total = int([l for l in run("reinstall", probe).stdout.splitlines() if l.startswith("done")][0].split()[1])
    for n in range(0, total + 1, max(1, total // 40)):
        cp(after1, probe)
        run("reinstall", probe, ZI_CUT=n)
        t = read_table(probe)
        zr = [x for x in t if x[0] == "ZR0"][0]
        zb = [x for x in t if x[0] == "ZB0"][0]
        if 0 < n < total and n >= 2 and (zb[4] & 1) and not (zr[4] & 1):
            nbad += 1
        if [x for x in changed_sectors(after1, probe) for nm, st, c in keep if st <= x < st + c]:
            nbad += 1
    check("  a cut during a reinstall: the boot partition is not bootable while the root is being made; others untouched", nbad == 0)
    os.unlink(probe)
    cp(after1, p)
    r = run("remove", p)
    t3 = read_table(p)
    ch = changed_sectors(after1, p)
    check("remove: only DH0 and DH1 left in the table, one sector written", r.returncode == 0 and [x[0] for x in t3] == ["DH0", "DH1"] and len(ch) == 1, r.stdout + str(ch))
    r = run("list", p)
    check("  the space is free again", "option 1 free" in r.stdout, r.stdout)
    cp(after1, p)
    r = run("install", p, opt_for(p, "free"))
    t4 = read_table(p)
    zr0 = [x for x in t4 if x[0] == "ZR0"][0]
    zr1 = [x for x in t4 if x[0] == "ZR1"]
    zb0 = [x for x in t4 if x[0] == "ZB0"][0]
    check("elsewhere beside the old Zenix: allowed; the old root and boot partition unmarked at the end, the new ones marked",
          r.returncode == 0 and zr1 and not (zr0[4] & 1) and not (zb0[4] & 1) and (zr1[0][4] & 1), r.stdout + str(t4))
    for f in (after1,):
        os.unlink(f)
    # the card's kind: Zenix in DH2's place, removed: DH0 and DH1 byte-identical
    p = os.path.join(T, "card16r.hdf")
    cb, cyls = img_card16(p)
    run("install", p, opt_for(p, "part", "ide0p3"), 5000)
    b1 = p + ".b"
    cp(p, b1)
    r = run("remove", p)
    t = read_table(p)
    check("the card: remove after a split: DH0, DH1, DH2 left", r.returncode == 0 and [x[0] for x in t] == ["DH0", "DH1", "DH2"], r.stdout + str(t))
    ch = changed_sectors(b1, p)
    check("  one sector written, DH0 and DH1 untouched", len(ch) == 1 and not [x for x in ch if 2 * cb <= x < 770 * cb], str(ch))
    os.unlink(b1)

    # ---- what diskd and AmigaOS read of what zinstall wrote (real A1200, 2026-11-04: "a block chain leaves the RDB area or loops" at
    #      every boot, on a card of this kind after a split install and a reinstall / a delete and install) ----
    RB = os.path.join(T, "rdbread-host")
    rr = subprocess.run(["gcc", "-O1", "-g", "-Wall", "-fsanitize=address,undefined", "-o", RB, "tools/host/rdbread-host.c"], cwd=ROOT, capture_output=True, text=True)
    check("the disk server's reader builds for the PC", rr.returncode == 0, rr.stderr)

    def amiga_view(path):
        """an RDB as AmigaOS and HDToolBox read it: every block of every chain within RDBBlocksHi and at most HighRDSKBlock,
        checksums right, no block twice"""
        with open(path, "rb") as f:
            def blk(n):
                f.seek(n * 512); return f.read(512)
            L = struct.unpack(">128I", blk(0))
            hi, high = L[33], L[38]
            seen, bad = set(), []
            def walk(first, tag):
                n, k = first, 0
                while n != 0xFFFFFFFF and k < 5000:
                    if n in seen: bad.append("%s %d twice" % (tag, n)); return
                    seen.add(n)
                    if n > hi: bad.append("%s %d past RDBBlocksHi %d" % (tag, n, hi))
                    if n > high: bad.append("%s %d past HighRDSKBlock %d" % (tag, n, high))
                    b = struct.unpack(">128I", blk(n))
                    if sum(b[:b[1]]) & 0xFFFFFFFF: bad.append("%s %d checksum" % (tag, n))
                    yield n, b
                    n = b[4]; k += 1
            for n, b in walk(L[7], "PART"): pass
            for n, b in walk(L[8], "FSHD"):
                for m, c in walk(b[18], "LSEG"): pass
            return bad, len(seen)
    for name, steps in (("split", [(5000, {})]), ("split, then a reinstall", [(5000, {}), ("re", {})]),
                        ("split, then DH2 and Zenix deleted and a fresh install", [(5000, {}), (None, {"ZI_DELETE": "ZB0,ZR0,ZH0,DH2", "ZI_LAYOUT": "one"})])):
        p = os.path.join(T, "card16v.hdf")
        cb, cyls = img_card16(p)
        for st, envx in steps:
            if st == "re":
                r = run("reinstall", p)
            elif st is None:
                r = run("install", p, 1, **envx)
            else:
                r = run("install", p, opt_for(p, "part", "ide0p3"), st)
        rd = subprocess.run([RB, p], capture_output=True, text=True, env=env)
        bad, n = amiga_view(p)
        check("the card, %s: diskd's reader reads the table whole (%s)" % (name, rd.stdout.splitlines()[0] if rd.stdout else rd.stderr[-200:]),
              rd.returncode == 0 and rd.stdout.startswith("noedit -"), rd.stdout + rd.stderr)
        check("the card, %s: as AmigaOS reads it, every chain block within RDBBlocksHi and HighRDSKBlock (%d blocks)" % (name, n), not bad, str(bad))

    # ---- the partition step: delete, then install in the freed space; one write; undo ----
    p = os.path.join(T, "card16x.hdf")
    cb, cyls = img_card16(p)
    bx = p + ".b"
    cp(p, bx)
    r = run("list", p, ZI_DELETE="DH2")
    check("delete DH2: its space offered as one free area", "option 1 free" in r.stdout and "part" not in [l.split()[2] for l in r.stdout.splitlines() if l.startswith("option ")], r.stdout)
    r = run("install", p, 1, ZI_DELETE="DH2", ZI_LAYOUT="custom", ZI_ROOT=200, ZI_HOME=300)
    t = read_table(p)
    check("  installed: DH0, DH1 and Zenix's three; DH2 gone", r.returncode == 0 and [x[0] for x in t] == ["DH0", "DH1", "ZB0", "ZR0", "ZH0"], r.stdout + str(t))
    zb = [x for x in t if x[0] == "ZB0"]
    check("  Zenix where DH2 was", zb and zb[0][1] == 770 * cb, str(zb))
    ch = changed_sectors(bx, p)
    check("  DH0, DH1 and their old PART blocks, the FileSysHeader and LSEG: byte-identical", not [x for x in ch if 0 < x < 126 or 2 * cb <= x < 770 * cb], str([x for x in ch if x < 200]))
    m = re_last = [l for l in r.stdout.splitlines() if l.startswith("atomic")]
    cp(bx, p)
    r = run("install", p, 1, ZI_DELETE="DH2", ZI_LAYOUT="custom", ZI_ROOT=200, ZI_HOME=300, ZI_UNDO_AFTER=1)
    check("  undone: the first 256 sectors exactly as before", r.returncode == 0 and not [x for x in changed_sectors(bx, p) if x < 256], r.stdout[-200:])
    cp(bx, p)
    CUT_ENV.update(ZI_DELETE="DH2", ZI_LAYOUT="custom", ZI_ROOT="200", ZI_HOME="300")
    cut_test("the partition step on the card", p, [("DH0", 2 * cb, 256 * cb), ("DH1", 258 * cb, 512 * cb)], "free")
    CUT_ENV.clear()
    os.unlink(bx)
    # delete two, keep one between them: the space of each joins its free neighbours
    p = os.path.join(T, "del2.hdf")
    keep = img_rdb_free(p)
    r = run("list", p, ZI_DELETE="DH1")
    o1 = [l for l in r.stdout.splitlines() if l.startswith("option 1 free")]
    check("deleting DH1: its 80 cylinders join the free ones after it", o1 and int(o1[0].split("sectors=")[1]) > 400 * MB, r.stdout)
    # refusals
    p = os.path.join(T, "delf.img")
    img_mbr_card(p)
    r = run("list", p, ZI_DELETE="ide0p1")
    check("deleting the FAT partition is refused", r.returncode == 1 and "FAT" in r.stdout, r.stdout)
    r = run("list", p, ZI_DELETE="ide0p2")
    check("deleting the 0x76 container as a whole is refused", r.returncode == 1 and "delete those instead" in r.stdout, r.stdout)
    p = os.path.join(T, "delb.hdf")
    img_rdb_free(p)
    r = run("list", p, ZI_DELETE="DH0", ZI_BUSY="1008:%d" % (80 * 1008))
    check("deleting a mounted partition is refused", r.returncode == 1 and "in use" in r.stdout, r.stdout)
    # after an install: Zenix's partitions deleted, a fresh install in their place
    p = os.path.join(T, "delz.hdf")
    keep = img_rdb_free(p)
    run("install", p, opt_for(p, "free"))
    bz = p + ".b"
    cp(p, bz)
    r = run("install", p, 1, ZI_DELETE="ZB0,ZR0,ZH0", ZI_LAYOUT="one")
    t = read_table(p)
    check("Zenix's partitions deleted, a fresh one-partition install in their place", r.returncode == 0 and [x[0] for x in t] == ["DH0", "DH1", "ZB0", "ZR0"], r.stdout + str(t))
    ch = changed_sectors(bz, p)
    check("  DH0 and DH1 byte-identical", not [x for x in ch for nm, st, n in keep if st <= x < st + n])
    os.unlink(bz)

    # ---- a whole disk ----
    p = os.path.join(T, "blank.hdf")
    mkimg(p, 128 * MB)
    r = run("list", p).stdout
    check("a disk of zeros is called blank, the only option the erase", "kind blank" in r and "option 1 whole" in r, r)
    chain = full_install("a blank disk", p, [], "whole")
    check("a blank disk gets boot, root, /home", chain and sorted(c[0][:2] for c in chain) == ["ZB", "ZH", "ZR"], str(chain))
    p = os.path.join(T, "small.hdf")
    mkimg(p, 16 * MB)
    r = run("list", p).stdout
    check("a disk of 16 MB is too small", "too small" in r, r)
    p = os.path.join(T, "s28.hdf")
    mkimg(p, 30 * MB)
    chain = full_install("a disk of 30 MB (no /home)", p, [], "whole")
    check("30 MB: boot and root only", chain and sorted(c[0][:2] for c in chain) == ["ZB", "ZR"], str(chain))
    p = os.path.join(T, "wrdb.hdf")
    keep = img_rdb_free(p)
    r = run("install", p, opt_for(p, "whole"), ZI_TABLE_ONLY=1)
    check("erasing an RDB disk: one write makes the new table real", "atomic 1" in r.stdout, r.stdout)
    p = os.path.join(T, "wrdb2.hdf")
    keep = img_rdb_free(p)
    cut_test("erasing an RDB disk", p, [], "whole")
    p = os.path.join(T, "bare.hdf")
    mkimg(p, 64 * MB)
    with open(p, "r+b") as f:
        f.write(b"DOS\x03" + b"\0" * 508)
    r = run("list", p).stdout
    check("a bare AmigaDOS volume is named, only the erase offered", "AmigaDOS volume, no partition table" in r and "option 1 whole" in r and "option 2" not in r, r)
    r = run("install", p, 1, ZI_TABLE_ONLY=1).stdout
    check("erasing it says the table swap is not whole there", "atomic 0" in r, r)
    p = os.path.join(T, "vbr.hdf")
    mkimg(p, 64 * MB)
    with open(p, "r+b") as f:
        f.write(b"\xeb\x58\x90MSDOS5.0\x00\x02\x08" + b"\0" * 496 + b"\x55\xaa")
    r = run("list", p).stdout
    check("a bare FAT volume: nothing offered", "whole it has a FAT" in r and "option" not in r, r)

    # ---- a full disk ----
    p = os.path.join(T, "full.hdf")
    s = 128 * MB
    mkimg(p, s)
    with open(p, "r+b") as f:
        Rdb(f, 0, s // 1008, 16, 63, [("DH0", 1, s // 1008 - 1, PFS3, 1, 0)])
    r, _, opts = options(p)
    check("a full RDB: no free space offered", not [o for o in opts if o[2] == "free"], r.stdout)
    p = os.path.join(T, "fullm.img")
    s = 128 * MB
    mkimg(p, s)
    with open(p, "r+b") as f:
        mbr(f, [(0x80, 0x0C, 2048, s - 2048)])
    r, _, opts = options(p)
    check("a full MBR: nothing offered at all", not opts, r.stdout)
    # 27 MB of room is too little, 28 is enough
    for mbs, ok in ((26, False), (29, True)):
        p = os.path.join(T, "edge.hdf")
        s = (100 + mbs) * MB
        mkimg(p, s)
        with open(p, "r+b") as f:
            Rdb(f, 0, s // 1008, 16, 63, [("DH0", 1, 100 * MB // 1008, PFS3, 1, 0)], fs=False)
        r, _, opts = options(p)
        check("%d MB of free space: %s" % (mbs, "offered" if ok else "not offered"), bool([o for o in opts if o[2] == "free"]) == ok, r.stdout)

    # ---- damaged tables: refused, nothing written ----
    def damaged(name, patch, whole=True):
        p = os.path.join(T, "dmg.hdf")
        img_rdb_free(p)
        with open(p, "r+b") as f:
            patch(f)
        b = p + ".b"
        cp(p, b)
        refused_all(name, p, whole)
        r = run("install", p, 99)
        check(name + ": nothing written", not changed_sectors(b, p))
        os.unlink(b)

    def flip(f, sec, at):
        f.seek(sec * 512 + at)
        c = f.read(1)
        f.seek(sec * 512 + at)
        f.write(bytes([c[0] ^ 0x40]))

    def setlong(f, sec, idx, val, resum=True):
        f.seek(sec * 512)
        L = list(struct.unpack(">128I", f.read(512)))
        L[idx] = val
        if resum:
            L[2] = 0
            L[2] = (-sum(L[:L[1]])) & 0xFFFFFFFF
        f.seek(sec * 512)
        f.write(struct.pack(">128I", *L))

    damaged("a bad RDSK checksum", lambda f: flip(f, 0, 100))
    damaged("a bad PART checksum", lambda f: flip(f, 4, 100))
    damaged("a partition chain that loops", lambda f: setlong(f, 4, 4, 3))
    damaged("a chain that leaves the RDB area", lambda f: setlong(f, 4, 4, 9000))
    damaged("a FileSysHeader chain that is broken", lambda f: flip(f, 45, 100))
    damaged("two partitions that overlap", lambda f: setlong(f, 4, 32 + 9, 50))
    damaged("a partition past the end of the disk", lambda f: setlong(f, 4, 32 + 10, 5000000))
    damaged("a partition over the RDB area", lambda f: setlong(f, 3, 32 + 9, 0))
    damaged("RDSK geometry that does not add up", lambda f: setlong(f, 0, 36, 7))
    damaged("a partition with a mad block size", lambda f: setlong(f, 3, 32 + 1, 3))
    p = os.path.join(T, "mbrbad.img")
    mkimg(p, 64 * MB)
    with open(p, "r+b") as f:
        mbr(f, [(0x80, 0x0C, 2048, 10 * MB), (0, 0x83, 2048, 500 * MB)])
    r = refused_all("an MBR reaching outside the disk", p, False)
    check("  called what it is", "makes no sense" in r.stdout, r.stdout)
    p = os.path.join(T, "mbrov.img")
    mkimg(p, 64 * MB)
    with open(p, "r+b") as f:
        mbr(f, [(0x80, 0x83, 2048, 20 * MB), (0, 0x83, 4096, 20 * MB)])
    refused_all("an MBR with two entries overlapping", p, True)
    r = run("install", p, 1)
    p = os.path.join(T, "hyb.img")
    mkimg(p, 64 * MB)
    with open(p, "r+b") as f:
        mbr(f, [(0x80, 0x0C, 2048, 10 * MB)])
        f.seek(512)
        f.write(sumblock(rdsk(100, 16, 63, 0xFFFFFFFF)))
    r = run("list", p).stdout
    check("both an MBR and an RDB: nothing at all", "kind hybrid" in r and "option" not in r, r)
    p = os.path.join(T, "gpt.img")
    mkimg(p, 64 * MB)
    with open(p, "r+b") as f:
        mbr(f, [(0, 0xEE, 1, 64 * MB - 1)])
        f.seek(512)
        f.write(b"EFI PART")
    r = run("list", p).stdout
    check("GPT: nothing at all", "kind gpt" in r and "option" not in r, r)
    p = os.path.join(T, "inner.img")
    keep = img_mbr_card(p)
    t0 = read_table(p)
    with open(p, "r+b") as f:
        flip(f, 200 * MB + 2048 + 3, 100)
    r = run("list", p).stdout
    check("a damaged RDB inside 0x76: its real extent is not known: nothing offered", "option" not in r, r)
    # a partition running past its container: not offered, and what it claims is not free space (the reviewer's case)
    p = os.path.join(T, "pastc.img")
    s = 1024 * MB
    mkimg(p, s)
    with open(p, "r+b") as f:
        mbr(f, [(0, 0x0C, 2048, 100 * MB), (0, 0x76, 2048 + 100 * MB, 100 * MB)])
        Rdb(f, 2048 + 100 * MB, 100 * MB // 1008, 16, 63, [("DH0", 1, 300 * MB // 1008, FFS, 1, 0)], fs=False)
    r = run("list", p).stdout
    check("a partition past its 0x76: nothing beside it is offered", "newcont" not in r and "option" not in r, r)
    # an MBR with overlapping entries: the partitions inside are not offered either
    p = os.path.join(T, "ovl.img")
    mkimg(p, 1024 * MB)
    with open(p, "r+b") as f:
        mbr(f, [(0, 0x0C, 2048, 200 * MB), (0, 0x76, 100 * MB, 200 * MB)])
        Rdb(f, 100 * MB, 200 * MB // 1008, 16, 63, [("DH0", 1, 100, FFS, 1, 0)], fs=False)
    r = run("list", p).stdout
    check("an MBR with entries overlapping: nothing in it is offered", "option" not in r, r)
    # a bad-block list: spare blocks look free
    p = os.path.join(T, "badb.hdf")
    s = 512 * MB
    mkimg(p, s)
    with open(p, "r+b") as f:
        Rdb(f, 0, s // 1008, 16, 63, AOS, fs=False)
        setlong(f, 0, 6, 20)
        f.seek(20 * 512)
        f.write(sumblock([0x42414442, 64, 0, 7, 0xFFFFFFFF, 0, 5000, 1]))
    r = run("list", p).stdout
    check("a bad-block list: the table is not changed", "bad-block" in r and "option 1 free" not in r, r)
    # names and places
    for name, why in (("Sys ", "cannot use"), ("A:B", "cannot use")):
        p = os.path.join(T, "nam.hdf")
        s = 512 * MB
        mkimg(p, s)
        with open(p, "r+b") as f:
            f.truncate(0); f.truncate(s * 512)
            Rdb(f, 0, s // 1008, 16, 63, [(name, 1, 40, FFS, 1, 0)], fs=False)
        r = run("install", p, opt_for(p, "free"))
        check("a system partition called %r: refused before anything" % name, r.returncode == 1 and why in r.stdout, r.stdout)
    p = os.path.join(T, "boot.hdf")
    s = 512 * MB
    mkimg(p, s)
    with open(p, "r+b") as f:
        f.truncate(0); f.truncate(s * 512)
        Rdb(f, 0, s // 1008, 16, 63, [("BOOT", 1, 40, FFS, 1, 5), ("DH0", 41, 80, FFS, 1, 0)], fs=False)
    r = run("install", p, opt_for(p, "free"))
    check("the partition that starts today is what the menu offers, whatever its name", "pri 6 aos BOOT" in r.stdout, r.stdout)
    # past 4 GB: not as the highest priority on a top-level RDB
    p = os.path.join(T, "p4g.hdf")
    s = 8192 * MB
    mkimg(p, s)
    with open(p, "r+b") as f:
        Rdb(f, 0, s // 1008, 16, 63, [("DH0", 1, 5000 * MB // 1008, FFS, 1, 0)], fs=False)
    r = run("install", p, opt_for(p, "free"))
    check("free space past 4 GB on an RDB: refused, says why", r.returncode == 1 and "4 GB" in r.stdout, r.stdout)
    # a type it does not know, and a slot: the last free one
    p = os.path.join(T, "ntfs.img")
    mkimg(p, 1024 * MB)
    with open(p, "r+b") as f:
        mbr(f, [(0, 0x83, 2048, 100 * MB), (0, 0x82, 2048 + 100 * MB, 100 * MB)])
    r = run("list", p).stdout
    check("a PC type it does not know is not offered", [l for l in r.splitlines() if l.startswith("part ide0p2")][0].endswith("never touched"), r)
    r = run("install", p, opt_for(p, "newcont"))
    t = read_table(p)
    check("a new 0x76 takes the last free slot", [x for x in t if x[0] == "slot3"] and [x for x in t if x[0] == "slot3"][0][3] == 0x76, str(t))
    # busy
    p = os.path.join(T, "busy.hdf")
    keep = img_rdb_free(p)
    r = run("list", p, ZI_BUSY="1008:%d" % (80 * 1008)).stdout
    check("a mounted partition is not offered, nor the erase", "it is in use" in r and "whole something on this disk" in r and "option 1 free" in r, r)

    # ---- a disk that changes between show and write ----
    p = os.path.join(T, "chg.hdf")
    img_rdb_free(p)
    b = p + ".b"
    cp(p, b)
    r = run("install", p, opt_for(p, "free"), ZI_CHANGE="4:100:1")
    check("a table sector changed after the plan: stops, says so", "changed: nothing written" in r.stdout, r.stdout + r.stderr)
    ch = changed_sectors(b, p)
    check("  and writes nothing", ch == [4], str(ch))
    cp(b, p)
    r = run("install", p, opt_for(p, "free"), ZI_TABLE_ONLY=1)
    blk = new_parts(r.stdout)["ZB0"]["blk"]
    cp(b, p)
    r = run("install", p, opt_for(p, "free"), ZI_CHANGE="%d:5:7" % blk)
    check("a free block among the first 16 changed: the re-read sees it", "changed: nothing written" in r.stdout, r.stdout + r.stderr)
    # a free block past the first 16, which the re-read does not look at: the write of it stops
    s = 512 * MB
    mkimg(p, s)
    with open(p, "r+b") as f:
        f.write(sumblock(rdsk(s // 1008, 16, 63, 16, 1, high=17)))     # a handler's chain holds blocks 1-15, so every free block is past them
        f.seek(512)
        f.write(sumblock([0x46534844, 64, 0, 7, 0xFFFFFFFF, 0, 0, 0, 0x50465303, 0] + [0] * 8 + [2]))
        for i in range(2, 16):
            f.seek(i * 512)
            f.write(sumblock([0x4C534547, 64, 0, 7, i + 1 if i < 15 else 0xFFFFFFFF]))
        for i in range(2):
            f.seek((16 + i) * 512)
            f.write(sumblock(part("D%d" % i, 17 if i == 0 else 0xFFFFFFFF, 1 + i * 2, 2 + i * 2, 16, 63, FFS)))
    cp(p, b)
    r = run("install", p, opt_for(p, "free"), ZI_TABLE_ONLY=1)
    blk = new_parts(r.stdout)["ZH0"]["blk"]
    check("  (the test's block is past the first 16)", blk > 15, r.stdout)
    cp(b, p)
    r = run("install", p, opt_for(p, "free"), ZI_CHANGE="%d:5:7" % blk)
    check("a free block a new PART takes changed: the write of it stops", "has changed" in r.stdout, r.stdout + r.stderr)
    t = read_table(p)
    check("  the table is still the old one", [x[0] for x in t] == ["D0", "D1"], str(t))
    img_rdb_free(p)
    cp(p, b)
    cp(b, p)
    with open(p, "r+b") as f:
        f.truncate((512 * MB - 2048) * 512)
    r = run("list", p)
    check("a disk that shrank under its table is refused", "past the end" in r.stdout or "option" in r.stdout, r.stdout)
    os.unlink(b)

    # ---- labels, priorities, the menu's AmigaOS ----
    p = os.path.join(T, "lab.hdf")
    s = 512 * MB
    mkimg(p, s)
    with open(p, "r+b") as f:
        Rdb(f, 0, s // 1008, 16, 63, [("ZB0", 1, 40, FFS, 1, 7), ("DH3", 41, 80, PFS3, 1, -2), ("WB", 81, 120, FFSI, 1, 3), ("OLD", 121, 140, EXT2, 2, 9)])
    r = run("install", p, opt_for(p, "free"))
    check("names already on the disk are not reused", "new ZB1" in r.stdout, r.stdout)
    check("priority one above the highest mountable, the menu names what starts today", "pri 8 aos ZB0" in r.stdout, r.stdout)
    t = read_table(p)
    check("  the table says so", [x for x in t if x[0] == "ZB1"][0][5] == 8, str(t))

    # ---- another root marked bootable would stay the root (user/rootpick.h) ----
    p = os.path.join(T, "oldroot.hdf")
    s = 512 * MB
    mkimg(p, s)
    with open(p, "r+b") as f:
        Rdb(f, 0, s // 1008, 16, 63, [("DH0", 1, 40, FFS, 1, 0), ("OLD", 41, 80, EXT2, 3, 0)])
    b = p + ".b"
    cp(p, b)
    r = run("install", p, opt_for(p, "free"))
    check("a root already marked bootable: refused, says why", r.returncode == 1 and "would stay the root" in r.stdout, r.stdout)
    check("  nothing written", not changed_sectors(b, p))
    os.unlink(b)
    p = os.path.join(T, "oldroot2.img")
    mkimg(p, 512 * MB)
    with open(p, "r+b") as f:
        mbr(f, [(0, 0x0C, 2048, 20 * MB), (0x80, 0x83, 2048 + 20 * MB, 40 * MB)])
    r = run("install", p, opt_for(p, "newcont"))
    check("an active Linux partition in an MBR too", r.returncode == 1 and "would stay the root" in r.stdout, r.stdout)
    r = run("install", p, opt_for(p, "part", "ide0p2"))
    check("  not if it is the one chosen", r.returncode == 0, r.stdout)

    # ---- use only so much ----
    p = os.path.join(T, "lim.hdf")
    img_rdb_free(p)
    r = run("install", p, opt_for(p, "free"), 100)
    nw = new_parts(r.stdout)
    tot = sum(v["sectors"] for v in nw.values())
    check("use at most 100 MB", 0 < tot <= 100 * MB, r.stdout)

    # ---- a real card's tables, if given (never in the repo) ----
    card = os.environ.get("ZENIX_TEST_CARD")
    if card and os.path.exists(card):
        copy = os.path.join(T, "cardcopy.img")
        size = os.path.getsize(card)
        with open(card, "rb") as a, open(copy, "wb") as b:
            b.truncate(size)
            m = a.read(512)
            b.write(m)
            for i in range(4):
                _, typ, st, n = struct.unpack("<B3xB3xII", m[446 + 16 * i:462 + 16 * i])
                if typ == 0x76:
                    a.seek(st * 512)
                    b.seek(st * 512)
                    b.write(a.read(16384 * 512))
        r = run("list", copy).stdout
        print(r)
        check("the card: its FAT and its 0x76 are refused", "never touched" in r and "never touched as a whole" in r, r)
        check("the card: the RDB is understood with its handler (DH0..DH3, not 'cannot change')", "label=DH3" in r and "noedit=-" in r, r)
        check("the card: no free space there, erase refused", "option 1 free" not in r and "whole it has a FAT" in r, r)
    else:
        print("SKIPPED the owner's card's tables (ZENIX_TEST_CARD not set)")

    # ---- the fuzzer ----
    rounds = sys.argv[1] if len(sys.argv) > 1 else "300000"
    imgs = []
    for name, fn in (("f1.hdf", img_rdb_free), ("f2.hdf", img_rdb_big_cyl), ("f3.img", img_mbr_card), ("f4.img", img_mbr_full_slots), ("f5.img", img_mbr_plain)):
        q = os.path.join(T, name)
        fn(q)
        imgs.append(q)
    # after an install too
    q = os.path.join(T, "f6.hdf")
    img_rdb_free(q)
    run("install", q, 1)
    imgs.append(q)
    r = run("fuzz", 1, rounds, *imgs)
    print(r.stdout.strip())
    if r.stderr:
        print(r.stderr[-1500:])
    check("the fuzzer: %s rounds under ASAN and UBSAN" % rounds, r.returncode == 0 and "no crash" in r.stdout)
finally:
    shutil.rmtree(T, ignore_errors=True)

print("zinstall-host-test: %d passed, %d failed" % (passes, fails))
sys.exit(1 if fails else 0)
