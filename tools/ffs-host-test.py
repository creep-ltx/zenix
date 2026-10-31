#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The OFS/FFS server (user/ffsd.c) built for the PC, against images from tools/mkffs.py and xdftool (amitools), judged by
xdfscan where it is installed and by this file's own walker: what it reads of other tools' volumes, what it writes, the
refusals, the check before a writable mount, a power cut after every sector written, damaged volumes (ASAN + UBSAN: no
crash, no hang).
usage: tools/ffs-host-test.py [fuzz-rounds]   (default 440; one small image at a time). FFS_ONLY="cuts fuzz" runs parts."""
import os, random, re, resource, shutil, signal, struct, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True
from mkffs import Volume
from ffscheck import Disk

CF = "-m32 -DZHOST -DZFSD -DFFSD -Wall -Wextra -Wno-builtin-declaration-mismatch -Wno-unused-function -O1 -g -Iuser".split()
SAN = "-fsanitize=address,undefined -fno-sanitize-recover=undefined".split()
SRC = ["tools/host/fsd-host.c", "tools/host/hostdisk.c"]
EPOCH = 1700000000
os.environ["SOURCE_DATE_EPOCH"] = str(EPOCH)
fails = passes = 0
T = tempfile.mkdtemp(prefix="zenix-ffshost.")
XDF = shutil.which("xdftool")
XSCAN = shutil.which("xdfscan")
ENV8 = dict(os.environ, LC_ALL="C.UTF-8")
EXE = os.environ.get("FFS_EXE", "ffsd")


def check(name, ok, extra=""):
    global fails, passes
    if ok:
        passes += 1
        print("PASS", name)
    else:
        fails += 1
        print("FAIL", name, str(extra)[:1500])


def sh(*a, **k):
    return subprocess.run(a, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=ENV8, **k)


def run(img, script, exe=None, env=None, timeout=120):
    e = dict(os.environ, HOSTDISK=img)
    e.update(env or {})
    r = subprocess.run([os.path.join(T, exe or EXE)], input=script.encode(), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       env=e, timeout=timeout)
    return r.returncode, r.stdout.decode(errors="replace")


def pat(n, seed=0):
    return bytes((i * 7 + seed + (i >> 8)) & 0xFF for i in range(n))


def patn(n):                                               # fsd-host's write @N
    return bytes(ord('a') + i % 23 for i in range(n))


def errors(out):
    return [l for l in out.splitlines() if re.search(r"-> -\d", l)]


def scan_ok(img, base=None):
    """xdfscan's verdict: blocks marked used that nothing names are allowed, what the fresh image says too. True without it"""
    if not XSCAN:
        return True, ""
    p = img if img.endswith(".hdf") else img + ".hdf"
    if p != img:
        shutil.copy(img, p)
    r = sh(XSCAN, p)
    out = r.stdout.decode(errors="replace")
    if p != img:
        os.remove(p)
    if "Traceback" in out:
        return False, out[-600:]
    badlines = [l for l in out.splitlines() if re.search(r"(?i)error|invalid|bad|mismatch", l) and "Invalid bitmap allocation" not in l]
    return not badlines, "\n".join(badlines)[:600]


