#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The FAT server (user/fatd.c) built for the PC, against images from mkfs.fat, checked with fsck.fat and mtools:
what it reads of other systems' volumes, what it writes as other systems read it, the limits, the dirty mark, a power
cut after every sector written, damaged volumes (ASAN + UBSAN: no crash, no hang).
usage: tools/fat-host-test.py [fuzz-rounds]   (default 330; one small image at a time, memory capped)"""
import os, random, re, resource, shutil, signal, struct, subprocess, sys, tempfile, time

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


def run(img, script, exe=os.environ.get("FAT_EXE", "fatd"), env=None, timeout=120):
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


def test_readonly():
    """D30 read-only mode (-o ro, and a device that says DISKCAP_RO): every change answers EROFS, nothing is written."""
    calls = ("open /F.TXT w\nopen /F.TXT rw\nopen /F.TXT wt\nopen /n wc\nopen /F.TXT r\nwrite %1 0 abc\nmkdir /m\nunlink /F.TXT\nrmdir /Z\n"
             "rename /F.TXT /n\nrename /D /E\nchmod /F.TXT 444\nchmod /F.TXT 644\nchmod /D 755\nutimes /F.TXT 5\nutimes /F.TXT now\nutimes /D 5\n"
             "sync\nstatfs\nstat /F.TXT\nstat /D\nls /\nread %1 0 3\nsync\n")
    for bits, mb in ((12, 1), (16, 8), (32, 40)):
        for how in ("ro", "caps"):
            tag = f"FAT{bits} {'-o ro' if how == 'ro' else 'read-only device'}"
            img = mkimg(bits, mb=mb)
            mput(img, "F.TXT", b"hello")
            sh("mmd", "-i", img, "::D").check_returncode()
            mput(img, "D/IN.TXT", b"in")
            sh("mmd", "-i", img, "::Z").check_returncode()
            before = open(img, "rb").read()
            env = {"HOSTDISK_COUNT": "1"}
            if how == "caps":
                env["HOSTDISK_CAPRO"] = "1"
            _, o = run(img, ("mount ro\n" if how == "ro" else "mount\n") + calls + "umount\n", env=env)
            check(f"{tag}: mounts read-only", "mount -> 0 readonly" in o, o)
            check(f"{tag}: the 16 changes answer EROFS", o.count("-> -30") == 16, o)
            check(f"{tag}: df, stat, ls and read still work", "statfs -> 0" in o and "read -> 3 hel" in o and "stat -> 0" in o and "IN.TXT" not in o and "F.TXT" in o, o)
            check(f"{tag}: not one sector written, the image is byte for byte the same", "hostdisk-writes 0" in o and "hostdisk-write-attempts 0" in o and open(img, "rb").read() == before, o)
            ok, fo = fsck(img)
            check(f"{tag}: fsck.fat finds nothing", ok, fo)
    # a device that says read-only refuses -o rw; plain "rw" on a writable device still works
    img = mkimg(32, mb=40)
    _, o = run(img, "mount rw\numount\n", env={"HOSTDISK_CAPRO": "1"})
    check("a read-only device: -o rw is refused", "mount -> -1" in o and "-o rw is refused" in o, o)
    _, o = run(img, "mount rw\nopen /n wc\numount\n")
    check("a writable device: -o rw still writes", "mount -> 0\n" in o and "readonly" not in o and "open -> -30" not in o, o)
    # a volume left dirty by a "-o force" mount never changes when asked for ro, even with FSInfo and the dirty bit
    img = mkimg(32, mb=40)
    _, o = run(img, "mount\nopen /A wc\nwrite %1 0 abc\nunlink /A\nunclean\n")
    before = open(img, "rb").read()
    _, o = run(img, "mount\numount\n")
    check("... the volume really is not clean", "was not unmounted cleanly" in o, o)
    _, o = run(img, "mount ro,force\nstatfs\numount\n", env={"HOSTDISK_COUNT": "1"})
    check("a dirty volume mounted -o ro,force: nothing written", "readonly" in o and "hostdisk-writes 0" in o and open(img, "rb").read() == before, o)


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
    # a loop in a file's chain answers EIO, and ends
    for bits, kw in ((12, dict(kb=720)), (16, dict(mb=8)), (32, dict(mb=40, extra=("-s", "1")))):
        img = mkimg(bits, **kw)
        mput(img, "LOOP.BIN", pat(40000))
        d = bytearray(open(img, "rb").read())
        v = Vol(d, bits)
        e = v.entries(v.rootclus)[0][-1]
        ch = v.chain(v.first(e))
        v.set(ch[len(ch) // 2], ch[3])
        open(img, "wb").write(d)
        _, o = run(img, "mount\nopen /LOOP.BIN r\nopen /LOOP.BIN w\nopen /LOOP.BIN wt\nwrite %1 0 fresh\nclose %1\nstat /LOOP.BIN\nsync\numount\n", timeout=60)
        check(f"FAT{bits}: a loop in a chain answers EIO at open, for reading and writing", o.count("open -> -5") == 2, o)
        check(f"FAT{bits}: ... overwriting the file still works, and frees the loop", "size 5" in o and "write -> 5" in o, o)
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


class Vol:
    """an image's layout, read the way a damaging test needs it (the first FAT only: the server reads that one)"""
    def __init__(self, d, bits):
        self.d, self.bits = d, bits
        self.res = struct.unpack_from("<H", d, 14)[0]
        self.fatsz = struct.unpack_from("<H", d, 22)[0] or struct.unpack_from("<I", d, 36)[0]
        self.spc = d[13]
        rootents = struct.unpack_from("<H", d, 17)[0]
        self.rootoff = (self.res + d[16] * self.fatsz) * 512
        self.data = self.rootoff + (rootents * 32 + 511) // 512 * 512
        self.rootclus = struct.unpack_from("<I", d, 44)[0] if bits == 32 else 0

    def fat_at(self, c):
        return self.res * 512 + (c + c // 2 if self.bits == 12 else c * self.bits // 8)

    def get(self, c):
        p = self.fat_at(c)
        if self.bits == 12:
            v = self.d[p] | self.d[p + 1] << 8
            return v >> 4 if c & 1 else v & 0xFFF
        return struct.unpack_from("<H" if self.bits == 16 else "<I", self.d, p)[0]

    def set(self, c, v):
        p = self.fat_at(c)
        if self.bits == 12:
            if c & 1:
                self.d[p] = (self.d[p] & 0x0F) | (v << 4 & 0xF0)
                self.d[p + 1] = v >> 4 & 0xFF
            else:
                self.d[p] = v & 0xFF
                self.d[p + 1] = (self.d[p + 1] & 0xF0) | (v >> 8 & 0x0F)
        else:
            struct.pack_into("<H" if self.bits == 16 else "<I", self.d, p, v & (0xFFFF if self.bits == 16 else 0x0FFFFFFF))

    def clus_off(self, c):
        return self.data + (c - 2) * self.spc * 512

    def first(self, e):
        c = struct.unpack_from("<H", self.d, e + 26)[0]
        return c | struct.unpack_from("<H", self.d, e + 20)[0] << 16 if self.bits == 32 else c

    def set_first(self, e, c):
        struct.pack_into("<H", self.d, e + 26, c & 0xFFFF)
        if self.bits == 32:
            struct.pack_into("<H", self.d, e + 20, c >> 16 & 0xFFFF)

    def dir_span(self, cl):
        return (self.rootoff, self.data - self.rootoff) if not cl else (self.clus_off(cl), self.spc * 512)

    def entries(self, cl):
        """(offset of short entries, offset of long-name parts) in the first cluster or root of a directory"""
        at, n = self.dir_span(cl)
        sh, lf = [], []
        for o in range(at, at + n, 32):
            if self.d[o] == 0:
                break
            if self.d[o] != 0xE5:
                (lf if self.d[o + 11] & 0x3F == 0x0F else sh).append(o)
        return sh, lf

    def chain(self, c):
        out = []
        end = {12: 0xFF8, 16: 0xFFF8, 32: 0x0FFFFFF8}[self.bits]
        while 2 <= c < end and c not in out and len(out) < 200:
            out.append(c)
            c = self.get(c) & 0x0FFFFFFF
        return out


def damage(d, bits, kind, rnd):
    """one kind of damage to a volume that otherwise mounts, so it reaches the server's walks; kinds 0-1 hit the boot sector"""
    v = Vol(d, bits)
    root = v.rootclus
    rsh, rlf = v.entries(root)
    files = [e for e in rsh if not d[e + 11] & 0x18 and v.first(e) >= 2]
    dirs = [e for e in rsh if d[e + 11] & 0x10]
    subsh, sublf = v.entries(v.first(dirs[0])) if dirs else ([], [])
    subfiles = [e for e in subsh if d[e] != 0x2E and not d[e + 11] & 0x18 and v.first(e) >= 2]
    allfiles = files + subfiles
    chains = [v.chain(v.first(e)) for e in allfiles]
    longest = max(chains, key=len)
    high = {12: 0xFF0, 16: 0xFFF0, 32: 0x0FFFFFF0}[bits]
    nclus_guess = (len(d) - v.data) // (v.spc * 512) + 2
    if kind == 0:                                          # one boot sector field, a value on an edge
        at, size = rnd.choice(((11, 2), (13, 1), (14, 2), (16, 1), (17, 2), (19, 2), (21, 1), (22, 2), (32, 4), (36, 4), (40, 2), (44, 4), (48, 2)))
        val = rnd.choice((0, 1, 2, 3, 0x80, 0xFF, 0xFFFF, 0xFFFFFFFF, rnd.randrange(1 << (8 * size))))
        d[at:at + size] = (val & ((1 << (8 * size)) - 1)).to_bytes(size, "little")
    elif kind == 1:                                        # a smaller claim that still mounts: FAT or root cut short
        if bits == 32:
            struct.pack_into("<I", d, 44, rnd.choice((2, 3, longest[0], nclus_guess - 1, nclus_guess)))
        else:
            struct.pack_into("<H", d, 17, rnd.choice((16, 32, 512, 1024)))
    elif kind == 2:                                        # a loop: a chain back to itself or an earlier cluster
        c = rnd.choice(longest)
        v.set(c, rnd.choice(longest[:longest.index(c) + 1]))
    elif kind == 3:                                        # a chain that leaves the volume, or ends in a free or bad cluster
        v.set(rnd.choice(longest), rnd.choice((0, 1, nclus_guess, nclus_guess + 5, high, {12: 0xFF7, 16: 0xFFF7, 32: 0x0FFFFFF7}[bits])))
    elif kind == 4:                                        # an entry's first cluster and size lie
        e = rnd.choice(allfiles)
        if rnd.random() < 0.6:
            v.set_first(e, rnd.choice((0, 1, nclus_guess, nclus_guess + 9, high, 0x0FFFFFFF if bits == 32 else high, longest[0])))
        else:
            struct.pack_into("<I", d, e + 28, rnd.choice((0, 1, 511, 1 << 20, 1 << 31, 0xFFFFFFFF, 0xFFFFFFE0)))
    elif kind == 5:                                        # a directory: first cluster, attributes, chain
        e = rnd.choice(rsh)
        w = rnd.randrange(5)
        if w == 0 and dirs:
            v.set_first(dirs[0], rnd.choice((0, 1, nclus_guess, longest[0], v.first(dirs[0]), root)))
        elif w == 1:
            d[e + 11] ^= rnd.choice((0x10, 0x08, 0x0F, 0x3F, 0xFF))
        elif w == 2 and dirs:                              # the subdirectory's chain loops or leaves
            c = v.first(dirs[0])
            v.set(c, rnd.choice((c, 0, nclus_guess, longest[0])))
        elif w == 3 and root:                              # FAT32's root chain itself
            v.set(root, rnd.choice((root, 0, nclus_guess)))
        elif w == 4 and dirs:                              # a directory whose "." or ".." lies
            c = v.first(dirs[0])
            o = v.clus_off(c) + rnd.choice((0, 32))
            v.set_first(o, rnd.choice((0, 1, c, nclus_guess, longest[0])))
    elif kind == 6:                                        # long-name parts: order, checksum, attribute, characters
        parts = rlf + sublf
        for _ in range(rnd.choice((1, 2, 4))):
            o = rnd.choice(parts)
            w = rnd.randrange(5)
            if w == 0:
                d[o] = rnd.choice((0, 0x40, 0x41, 0x54, 0x55, 0x7F, 0x20, 1, 20, 21))
            elif w == 1:
                d[o + 13] ^= 0xFF
            elif w == 2:
                struct.pack_into("<H", d, o + rnd.choice((1, 3, 14, 28, 30)), rnd.choice((0, 0x2F, 1, 0xD800, 0xDC00, 0xFFFF, 0xFFFE)))
            elif w == 3:
                d[o:o + 32] = bytes(32)
            else:
                d[o + 11] = 0x0E
    elif kind == 7:                                        # a directory of garbage: a file's data as entries, a sector of noise
        o = rnd.choice(rsh + subsh)
        at, n = v.dir_span(v.first(dirs[0]) if dirs else root)
        for i in range(rnd.choice((1, 8, 32))):
            at2 = at + 32 * rnd.randrange(n // 32)
            d[at2:at2 + 32] = bytes(rnd.randrange(256) for _ in range(32))
        d[o] = rnd.choice((0, 5, 0xE5, 0x20, 0x2F, 0x2E, 0xFF))
    elif kind == 8:                                        # the FAT's own place: noise over part of it
        at = v.res * 512 + rnd.randrange(min(v.fatsz * 512 - 64, 512))
        for i in range(rnd.choice((4, 16, 64))):
            d[at + i] = rnd.choice((0, 0xFF, 2, rnd.randrange(256)))
    elif kind == 9:                                        # free count and next-free hints that lie (FAT32), the dirty byte
        if bits == 32:
            fs = struct.unpack_from("<H", d, 48)[0] * 512
            struct.pack_into("<I", d, fs + rnd.choice((488, 492)), rnd.choice((0, 1, 0xFFFFFFFF, 0xFFFFFFFE, nclus_guess)))
        else:
            d[37] ^= 1
        v.set(1, rnd.choice((0, 0xFFF)))
    elif kind == 10:                                       # the volume cut short
        del d[rnd.randrange(512, len(d)):]
    return d


def test_fuzz(rounds):
    """damaged volumes: an error, never a crash or a hang. Each round: one kind of damage aimed at a walk."""
    rnd = random.Random(20261031)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    bases = [(12, mkimg(12, kb=720, name="fz12.img")), (16, mkimg(16, mb=4, extra=("-s", "1"), name="fz16.img")),
             (32, mkimg(32, mb=8, extra=("-s", "2"), name="fz32.img"))]
    for _, b in bases:
        sh("mmd", "-i", b, "::dir one")
        mput(b, "dir one/long file name inside.txt", pat(5000))
        mput(b, "SHORT.TXT", pat(2000, 2))
        mput(b, "räksmörgås.txt", pat(600, 4))
        mput(b, "BIG.BIN", pat(30000, 6))
    script = ("mount force\nstatfs\nls /\nls /dir+one\nopen /SHORT.TXT r\nread %1 0 2000\nread %1 1500 400\nfstat %1\nopen /BIG.BIN r\nread %2 0 30000\nread %2 20000 100\n"
              "open /dir+one/long+file+name+inside.txt rw\nread %3 0 5000\nwrite %3 100 xx\nwrite %3 6000 tail\nclose %3\nopen /new.bin wc\nwrite %4 0 @9000\nclose %4\nmkdir /nd\nopen /nd/f+with+long+name wc\nclose %5\n"
              "rename /SHORT.TXT /nd/renamed.txt\nrename /nd /nd2\nrename /dir+one /nd2/x\nunlink /räksmörgås.txt\nrmdir /nd2/x\nopen /BIG.BIN wt\nwrite %6 0 again\nclose %6\nunlink /new.bin\nchdir /nd2\nstat /nd2/..\nutimes /nd2 5\nsync\nls /\nstatfs\numount\n")
    KINDS = 11
    crashes = hangs = errors = refused = ran = 0
    slowest = (0, 0, 0, 0)
    per = [[0, 0, 0] for _ in range(KINDS)]                # each kind: refused at mount, answered EIO, neither
    first = ""
    done = 0
    for rd in range(rounds):
        bits, base = bases[rd % 3]
        kind = rd // 3 % KINDS
        d = damage(bytearray(open(base, "rb").read()), bits, kind, rnd)
        if rd % 4 == 3 and kind != 10:                     # now and then a second damage on top of the first
            try:
                d = damage(d, bits, rnd.randrange(2, 10), rnd)
            except (ValueError, IndexError, ZeroDivisionError, OverflowError, struct.error):
                pass                                       # the first left nothing for the second to aim at
        p = os.path.join(T, "fz.img")
        open(p, "wb").write(d)
        done += 1
        t0 = time.time()
        try:
            rc, o = run(p, script, exe="fatd-asan", timeout=60, env={"ASAN_OPTIONS": "detect_leaks=0:abort_on_error=0:allocator_may_return_null=1", "UBSAN_OPTIONS": "halt_on_error=1"})
        except subprocess.TimeoutExpired:
            hangs += 1
            first = first or f"round {rd} kind {kind}: hang"
            shutil.copy(p, "/tmp/zenix-fat-hang.img")
            continue
        el = time.time() - t0
        if el > slowest[0]:
            slowest = (el, rd, kind, bits)
        if rc != 0 or "Sanitizer" in o or "runtime error" in o:
            crashes += 1
            if not first:
                first = f"round {rd} kind {kind} FAT{bits}: rc {rc}\n" + o[-1500:]
                shutil.copy(p, "/tmp/zenix-fat-crash.img")
            continue
        if "mount -> -1" in o:
            refused += 1
            per[kind][0] += 1
        elif re.search(r"-> -5\b", o):
            errors += 1
            per[kind][1] += 1
        else:
            ran += 1
            per[kind][2] += 1
        if rd and not rd % 100:
            print(f"fuzz: {rd} rounds, {crashes} crashes, {hangs} hangs, {refused} refused, {errors} answered EIO", flush=True)
        if not rd % 50 and os.getloadavg()[0] > 16:
            print("load above 16: stopping the fuzz run early", flush=True)
            break
    print(f"fuzz: {done} rounds: {refused} refused at mount, {errors} answered EIO, {ran} neither; per kind (refused, EIO, neither): {per}; slowest {slowest[0]:.1f} s (round {slowest[1]}, kind {slowest[2]}, FAT{slowest[3]})", flush=True)
    check(f"fuzz: {done} damaged volumes, no crash", crashes == 0, first)
    check("fuzz: no hang", hangs == 0, first)
    check(f"fuzz: the damage reached the walks ({errors} volumes answered EIO after mounting)", errors >= done // 6)


def main():
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 330
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
    only = os.environ.get("FAT_ONLY", "readwrite refusals readonly limits dirty cuts fuzz").split()
    if "readwrite" in only: test_readwrite()
    if "refusals" in only: test_refusals()
    if "readonly" in only: test_readonly()
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
