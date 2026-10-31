#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The PFS3 server (user/pfs3d.c) built for the PC, against images from tools/mkpfs3.py: every block size and index layout,
names, links, a big directory, damage refused at the mount, nothing ever written, and a fuzz run with ASAN and UBSAN aimed at
the format's own structures (no crash, no hang). What AmigaOS's own handler makes is judged by tools/pfs3-aos-test.py.
usage: tools/pfs3-host-test.py [fuzz-rounds]   (default 1500). PFS3_ONLY="fuzz" runs parts: kinds, names, refuse, fuzz; PFS3_CF adds compiler flags (--coverage)."""
import calendar, hashlib, os, random, re, resource, shutil, struct, subprocess, sys, tempfile, time, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True
from mkpfs3 import Volume

CF = "-m32 -D_FILE_OFFSET_BITS=64 -DZHOST -DZFSD -DPFS3D -Wall -Wextra -Wno-builtin-declaration-mismatch -Wno-unused-function -O1 -g -Iuser".split()
SAN = "-fsanitize=address,undefined -fno-sanitize-recover=undefined".split()
SRC = ["tools/host/fsd-host.c", "tools/host/hostdisk.c"]
ROUNDS = int(sys.argv[1]) if len(sys.argv) > 1 else 1500
ONLY = os.environ.get("PFS3_ONLY", "kinds names refuse fuzz").split()
T = tempfile.mkdtemp(prefix="zenix-pfs3host.")
EXE = os.path.join(T, "pfs3d")
fails = passes = 0


def check(name, ok, extra=""):
    global fails, passes
    if ok:
        passes += 1
        print("PASS", name, flush=True)
    else:
        fails += 1
        print("FAIL", name, str(extra)[:1500], flush=True)


def run(img, script, timeout=120, exe=None, env=None):
    e = dict(os.environ, HOSTDISK=img, ASAN_OPTIONS="detect_leaks=0")
    e.update(env or {})
    r = subprocess.run([exe or EXE], input=script.encode(), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=e, timeout=timeout)
    return r.returncode, r.stdout.decode(errors="replace")


def pat(n, seed=0):
    return (random.Random(seed).randbytes(4099) * (n // 4099 + 1))[:n]


def lines(out):
    return [l for l in out.splitlines() if l]


def save(v, name):
    p = os.path.join(T, name)
    open(p, "wb").write(v.image())
    return p


def tree(out):
    got, errs = {}, []
    for l in lines(out):
        if l.startswith("!") or l.startswith("toolong"):
            errs.append(l)
            continue
        m = re.match(r"([fdl]) ([0-7]+) (\d+) (\d+) (\d+) (\w+) (/.*?) #(\d+)(?: -> (.*))?$", l)
        if m:
            got[m.group(7)] = m.groups()
    return got, errs


def standard(v):
    """what every layout carries; path -> data"""
    exp = {}
    for n in (0, 1, 511, 512, 513, 1024, 4096, 65536, 300000):
        exp[f"/b{n}"] = pat(n, n)
        v.add_file(f"b{n}", exp[f"/b{n}"])
    exp["/frag"] = pat(400000, 7)
    v.add_file("frag", exp["/frag"], pieces=30)
    exp["/Under/inner.txt"] = b"inner\n"
    v.add_file("Under/inner.txt", exp["/Under/inner.txt"])
    for i in range(300):
        exp[f"/Lots/fil{i:03}"] = pat(i, i)
        v.add_file(f"Lots/fil{i:03}", exp[f"/Lots/fil{i:03}"])
    return exp


def test_kinds():
    """each block size, reserved block size and index layout real volumes (or the format) have"""
    kinds = [dict(), dict(super_=True), dict(split=False), dict(bs=1024, rb=1024, super_=True), dict(bs=1024, rb=2048, super_=True), dict(rb=2048, super_=True),
             dict(rb=4096, super_=True), dict(bs=2048, rb=2048, super_=True), dict(bs=4096, rb=4096, super_=True), dict(large=True, super_=True)]
    for k in kinds:
        tag = ",".join(f"{a}={b}" for a, b in k.items()) or "plain"
        v = Volume(16384, **k)
        exp = standard(v)
        p = save(v, "k.img")
        h = hashlib.sha256(open(p, "rb").read()).hexdigest()
        rc, o = run(p, "mount\nstatfs\ntree /\nwrites\n")
        got, errs = tree(o)
        check(f"{tag}: mounts", "mount -> 0" in o, o[:300])
        check(f"{tag}: read without an error", not errs and rc == 0, errs[:4])
        bad = [q for q, d in exp.items() if q not in got or int(got[q][2]) != len(d) or int(got[q][5], 16) != zlib.crc32(d)]
        check(f"{tag}: every file as it was written ({len(exp)})", not bad, bad[:5])
        check(f"{tag}: nothing written", "writes -> 0" in o and hashlib.sha256(open(p, "rb").read()).hexdigest() == h)
    # a directory block filled to its last byte: one entry of 24 bytes and 35 of 28 make the 1004 that fit
    v = Volume(16384)
    names = ["abc"] + [f"nam{i:04}" for i in range(35)] + ["after-the-full-block"]
    for n in names:
        v.add_file("Full/" + n, n.encode())
    p = save(v, "full.img")
    d = open(p, "rb").read()
    fb = v.anodes[v.find("Full").an][1]
    rc, o = run(p, "mount\nls /Full\nstat /Full/after-the-full-block\nstat /Full/nam0034\ntree /\n")
    got, errs = tree(o)
    check("a directory block filled to its last byte", sum(d[fb * 1024 // 1024 * 512 + 20:fb * 512 + 1024]) > 0 and d[fb * 512 + 1022:fb * 512 + 1024] != b"\xff\xff" and "(37, 0)" in o and not errs and "stat -> 0" in lines(o)[2] and "stat -> 0" in lines(o)[3], o[:500])
    # a directory over many blocks and a file over many anode blocks: the chains and the listing's place in them
    v = Volume(65536, rb=1024, reserved=1024)
    exp = {}
    for i in range(6000):
        nm = f"/Stor/en-fil-med-ett-ganska-langt-namn-{i:05}"
        exp[nm] = b"x" * (i % 7)
        v.add_file(nm[1:], exp[nm])
    exp["/manybits"] = pat(1000 * 512, 3)
    v.add_file("manybits", exp["/manybits"], pieces=1000)
    p = save(v, "big.img")
    t0 = time.time()
    rc, o = run(p, "mount\ntree /\n", timeout=300)
    got, errs = tree(o)
    bad = [q for q, d in exp.items() if q not in got or int(got[q][2]) != len(d) or int(got[q][5], 16) != zlib.crc32(d)]
    check(f"6000 names in one directory, a file in 1000 pieces ({time.time() - t0:.1f} s, ASAN)", not errs and not bad, (errs[:4], bad[:4]))


def named_volume():
    v = Volume(16384, "Svensk")
    v.add_file("räksmörgås.txt", b"svenska\n", comment=b"en kommentar", date=(15779, 754, 2800))
    v.add_file("ÅÄÖ/ö é.txt", b"i-mappen\n")
    v.add_file("n" * 31, b"31\n")
    long = "L" * 100
    v.add_file(long, b"a long name\n")
    v.add_file("Under/x.txt", b"xfile\n")
    v.add_file("Under/deep/y.txt", b"yfile\n")
    v.add_file("rw", b"1\n", prot=1 << 3)                   # not readable
    v.add_file("ne", b"2\n", prot=1 << 1)                   # not executable
    v.add_file("ro", b"3\n", prot=1 << 2)                   # not writable
    v.add_file("b1", pat(5000, 1))
    v.hard_link("hl1", "b1")
    v.hard_link("Under/hl2", "b1")
    v.hard_link("dhl", "Under")
    v.soft_link("sl1", "b1")
    v.soft_link("Under/sl2", "/b1")
    v.soft_link("Under/sl3", "Svensk:b1")
    v.soft_link("Under/deep/sl4", ":Under/x.txt")
    v.soft_link("sl5", "Annan:elsewhere")
    v.soft_link("sl6", "Under/deep")
    v.soft_link("loop", "loop")
    v.soft_link("sv", "räksmörgås.txt".encode("latin-1"))
    v.hard_link("hlsoft", "sl1")
    r = v.rollover("circle", b"6789012345", 4, 10)
    r2 = v.rollover("partly", b"abcdefghij", 8, 6)
    big = v.add_file("huge", b"just-the-start")
    big.size = 5
    big.sizehi = 1
    return v, long


def test_names():
    v, long = named_volume()
    v.large = True
    v.typ = 0x50465302
    p = save(v, "n.img")
    rc, o = run(p, "mount\nls /\nstat /RÄKSMÖRGÅS.txt\nopen /räksmörgås.txt r\nread %1 0 100\nclose %1\nstat /åäö/Ö+É.TXT\nstat /" + "N" * 31 + "\n")
    ls = lines(o)
    check("a Swedish name listed", "räksmörgås.txt" in ls[1], o)
    check("found in any case", " -> 0 " in ls[2], o)
    check("its date: 2021-03-15 12:34:56", f"mtime {calendar.timegm((2021, 3, 15, 12, 34, 56))}" in ls[2], ls[2])
    check("read", "read -> 8 svenska" in o, o)
    check("in a Swedish directory", " -> 0 " in ls[6], o)
    check("31 characters", " -> 0 " in ls[7], o)
    m = re.search(r"\b(L+~[0-9a-f]+)\b", ls[1])
    check("a name over 63 bytes is listed by its start and anode", bool(m), ls[1])
    if m:
        rc, o = run(p, f"mount\nopen /{m.group(1)} r\nread %1 0 100\nstat /{long.lower()}\nstat /{m.group(1)}x\n")
        check("... which opens it, and so does its full name", "read -> 12 a long name" in o and " stat -> 0 ".strip() in lines(o)[3] and " -> -2" in lines(o)[4], o)
    rc, o = run(p, "mount\nlstat /rw\nlstat /ne\nlstat /ro\nas 100 100\nopen /rw r\nas 0 0\nopen /ne r\nopen /ro r\nopen /b1 w\n")
    ls = lines(o)
    check("protection bits are modes", "mode 100311" in ls[1] and "mode 100644" in ls[2] and "mode 100555" in ls[3], o)
    check("a file a user may not read", ls[5].endswith("-13"), o)
    check("... root may, as on any filesystem here", int(ls[7].split()[-1]) > 0, o)
    check("opening for writing: read-only filesystem", ls[9].endswith("-30"), o)
    rc, o = run(p, "mount uid=5,gid=6,fmode=640,dmode=750\nstat /b1\nstat /Under\nas 7 7\nopen /b1 r\nstat /Under/x.txt\nas 5 6\nopen /b1 r\nstat /Under/x.txt\n")
    ls = lines(o)
    check("owner and modes from the mount's options", "mode 100640" in ls[1] and "uid 5 gid 6" in ls[1] and "mode 40750" in ls[2], o)
    check("... and they count", ls[4].endswith("-13") and int(lines(o)[7].split()[-1]) > 0, o)
    rc, o = run(p, "mount\nstat /hl1\nstat /b1\nstat /Under/hl2\nstat /dhl/x.txt\nopen /hl1 r\nlstat /hlsoft\nreadlink /hlsoft\nstat /hlsoft\n")
    ls = lines(o)
    ino = re.search(r"ino (\d+)", ls[2]).group(1)
    check("a hard link is its file", f"ino {ino} " in ls[1] and f"ino {ino} " in ls[3] and "links 3" in ls[1], o)
    check("a hard link to a directory", " -> 0 " in ls[4], o)
    check("a hard link to a soft link", "mode 120777" in ls[6] and ls[7].endswith(" b1") and "size 5000" in ls[8], o)
    rc, o = run(p, "mount\nreadlink /sl1\nreadlink /Under/sl2\nreadlink /Under/sl3\nreadlink /Under/deep/sl4\nreadlink /sl5\nstat /sl1\nstat /Under/sl2\nstat /Under/sl3\nstat /Under/deep/sl4\nstat /sl5\nstat /sl6/y.txt\nstat /loop\nreadlink /sv\nstat /sv\n")
    ls = lines(o)
    check("soft link texts: plain, / is .., own volume's name and : are the way to the root, another volume stays",
          ls[1].endswith(" b1") and ls[2].endswith(" ../b1") and ls[3].endswith(" ../b1") and ls[4].endswith(" ../../Under/x.txt") and ls[5].endswith(" Annan:elsewhere"), o)
    check("... followed", all(" -> 0 " in ls[i] for i in (6, 7, 8, 9, 11)) and ls[10].endswith("-2"), o)
    check("a link to itself ends", ls[12].endswith("-40") or "-40" in ls[12], o)
    check("a Swedish soft link", ls[13].endswith("räksmörgås.txt") and " -> 0 " in ls[14], o)
    rc, o = run(p, "mount\nlinks on\nopen /sl1 r\nstat /Under/sl2\nopen /Under/deep/sl4 r\nlinks off\nopen /hl1 r\n")
    ls = lines(o)
    check("across mounts (D26): the walk stops at a link with what the kernel needs", "-102" in ls[2] and ls[2].endswith(" b1") and ls[3].endswith(" Under/../b1") and ls[4].endswith(" Under/deep/../../Under/x.txt"), o)
    rc, o = run(p, "mount\nstat /circle\nopen /circle r\nread %1 0 100\nread %1 3 4\nread %1 8 100\nstat /partly\nopen /partly r\nread %2 0 100\n")
    ls = lines(o)
    check("a rollover file reads from where it starts, around the end", "size 10" in ls[1] and "read -> 10 0123456789" in ls[3] and "read -> 4 3456" in ls[4] and "read -> 2 89" in ls[5], o)
    check("... and only as much as it holds", "size 6" in ls[6] and "read -> 6 ijabcd" in ls[8], o)
    rc, o = run(p, "mount\nstat /huge\nopen /huge r\nls /Under\nchdir /Under\nopen /Under r\nfstat 8\nread %1 0 10\nstatfs\nsync\n")
    ls = lines(o)
    check("a file of 4 GB or more: seen, too large to open", "size 4294967295" in ls[1] and ls[2].endswith("-27"), o)
    check("a directory is opened and read as one", " -> -21" in ls[7] or ls[7].endswith("-21"), o)
    rc, o = run(p, "mount\nopen /new wc\nopen /new2 c\nopen /b1 wt\nmkdir /d\nrmdir /Under\nunlink /b1\nrename /b1 /b2\nlink /b1 /b3\nsymlink b1 /s\nchmod /b1 600\nchown /b1 1 1\nutimes /b1 now\nwrites\numount\n")
    ls = lines(o)
    check("every change is refused: read-only filesystem", all(l.endswith("-30") for l in ls[1:13]), o)
    check("nothing written", "writes -> 0" in o)
    rc, o = run(p, "raw on\nas 5 5\nmount\nraw on\nas 5 5\nopen /b1 r\n")
    check("a user's raw request is refused", lines(o)[-1].endswith("-13"), o)


def patched(base, *edits):
    d = bytearray(base)
    for at, val in edits:
        d[at:at + len(val)] = val
    return bytes(d)


def test_refuse():
    v = Volume(16384)
    standard(v)
    base = v.image()
    good = os.path.join(T, "r.img")

    def one(name, data, opts="", want=None):
        open(good, "wb").write(data)
        rc, o = run(good, f"mount {opts}\n")
        ok = "mount -> -1" in o or "bad option" in o
        check(f"refused: {name}", ok and rc == 0 and (want is None or want in o), o[:300])

    R = 1024
    one("-o rw", base, "rw", "is refused")
    one("rw among other options", base, "uid=1,rw", "is refused")
    one("a bad option", base, "nonsense", "bad option")
    one("an impossible block size", base, "blocksize=3000", "only blocks")
    one("nothing like a volume", bytes(len(base)), "", "not a PFS volume")
    dos = bytearray(base)
    dos[:4] = b"DOS\x03"
    dos[R:R + 4] = b"DOS\x03"
    one("an FFS volume", bytes(dos), "", "not a PFS volume")
    one("options nobody knows", patched(base, (R + 4, struct.pack(">I", 0x80000077))), "", "does not know")
    one("no hard disk mode", patched(base, (R + 4, struct.pack(">I", 0x76))), "", "hard disk mode")
    two = patched(base, (0, b"PFS\x02"), (R, b"PFS\x02"))
    one("reserved blocks of 3 KB", patched(two, (R + 64, struct.pack(">H", 3072))), "", "impossible")
    one("reserved blocks of 8 KB", patched(two, (R + 64, struct.pack(">H", 8192))), "", "impossible")
    one("2 KB reserved blocks on an original PFS type", patched(base, (R + 64, struct.pack(">H", 2048))), "", "newer format")
    one("large files on an original PFS type", patched(base, (R + 4, struct.pack(">I", 0x87F))), "", "newer format")
    one("the reserved area past the end", patched(base, (R + 52, struct.pack(">I", 1 << 30))), "", "reserved area")
    one("a volume larger than its partition", patched(base, (R + 84, struct.pack(">I", 1 << 20))), "", "larger than")
    one("no extension where it says", patched(base, (R + 88, struct.pack(">I", 3))), "", "extension")
    one("super blocks, no extension", patched(base, (R + 4, struct.pack(">I", 0x77 | 128 & ~32 | 1) )), "", None)
    # a postponed delete: read as it is, with a line
    ext = struct.unpack(">I", base[R + 88:R + 92])[0]
    open(good, "wb").write(patched(base, (ext * 512 + 28, struct.pack(">I", 1))))
    rc, o = run(good, "mount\nstat /b1\ntree /\nwrites\n")
    got, errs = tree(o)
    check("a delete left half done: mounted, said so, read as it is, not touched", "mount -> 0" in o and "cut off" in o and not errs and "writes -> 0" in o, o[:400])
    # the source has no way to write
    src = open(os.path.join(HERE, "..", "user", "pfs3d.c")).read()
    check("the server's source sends nothing but reads", not re.search(r"DISK_WRITE|DISK_FLUSH|DISK_RAW", src))


def fuzz():
    """Damage aimed at what the server walks. Each image: a few of these at random, then every kind of request"""
    images = []
    for k in (dict(), dict(super_=True), dict(bs=1024, rb=2048, super_=True, large=True)):
        v, _ = named_volume()
        v2 = Volume(4096, **k)
        v2.add_file("räksmörgås.txt", b"svenska\n", comment=b"note")
        v2.add_file("Mapp/big", pat(60000, 5), pieces=12)
        v2.add_file("Mapp/Under/x", b"x\n")
        for i in range(40):
            v2.add_file(f"Lots/en-fil-med-langt-namn-{i:03}", pat(i * 3, i))
        v2.hard_link("hl", "Mapp/big")
        v2.hard_link("dhl", "Mapp/Under")
        v2.soft_link("sl", "Mapp/big")
        v2.soft_link("Mapp/Under/up", "/hl")
        v2.soft_link("Mapp/Under/rot", ":Mapp/big")
        v2.soft_link("Mapp/Under/vol", "Pfstest:Mapp/big")
        v2.rollover("ro", b"0123456789", 3, 8)
        v2.add_file("L" * 100, b"long\n")
        d = v2.image()
        images.append((d, v2))
    script = "mount\nstatfs\ntree / 400 8\nstat /Mapp/big\nopen /hl r\nread %1 0 4096\nread %1 60000 10\nfstat %1\nclose %1\nls /\nls /Mapp\nstat /Mapp/Under/up\nstat /Mapp/Under/rot\nstat /Mapp/Under/vol\nstat /Mapp/Under/.\nstat /Mapp/./Under/../Under/\nstat /dhl/.\nreadlink /sl\nlinks on\nopen /sl r\nstat /Mapp/Under/up/x\nlinks off\nopen /dhl/up/.. r\nchdir /Mapp/Under/../..\nstat /" + "L" * 100 + "\nread 77 0 10\nfstat 5\nquit\n"
    rng = random.Random(0x50F5)
    p = os.path.join(T, "fuzz.img")
    bad = hangs = mounted = 0
    first = None
    t0 = time.time()
    for rnd in range(ROUNDS):
        base, v = images[rnd % len(images)]
        d = bytearray(base)
        bs, rb = v.bs, v.rb
        rootat = 2 * bs
        res_blocks = sorted(b for b in v.blocks if v.res_first <= b <= v.res_last and b != 2)
        data_blocks = sorted(b for b in v.blocks if b > v.res_last)

        def word():
            return rng.choice([0, 1, 2, 3, 5, 0xFF, 0xFFFF, 0x10000, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, v.res_first, v.res_last, v.nblocks, v.nblocks - 1, rng.getrandbits(32), rng.randrange(v.nblocks), rng.randrange(v.next_an) if v.next_an else 0, (rng.randrange(4) << 16) | rng.randrange(90)])

        for _ in range(rng.choice([1, 1, 1, 2, 3, 6, 20])):
            what = rng.randrange(13)
            if what == 0:                                  # a field of the root
                at = rootat + rng.choice([0, 4, 52, 56, 60, 64, 66, 68, 84, 88, 116, 120])
                if rng.random() < 0.5 or at - rootat in (64, 66):
                    d[at:at + 2] = struct.pack(">H", word() & 0xFFFF)
                else:
                    d[at:at + 4] = struct.pack(">I", word())
            elif what == 1:                                # any long of a reserved block
                b = rng.choice(res_blocks)
                at = b * bs + 4 * rng.randrange(rb // 4)
                d[at:at + 4] = struct.pack(">I", word())
            elif what == 2:                                # an id, a sequence number
                b = rng.choice(res_blocks)
                d[b * bs + rng.choice([0, 8, 9, 12, 16]):][:4]
                at = b * bs + rng.choice([0, 8, 12, 16])
                d[at:at + 4] = struct.pack(">I", word())
            elif what == 3:                                # a word of an anode block: cluster, block, next
                abl = [b for b in res_blocks if d[b * bs:b * bs + 2] == b"AB"]
                if abl:
                    b = rng.choice(abl)
                    at = b * bs + 16 + 12 * rng.randrange(min(50, (rb - 16) // 12)) + rng.choice([0, 4, 8])
                    d[at:at + 4] = struct.pack(">I", word())
            elif what == 4:                                # an anode chain that loops: next = itself or an earlier one
                abl = [b for b in res_blocks if d[b * bs:b * bs + 2] == b"AB"]
                if abl:
                    b = rng.choice(abl)
                    off = rng.randrange(6, min(60, (rb - 16) // 12))
                    at = b * bs + 16 + 12 * off + 8
                    d[at:at + 4] = struct.pack(">I", rng.randrange(6, off + 1) if not v.split else (0 << 16) | rng.randrange(6, off + 1))
                    if rng.random() < 0.3:
                        d[at - 8:at - 4] = struct.pack(">I", 0)
            elif what in (5, 6, 7, 8):                     # a directory's entries: sizes, names, comments, flags
                dbl = [b for b in res_blocks if d[b * bs:b * bs + 2] == b"DB"]
                if dbl:
                    b = rng.choice(dbl)
                    o = 20
                    offs = []
                    while o + 2 <= rb and d[b * bs + o]:
                        offs.append(o)
                        o += d[b * bs + o] or 2
                    if offs:
                        o = rng.choice(offs)
                        base_at = b * bs + o
                        sz = d[base_at]
                        if what == 5:
                            d[base_at] = rng.choice([0, 1, 2, 3, 17, 18, 19, 20, 254, 255, rng.randrange(256)])
                        elif what == 6:
                            d[base_at + 17] = rng.choice([0, 1, 107, 200, 255, rng.randrange(256)])
                            if rng.random() < 0.3:
                                d[base_at + 18 + min(d[base_at + 17], 60)] = 255
                        elif what == 7:
                            d[base_at + 1] = rng.choice([0, 1, 2, 3, 4, 0xFD, 0xFC, 0xF0, 0xFB, 0x7F, rng.randrange(256)])
                        else:
                            at = base_at + sz - rng.choice([2, 4, 6, 8, 10, 12])
                            d[at:at + 2] = struct.pack(">H", rng.getrandbits(16) if rng.random() < 0.6 else 0xFFFF)
            elif what == 9:                                # a directory block's own anode, its parent
                dbl = [b for b in res_blocks if d[b * bs:b * bs + 2] == b"DB"]
                if dbl:
                    at = rng.choice(dbl) * bs + rng.choice([12, 16])
                    d[at:at + 4] = struct.pack(">I", word())
            elif what == 10:                               # one block copied over another, zeroed: chains that lead astray
                a = rng.choice(res_blocks)
                c = rng.choice(res_blocks + [None])
                d[a * bs:a * bs + rb] = bytes(rb) if c is None else d[c * bs:c * bs + rb]
            elif what == 11 and data_blocks:               # a soft link's text
                b = rng.choice(data_blocks)
                d[b * bs:b * bs + 32] = rng.randbytes(32) if rng.random() < 0.5 else (b"/" * 31 + b":")
            else:                                          # any byte at all
                for _ in range(rng.randrange(1, 12)):
                    d[rng.randrange(min(len(d), (v.res_last + 1) * bs))] = rng.getrandbits(8)
        open(p, "wb").write(d)
        before = hashlib.sha256(d).digest()
        try:
            rc, o = run(p, script, timeout=25, exe=EXE)
        except subprocess.TimeoutExpired:
            hangs += 1
            if not first:
                first = f"hang in round {rnd}"
                shutil.copy(p, os.path.join(tempfile.gettempdir(), "zenix-pfs3-fuzz-hang.img"))
            continue
        if "mount -> 0" in o:
            mounted += 1
        if rc or "AddressSanitizer" in o or "runtime error" in o or hashlib.sha256(open(p, "rb").read()).digest() != before:
            bad += 1
            if not first:
                first = f"round {rnd} rc {rc}\n{o[-1500:]}"
                shutil.copy(p, os.path.join(tempfile.gettempdir(), "zenix-pfs3-fuzz-bad.img"))
    print(f"fuzz: {ROUNDS} damaged volumes, {mounted} still mounted, {bad} crashes, {hangs} hangs, {time.time() - t0:.0f} s", flush=True)
    check(f"fuzz: {ROUNDS} damaged volumes under ASAN + UBSAN: no crash, no hang, never written", not bad and not hangs and mounted > ROUNDS // 4, first)


def main():
    os.chdir(os.path.join(HERE, ".."))
    r = subprocess.run(["gcc", *CF, *SAN, *os.environ.get("PFS3_CF", "").split(), *SRC, "-o", EXE], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    warn = r.stdout.decode()
    if r.returncode:
        print(warn)
        return 1
    check("builds without a warning", not warn.strip(), warn)
    try:
        if "kinds" in ONLY:
            test_kinds()
        if "names" in ONLY:
            test_names()
        if "refuse" in ONLY:
            test_refuse()
        if "fuzz" in ONLY:
            fuzz()
    finally:
        shutil.rmtree(T, ignore_errors=True)
    print(f"pfs3-host-test: {passes} passed, {fails} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