def mkvol(name, dos=3, bs=512, kb=1024, reserved=2, tree=None):
    v = Volume(kb * 1024 // bs, "Testvol", dos >= 2, dos, bs, reserved)
    p = os.path.join(T, name)
    if tree is None:
        v.add_file("plain.txt", pat(1000))
        v.add_file("big.bin", pat(90000, 3))
        v.add_file("Mapp/sub/lower.txt", b"in sub\n")
        v.add_file("r\xe4ksm\xf6rg\xe5s \xe5\xe4\xf6.txt", b"swedish\n")
        v.add_file("empty", b"")
    else:
        tree(v)
    open(p, "wb").write(v.image())
    return p


def xdf(name, form, kb=None):
    """an image made by another program"""
    p = os.path.join(T, name)
    if os.path.exists(p):
        os.remove(p)
    if name.endswith(".adf"):
        sh(XDF, p, "create", "+", "format", "Testvol", form).check_returncode()
    else:
        sh(XDF, p, "create", f"size={kb}Ki", "+", "format", "Testvol", form).check_returncode()
    h = os.path.join(T, "x.tmp")
    open(h, "wb").write(pat(90000, 3))
    sh(XDF, p, "write", h, "big.bin").check_returncode()
    open(h, "wb").write(b"small file\n")
    sh(XDF, p, "makedir", "Mapp", "+", "write", h, "Mapp/small.txt", "+", "protect", "Mapp/small.txt", "rd").check_returncode()
    return p


def test_read():
    """what other tools made, every type"""
    if XDF:
        for form, dos, nm in (("ofs", 0, "dos0.adf"), ("ffs", 1, "dos1.adf"), ("ofs+intl", 2, "dos2.adf"), ("ffs+intl", 3, "dos3.adf"),
                              ("ofs+dc", 4, "dos4.adf"), ("ffs+dc", 5, "dos5.adf"), ("ofs+ln", 6, "dos6.adf"), ("ffs+ln", 7, "dos7.adf")):
            p = xdf(nm, form)
            check(f"DOS{dos}: xdftool made what the test wants", open(p, "rb").read(4) == b"DOS" + bytes([dos]))
            out = os.path.join(T, "o.bin")
            _, o = run(p, f"mount\nls /\nls /Mapp\nopen /BIG.BIN r\ndump %1 {out}\nfstat %1\nclose %1\nopen /mapp/small.txt r\nread %2 0 100\nstat /mapp/small.txt\nstatfs\numount\n")
            check(f"DOS{dos}: mounts", "mount -> 0 readonly" in o, o)
            check(f"DOS{dos}: lists", "big.bin" in o and "Mapp" in o and "small.txt" in o, o)
            check(f"DOS{dos}: a file with extension blocks, byte for byte", open(out, "rb").read() == pat(90000, 3) and "size 90000" in o, o)
            check(f"DOS{dos}: a small file, a protected one (r and d cleared: -wed... set)", "small file" in o, o)
            check(f"DOS{dos}: statfs answers", re.search(r"statfs -> 0 bsize 1024 blocks 879 bfree \d+", o), o)
            check(f"DOS{dos}: no error", not errors(o), o)
    else:
        print("SKIP no xdftool: other tools' images are not tried")
    if XDF:
        p = xdf("large.hdf", "ffs", kb=70 * 1024)
        d = Disk(p)
        check("70 MB volume: a bitmap extension block is there", d.L(d.root, d.longs - 24) != 0)
        _, o = run(p, "mount\nstatfs\nls /\numount\n")
        check("70 MB volume: read-only, free blocks counted through the extension block", re.search(r"statfs -> 0 bsize 1024 blocks 71679 bfree 71[0-9]+", o) and "big.bin" in o, o)
    # own images: every field
    for dos in range(4):
        for bs in (512, 1024, 4096):
            p = mkvol(f"own{dos}.img", dos, bs)
            out = os.path.join(T, "o.bin")
            _, o = run(p, f"mount\nls /\nls /mapp/SUB\nopen /big.bin r\ndump %1 {out}\nclose %1\nopen /plain.txt r\nread %2 999 10\nread %2 1000 10\nclose %2\n"
                          f"open /empty r\nread %3 0 10\nfstat %3\nclose %3\nstat /Mapp/sub/lower.txt\nstat /Mapp/sub/..\nstat /no/such\nstat /plain.txt/x\nchdir /Mapp/sub\numount\n")
            label = f"DOS{dos} {bs} bytes"
            check(f"{label}: reads", open(out, "rb").read() == pat(90000, 3) and "read -> 1 " in o and "size 0" in o, o)
            check(f"{label}: the date", f"mtime {EPOCH}" in o, o)
            check(f"{label}: Latin-1 names as UTF-8", "räksmörgås åäö.txt" in o, o)
            check(f"{label}: errors for what is not there", "stat -> -2" in o and "stat -> -20" in o, o)
    # case: as the type says
    p = mkvol("c3.img", 3)
    _, o = run(p, "mount\nopen /RÄKSMÖRGÅS+ÅÄÖ.TXT r\nread %1 0 100\n")
    check("international: Å and å are one letter", "swedish" in o, o)
    p = mkvol("c1.img", 1)
    _, o = run(p, "mount\nopen /räksmörgås+åäö.txt r\nread %1 0 100\nopen /RäKSMöRGåS+åäö.TXT r\nread %2 0 100\nopen /RÄKSMÖRGÅS+ÅÄÖ.TXT r\n")
    check("not international: only A-Z fold", "swedish" in o and "open -> -2" in o, o)
    # a volume with other reserved blocks is said with an option; a block size is found by itself
    p = mkvol("res.img", 3, 512, reserved=4)
    _, o = run(p, "mount reserved=4\nopen /plain.txt r\nread %1 0 5\nstatfs\n")
    check("reserved=4 by option", "read -> 5" in o and re.search(r"blocks 1022 ", o), o)
    # protection bits
    def prot(v):
        for n, bits in (("all", 0), ("nor", 8), ("now", 4), ("noe", 2), ("nod", 1), ("none", 15), ("script", 0x40)):
            v.add_file(n, b"x", bits)
    p = mkvol("prot.img", 3, tree=prot)
    _, o = run(p, "mount\nstat /all\nstat /nor\nstat /now\nstat /noe\nstat /nod\nstat /none\nstat /script\nas 5 5\nopen /nor r\nopen /now w\nopen /all r\n")
    modes = re.findall(r"mode (\d+)", o)
    check("modes from r, w, e", modes == ["100755", "100311", "100555", "100644", "100755", "100000", "100755"], o)
    check("... obeyed", "open -> -13" in o, o)
    _, o = run(p, "mount fmode=640,dmode=750,uid=7,gid=9\nstat /all\nstat /\n")
    check("fmode, dmode, uid, gid", "mode 100640" in o and "mode 40750" in o and "uid 7 gid 9" in o, o)
    # links
    def links(v):
        h = v.add_file("Dir/orig.txt", b"the data\n")
        v.hard_link("Other/hard.txt", h)
        v.hard_link("hard2", h)
        d = v.mkdir("Dir/Sub")
        v.add_file("Dir/Sub/inner", b"inner\n")
        v.hard_link("dirlink", d)
        v.soft_link("Dir/s-plain", b"orig.txt")
        v.soft_link("Dir/s-up", b"/Other/hard.txt")
        v.soft_link("Dir/Sub/s-up2", b"//hard2")
        v.soft_link("Dir/Sub/s-vol", b"Testvol:Dir/orig.txt")
        v.soft_link("Dir/Sub/s-colon", b":hard2")
        v.soft_link("s-other", b"Work:foo/bar")
        v.soft_link("Dir/s-mid", b"Sub//orig.txt")
        v.soft_link("s-self", b"s-self")
        v.soft_link("s-l\xe5ng", b"Dir/orig.txt")
    p = mkvol("links.img", 3, tree=links)
    _, o = run(p, "mount\nstat /hard2\nstat /Other/hard.txt\nstat /Dir/orig.txt\nopen /hard2 r\nread %1 0 100\nstat /dirlink/inner\nstat /dirlink/..\nls /dirlink\n")
    inos = re.findall(r"ino (\d+) mode 100\d+ size 9 links (\d)", o)
    check("hard links: one number, the count of three", len(inos) == 3 and len(set(inos)) == 1 and inos[0][1] == "3", o)
    check("... read, a link to a directory is walked", "the data" in o and "ls -> " in o and "inner" in o, o)
    _, o = run(p, "mount\nreadlink /Dir/s-plain\nreadlink /Dir/s-up\nreadlink /Dir/Sub/s-up2\nreadlink /Dir/Sub/s-vol\nreadlink /Dir/Sub/s-colon\nreadlink /s-other\nreadlink /Dir/s-mid\nreadlink /s-lång\nreadlink /hard2\nlstat /Dir/s-plain\nstat /Dir/s-plain\nstat /s-other\nstat /s-self\n"
                  "open /Dir/s-up r\nread %1 0 100\nopen /Dir/Sub/s-up2 r\nread %2 0 100\nopen /Dir/Sub/s-vol r\nread %3 0 100\nopen /Dir/Sub/s-colon r\nread %4 0 100\nls /Dir\n")
    for w in ("readlink -> 8 orig.txt", "readlink -> 17 ../Other/hard.txt", "readlink -> 11 ../../hard2", "readlink -> 18 ../../Dir/orig.txt", "readlink -> 11 ../../hard2",
              "readlink -> 12 Work:foo/bar", "readlink -> 15 Sub/../orig.txt", "readlink -> -22", "mode 120777", "stat -> -2", "stat -> -40"):
        check(f"soft links: {w}", w in o, o)
    check("soft links followed in the volume", o.count("read -> 9 the data") == 4, o)
    # D26: the kernel follows
    _, o = run(p, "mount\nlinks on\nstat /Dir/s-plain\nstat /Dir/Sub/s-up2\nopen /Dir/s-up r\nlstat /Dir/s-plain\nstat /Dir/s-up/x\nstat /s-other\n")
    check("D26: the path continues at TEXT", "stat -> -1024 a0 Dir/orig.txt" in o and "stat -> -1024 a0 Dir/Sub/../../hard2" in o and "open -> -1026 a0 Dir/../Other/hard.txt" in o, o)
    check("... the last name of lstat is not followed", "lstat -> 0" in o, o)
    # reading writes nothing
    w0 = open(p, "rb").read()
    _, o = run(p, "mount\nls /\nopen /Dir/orig.txt r\nread %1 0 9\nclose %1\nstat /hard2\nstatfs\nsync\numount\n", env={"HOSTDISK_COUNT": "1"})
    check("a read-only mount writes nothing", "hostdisk-writes 0" in o and open(p, "rb").read() == w0, o)
    _, o = run(p, "mount\nopen /Dir/orig.txt w\nopen /n wc\nmkdir /m\nunlink /hard2\nrename /hard2 /x\nchmod /hard2 444\nutimes /hard2 5\nsymlink a /z\n")
    check("read-only is the default: every change answers EROFS", o.count("-> -30") == 8, o)
    # long names
    if XDF:
        p = os.path.join(T, "ln.adf")
        if os.path.exists(p):
            os.remove(p)
        sh(XDF, p, "create", "+", "format", "Testvol", "ffs+ln").check_returncode()
        h = os.path.join(T, "x.tmp")
        open(h, "wb").write(b"long name file\n")
        nm40, nm70 = "a_name_with_more_than_thirty_characters_in_it", "n" * 70
        sh(XDF, p, "write", h, nm40, "+", "write", h, nm70).check_returncode()
        d = Disk(p).walk()
        key = [v[2] for k, v in d.files.items() if len(k) == 71][0]
        _, o = run(p, "mount\nls /\n")
        m = re.search(r"\b(n+~[0-9a-f]+)\b", o)
        alias = m.group(1) if m else "none"
        check("long names: over 63 bytes listed as its start and number", alias.endswith(f"~{key:x}") and len(alias) <= 63, o)
        _, o = run(p, f"mount rw\nls /\nopen /{nm40.upper()} r\nread %1 0 100\nopen /{alias} r\nread %2 0 100\nopen /x wc\n")
        check("long names: read-only even asked for rw", "mount -> 0 readonly" in o and "never written" in o and "open -> -30" in o, o)
        check("long names: over 30 characters found in any case; the listed form opens", o.count("read -> 15 long name file") == 2, o)


def test_refusals():
    p = mkvol("rf.img", 3)
    _, o = run(p, "mount rw\nlink /Mapp /g\nchmod /plain.txt 777\nchmod /plain.txt 600\nchmod /plain.txt 444\nstat /plain.txt\nunlink /plain.txt\nchmod /plain.txt 755\nunlink /plain.txt\n"
                  "chown /big.bin 5 5\nchown /big.bin 0 0\nreadlink /big.bin\nmkdir /x:y\nopen /a:b wc\nopen /kanji-日本 wc\nopen /" + "n" * 31 + " wc\nrmdir /nope\nrmdir /big.bin\nunlink /nope\nmkdir /d\nopen /d w\nunlink /d\n"
                  "open /d/f wc\nclose %1\nrmdir /d\nrename /d /d/e\nmkdir /big.bin\nopen /big.bin wcx\nsymlink /etc/x /abs\nsymlink ../a/b /rel\nreadlink /rel\nsymlink a /rel\nas 5 5\nopen /big.bin w\nopen /new wc\nchmod /big.bin 700\nunlink /big.bin\numount\n")
    want = ["link -> -1", "chmod -> -1", "chown -> -1", "readlink -> -22", "mkdir -> -22", "open -> -22", "open -> -84", "open -> -36",
            "open -> -13", "unlink -> -13", "rmdir -> -39", "rename -> -22", "mkdir -> -17", "open -> -17", "unlink -> -21", "open -> -21",
            "symlink -> -22", "readlink -> 6 ../a/b", "symlink -> -17"]
    for w in want:
        check(f"refuses clearly: {w}", w in o, o)
    check("chmod a-w: Amiga's read-only, also delete protected", "mode 100444" in o and "unlink -> -1" in o, o)
    check("chmod back: it goes", o.count("stat -> -2") <= 2, o)
    d = Disk(p).walk()
    check("after the refusals the volume is whole", d.walked and not d.bad, d.bad)
    # a file open while its name goes
    _, o = run(p, "mount rw\nopen /keep.bin wc\nwrite %1 0 @5000\nunlink /keep.bin\nread %1 4000 5\nfstat %1\nopen /other wc\nwrite %2 0 abc\nclose %2\nread %1 0 5\nclose %1\nstat /keep.bin\nsync\numount\n")
    check("a removed open file stays readable", o.count("read -> 5") == 2 and "links 0" in o and "stat -> -2" in o, o)
    d = Disk(p).walk()
    check("... and its blocks go at the last close", d.walked and not d.bad and d.leak == 0, str(d.bad) + str(d.leak))
    _, o = run(p, "mount rw\nopen /A wc\nwrite %1 0 aaaa\nopen /B wc\nwrite %2 0 bbbbbb\nrename /A /B\nread %2 0 6\nread %1 0 4\nfstat %1\nclose %1\nclose %2\nstat /A\nopen /B r\nread %3 0 10\nsync\numount\n")
    check("rename over an open file: both handles keep their data", "read -> 6 bbbbbb" in o and "read -> 4 aaaa" in o and "stat -> -2" in o, o)
    d = Disk(p).walk()
    check("... the new name has the renamed file", d.read(b"/B") == b"aaaa" and not d.bad and d.leak == 0, str(d.bad) + str(d.leak))


def test_write():
    """a workload of changes on every type written; judged by the walker, xdfscan, xdftool"""
    for dos, bs in ((3, 512), (1, 512), (0, 512), (2, 512)):
        label = f"DOS{dos} {bs} bytes"
        p = mkvol(f"w{dos}{bs}.img", dos, bs, kb=2048)
        s = ("mount rw\nmkdir /Nya\nopen /Nya/räksmörgås+på+svenska.txt wc\nwrite %1 0 hej\nclose %1\n"
             "open /big2.bin wc\nwrite %2 0 @300000\nwrite %2 700000 tail\nclose %2\n"
             "open /Nya/UPPER.TXT wc\nwrite %3 0 upper\nclose %3\n"
             "rename /plain.txt /Nya/renamed+file.txt\nunlink /Mapp/sub/lower.txt\nrmdir /Mapp/sub\nmkdir /Tom\nmkdir /Tom/under\n"
             "rename /Tom/under /upp\nstat /upp/..\nsync\nls /\nls /Nya\nsymlink ../big2.bin /Nya/lnk\nreadlink /Nya/lnk\nopen /Nya/lnk r\nread %4 700000 4\nclose %4\nchmod /Nya/UPPER.TXT 644\nutimes /Nya/UPPER.TXT 1600000000\numount\n")
        _, o = run(p, s)
        check(f"{label}: a workload of writes answers no error", not errors(o), errors(o))
        d = Disk(p, bs).walk()
        check(f"{label}: the walker finds nothing, no lost block", d.walked and not d.bad and d.leak == 0, str(d.bad) + str(d.leak))
        ok, so = scan_ok(p)
        check(f"{label}: xdfscan finds nothing", ok, so)
        check(f"{label}: a Swedish name", d.read("/Nya/räksmörgås på svenska.txt".encode("latin-1")) == b"hej", list(d.files))
        big = d.read(b"/big2.bin")
        check(f"{label}: a big file, a gap of zeros, the tail", big is not None and big[:300000] == patn(300000) and big[300000:700000] == bytes(400000) and big[700000:] == b"tail", str(len(big or b"")))
        check(f"{label}: a renamed file keeps its data", d.read(b"/Nya/renamed file.txt") == pat(1000))
        check(f"{label}: removed things are gone", b"/Mapp/sub/lower.txt" not in d.files and b"/Mapp/sub" not in d.files)
        check(f"{label}: a directory moved out", b"/Tom/under" not in d.files)
        check(f"{label}: chmod clears w only as it says", d.L(d.files[b"/Nya/UPPER.TXT"][2], d.longs - 48) == 2, d.L(d.files[b"/Nya/UPPER.TXT"][2], d.longs - 48))
        days = d.L(d.files[b"/Nya/UPPER.TXT"][2], d.longs - 23)
        check(f"{label}: a date", days == (1600000000 - 252460800) // 86400, days)
        if XDF and bs == 512:
            g = os.path.join(T, "got.tmp")
            shutil.copy(p, p + ".hdf")
            q = sh(XDF, p + ".hdf", "read", "big2.bin", g)
            os.remove(p + ".hdf")
            check(f"{label}: xdftool reads it", q.returncode == 0 and open(g, "rb").read() == big, q.stdout[-300:])
        _, o = run(p, "mount rw\nopen /big2.bin rw\nwrite %1 100 XYZ\nread %1 99 5\nclose %1\nopen /big2.bin wt\nwrite %2 0 short\nclose %2\nstat /big2.bin\n"
                      "rename /Nya/UPPER.TXT /Nya/renamed+file.txt\nstat /Nya/UPPER.TXT\nrename /Nya/renamed+file.txt /Nya/RENAMED+FILE.TXT\nstat /Nya/renamed+file.txt\nrename /upp /Tom\nstatfs\numount\n")
        check(f"{label}: overwrites, truncates, renames over a file and an empty directory, a case change", "read -> 5 hXYZl" in o and "size 5" in o and "stat -> -2" in o and not errors(o.replace("stat -> -2", "")), o)
        d = Disk(p, bs).walk()
        check(f"{label}: still whole", d.walked and not d.bad and d.leak == 0, str(d.bad) + str(d.leak))
        check(f"{label}: the case change", b"/Nya/RENAMED FILE.TXT" in d.files and d.read(b"/Nya/RENAMED FILE.TXT") == b"upper", list(d.files))
        check(f"{label}: a truncated file's blocks are free again", d.read(b"/big2.bin") == b"short")
        ok, so = scan_ok(p)
        check(f"{label}: xdfscan again", ok, so)


def test_limits():
    # blocks over 512 bytes are read-only
    p = mkvol("bk.img", 3, 1024)
    _, o = run(p, "mount rw\nopen /x wc\nopen /plain.txt r\nread %1 0 5\n")
    check("1 KB blocks: read-only even asked for rw", "never written" in o and "open -> -30" in o and "read -> 5" in o, o)
    # ln, and a rename on a full disk
    p = mkvol("ln.img", 3)
    _, o = run(p, "mount rw\nlink /plain.txt /Mapp/andra\nstat /Mapp/andra\nlink /plain.txt /Mapp/andra\nunlink /plain.txt\nstat /Mapp/andra\nsync\numount\n")
    d = Disk(p).walk()
    check("ln: two names, the first goes, the file stays", "links 2" in o and "links 1" in o and "link -> -17" in o and d.read(b"/Mapp/andra") == pat(1000) and not d.bad and d.leak == 0, o + str(d.bad))
    # a program that removes what it lists misses nothing
    p = mkvol("rmall.img", 3, tree=lambda v: [v.add_file(f"d/f{i}", b"x") for i in range(178)])
    _, o = run(p, "mount rw\nrmall /d\nrmdir /d\nsync\numount\n")
    check("a listing while its names are removed misses none", "rmall -> 178" in o and "rmdir -> 0" in o, o)
    d = Disk(p).walk()
    check("... and the volume is whole", d.walked and not d.bad and d.leak == 0, str(d.bad) + str(d.leak))
    # many names: chains grow, stay in order
    p = mkvol("many.img", 3, kb=4096)
    names = [f"file number {i:04}.txt" for i in range(600)]
    s = "mount rw\nmkdir /d\n" + "".join(f"open /d/{n.replace(' ', '+')} wc\nwrite %{i + 1} 0 {i}\nclose %{i + 1}\n" for i, n in enumerate(names)) + "ls /d\nsync\numount\n"
    _, o = run(p, s)
    check("600 names in a directory, listed", "(600, 0)" in o, o[-300:])
    d = Disk(p).walk()
    check("... the chains in block order, the walker agrees", d.walked and not d.bad, d.bad)
    check("... a read back", d.read(b"/d/file number 0377.txt") == b"377")
    ok, so = scan_ok(p)
    check("... xdfscan", ok, so)
    s = "mount rw\n" + "".join(f"unlink /d/{n.replace(' ', '+')}\n" for n in names[:300]) + "".join(f"open /d/new{i} wc\nclose %{i + 1}\n" for i in range(50)) + "ls /d\nsync\numount\n"
    _, o = run(p, s)
    d = Disk(p).walk()
    check("half removed, others added", "(350, 0)" in o and d.walked and not d.bad and d.leak == 0, o[-200:] + str(d.bad))
    # the disk full and back, OFS and FFS
    for dos in (1, 0):
        p = mkvol("full.img", dos, kb=1024)
        s = "mount rw\nstatfs\nopen /full wc\n" + "".join(f"write %1 {i * 300000} @300000\n" for i in range(5)) + "fstat %1\nclose %1\nmkdir /d\nsync\nstatfs\nunlink /full\nsync\nstatfs\nmkdir /d\nsync\numount\n"
        _, o = run(p, s)
        check(f"DOS{dos}: the disk full: a short write, then ENOSPC", re.search(r"write -> [0-9]{1,6}\n", o) and "mkdir -> -28" in o, o)
        fr = re.findall(r"bfree (\d+)", o)
        check(f"DOS{dos}: statfs: free space falls and comes back", len(fr) == 3 and int(fr[1]) == 0 and fr[2] == fr[0], str(fr) + o)
        d = Disk(p).walk()
        check(f"DOS{dos}: after it the volume is whole", d.walked and not d.bad and d.leak == 0, str(d.bad) + str(d.leak))
    # a file past the root's bitmap: a volume with a bitmap extension block, written
    p = mkvol("bmx.img", 3, kb=70 * 1024, tree=lambda v: v.add_file("a", b"x"))
    d = Disk(p)
    check("own 70 MB image has a bitmap extension block", d.L(d.root, d.longs - 24) != 0)
    _, o = run(p, "mount rw\nstatfs\nopen /b wc\nwrite %1 0 @400000\nclose %1\nsync\nstatfs\numount\n")
    d = Disk(p).walk()
    check("... written, the free count right", d.walked and not d.bad and d.leak == 0 and d.read(b"/b") == patn(400000) and not errors(o), o + str(d.bad))
    # a volume full: a rename across chains needs one block
    p = mkvol("rfull.img", 1, kb=128, tree=lambda v: (v.add_file("a", b"x"), v.add_file("fill", bytes(1))))
    _, o = run(p, "mount rw\nopen /fill w\nwrite %1 0 @200000\nclose %1\nrename /a /zzzzz\nstat /a\nsync\numount\n")
    d = Disk(p).walk()
    check("a full disk: a rename answers ENOSPC, nothing lost", "rename -> -28" in o and d.read(b"/a") == b"x" and not d.bad, o + str(d.bad))
    # the walks: hard links removed and promoted
    def links(v):
        h = v.add_file("orig", b"the data\n")
        v.hard_link("one", h)
        v.hard_link("Dir/two", h)
        v.add_file("single", b"s")
        d = v.mkdir("D2")
        v.hard_link("dl", d)
    p = mkvol("hl.img", 3, tree=links)
    d0 = Disk(p).walk()
    _, o = run(p, "mount rw\nstat /orig\nunlink /Dir/two\nstat /orig\nopen /orig r\nunlink /orig\nread %1 0 100\nstat /one\nstat /orig\nclose %1\nunlink /one\nstat /one\nrmdir /dl\nsync\numount\n")
    check("hard links: a link goes, the count falls", "links 3" in o and "links 2" in o, o)
    check("the original goes: the link has the file, open handle too", o.count("read -> 9 the data") == 1 and "links 1" in o and o.count("stat -> -2") == 2, o)
    d = Disk(p).walk()
    check("... the last name goes: blocks back", d.walked and not d.bad and d.leak == 0 and b"/one" not in d.files and b"/single" in d.files, str(d.bad) + str(d.leak) + str(list(d.files)))
    # a block size the table says; a wrong one does not mount
    p = mkvol("bs.img", 3, 4096)
    _, o = run(p, "mount blocksize=512\n")
    check("the wrong block size: refused", "mount -> -1" in o and "no root block" in o, o)
    _, o = run(p, "mount blocksize=777\n")
    check("an impossible block size: refused", "mount -> -1" in o, o)
    # not a volume
    p = os.path.join(T, "zero.img")
    open(p, "wb").write(bytes(1 << 20))
    _, o = run(p, "mount\n")
    check("no volume: refused with a reason", "mount -> -1" in o and "not an AmigaDOS volume" in o, o)
    _, o = run(p, "mount nonsense=1\n")
    check("a bad option", "bad option" in o, o)


def set_long(d, bs, b, i, v, fix=True):
    """a long of block b, and the block's checksum made right (so it is the logic that meets it)"""
    struct.pack_into(">I", d, b * bs + 4 * i, v & 0xFFFFFFFF)
    if fix:
        struct.pack_into(">I", d, b * bs + 20, 0)
        s = sum(struct.unpack_from(f">{bs // 4}I", d, b * bs)) & 0xFFFFFFFF
        struct.pack_into(">I", d, b * bs + 20, -s & 0xFFFFFFFF)


def test_check():
    """the check before -o rw"""
    def tree(v):
        h = v.add_file("a", pat(100000))
        v.add_file("b", pat(5000, 2))
        v.hard_link("hl", h)
    good = mkvol("ck.img", 1, tree=tree)
    base = open(good, "rb").read()
    d0 = Disk(base).walk()
    root = d0.root
    fa = d0.files[b"/a"][2]
    fb = d0.files[b"/b"][2]
    L = d0.longs

    def trial(label, change, expect="mounted read-only"):
        d = bytearray(base)
        change(d)
        open(good, "wb").write(d)
        _, o = run(good, "mount rw\nopen /x wc\nstat /b\numount\n")
        check(f"-o rw: {label}", expect in o and ("mount -> 0 readonly" in o) and "open -> -30" in o, o)
        check(f"... {label}: nothing written", open(good, "rb").read() == bytes(d))

    trial("bitmap marked invalid", lambda d: set_long(d, 512, root, L - 50, 0), "marked invalid")
    trial("a data block of two files", lambda d: set_long(d, 512, fb, 6 + 72 - 1, d0.files[b"/a"][0][5]), "named twice")
    trial("a block in use marked free", lambda d: (lambda p, k: (struct.pack_into(">I", d, p * 512 + 4 + 4 * (k // 32), struct.unpack_from(">I", d, p * 512 + 4 + 4 * (k // 32))[0] | 1 << (k % 32)),
                                                                 set_bm_sum(d, p)))(d0.pages[0], d0.files[b"/a"][0][0] - 2), "marked free")
    trial("a loop in an extension chain", lambda d: set_long(d, 512, d0_exts(d0, fa)[1], L - 2, d0_exts(d0, fa)[0]), "damaged")
    trial("a hash chain into itself", lambda d: set_long(d, 512, fb, L - 4, fb), "named twice")
    trial("a bad checksum in a header", lambda d: set_long(d, 512, fb, L - 47, 9, fix=False), "damaged")
    trial("a hard link its file does not know", lambda d: set_long(d, 512, fa, L - 10, 0), "does not know")
    trial("a block missing from a file", lambda d: set_long(d, 512, fb, L - 47, 1 << 20), "fewer blocks")
    open(good, "wb").write(base)
    # lost blocks come back
    d = bytearray(base)
    free_b = next(b for b in range(d0.root + 5, d0.n) if d0.bit_free(b))
    k = free_b - 2
    w = struct.unpack_from(">I", d, d0.pages[0] + 0)[0]
    p = d0.pages[0] * 512 + 4 + 4 * (k // 32)
    struct.pack_into(">I", d, p, struct.unpack_from(">I", d, p)[0] & ~(1 << (k % 32)))
    set_bm_sum(d, d0.pages[0])
    open(good, "wb").write(d)
    check("a lost block: the walker sees it", Disk(good).walk().leak == 1)
    w0 = open(good, "rb").read()
    _, o = run(good, "mount\nstatfs\numount\n", env={"HOSTDISK_COUNT": "1"})
    check("read-only leaves it", "hostdisk-writes 0" in o and open(good, "rb").read() == w0, o)
    _, o = run(good, "mount rw\nstatfs\numount\n")
    dd = Disk(good).walk()
    check("-o rw gives it back", "1 lost blocks given back" in o and dd.walked and dd.leak == 0, o + str(dd.leak))
    # a clean volume mounted rw and not changed writes nothing
    open(good, "wb").write(base)
    _, o = run(good, "mount rw\nls /\nstatfs\nopen /a r\nread %1 0 100\nclose %1\nsync\numount\n", env={"HOSTDISK_COUNT": "1"})
    check("-o rw and no change: not a block written", "hostdisk-writes 0" in o and "mount -> 0\n" in o and open(good, "rb").read() == base, o)
    # dircache: read-only by name
    if XDF:
        p = xdf("dc.adf", "ffs+dc")
        _, o = run(p, "mount rw\nopen /x wc\n")
        check("a directory cache volume is never written", "never written" in o and "open -> -30" in o, o)


def set_bm_sum(d, p):
    struct.pack_into(">I", d, p * 512, 0)
    s = sum(struct.unpack_from(">128I", d, p * 512)) & 0xFFFFFFFF
    struct.pack_into(">I", d, p * 512, -s & 0xFFFFFFFF)


def d0_exts(disk, h):
    out = []
    c = disk.L(h, disk.longs - 2)
    while c:
        out.append(c)
        c = disk.L(c, disk.longs - 2)
    return out


def test_cuts():
    """a power cut after every sector written: the volume stays valid, others whole, the next -o rw mount works"""
    for label, dos, bs in (("FFS", 3, 512), ("OFS", 2, 512)):
        def tree(v):
            for i in range(6):
                v.add_file(f"OTHER{i}.BIN", pat(3000 + 900 * i, i))
            v.add_file("keep/kept with long name.txt", pat(1500, 9))
            v.add_file("old.bin", pat(4000, 5))
            v.add_file("zap.bin", pat(3000, 7))
            h = v.add_file("hl.bin", pat(2000, 8))
            v.hard_link("keep/hl2", h)
            h2 = v.add_file("hl3.bin", pat(1200, 11))
            v.hard_link("keep/hl4", h2)
            for i in range(5):
                v.add_file(f"tree/sub/f{i}", pat(700 + i, i))
        base = mkvol("base.img", dos, bs, kb=1024, tree=tree)
        d0 = Disk(base, bs).walk()
        others = {k: d0.read(k) for k in d0.files if b"OTHER" in k and b"OTHER5" not in k or b"kept" in k}
        s = ("mount rw\nopen /new.bin wc\nwrite %1 0 @6000\nclose %1\nmkdir /nydir\nopen /nydir/en+fil+med+långt+namn.txt wc\nwrite %2 0 @700\nclose %2\n"
             "commit\nunlink /old.bin\nrename /new.bin /nydir/moved.bin\nopen /app.bin wc\nwrite %3 0 @3000\nwrite %3 3000 @60000\nclose %3\n"
             "commit\nopen /nydir/en+fil+med+långt+namn.txt wt\nwrite %4 0 @100\nclose %4\nrename /app.bin /OTHER5.BIN\nunlink /hl.bin\nunlink /keep/hl2\nopen /zap.bin w\nwrite %5 0 ZZZZ\nclose %5\nsymlink ../zap.bin /nydir/sl\nrename /tree /nydir/moved+tree\nrename /hl3.bin /nydir/hl5\nlink /keep/kept+with+long+name.txt /nyl\nrename /nydir/sl /sl2\nrmdir /nope\nsync\numount\n")
        cut = os.path.join(T, "cut.img")
        shutil.copy(base, cut)
        rc, o = run(cut, s, env={"HOSTDISK_COUNT": "1"})
        total = int(re.search(r"hostdisk-writes (\d+)", o).group(1))
        dw = Disk(cut, bs).walk()
        check(f"{label}: the workload runs whole ({total} sectors)", rc == 0 and dw.walked and not dw.bad and dw.leak == 0 and len(errors(o)) == 1, o[-600:] + str(dw.bad) + str(dw.leak))
        bad_struct = bad_other = bad_old = bad_remount = bad_scan = bad_lost = leaked = 0
        first = ""
        step = 1 if total < 300 else 2
        spb = bs // 512
        for k in range(0, total, step):
            shutil.copy(base, cut)
            rc, o = run(cut, s, env={"HOSTDISK_CUT": str(k)})
            d = Disk(cut, bs).walk()
            if d.bad:
                bad_struct += 1
                first = first or f"cut {k}: {d.bad}"
                continue
            leaked += d.leak > 0
            for n, data in others.items():
                if d.read(n) != data:
                    bad_other += 1
                    first = first or f"cut {k}: {n}"
            r5 = d.read(b"/OTHER5.BIN")
            if r5 not in (d0.read(b"/OTHER5.BIN"), patn(3000) * 1 + patn(60000)) and r5 is not None:
                bad_other += 1
                first = first or f"cut {k}: OTHER5 {len(r5 or b'')}"
            z = d.read(b"/zap.bin")
            if z is not None and z[4:] != d0.read(b"/zap.bin")[4:]:
                bad_old += 1
                first = first or f"cut {k}: zap"
            for m in (b"/nydir/moved.bin", b"/new.bin"):
                got = d.read(m)
                if got is not None and got != patn(len(got)):
                    bad_old += 1
                    first = first or f"cut {k}: {m} wrong data"
            if k % 6 == 0:
                ok, so = scan_ok(cut)
                if not ok:
                    bad_scan += 1
                    first = first or f"cut {k}: xdfscan {so}"
            # the next mount: always writable; what a cut left half done is finished
            _, o2 = run(cut, "mount rw\nopen /after.txt wc\nwrite %1 0 ok\nclose %1\nls /\nsync\numount\n", env={})
            if "readonly" in o2:
                bad_remount += 1
                first = first or f"cut {k}: remount: {o2[:300]}"
            else:
                d2 = Disk(cut, bs).walk()
                if d2.bad or d2.leak or d2.read(b"/after.txt") != b"ok":
                    bad_remount += 1
                    first = first or f"cut {k}: remount {d2.bad} leak {d2.leak}"
                # nothing that was being moved is lost: the tree and the linked file, under one name or the other
                sub = [d2.read(pre + b"/sub/f3") for pre in (b"/tree", b"/nydir/moved tree")]
                if pat(703, 3) not in sub:
                    bad_lost += 1
                    first = first or f"cut {k}: the tree is gone: {sorted(d2.files)}"
                if pat(1200, 11) not in (d2.read(b"/hl3.bin"), d2.read(b"/nydir/hl5")):
                    bad_lost += 1
                    first = first or f"cut {k}: the linked file is gone"
        check(f"{label}: valid after a cut at any of {total} sectors", bad_struct == 0, first)
        check(f"{label}: other files whole", bad_other == 0, first)
        check(f"{label}: files being changed have good data or none", bad_old == 0, first)
        check(f"{label}: xdfscan agrees (every 6th cut)", bad_scan == 0, first)
        check(f"{label}: the next -o rw mount works, gives lost blocks back, the walker agrees", bad_remount == 0, first)
        check(f"{label}: a directory and a file being renamed are never lost", bad_lost == 0, first)
        print(f"  ({leaked} of the cuts left lost blocks)")


def damage(d, base, bs, kind, rnd):
    """one kind of damage aimed at a walk, the checksum made right so the logic meets it. Kinds 0-1 the root and bitmap"""
    L = base.longs
    fs = {k: v for k, v in base.files.items() if v[1] is not None and v[1] > 0}
    big = max(fs.values(), key=lambda v: v[1])
    hdrs = [v[2] for v in base.files.values()]
    dirs = [b for b, w in base.named.items() if w.startswith("header") and base.L(b, L - 1) == 2]
    exts = d0_exts(base, big[2])
    wild = lambda: rnd.choice((0, 1, 2, base.root, base.n - 1, base.n, base.n + 5, 0x7FFFFFFF, 0xFFFFFFFF, rnd.randrange(base.n), rnd.choice(hdrs)))
    r = base.root
    if kind == 0:                                          # the root's fields
        at = rnd.choice((0, 1, 2, 3, 4, L - 50, L - 49, L - 48, L - 24, L - 20, L - 4, L - 3, L - 2, L - 1, 6 + rnd.randrange(base.ht)))
        set_long(d, bs, r, at, wild())
    elif kind == 1:                                        # the bitmap: pointers, checksums, bits, an extension
        w = rnd.randrange(4)
        if w == 0:
            set_long(d, bs, r, L - 49 + rnd.randrange(3), wild())
        elif w == 1:
            set_long(d, bs, r, L - 24, wild())
        elif w == 2:
            p = base.pages[0]
            struct.pack_into(">I", d, p * bs + 4 + 4 * rnd.randrange(L - 1), rnd.choice((0, 0xFFFFFFFF, rnd.randrange(1 << 32))))
        else:
            set_long(d, bs, base.pages[0], rnd.randrange(L), wild(), fix=False)
    elif kind == 2:                                        # a loop in a hash chain, to itself, to the head, to a directory
        e = rnd.choice(hdrs)
        set_long(d, bs, e, L - 4, rnd.choice((e, hdrs[0], rnd.choice(hdrs), rnd.choice(dirs + [r]), wild())))
    elif kind == 3:                                        # a hash slot
        t = rnd.choice(dirs + [r])
        set_long(d, bs, t, 6 + rnd.randrange(base.ht), rnd.choice((t, r, rnd.choice(hdrs), wild())))
    elif kind == 4:                                        # extension chain: loops, wrong blocks, a wrong owner, counts
        if exts:
            e = rnd.choice(exts)
            w = rnd.randrange(4)
            if w == 0:
                set_long(d, bs, e, L - 2, rnd.choice((e, exts[0], big[2], wild())))
            elif w == 1:
                set_long(d, bs, e, L - 3, wild())
            elif w == 2:
                set_long(d, bs, e, 2, rnd.choice((0, 1, base.ht, base.ht + 1, 0xFFFFFFFF)))
            else:
                set_long(d, bs, e, 0, rnd.choice((2, 8, 0)))
        else:
            set_long(d, bs, big[2], L - 2, wild())
    elif kind == 5:                                        # a file header: size, count, pointers, next extension
        h = rnd.choice(list(fs.values()))[2]
        w = rnd.randrange(5)
        if w == 0:
            set_long(d, bs, h, L - 47, rnd.choice((0, 1, 1 << 20, 1 << 31, 0xFFFFFFFF, 0x7FFFFFFF)))
        elif w == 1:
            set_long(d, bs, h, 2, rnd.choice((0, 1, base.ht, base.ht + 1, 0xFFFFFFFF)))
        elif w == 2:
            set_long(d, bs, h, 6 + rnd.randrange(base.ht), wild())
        elif w == 3:
            set_long(d, bs, h, L - 2, wild())
        else:
            set_long(d, bs, h, L - 1, rnd.choice((0, 1, 2, 3, 4, 5, 0xFFFFFFFD, 0xFFFFFFFC, 0xFFFFFFFB)))
    elif kind == 6:                                        # a name: length, characters; a date
        h = rnd.choice(hdrs)
        p = h * bs + bs - 80
        w = rnd.randrange(3)
        if w == 0:
            d[p] = rnd.choice((0, 1, 30, 31, 255))
        elif w == 1:
            d[p + 1 + rnd.randrange(8)] = rnd.choice((0, 0x2F, 1, 0xFF, 0x80, 0x3A))
        else:
            struct.pack_into(">I", d, h * bs + bs - 92 + rnd.randrange(3) * 4, rnd.choice((0xFFFFFFFF, 1 << 31, 100000)))
        set_long(d, bs, h, L - 3, base.L(h, L - 3))
    elif kind == 7:                                        # links: targets, lists, soft link text
        hl = [v[2] for v in base.files.values() if v[1] is None and base.L(v[2], L - 1) != 3]
        sl = [v[2] for v in base.files.values() if v[1] is None and base.L(v[2], L - 1) == 3]
        w = rnd.randrange(4)
        if w == 0 and hl:
            set_long(d, bs, rnd.choice(hl), L - 11, wild())
        elif w == 1 and hl:
            e = rnd.choice(hl)
            set_long(d, bs, e, L - 10, rnd.choice((e, wild())))
        elif w == 2 and sl:
            e = rnd.choice(sl)
            for i in range(bs - 224):
                d[e * bs + 24 + i] = rnd.choice((0x2F, 0x41, 0x3A, 0xFF))
            set_long(d, bs, e, L - 3, base.L(e, L - 3))
        else:
            set_long(d, bs, rnd.choice(fs.values().__iter__().__next__()[0] and [big[2]] or [big[2]]), L - 10, wild())
    elif kind == 8:                                        # OFS data blocks, or FFS's raw blocks (nothing to check there)
        b = rnd.choice(big[0])
        w = rnd.randrange(5)
        if base.ofs and w < 4:
            set_long(d, bs, b, w, rnd.choice((0, 1, 8, bs, 0xFFFFFFFF, big[2])))
        else:
            d[b * bs + rnd.randrange(bs)] ^= 0xFF
    elif kind == 9:                                        # a directory's header: type, parent, self
        t = rnd.choice(dirs)
        set_long(d, bs, t, rnd.choice((L - 3, L - 1, 1, 0)), rnd.choice((t, r, wild())))
    elif kind == 10:                                       # noise over one header
        h = rnd.choice(hdrs)
        for i in range(rnd.choice((4, 32, bs))):
            d[h * bs + rnd.randrange(bs)] = rnd.randrange(256)
        if rnd.random() < 0.5:
            set_long(d, bs, h, 1, h)
    elif kind == 11:                                       # the volume cut short
        del d[rnd.randrange(512, len(d)):]
    return d


def test_fuzz(rounds):
    """damaged volumes: an error, never a crash or a hang. Each round one kind of damage, three mounts: read-only, -o rw with
    its check, and -o rw past the check (host builds only), so the write code meets the damage too"""
    rnd = random.Random(20261031)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

    def tree(v):
        h = v.add_file("dir one/long file name inside.txt", pat(5000))
        v.add_file("SHORT.TXT", pat(2000, 2))
        v.add_file("räksmörgås.txt".encode("utf-8").decode("utf-8").encode("latin-1").decode("latin-1"), pat(600, 4))
        v.add_file("BIG.BIN", pat(90000, 6))
        v.add_file("dir one/sub/inner", pat(100, 7))
        v.hard_link("hl", h)
        v.hard_link("dl", v.mkdir("dir one/sub"))
        v.soft_link("sl", b"dir one/sub/inner")
        v.soft_link("dir one/sl2", b"/SHORT.TXT")
        for i in range(40):
            v.add_file(f"many/f{i}", b"x")
    bases = [(mkvol("fz3.img", 3, 512, 512, tree=tree), 512), (mkvol("fz0.img", 0, 512, 512, tree=tree), 512), (mkvol("fz1k.img", 2, 1024, 512, tree=tree), 1024), (mkvol("fz4k.img", 1, 4096, 1024, tree=tree), 4096)]
    disks = [(open(p, "rb").read(), Disk(p, bs).walk(), bs) for p, bs in bases]
    ro = ("mount\nstatfs\nls /\nls /dir+one\nls /many\nls /dl\nopen /SHORT.TXT r\nread %1 0 2000\nread %1 1500 400\nfstat %1\nopen /BIG.BIN r\nread %2 0 90000\nread %2 80000 100\n"
          "stat /hl\nopen /hl r\nread %3 0 5000\nstat /sl\nreadlink /sl\nlstat /sl\nopen /sl r\nreadlink /dir+one/sl2\nstat /dir+one/sl2\nstat /dl/inner\nstat /dl/..\nchdir /dir+one\numount\n")
    rw = ("mount %s\nstatfs\nls /\nopen /dir+one/long+file+name+inside.txt rw\nread %%1 0 5000\nwrite %%1 100 xx\nwrite %%1 6000 tail\nclose %%1\nopen /new.bin wc\nwrite %%2 0 @9000\nclose %%2\nmkdir /nd\nopen /nd/f+with+long+name wc\nclose %%3\n"
          "rename /SHORT.TXT /nd/renamed.txt\nrename /nd /nd2\nrename /dir+one /nd2/x\nunlink /räksmörgås.txt\nunlink /hl\nrmdir /nd2/x\nrmdir /dl\nopen /BIG.BIN wt\nwrite %%4 0 again\nclose %%4\nopen /BIG.BIN rw\nwrite %%5 0 @90000\nclose %%5\nunlink /new.bin\nchdir /nd2\nstat /nd2/..\nutimes /nd2 5\nchmod /many/f3 444\nsymlink ../x /many/s\nunlink /many/f1\nunlink /many/f2\nsync\nls /\nstatfs\numount\n")
    KINDS = 12
    crashes = hangs = 0
    per = [[0, 0, 0] for _ in range(KINDS)]                # each kind: refused at mount, answered EIO, neither (read-only run)
    rwro = 0
    first = ""
    done = 0
    slowest = (0, 0, 0)
    ENVS = {"ASAN_OPTIONS": "detect_leaks=0:abort_on_error=0:allocator_may_return_null=1", "UBSAN_OPTIONS": "halt_on_error=1"}
    for rd in range(rounds):
        raw, base, bs = disks[rd % len(disks)]
        kind = rd // len(disks) % KINDS
        d = damage(bytearray(raw), base, bs, kind, rnd)
        if rd % 4 == 3 and kind != 11:
            try:
                d = damage(d, base, bs, rnd.randrange(2, 11), rnd)
            except (ValueError, IndexError, ZeroDivisionError, OverflowError, struct.error):
                pass
        p = os.path.join(T, "fz.img")
        done += 1
        for mode, script in (("ro", ro), ("rw", rw % "rw"), ("nc", rw % ("rw,nocheck"))):
            open(p, "wb").write(d)
            t0 = time.time()
            try:
                rc, o = run(p, script, exe="ffsd-asan", timeout=60, env=ENVS)
            except subprocess.TimeoutExpired:
                hangs += 1
                first = first or f"round {rd} kind {kind} {mode}: hang"
                shutil.copy(p, "/tmp/zenix-ffs-hang.img")
                continue
            el = time.time() - t0
            if el > slowest[0]:
                slowest = (el, rd, kind)
            if rc != 0 or "Sanitizer" in o or "runtime error" in o:
                crashes += 1
                if not first:
                    first = f"round {rd} kind {kind} {mode} bs {bs}: rc {rc}\n" + o[-1800:]
                    shutil.copy(p, "/tmp/zenix-ffs-crash.img")
                continue
            if mode == "ro":
                if "mount -> -1" in o:
                    per[kind][0] += 1
                elif re.search(r"-> -5\b", o):
                    per[kind][1] += 1
                else:
                    per[kind][2] += 1
            elif mode == "rw" and "readonly" in o:
                rwro += 1
        if rd and not rd % 100:
            print(f"fuzz: {rd} rounds, {crashes} crashes, {hangs} hangs", flush=True)
        if not rd % 50 and os.getloadavg()[0] > 16:
            print("load above 16: stopping the fuzz run early", flush=True)
            break
    print(f"fuzz: {done} rounds; per kind (refused, EIO, neither, read-only run): {per}; -o rw fell to read-only {rwro} times; slowest {slowest[0]:.1f} s (round {slowest[1]}, kind {slowest[2]})", flush=True)
    check(f"fuzz: {done} damaged volumes, three mounts each, no crash", crashes == 0, first)
    check("fuzz: no hang", hangs == 0, first)
    ei = sum(k[1] for k in per)
    check(f"fuzz: the damage reached the walks ({ei} volumes answered EIO after mounting)", ei >= done // 6)
    check(f"fuzz: the check caught damage ({rwro} of {done} fell to read-only)", rwro >= done // 3)


def main():
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 440
    only = os.environ.get("FFS_ONLY", "read refusals write limits check cuts fuzz").split()
    if subprocess.run(["gcc"] + CF + ["-o", os.path.join(T, "ffsd")] + SRC).returncode:
        print("FAIL build")
        return 1
    if subprocess.run(["gcc"] + CF + SAN + ["-o", os.path.join(T, "ffsd-asan")] + SRC).returncode:
        print("FAIL build (asan)")
        return 1
    if "read" in only: test_read()
    if "refusals" in only: test_refusals()
    if "write" in only: test_write()
    if "limits" in only: test_limits()
    if "check" in only: test_check()
    if "cuts" in only: test_cuts()
    if "fuzz" in only: test_fuzz(rounds)
    print(f"ffs-host-test: {passes} passed, {fails} failed")
    return 1 if fails else 0


signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))    # the temporary folder goes also when stopped
try:
    rc = main()
finally:
    shutil.rmtree(T, ignore_errors=True)
sys.exit(rc)
