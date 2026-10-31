#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The FAT server (user/fatd.c) built for the PC, against images from mkfs.fat, checked with fsck.fat and mtools:
what it reads of other systems' volumes, what it writes as other systems read it, the limits, the dirty mark, a power
cut after every sector written, damaged volumes (ASAN + UBSAN: no crash, no hang).
usage: tools/fat-host-test.py [fuzz-rounds]   (default 100; one small image at a time, memory capped)"""
import os, random, re, resource, shutil, signal, struct, subprocess, sys, tempfile

CF = "-m32 -DZHOST -DZFSD -DFATD -Wall -Wextra -Wno-builtin-declaration-mismatch -Wno-unused-function -O1 -g -Iuser".split()
SAN = "-fsanitize=address,undefined -fno-sanitize-recover=undefined".split()
SRC = ["tools/host/fsd-host.c", "tools/host/hostdisk.c"]
fails = passes = 0
T = tempfile.mkdtemp(prefix="zenix-fathost.")
ENV8 = dict(os.environ, LC_ALL="C.UTF-8", MTOOLS_SKIP_CHECK="1", MTOOLS_NO_VFAT="", MTOOLSRC="/dev/null")


def check(name, ok, extra=""):
    global fails, passes
    if ok:
        passes += 1
        print("PASS", name)
    else:
        fails += 1
        print("FAIL", name, extra)


def sh(*a, **k):
    return subprocess.run(a, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=ENV8, **k)


def mkimg(bits, mb=None, extra=(), kb=None, name="i.img"):
    p = os.path.join(T, name)
    with open(p, "wb") as f:
        f.truncate(kb * 1024 if kb else mb * 1024 * 1024)
    if bits == 16 and not extra and mb and mb < 16:
        extra = ("-s", "1")                            # enough clusters for FAT16 on a small volume
    sh("mkfs.fat", "-F", str(bits), "-n", "TEST", *extra, p).check_returncode()
    return p


def run(img, script, exe="fatd", env=None, timeout=120):
    e = dict(os.environ, HOSTDISK=img)
    e.update(env or {})
    r = subprocess.run([os.path.join(T, exe)], input=script.encode(), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       env=e, timeout=timeout)
    return r.returncode, r.stdout.decode(errors="replace")


def fsck(img):
    r = sh("fsck.fat", "-n", img)
    return r.returncode == 0, r.stdout.decode(errors="replace")


def mput(img, name, data):
    p = os.path.join(T, "put.tmp")
    open(p, "wb").write(data)
    sh("mcopy", "-i", img, p, "::" + name).check_returncode()


def mget(img, name):
    p = os.path.join(T, "get.tmp")
    if os.path.exists(p):
        os.remove(p)
    r = sh("mcopy", "-n", "-i", img, "::" + name, p)
    return open(p, "rb").read() if r.returncode == 0 and os.path.exists(p) else None


def pat(n, seed=0):
    return bytes((i * 7 + seed + (i >> 8)) & 0xFF for i in range(n))


def patn(n):                                          # fsd-host's write @N
    return bytes(ord('a') + i % 23 for i in range(n))


def lines(out):
    return dict((l.split(" -> ")[0], l) for l in out.splitlines() if " -> " in l)


def volumes():
    yield "FAT12", (12,), dict(kb=1440)
    yield "FAT16", (16,), dict(mb=16)
    yield "FAT32", (32,), dict(mb=40, extra=("-s", "1"))
    yield "FAT32 below the specification's minimum (the way boot partitions are made)", (32,), dict(mb=16, extra=("-s", "8"))


def test_readwrite():
    for label, (bits,), kw in volumes():
        img = mkimg(bits, **kw)
        small = pat(1000)
        multi = pat(70000, 3)
        mput(img, "PLAIN.TXT", small)
        mput(img, "A long name with spaces and more than thirteen characters.dat", multi)
        sh("mmd", "-i", img, "::Sub dir")
        mput(img, "Sub dir/lower.txt", b"in sub\n")
        mput(img, "räksmörgås åäö.txt", b"swedish\n")
        # reading what another system wrote
        _, o = run(img, "mount\nls /\nopen /a+long+name+with+SPACES+and+more+than+thirteen+characters.DAT r\nread %1 0 70000\n"
                        "fstat %1\nclose %1\nopen /plain.txt r\nread %2 0 2000\nclose %2\nopen /Sub+dir/LOWER.TXT r\nread %3 0 100\nclose %3\n"
                        "open /RÄKSMÖRGÅS+ÅÄÖ.txt r\nread %4 0 100\nclose %4\nstat /Sub+dir/..\nchdir /Sub+dir\nstat /no/such\nstat /PLAIN.TXT/x\n")
        check(f"{label}: mounts", "mount -> 0" in o, o)
        ls = [l for l in o.splitlines() if l.startswith("ls ->")][0]
        check(f"{label}: lists long names, UTF-8 and the plain one", "räksmörgås" in ls and "A" in ls and "PLAIN.TXT" in ls and "Sub" in ls, ls)
        check(f"{label}: reads a long file through a long name, any case", "read -> 70000 (long)" in o and "size 70000" in o, o)
        check(f"{label}: reads a small file, in a subdirectory, a Swedish name in other case",
              "read -> 1000" in o and "in sub" in o and "swedish" in o, o)
        check(f"{label}: missing names and a file used as a directory", "stat -> -2" in o and "stat -> -20" in o, o)
        # writing what another system reads
        s = ("mount\nmkdir /Nya\nopen /Nya/räksmörgås+långt+namn+på+svenska.txt wc\nwrite %1 0 hej\nclose %1\n"
             "open /Nya/UPPER.TXT wc\nwrite %2 0 upper\nclose %2\nopen /Nya/lower.txt wc\nwrite %3 0 lower\nclose %3\n"
             "open /Nya/.hidden wc\nclose %4\nopen /big.bin wc\nwrite %5 0 @300000\nwrite %5 700000 tail\nclose %5\n"
             "rename /PLAIN.TXT /Nya/renamed+file.txt\nunlink /Sub+dir/lower.txt\nrmdir /Sub+dir\nmkdir /Tom\nmkdir /Tom/under\n"
             "rename /Tom/under /upp\nstat /upp/..\nsync\nls /\nls /Nya\numount\n")
        _, o = run(img, s)
        bad = [l for l in o.splitlines() if re.search(r"-> -\d", l)]
        check(f"{label}: a workload of writes answers no error", not bad, str(bad))
        ok, fo = fsck(img)
        check(f"{label}: fsck.fat finds nothing after the writes", ok, fo)
        check(f"{label}: other systems read a Swedish long name", mget(img, "Nya/räksmörgås långt namn på svenska.txt") == b"hej", o)
        check(f"{label}: ... the 8.3 and lower-case names", mget(img, "Nya/UPPER.TXT") == b"upper" and mget(img, "Nya/lower.txt") == b"lower")
        big = mget(img, "big.bin")
        check(f"{label}: ... a big file, a gap of zeros, the tail", big is not None and big[:300000] == patn(300000) and
              big[300000:700000] == bytes(400000) and big[700000:] == b"tail", str(len(big or b"")))
        check(f"{label}: ... a renamed file keeps its data", mget(img, "Nya/renamed file.txt") == small)
        r = sh("mdir", "-i", img, "::upp")
        check(f"{label}: a directory moved out has its parent right (fsck above, mdir here)", r.returncode == 0, r.stdout.decode())
        check(f"{label}: removed things are gone", mget(img, "Sub dir/lower.txt") is None and sh("mdir", "-i", img, "::Sub dir").returncode != 0)
        # our writes read back by ourselves after a fresh start, and overwriting
        _, o = run(img, "mount\nopen /big.bin rw\nwrite %1 100 XYZ\nread %1 99 5\nclose %1\nopen /big.bin wt\nwrite %2 0 short\nclose %2\n"
                        "stat /big.bin\nrename /Nya/UPPER.TXT /Nya/lower.txt\nstat /Nya/UPPER.TXT\nrename /upp /Tom\nrename /Nya/lower.txt /Nya/LOWER.txt\n"
                        "stat /Nya/LOWER.txt\nstatfs\numount\n")
        check(f"{label}: overwrites in place, truncates, renames over a file and over an empty directory, a case change",
              "read -> 5 aXYZe" in o or "read -> 5" in o and "size 5" in o and "stat -> -2" in o, o)
        ok, fo = fsck(img)
        check(f"{label}: fsck.fat still finds nothing", ok, fo)
        check(f"{label}: other systems see the case change and the overwrite", mget(img, "big.bin") == b"short" and mget(img, "Nya/LOWER.txt") == b"upper")


def test_refusals():
    img = mkimg(16, mb=8)
    mput(img, "F.TXT", b"x")
    _, o = run(img, "mount\nsymlink /a /l\nlink /F.TXT /g\nchmod /F.TXT 755\nchmod /F.TXT 444\nstat /F.TXT\nchmod /F.TXT 644\nchown /F.TXT 5 5\nchown /F.TXT 0 0\n"
                    "readlink /F.TXT\nmkdir /x:y\nopen /a*b wc\nopen /end. wc\nopen /sp+ wc\nrmdir /nope\nrmdir /F.TXT\nunlink /nope\nmkdir /d\nopen /d w\nunlink /d\n"
                    "open /d/f wc\nclose %1\nrmdir /d\nrename /d /d/e\nmkdir /F.TXT\nopen /F.TXT wcx\nas 5 5\nopen /F.TXT w\nopen /new wc\nchmod /F.TXT 600\nunlink /F.TXT\numount\n")
    want = ["symlink -> -1", "link -> -1", "chmod -> -1", "chown -> -1", "readlink -> -22", "mkdir -> -22", "open -> -22",
            "open -> -13", "chmod -> -1", "unlink -> -13", "rmdir -> -39", "rename -> -22", "mkdir -> -17", "open -> -17",
            "unlink -> -21", "open -> -21"]
    for w in want:
        check(f"refuses clearly: {w}", w in o, o)
    check("the read-only attribute is chmod's one bit", "mode 100444" in o, o)
    ok, fo = fsck(img)
    check("fsck.fat finds nothing after the refusals", ok, fo)
    # options
    _, o = run(img, "mount uid=1000,gid=100,fmode=660,dmode=750\nstat /F.TXT\nstat /\nas 1000 100\nopen /F.TXT rw\nas 7 7\nstat /d/f\nopen /F.TXT r\ngroups 100\nopen /F.TXT r\n")
    check("mount options set owner, group and modes", "mode 100660" in o and "uid 1000 gid 100" in o and "mode 40750" in o, o)
    check("... and they are obeyed", "stat -> -13" in o and o.count("open -> -13") == 1, o)
    _, o = run(img, "mount nonsense=1\n")
    check("a bad option is refused", "bad option" in o, o)
    w0 = open(img, "rb").read()
    _, o = run(img, "mount ro\nopen /F.TXT r\nread %1 0 1\nopen /F.TXT w\nopen /n wc\nmkdir /m\nunlink /F.TXT\nrename /F.TXT /n\nchmod /F.TXT 444\nutimes /F.TXT 5\nsync\numount\n",
               env={"HOSTDISK_COUNT": "1"})
    check("ro: every change answers EROFS", o.count("-> -30") == 7, o)
    check("ro: not one sector is written", "hostdisk-writes 0" in o and open(img, "rb").read() == w0, o)
    # reading writes nothing either
    _, o = run(img, "mount\nls /\nopen /F.TXT r\nread %1 0 1\nclose %1\nstat /F.TXT\nstatfs\numount\n", env={"HOSTDISK_COUNT": "1"})
    check("reading a volume writes nothing", "hostdisk-writes 0" in o and open(img, "rb").read() == w0, o)
    # an open file's name goes
    _, o = run(img, "mount\nopen /keep.bin wc\nwrite %1 0 @5000\nunlink /keep.bin\nread %1 4000 5\nfstat %1\nopen /other wc\nwrite %2 0 abc\nclose %2\nread %1 0 5\nclose %1\nstat /keep.bin\nsync\numount\n")
    check("a removed open file stays readable", "read -> 5 fghij" in o or o.count("read -> 5") == 2, o)
    ok, fo = fsck(img)
    check("... and its clusters go at the last close", ok, fo)
    _, o = run(img, "mount\nopen /A wc\nwrite %1 0 aaaa\nopen /B wc\nwrite %2 0 bbbbbb\nrename /A /B\nread %2 0 6\nread %1 0 4\nfstat %1\nclose %1\nclose %2\nstat /A\nopen /B r\nread %3 0 10\nsync\numount\n")
    check("rename over an open file: both handles keep their data", "read -> 6 bbbbbb" in o and "read -> 4 aaaa" in o and "stat -> -2" in o, o)
    ok, fo = fsck(img)
    check("... fsck.fat agrees", ok, fo)
    check("... and the new name has the renamed file", mget(img, "B") == b"aaaa")


def test_limits():
    # a FAT12 root is full at its size; a subdirectory grows
    img = mkimg(12, kb=1440, extra=("-r", "16"))
    s = "mount\n" + "".join(f"open /F{i} wc\nclose %{i + 1}\n" for i in range(17)) + "umount\n"
    _, o = run(img, s)
    check("a full FAT12 root answers ENOSPC", "open -> -28" in o, o)
    ok, fo = fsck(img)
    check("... fsck.fat agrees", ok, fo)
    for bits, kw in ((16, dict(mb=8)), (32, dict(mb=40, extra=("-s", "1")))):
        img = mkimg(bits, **kw)
        names = [f"file with a long name number {i:04}.txt" for i in range(500)]
        s = "mount\nmkdir /d\n" + "".join(f"open /d/{n.replace(' ', '+')} wc\nwrite %{i + 1} 0 {i}\nclose %{i + 1}\n" for i, n in enumerate(names))
        if bits == 32:
            s += "".join(f"open /root{i:04}+long+name+in+the+root.x wc\nclose %{i + 501}\n" for i in range(100))
        s += "ls /d\nsync\numount\n"
        _, o = run(img, s)
        check(f"FAT{bits}: 500 long names in a directory that grows", "(502, 0)" in o, o[-300:])
        ok, fo = fsck(img)
        check(f"FAT{bits}: fsck.fat finds nothing", ok, fo)
        check(f"FAT{bits}: another system reads them", mget(img, "d/" + names[377]) == b"377")
        # removed, half of them: the holes are used again, the short names stay unique
        s = "mount\n" + "".join(f"unlink /d/{n.replace(' ', '+')}\n" for n in names[:250]) + \
            "".join(f"open /d/new{i}+longer+name+than+eight.txt wc\nclose %{i + 1}\n" for i in range(50)) + "sync\numount\n"
        _, o = run(img, s)
        ok, fo = fsck(img)
        check(f"FAT{bits}: holes used again", ok and not re.search(r"-> -\d", o), fo + o[-200:])
    # the disk full, and back
    img = mkimg(16, mb=4, extra=("-s", "1"))
    _, o = run(img, "mount\nstatfs\nopen /full wc\nwrite %1 0 @1000000\nwrite %1 1000000 @1000000\nwrite %1 2000000 @1000000\nwrite %1 3000000 @1000000\nwrite %1 4000000 @1000000\nfstat %1\nclose %1\nmkdir /d\nsync\nstatfs\nunlink /full\nstatfs\nmkdir /d\nsync\numount\n")
    check("the disk full: a short write, then ENOSPC", re.search(r"write -> [0-9]{1,6}\n", o) and "mkdir -> -28" in o, o)
    fr = re.findall(r"bfree (\d+)", o)
    check("statfs: free space falls to nothing and comes back", len(fr) == 3 and fr[1] == "0" and fr[2] == fr[0], str(fr) + o)
    check("after it all fsck.fat finds nothing", fsck(img)[0], fsck(img)[1])
    # FAT32's FSInfo is kept
    img = mkimg(32, mb=40, extra=("-s", "1"))
    _, o = run(img, "mount\nopen /a wc\nwrite %1 0 @100000\nclose %1\nsync\numount\n")
    ok, fo = fsck(img)
    check("FAT32: FSInfo stays right (fsck.fat)", ok and "Free cluster summary" not in fo, fo)
    _, o = run(img, "mount\nstatfs\n", env={"HOSTDISK_COUNT": "1"})
    reads = int(re.search(r"hostdisk-reads (\d+)", o).group(1))
    check("FAT32: the free count comes from FSInfo, no scan of the FAT", reads < 20, o)
    # a mirror-off FAT32: only the active FAT
    d = bytearray(open(img, "rb").read())
    struct.pack_into("<H", d, 40, 0x81)                    # FAT 1 active, not mirrored
    fatsz = struct.unpack_from("<I", d, 36)[0]
    res = struct.unpack_from("<H", d, 14)[0]
    d[(res + fatsz) * 512:(res + 2 * fatsz) * 512] = d[res * 512:(res + fatsz) * 512]
    before = bytes(d[res * 512:(res + fatsz) * 512])
    open(img, "wb").write(d)
    _, o = run(img, "mount\nopen /b wc\nwrite %1 0 @10000\nclose %1\nsync\numount\n")
    n = open(img, "rb").read()
    check("FAT32 with mirroring off: the other FAT is never written", n[res * 512:(res + fatsz) * 512] == before and not re.search(r"-> -\d", o), o)
    check("... the active one is", n[(res + fatsz) * 512:(res + 2 * fatsz) * 512] != before)
    # an MBR is not a volume
    img = mkimg(16, mb=8)
    d = bytearray(open(img, "rb").read())
    head = bytearray(512)
    head[510:512] = b"\x55\xaa"
    head[446 + 4] = 0x0C
    open(img, "wb").write(bytes(head) + bytes(d))
    _, o = run(img, "mount\n")
    check("a disk with a partition table is refused, with a reason", "mount -> -1" in o and "not a FAT volume" in o, o)
    _, o = run(img, "mount\nopen /x wc\nwrite %1 0 hello\nclose %1\nls /\numount\n", env={"HOSTDISK_START": "1"})
    check("the same volume as a partition (the sectors after the table) works", "ls -> x" in o, o)
    # 4 KB sectors are named
    img = mkimg(32, mb=40, extra=("-S", "4096"))
    _, o = run(img, "mount\n")
    check("4 KB sectors are refused by name", "only 512-byte" in o, o)


def test_dirty():
    img = mkimg(16, mb=8)
    mput(img, "OTHER.TXT", b"other\n")
    _, o = run(img, "mount\nopen /a wc\nwrite %1 0 abc\nclose %1\ncommit\n")     # no umount: as a power cut after a commit
    d = open(img, "rb").read()
    res = struct.unpack_from("<H", d, 14)[0]
    v = struct.unpack_from("<H", d, res * 512 + 2)[0]
    check("FAT16: a change marks FAT[1] not clean", not v & 0x8000, hex(v))
    fatsz = struct.unpack_from("<H", d, 22)[0]
    check("... in both FATs", not struct.unpack_from("<H", d, (res + fatsz) * 512 + 2)[0] & 0x8000)
    check("... the boot sector is as it was", d[0:512] == open(mkimg(16, mb=8, name="z.img"), "rb").read()[0:512] or d[37] == 0)
    _, o = run(img, "mount\nstat /a\nopen /b wc\nopen /a w\numount\n")
    check("a volume not marked clean is mounted read-only", "mount -> 0 readonly" in o and "was not unmounted cleanly" in o and "open -> -30" in o, o)
    check("... what was committed is there", "size 3" in o, o)
    w0 = open(img, "rb").read()
    check("... and nothing was written", w0 == d)
    _, o = run(img, "mount force\nopen /b wc\nclose %1\nsync\numount\n")
    check("-o force mounts it writable", "mount -> 0\n" in o and "open -> -" not in o, o)
    d = open(img, "rb").read()
    check("... and it is clean afterwards", struct.unpack_from("<H", d, res * 512 + 2)[0] & 0x8000 != 0)
    _, o = run(img, "mount\nopen /c wc\nclose %1\numount\n")
    check("a volume cleanly unmounted mounts writable", "mount -> 0\n" in o and "open -> -" not in o, o)
    check("the mark never reaches fsck.fat as damage", fsck(img)[0], fsck(img)[1])
    # the boot sector's dirty byte, set by another system
    d = bytearray(open(img, "rb").read())
    d[37] |= 1
    open(img, "wb").write(d)
    _, o = run(img, "mount\n")
    check("another system's dirty byte: read-only too", "readonly" in o, o)
    # the mark's FAT32 form
    img = mkimg(32, mb=40, extra=("-s", "1"))
    _, o = run(img, "mount\nopen /a wc\nclose %1\ncommit\n")
    d = open(img, "rb").read()
    res = struct.unpack_from("<H", d, 14)[0]
    v = struct.unpack_from("<I", d, res * 512 + 4)[0]
    check("FAT32: the mark, and the FAT's reserved top bits kept", not v & 0x08000000 and v >> 28 == 0, hex(v))
    _, o = run(img, "mount force\nsync\numount\n")
    d = open(img, "rb").read()
    check("FAT32: cleared", struct.unpack_from("<I", d, res * 512 + 4)[0] & 0x08000000 != 0)
    # the boot sector is never written
    img = mkimg(32, mb=40, extra=("-s", "1"))
    b0 = open(img, "rb").read(512 * 6 + 512)
    run(img, "mount\nopen /a wc\nwrite %1 0 @5000\nclose %1\nmkdir /m\nsync\numount\n")
    b1 = open(img, "rb").read(512 * 6 + 512)
    check("the boot sector and its backup are never written", b0[:512] == b1[:512] and b0[6 * 512:] == b1[6 * 512:])


def test_cuts():
    """a power cut after every sector written: other files stay whole, fsck.fat -a repairs the rest"""
    for label, bits, kw in (("FAT16", 16, dict(mb=4, extra=("-s", "1"))), ("FAT32", 32, dict(mb=40, extra=("-s", "1"))), ("FAT12", 12, dict(kb=720))):
        base = mkimg(bits, name="base.img", **kw)
        others = {f"OTHER{i}.BIN": pat(3000 + 900 * i, i) for i in range(6)}
        for n, data in others.items():
            mput(base, n, data)
        sh("mmd", "-i", base, "::keep")
        mput(base, "keep/kept with long name.txt", pat(1500, 9))
        mput(base, "old.bin", pat(4000, 5))
        s = ("mount\nopen /new.bin wc\nwrite %1 0 @6000\nclose %1\nmkdir /nydir\nopen /nydir/en+fil+med+långt+namn.txt wc\nwrite %2 0 @700\nclose %2\n"
             "commit\nunlink /old.bin\nrename /new.bin /nydir/moved.bin\nopen /app.bin wc\nwrite %3 0 @3000\nwrite %3 3000 @3000\nclose %3\n"
             "commit\nopen /nydir/en+fil+med+långt+namn.txt wt\nwrite %4 0 @100\nclose %4\nrename /app.bin /OTHER5.BIN\nsync\numount\n")
        cut = os.path.join(T, "cut.img")
        shutil.copy(base, cut)
        rc, o = run(cut, s, env={"HOSTDISK_COUNT": "1"})
        total = int(re.search(r"hostdisk-writes (\d+)", o).group(1))
        check(f"{label}: the workload runs whole ({total} sectors)", rc == 0 and fsck(cut)[0] and not re.search(r"-> -\d", o), o)
        bad_other = bad_fsck = bad_rename = 0
        step = 1 if total < 150 else 2
        first = ""
        for k in range(0, total, step):
            shutil.copy(base, cut)
            rc, o = run(cut, s, env={"HOSTDISK_CUT": str(k)})
            for n, data in others.items():
                got = mget(cut, n)
                # OTHER5 is replaced by the workload's last rename: the old or the new file, never anything else
                if got != data and not (n == "OTHER5.BIN" and got == patn(3000) * 2):
                    bad_other += 1
                    first = first or f"cut {k}: {n}"
            if mget(cut, "keep/kept with long name.txt") != pat(1500, 9):
                bad_other += 1
                first = first or f"cut {k}: keep"
            sh("fsck.fat", "-a", cut)
            r = sh("fsck.fat", "-n", cut)
            if r.returncode:
                bad_fsck += 1
                first = first or f"cut {k}: fsck: " + r.stdout.decode(errors="replace")[:400]
            # old.bin / moved file: after repair, either name has whole data or (written) a prefix within its size
            moved = mget(cut, "nydir/moved.bin"), mget(cut, "new.bin")
            for m in moved:
                if m and m != patn(6000) and m != patn(len(m)):
                    bad_rename += 1
                    first = first or f"cut {k}: moved {len(m)}"
        check(f"{label}: other files whole after a cut at any of {total} sectors", bad_other == 0, first)
        check(f"{label}: fsck.fat -a repairs every cut in one run", bad_fsck == 0, first)
        check(f"{label}: a file being renamed has its data under a name", bad_rename == 0, first)


def test_fuzz(rounds):
    """damaged volumes: an error, never a crash or a hang"""
    rnd = random.Random(20261031)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    bases = [("f12", mkimg(12, kb=720, name="fz12.img")), ("f16", mkimg(16, mb=4, extra=("-s", "1"), name="fz16.img")),
             ("f32", mkimg(32, mb=8, extra=("-s", "2"), name="fz32.img"))]
    for _, b in bases:
        sh("mmd", "-i", b, "::dir one")
        mput(b, "dir one/long file name inside.txt", pat(5000))
        mput(b, "SHORT.TXT", pat(2000, 2))
        mput(b, "räksmörgås.txt", pat(600, 4))
    script = ("mount force\nstatfs\nls /\nls /dir+one\nopen /SHORT.TXT r\nread %1 0 2000\nread %1 1500 400\nfstat %1\nopen /dir+one/long+file+name+inside.txt rw\n"
              "read %2 0 5000\nwrite %2 100 xx\nwrite %2 6000 tail\nclose %2\nopen /new.bin wc\nwrite %3 0 @9000\nclose %3\nmkdir /nd\nopen /nd/f+with+long+name wc\nclose %4\n"
              "rename /SHORT.TXT /nd/renamed.txt\nrename /nd /nd2\nrename /dir+one /nd2/x\nunlink /räksmörgås.txt\nrmdir /nd2/x\nunlink /new.bin\nchdir /nd2\nstat /nd2/..\nutimes /nd2 5\nsync\nls /\numount\n")
    crashes = hangs = errors = 0
    first = ""
    for rd in range(rounds):
        name, base = bases[rd % 3]
        d = bytearray(open(base, "rb").read())
        size = len(d)
        res = struct.unpack_from("<H", d, 14)[0]
        fatsz = struct.unpack_from("<H", d, 22)[0] or struct.unpack_from("<I", d, 36)[0]
        root = (res + d[16] * fatsz) * 512
        kind = rd % 7
        rng = {0: (0, 90), 1: (res * 512, min(4096, fatsz * 512)), 2: (root, 4096), 3: (0, 512), 4: (root, 2048), 5: (res * 512, 512), 6: (0, min(size, 1 << 20))}[kind]
        n = rnd.choice((1, 2, 4, 8, 32, 200))
        for _ in range(n):
            at = rng[0] + rnd.randrange(rng[1])
            d[at] = rnd.choice((0, 1, 2, 0xFF, 0xF8, 0xE5, 0x0F, 0x80, rnd.randrange(256)))
        if kind == 5:                                      # a loop in a chain: a cluster that points to itself or back
            bits = {"f12": 0, "f16": 16, "f32": 32}[name]
            if bits:
                for c in range(3, 40, 3):
                    v = rnd.randrange(2, c + 1)
                    if bits == 16:
                        struct.pack_into("<H", d, res * 512 + c * 2, v)
                    else:
                        struct.pack_into("<I", d, res * 512 + c * 4, v)
        if kind == 6 and rd % 2:                           # a truncated volume
            d = d[:rnd.randrange(512, size)]
        p = os.path.join(T, "fz.img")
        open(p, "wb").write(d)
        try:
            rc, o = run(p, script, exe="fatd-asan", timeout=60, env={"ASAN_OPTIONS": "detect_leaks=0:abort_on_error=0:allocator_may_return_null=1", "UBSAN_OPTIONS": "halt_on_error=1"})
        except subprocess.TimeoutExpired:
            hangs += 1
            first = first or f"round {rd}: hang"
            shutil.copy(p, "/tmp/zenix-fat-hang.img")
            continue
        if rc != 0 or "Sanitizer" in o or "runtime error" in o:
            crashes += 1
            if not first:
                first = f"round {rd} kind {kind}: rc {rc}\n" + o[-1500:]
                shutil.copy(p, "/tmp/zenix-fat-crash.img")
        elif re.search(r"-> -5\b", o):
            errors += 1
        if rd and not rd % 100:
            print(f"fuzz: {rd} rounds, {crashes} crashes, {hangs} hangs, {errors} answered EIO", flush=True)
        if not rd % 50 and os.getloadavg()[0] > 16:
            print("load above 16: stopping the fuzz run early", flush=True)
            break
    check(f"fuzz: {rounds} damaged volumes, no crash", crashes == 0, first)
    check("fuzz: no hang", hangs == 0, first)
    check(f"fuzz: damage was seen ({errors} volumes answered EIO)", errors > 0)


def main():
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 100
    for t in ("mkfs.fat", "fsck.fat", "mcopy"):
        if not shutil.which(t):
            print(f"FAIL {t} is missing")
            return 1
    if subprocess.run(["gcc"] + CF + ["-o", os.path.join(T, "fatd")] + SRC).returncode:
        print("FAIL build")
        return 1
    if subprocess.run(["gcc"] + CF + SAN + ["-o", os.path.join(T, "fatd-asan")] + SRC).returncode:
        print("FAIL build (asan)")
        return 1
    only = os.environ.get("FAT_ONLY", "readwrite refusals limits dirty cuts fuzz").split()
    if "readwrite" in only: test_readwrite()
    if "refusals" in only: test_refusals()
    if "limits" in only: test_limits()
    if "dirty" in only: test_dirty()
    if "cuts" in only: test_cuts()
    if "fuzz" in only: test_fuzz(rounds)
    print(f"fat-host-test: {passes} passed, {fails} failed")
    return 1 if fails else 0


signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))    # the temporary folder goes also when stopped
try:
    rc = main()
finally:
    shutil.rmtree(T, ignore_errors=True)
sys.exit(rc)
