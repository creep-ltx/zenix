#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The ISO 9660 server (user/iso9660d.c, DESIGN D44) built for the PC, against images the PC's bsdtar (libarchive) makes:
Rock Ridge (names, modes, symbolic links, a deep tree, a long name in a continuation area), Joliet only, plain ISO
names; every file read in odd pieces and compared with the tree it was made from; directories over many blocks; what
would change refused, nothing ever written; D26's FS_LINKED answers; volumes refused at the mount; damage aimed at the
descriptors, the records and the Rock Ridge entries (an extent off the volume, a record across a block, a continuation
that loops); and a fuzz run with ASAN and UBSAN (no crash, no hang).
usage: tools/iso9660-host-test.py [fuzz-rounds]   (default 600). SKIP without bsdtar."""
import os, random, re, shutil, struct, subprocess, sys, tempfile, zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
CF = "-m32 -D_FILE_OFFSET_BITS=64 -DZHOST -DZFSD -DISO9660D -Wall -Wextra -Wno-builtin-declaration-mismatch -Wno-unused-function -O1 -g -Iuser".split()
SAN = "-fsanitize=address,undefined -fno-sanitize-recover=undefined".split()
SRC = ["tools/host/fsd-host.c", "tools/host/hostdisk.c"]
ROUNDS = int(sys.argv[1]) if len(sys.argv) > 1 else 600
T = tempfile.mkdtemp(prefix="zenix-isohost.")
EXE = os.path.join(T, "iso9660d")
fails = passes = 0


def check(name, ok, extra=""):
    global fails, passes
    if ok:
        passes += 1
        print("PASS", name, flush=True)
    else:
        fails += 1
        print("FAIL", name, str(extra)[:1500], flush=True)


def run(img, script, timeout=60):
    e = dict(os.environ, HOSTDISK=img, HOSTDISK_RDONLY="1", HOSTDISK_COUNT="1", ASAN_OPTIONS="detect_leaks=0")
    r = subprocess.run([EXE], input=script.encode(), stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=e, timeout=timeout)
    return r.returncode, r.stdout.decode(errors="replace") + r.stderr.decode(errors="replace")


def pat(n, seed):
    return random.Random(seed).randbytes(n)


def make_tree(base, kind):
    """the source tree; path -> (data or None for a directory, link text or None)"""
    exp = {}
    def f(p, data):
        full = os.path.join(base, p)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        open(full, "wb").write(data)
        exp["/" + p] = data
    for n in (0, 1, 2047, 2048, 2049, 4097, 300001):
        f(f"size{n}.bin", pat(n, n))
    f("Mapp/inner.txt", b"inner\n")
    f("Mapp/Under/deeper.txt", b"deeper\n")
    for i in range(260):                               # a directory of many blocks
        f(f"Lista/fil{i:03}.txt", b"file %d\n" % i)
    if kind != "plain":
        f("räksmörgås.txt", b"svenska\n")
        f("A long name with spaces and more words in it.txt", b"long\n")
    if kind == "rr":
        f("x" * 200 + ".txt", b"two hundred\n")       # NM over 128 bytes: bsdtar puts it in a continuation area
        f("/".join("d%d" % i for i in range(12)) + "/bottom.txt", b"twelve deep\n")
        os.symlink("../size1.bin", os.path.join(base, "Mapp/up"))
        os.symlink("/size1.bin", os.path.join(base, "abs"))
        os.symlink("Mapp", os.path.join(base, "dirlink"))
        exp["/Mapp/up"] = exp["/abs"] = exp["/dirlink"] = None
    return exp


def bsdtar(base, img, opts):
    a = ["bsdtar", "--format", "iso9660"] + (["--options", opts] if opts else []) + ["-cf", img, "-C", base, "."]
    r = subprocess.run(a, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode:
        raise SystemExit("bsdtar failed: " + r.stdout.decode(errors="replace"))


def tree(out):
    got, errs = {}, []
    for l in out.splitlines():
        if l.startswith("!") or l.startswith("toolong"):
            errs.append(l)
            continue
        m = re.match(r"([fdl]) ([0-7]+) (\d+) (\d+) (\d+) (\w+) (/.*?) #(\d+)(?: -> (.*))?$", l)
        if m:
            got[m.group(7)] = m.groups()
    return got, errs


def walk_script(paths):
    return "mount\ntree /\n" + "".join(f"stat {p.replace(' ', '+')}\n" for p in paths) + "quit\n"


def build():
    r = subprocess.run(["gcc"] + CF + SAN + SRC + ["-o", EXE], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode:
        print(r.stdout.decode())
        raise SystemExit("FAIL build")


def flavour(kind, opts):
    base = os.path.join(T, "src-" + kind)
    shutil.rmtree(base, ignore_errors=True)
    os.makedirs(base)
    exp = make_tree(base, kind)
    img = os.path.join(T, kind + ".iso")
    bsdtar(base, img, opts)
    rc, out = run(img, "mount\ntree /\nstatfs\nquit\n")
    got, errs = tree(out)
    check(f"{kind}: mounts", "mount -> 0 readonly" in out, out[:400])
    check(f"{kind}: no error in the tree", not errs and rc == 0, errs[:5] or out[-800:])
    check(f"{kind}: nothing written", "hostdisk-writes 0" in out, out[-200:])
    files = {p: d for p, d in exp.items() if d is not None}
    if kind == "plain":                                # names are ISO's own: compare what is there by contents
        want = sorted((len(d), zlib.crc32(d)) for d in files.values())
        have = sorted((int(g[2]), int(g[5], 16)) for g in got.values() if g[0] == "f")
        check("plain: every file's data, by size and CRC", want == have, (len(want), len(have)))
        check("plain: names in ISO's form (8.3, upper case, no ;1)", "/MAPP/INNER.TXT" in got and all(";" not in p for p in got), sorted(got)[:8])
        return img, out
    for p in list(files):                             # over a udirent's 63 bytes: listed as its start, ~ and a number
        if len(os.path.basename(p).encode()) > 63:
            alias = [k for k in got if k.startswith(p[:40]) and re.search(r"~[0-9a-f]+$", k)]
            if len(alias) == 1:
                got[p] = got.pop(alias[0])
    bad = [p for p, d in files.items() if p not in got or got[p][0] != "f" or int(got[p][2]) != len(d) or int(got[p][5], 16) != zlib.crc32(d)]
    check(f"{kind}: every file there, its size and CRC (read in odd pieces)", not bad, bad[:5])
    if kind == "rr":
        check("rr: symbolic links and their text", got.get("/Mapp/up", ("",) * 9)[8] == "../size1.bin" and
              got.get("/abs", ("",) * 9)[8] == "/size1.bin" and got.get("/dirlink", ("",) * 9)[0] == "l", [got.get(k) for k in ("/Mapp/up", "/abs", "/dirlink")])
        check("rr: modes from Rock Ridge (bsdtar's 'useful': 444 files, 555 directories)", got["/Mapp/inner.txt"][1] == "444" and got["/Mapp"][1] == "555", got["/Mapp"])
        check("rr: the deep tree (moved by Rock Ridge, found where it was)", "/" + "/".join("d%d" % i for i in range(12)) + "/bottom.txt" in got)
        check("rr: the moved directory not listed twice", sum(1 for p in got if p.endswith("/d11")) == 1, [p for p in got if p.endswith("d11")])
    if kind == "joliet":
        check("joliet: a Swedish name and a long one", "/räksmörgås.txt" in got and "/A long name with spaces and more words in it.txt" in got, sorted(got)[:6])
    check(f"{kind}: a directory of 260 entries", sum(1 for p in got if p.startswith("/Lista/")) == 260)
    return img, out


def main():
    if not shutil.which("bsdtar"):
        print("SKIP iso9660-host-test: bsdtar (libarchive) is not installed")
        return 0
    build()
    rr, _ = flavour("rr", None)
    flavour("joliet", "iso9660:!rockridge,iso9660:joliet=long")
    plain, _ = flavour("plain", "iso9660:!rockridge,iso9660:!joliet")

    # Rock Ridge with real modes, and the options
    base = os.path.join(T, "src-modes")
    os.makedirs(base + "/sub")
    open(base + "/secret", "w").write("x\n"); os.chmod(base + "/secret", 0o600)
    open(base + "/tool", "w").write("x\n"); os.chmod(base + "/tool", 0o4755)
    os.chmod(base + "/sub", 0o700)
    img = os.path.join(T, "modes.iso")
    bsdtar(base, img, "iso9660:rockridge=strict")
    rc, out = run(img, "mount\nstat /secret\nstat /tool\nstat /sub\nas 7 7\nopen /secret r\nls /sub\nquit\n")
    check("strict Rock Ridge: a file's mode", "stat -> 0 ino" in out and re.search(r"mode 100600 ", out), out)
    check("... set-id bits dropped (a disc is not trusted)", re.search(r"mode 100755 ", out), out)
    check("... a directory's", re.search(r"mode 40700 ", out), out)
    check("... and they are kept: another user cannot read", "open -> -13" in out and "ls -> -13" in out, out)
    rc, out = run(img, "mount norock,uid=5,gid=6,fmode=640,dmode=751\nstat /secret\nstat /sub\nquit\n")
    check("-o norock with the options: their owner and modes", re.search(r"mode 100640 size 2 links 1 uid 5 gid 6", out) and re.search(r"mode 40751 ", out), out)
    rc, out = run(img, "mount rw\nquit\n")
    check("-o rw refused by the server", "-o rw is refused" in out and "mount -> bad option" in out, out)

    # lookups, read-only answers, links across mounts (D26)
    rc, out = run(rr, "mount\nopen /MAPP/inner.txt r\nopen /Mapp/inner.txt r\nopen /size1.bin w\nopen /new c\nmkdir /Mapp/x\nunlink /size1.bin\n"
                      "rename /size1.bin /y\nchmod /size1.bin 777\nsymlink a /z\nlink /size1.bin /w\nreadlink /Mapp/up\n"
                      "links on\nstat /abs\nstat /Mapp/up\nopen /dirlink/inner.txt r\nlinks off\nstat /dirlink/inner.txt\nopen /Mapp/up r\n"
                      "ls /size1.bin\nstat /nosuch\nstat /size1.bin/x\nquit\n")
    lines = out.splitlines()
    def ans(cmd, n=0):
        hits = [l for l in lines if l.startswith(cmd + " -> ")]
        return hits[n] if len(hits) > n else ""
    check("Rock Ridge names are exact (MAPP is not Mapp)", ans("open") == "open -> -2" and re.match(r"open -> \d+", ans("open", 1)), lines[:4])
    check("open for writing: read-only", ans("open", 2) == "open -> -30" and ans("open", 3) == "open -> -30", lines)
    check("mkdir, unlink, rename, chmod, symlink, link: read-only", all(ans(c) == c + " -> -30" for c in ("mkdir", "unlink", "rename", "chmod", "symlink", "link")), lines)
    check("readlink", "readlink -> 12 ../size1.bin" in out, out)
    check("D26: an absolute link answers FS_LINKED with its text", "stat -> -1024 a0 /size1.bin" in out, out)
    check("D26: a relative one, from the path before it", "stat -> -1024 a0 Mapp/../size1.bin" in out, out)
    check("D26: a link in the middle of a path", "open -> -1024 a0 Mapp/inner.txt" in out, out)
    check("without D26: followed inside the volume", re.search(r"stat -> 0 ino \d+ mode 100444 size 6 ", out) and re.search(r"open -> \d+\n", out[out.index("links -> 0"):]), out)
    check("not found, not a directory", ans("stat", 3) == "stat -> -2" and ans("stat", 4) == "stat -> -20" and ans("ls") == "ls -> (0, -20)", lines[-8:])
    rc, out = run(rr, "mount\nstat /" + "x" * 200 + ".txt\nls /\nquit\n")
    m = re.search(r" (x{40,}~[0-9a-f]+) ", out)
    check("a name over 63 bytes: found by its whole name", re.search(r"stat -> 0 ino \d+ mode 100444 size 12 ", out), out[:300])
    check("... listed by its start, ~ and its number, which opens it", m and re.search(r"stat -> 0 ", run(rr, "mount\nstat /" + m.group(1) + "\nquit\n")[1]), out[:300])

    # plain ISO: names looked up in any case
    rc, out = run(plain, "mount\nopen /mapp/inner.txt r\nopen /MAPP/INNER.TXT r\nread %1 0 10\nopen /size1.bin r\nquit\n")
    check("plain names: found in any case, without ;1", len(re.findall(r"open -> \d+", out)) == 3 and "read -> 6 inner" in out, out)

    # refusals at the mount
    def refuse(name, data, why):
        p = os.path.join(T, "refuse.iso")
        open(p, "wb").write(data)
        rc, out = run(p, "mount\nquit\n")
        check(f"refused: {name}", "mount -> -1" in out and why in out and rc == 0, out[-400:])
    d = bytearray(open(rr, "rb").read())
    refuse("not ISO 9660 (zeros)", bytes(len(d)), "not an ISO 9660 volume")
    refuse("too small", bytes(16 * 2048), "too small")
    e = bytearray(d); struct.pack_into(">H", e, 16 * 2048 + 130, 512); refuse("512-byte logical blocks", e, "other than 2048")
    e = bytearray(d); e[16 * 2048 + 156] = 33; refuse("a root record too short", e, "root directory record is damaged")
    root_ext = struct.unpack_from(">I", d, 16 * 2048 + 156 + 6)[0]
    e = bytearray(d); struct.pack_into(">I", e, 16 * 2048 + 156 + 6, 0x10000); refuse("a root extent off the volume", e, "root directory")
    e = bytearray(d); e[root_ext * 2048 + 32] = 3; refuse("a root whose first record is not its '.'", e, "root directory is damaged")

    # aimed damage: the server answers errors, never crashes
    recs = records(d, root_ext)
    def damaged(name, patch, expect_err=True):
        e = bytearray(d)
        patch(e)
        p = os.path.join(T, "dmg.iso")
        open(p, "wb").write(e)
        try:
            rc, out = run(p, "mount\ntree /\nls /\nls /Mapp\nopen /size300001.bin r\nread %1 0 4096\nquit\n", timeout=30)
        except subprocess.TimeoutExpired:
            check(f"damage: {name}: no hang", False)
            return
        bad = re.search(r"(AddressSanitizer|runtime error|SUMMARY)", out)
        check(f"damage: {name}: no crash" + (", an error answered" if expect_err else ""), rc == 0 and not bad and (not expect_err or re.search(r"(-5|! )", out)), out[-600:])
    f300 = next(o for o in recs.values() if struct.unpack_from(">I", d, o + 14)[0] == 300001)
    damaged("a file's extent off the volume", lambda e: struct.pack_into(">I", e, f300 + 6, 0xFFFFF0))
    damaged("a file's size past the volume", lambda e: struct.pack_into(">I", e, f300 + 14, 0x7FFFFFFF))
    damaged("a record with an odd length", lambda e: e.__setitem__(f300, e[f300] | 1))
    damaged("a record running past its block", lambda e: e.__setitem__(f300, 255))
    damaged("a name longer than its record", lambda e: e.__setitem__(f300 + 32, 250))
    mapp = recs[b"MAPP"]
    damaged("a directory's size past the volume", lambda e: struct.pack_into(">I", e, mapp + 14, 0x7FFFF000))
    damaged("a directory's extent onto a file", lambda e: struct.pack_into(">I", e, mapp + 6, struct.unpack_from(">I", d, f300 + 6)[0]))
    sua = sua_of(d, f300)
    px = find_entry(d, sua, b"PX")
    def ce_loop(e):                                   # PX (44 bytes) becomes a CE back to this same area, then padding
        both = lambda v: struct.pack("<I", v) + struct.pack(">I", v)
        ce = b"CE\x1c\x01" + both(f300 // 2048) + both(sua[0] % 2048) + both(sua[1])
        e[px:px + 44] = ce + b"PD\x10\x01" + bytes(12)
    damaged("a Rock Ridge continuation that loops", ce_loop)
    damaged("a Rock Ridge entry of length 0", lambda e: e.__setitem__(px + 2, 0))
    damaged("a Rock Ridge entry past its area", lambda e: e.__setitem__(px + 2, 250))
    sl = next(x for x in (find_entry(d, sua_of(d, o), b"SL", True) for o in recs.values()) if x)
    damaged("a symbolic link's component past its entry", lambda e: e.__setitem__(sl + 6, 200), expect_err=False)
    damaged("a symbolic link with a NUL", lambda e: e.__setitem__(sl + 7, 0), expect_err=False)

    # layouts not read: interleaved, several extents (listed, answer EOPNOTSUPP)
    for name, patch in (("an interleaved file", lambda e: e.__setitem__(f300 + 26, 1)), ("a file in several extents", lambda e: e.__setitem__(f300 + 25, e[f300 + 25] | 0x80))):
        e = bytearray(d); patch(e)
        p = os.path.join(T, "lay.iso"); open(p, "wb").write(e)
        rc, out = run(p, "mount\nopen /size300001.bin r\nls /\nquit\n")
        check(f"{name}: listed, open answers EOPNOTSUPP", "open -> -95" in out and "size300001.bin" in out, out[:400])

    # fuzz: random bytes over the descriptors, the directories and the Rock Ridge areas
    rnd = random.Random(44)
    hot = [16 * 2048, 17 * 2048, root_ext * 2048] + [o for o in recs.values()]
    hangs = crashes = 0
    for i in range(ROUNDS):
        e = bytearray(d)
        for _ in range(rnd.randint(1, 12)):
            at = rnd.choice(hot) + rnd.randrange(0, 2048) if rnd.random() < 0.8 else rnd.randrange(0, min(len(e), 60 * 2048))
            if at < len(e):
                e[at] = rnd.randrange(256) if rnd.random() < 0.7 else rnd.choice((0, 1, 0xFF, 0x80))
        p = os.path.join(T, "fz.iso")
        open(p, "wb").write(e)
        try:
            rc, out = run(p, "mount\ntree /\nreadlink /Mapp/up\nopen /size300001.bin r\nread %1 1000 70000\nquit\n", timeout=30)
        except subprocess.TimeoutExpired:
            hangs += 1
            shutil.copy(p, os.path.join(T, f"hang{i}.iso"))
            continue
        if rc != 0 or re.search(r"(AddressSanitizer|runtime error)", out):
            crashes += 1
            shutil.copy(p, os.path.join(T, f"crash{i}.iso"))
            if crashes == 1:
                print(out[-1500:])
    check(f"fuzz: {ROUNDS} damaged volumes, no crash", crashes == 0, crashes)
    check(f"fuzz: ... no hang", hangs == 0, hangs)

    print(f"iso9660-host-test: {passes} passed, {fails} failed")
    if fails:
        print("kept for a look:", T)
    else:
        shutil.rmtree(T, ignore_errors=True)
    return 1 if fails else 0


def records(d, ext):
    """identifier -> byte offset of each record in the root directory (and MAPP's)"""
    out = {}
    def walk(e, size):
        o = 0
        while o < size:
            b = e * 2048 + o
            l = d[b]
            if not l:
                o = (o // 2048 + 1) * 2048
                continue
            ident = bytes(d[b + 33:b + 33 + d[b + 32]])
            out[ident] = b
            o += l
    walk(ext, struct.unpack_from(">I", d, 16 * 2048 + 156 + 14)[0])
    m = out[b"MAPP"]
    walk(struct.unpack_from(">I", d, m + 6)[0], struct.unpack_from(">I", d, m + 14)[0])
    return out


def sua_of(d, rec):
    il = d[rec + 32]
    s = 33 + il + (0 if il & 1 else 1)
    return rec + s, d[rec] - s


def find_entry(d, sua, sig, maybe=False):
    at, n = sua
    i = 0
    while i + 4 <= n and d[at + i + 2]:
        if d[at + i:at + i + 2] == sig:
            return at + i
        i += d[at + i + 2]
    if maybe:
        return None
    raise SystemExit(f"no {sig} entry")


if __name__ == "__main__":
    sys.exit(main())
