#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""OFS and FFS against AmigaOS itself (DESIGN D31): ffsd (the PC build) writes volumes, Kickstart 3.1's own filesystem in
FS-UAE reads and changes them, the validator is run on them, and ffsd reads and checks the result. Needs the test machines of
this PC (`amiga-test`, `wasabi`, a Workbench 3.1 machine): without them it says SKIP and passes. Emulators only.
usage: tools/ffs-aos-test.py [MACHINE]   (default a1200-31)"""
import hashlib, os, re, shutil, signal, struct, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True
from ffscheck import Disk
from mkffs import Volume

os.environ["SOURCE_DATE_EPOCH"] = "1700000000"
MACHINE = sys.argv[1] if len(sys.argv) > 1 else "a1200-31"
NAME, PORT = "zenixffs", "1298"
CYL = 504 * 1024
T = tempfile.mkdtemp(prefix="zenix-ffsaos.")
fails = passes = 0
started = False


def check(name, ok, extra=""):
    global fails, passes
    if ok:
        passes += 1
        print("PASS", name, flush=True)
    else:
        fails += 1
        print("FAIL", name, str(extra)[:1200], flush=True)


def run(*a, **k):
    return subprocess.run(a, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, **k)


def amiga_start(img):
    """the machine as an instance of its own, with img as a second hard disk"""
    global started
    r = run("amiga-test", MACHINE, "start", "--fresh", "--as", NAME, "--port", PORT, "--config", f"hard_drive_1 = {img}", "--config", "hard_drive_1_type = rdb", timeout=240)
    started = r.returncode == 0
    return started, r.stdout.decode(errors="replace")


def amiga_stop():
    global started
    if started:
        run("amiga-test", MACHINE, "stop", "--as", NAME, timeout=60)
    started = False


def wasabi(*a, timeout=120):
    r = run("wasabi", "--host", "127.0.0.1", "--port", PORT, *a, timeout=timeout)
    return r.returncode, r.stdout.decode("latin-1", errors="replace")


def aget(path):
    p = os.path.join(T, "got.tmp")
    if os.path.exists(p):
        os.remove(p)
    rc, o = wasabi("get", path, p)
    return open(p, "rb").read() if rc == 0 and os.path.exists(p) else None


def ffsd(img, script):
    e = dict(os.environ, HOSTDISK=img)
    r = subprocess.run([os.path.join(T, "ffsd")], input=script.encode(), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=e, timeout=120)
    return r.stdout.decode(errors="replace")


def pat(n, seed=0):
    return bytes((i * 7 + seed + (i >> 8)) & 0xFF for i in range(n))


def patn(n):
    return bytes(ord('a') + i % 23 for i in range(n))


def part(rdbimg, i):
    """partition i's sectors of the RDB image (cylinders 1.., two cylinders each)"""
    d = open(rdbimg, "rb").read()
    return d[CYL * (1 + 2 * i):CYL * (3 + 2 * i)]


def put_part(rdbimg, i, data):
    d = bytearray(open(rdbimg, "rb").read())
    d[CYL * (1 + 2 * i):CYL * (3 + 2 * i)] = data
    open(rdbimg, "wb").write(d)


def edit_part(rdbimg, i, script):
    p = os.path.join(T, "part.img")
    open(p, "wb").write(part(rdbimg, i))
    o = ffsd(p, script)
    put_part(rdbimg, i, open(p, "rb").read())
    return o


def test_made_by_amigaos():
    """OS 3.2's own Format and files: long names and the directory cache, as AmigaOS makes them; ffsd reads"""
    rdb = os.path.join(T, "card2.hdf")
    run(sys.executable, os.path.join(HERE, "mkrdb-multi.py"), rdb, f"{2 * CYL // 1024}K:ffs7:ZL0:-", f"{2 * CYL // 1024}K:ffs5:ZC0:-").check_returncode()
    ok, o = amiga_start(rdb)
    check("3.2 starts with empty partitions", ok, o)
    if not ok:
        return
    time.sleep(3)
    nm40 = "a name with more than thirty characters in it"
    nm70 = "n" * 70
    for dev, opt, title in (("ZL0:", "FFS LONGNAMES", "Longa"), ("ZC0:", "FFS DIRCACHE", "Cachad")):
        rc, o = wasabi("run", f"SYS:System/Format <NIL: DRIVE {dev} NAME {title} {opt} QUICK NOICONS")
        rc, o = wasabi("run", f"Info {dev}")
        check(f"3.2's Format: {opt}", title in o, o)
        cmds = [f"MakeDir {dev}Mapp", f"Copy C:Dir {dev}Mapp/dir.bin", f"MakeLink {dev}hard {dev}Mapp/dir.bin", f"FileNote {dev}Mapp/dir.bin kommentar"]
        if "LONG" in opt:
            cmds += [f'Copy C:Dir "{dev}{nm40}"', f'Copy C:Dir "{dev}{nm70}"', f'FileNote "{dev}{nm40}" "{"k" * 79}"']
        else:
            cmds += [f"Copy C:Dir {dev}short.bin"]
        for c in cmds:
            rc, o = wasabi("run", c)
            check(f"3.2: {c.split()[0]}", rc == 0, o)
    dirbin = aget("C:Dir")
    time.sleep(12)
    amiga_stop()
    for i, (nm, dos) in enumerate((("long names", 7), ("directory cache", 5))):
        p = os.path.join(T, "made.img")
        open(p, "wb").write(part(rdb, i))
        w0 = open(p, "rb").read()
        out = os.path.join(T, "made.out")
        check(f"{nm}: AmigaOS made DOS{dos}", w0[:4] == b"DOS" + bytes([dos]), w0[:4])
        o = ffsd(p, f"mount rw\nls /\nls /Mapp\nopen /Mapp/dir.bin r\ndump %1 {out}\nclose %1\nstat /hard\nstat /Mapp/dir.bin\nopen /x wc\nstatfs\numount\n")
        check(f"{nm}: read-only even with rw", "readonly" in o and "open -> -30" in o, o)
        check(f"{nm}: a file AmigaOS wrote", dirbin is not None and open(out, "rb").read() == dirbin, o)
        check(f"{nm}: a hard link", "links 2" in o, o)
        check(f"{nm}: nothing written", open(p, "rb").read() == w0)
        if dos == 7:
            o = ffsd(p, "mount\nls /\n")
            m = re.search(r"\b(n+~[0-9a-f]+)\b", o)
            o2 = ffsd(p, "mount\nopen /" + nm40.replace(" ", "+") + f" r\ndump %1 {out}\nclose %1\nopen /{m.group(1) if m else 'none'} r\nfstat %2\nstat /" + nm40.upper().replace(" ", "+") + "\n")
            check("long names: a name over 30 characters, read, any case", open(out, "rb").read() == dirbin and o2.count(f"size {len(dirbin)}") >= 2, o2)
            check("long names: the date from its own place", re.search(r"mtime 1[6-9]\d{8}", o2), o2)
            check("long names: over 63 bytes opens by its listed form", m is not None and "open -> -" not in o2, o + o2)


def main():
    for t in ("amiga-test", "wasabi"):
        if not shutil.which(t):
            print(f"SKIP {t} is not installed: AmigaOS itself is not asked")
            return 0
    if subprocess.run(["gcc", "-m32", "-DZHOST", "-DZFSD", "-DFFSD", "-w", "-O1", "-Iuser", "-o", os.path.join(T, "ffsd"), "tools/host/fsd-host.c", "tools/host/hostdisk.c"]).returncode:
        print("FAIL build")
        return 1
    # two volumes, FFS international and OFS, written by ffsd
    imgs = []
    for n, dos in (("a.img", 3), ("b.img", 0)):
        v = Volume(2 * CYL // 512, "ZenA" if dos else "ZenB", dos >= 2, dos)
        v.add_file("from linux.txt", b"made-by-mkffs\n")
        v.add_file("old.txt", pat(3000))
        v.add_file("Dir/gone.bin", pat(50000))
        p = os.path.join(T, n)
        open(p, "wb").write(v.image())
        script = ("mount rw\nmkdir /Nya\nopen /Nya/Svensk+räksmörgås.txt wc\nwrite %1 0 hej\nclose %1\nopen /big.bin wc\nwrite %2 0 @200000\nclose %2\n"
                  "rename /old.txt /Nya/moved.txt\nunlink /Dir/gone.bin\nsymlink ../from+linux.txt /Nya/lnk\nopen /ro.txt wc\nwrite %3 0 protected\nclose %3\nchmod /ro.txt 555\nsync\numount\n")
        o = ffsd(p, script)
        check(f"DOS{dos}: ffsd's workload", not re.search(r"-> -\d", o), o)
        imgs.append(p)
    rdb = os.path.join(T, "card.hdf")
    run(sys.executable, os.path.join(HERE, "mkrdb-multi.py"), rdb, f"{imgs[0]}:ffs3:ZF0:-", f"{imgs[1]}:ofs:ZF1:-").check_returncode()
    ok, o = amiga_start(rdb)
    check(f"{MACHINE} starts", ok, o)
    if not ok:
        return 1
    time.sleep(3)
    for dev, nm in (("ZF0:", "FFS"), ("ZF1:", "OFS")):
        rc, o = wasabi("ls", dev)
        check(f"AmigaOS lists {nm}, written by ffsd", rc == 0 and "big.bin" in o and "Nya" in o and "ro.txt" in o and "gone" not in o, o)
        check(f"AmigaOS reads ffsd's big file ({nm})", aget(dev + "big.bin") == patn(200000))
        check(f"... a moved file ({nm})", aget(dev + "Nya/moved.txt") == pat(3000))
        check(f"... a name written from UTF-8 ({nm})", "Svensk r" in wasabi("ls", dev + "Nya")[1])
        rc, o = wasabi("run", f"List {dev}ro.txt")
        check(f"... chmod 555 shows as AmigaOS's protection ({nm})", "r-e-" in o, o)
        rc, o = wasabi("run", f"Delete {dev}ro.txt")
        check(f"... and AmigaOS refuses to delete it ({nm})", "not deleted" in o or "protected" in o.lower() or rc != 0, o)
        rc, o = wasabi("run", f"List {dev}Nya/lnk")
        check(f"... a soft link, as AmigaOS shows it ({nm})", "Soft" in o or "soft" in o or "lnk" in o, o)
        rc, o = wasabi("run", f"Type {dev}Nya/lnk")
        check(f"... followed by AmigaOS: the Amiga form of ../ ({nm})", "made-by-mkffs" in o, o)
        # AmigaOS's own changes, for ffsd to read: a hard link, a soft link, a rename, a comment, a new big file
        cmds = [f"MakeDir {dev}Amiga", f"Copy C:Dir {dev}Amiga/dir.bin", f"MakeLink {dev}Amiga/hard {dev}Amiga/dir.bin", f"MakeLink {dev}Amiga/hard2 {dev}from\\ linux.txt",
                f"Rename {dev}Nya/moved.txt {dev}Amiga/moved2.txt", f"FileNote {dev}big.bin kommentar", f"Protect {dev}big.bin rwd"]
        cmds[3] = f'MakeLink {dev}Amiga/hard2 "{dev}from linux.txt"'
        for c in cmds:
            rc, o = wasabi("run", c)
            check(f"AmigaOS: {c.split()[0]} ({nm})", rc == 0, o)
    time.sleep(12)                        # AmigaOS saves when it has been quiet for a moment
    dirbin = aget("C:Dir")
    amiga_stop()
    # ffsd on what AmigaOS wrote
    for i, (nm, dos) in enumerate((("FFS", 3), ("OFS", 0))):
        d = Disk(part(rdb, i))
        d.walk()
        d.bad = [w for w in d.bad if "block order" not in w]
        check(f"{nm}: after AmigaOS the judge finds nothing", not d.bad, d.bad)
        o = edit_part(rdb, i, "mount rw\nls /Amiga\nopen /Amiga/dir.bin r\ndump %1 " + os.path.join(T, "dir.out") + "\nclose %1\nstat /Amiga/hard\nopen /Amiga/moved2.txt r\nread %2 2999 10\nstat /big.bin\nunlink /Amiga/hard\nunlink /Amiga/dir.bin\nstat /Amiga/dir.bin\nopen /after.txt wc\nwrite %3 0 zenix-after-amigaos\nclose %3\nsync\numount\n")
        check(f"{nm}: -o rw passes the check on AmigaOS's volume", "mount -> 0\n" in o and "readonly" not in o, o)
        check(f"{nm}: ffsd reads what AmigaOS wrote", dirbin is not None and open(os.path.join(T, "dir.out"), "rb").read() == dirbin, o)
        check(f"{nm}: the protection bits AmigaOS set", re.search(r"stat -> 0 ino \d+ mode 100644 size 200000", o), o)
        d = Disk(part(rdb, i))
        d.walk()
        d.bad = [w for w in d.bad if "block order" not in w]
        check(f"{nm}: removed a file with a hard link, the judge finds nothing", not d.bad and d.leak == 0, str(d.bad) + str(d.leak))
    # the validator as a judge: the flag cleared on a copy; AmigaOS rebuilds the bitmap: it must be ffsd's, block for block
    before = []
    for i in (0, 1):
        d = bytearray(part(rdb, i))
        x = Disk(bytes(d)).walk()
        before.append(sorted(b for b in range(x.res, x.n) if x.bit_free(b)))
        r = x.root * 512
        struct.pack_into(">I", d, r + 512 - 200, 0)
        struct.pack_into(">I", d, r + 20, 0)
        struct.pack_into(">I", d, r + 20, -sum(struct.unpack_from(">128I", d, r)) & 0xFFFFFFFF)
        put_part(rdb, i, bytes(d))
    ok, o = amiga_start(rdb)
    check(f"{MACHINE} starts again", ok, o)
    if ok:
        time.sleep(8)
        for dev, nm in (("ZF0:", "FFS"), ("ZF1:", "OFS")):
            rc, o = wasabi("run", f"Info {dev}")
            check(f"AmigaOS validated ffsd's volume ({nm})", rc == 0 and "Read/Write" in o, o)
            check(f"AmigaOS reads ffsd's last file ({nm})", aget(dev + "after.txt") == b"zenix-after-amigaos", wasabi("ls", dev)[1])
            rc, o = wasabi("run", f"Copy C:Type {dev}validated.bin")      # a write: it waits for the validator, and the flag is set at the flush
        time.sleep(5)
        amiga_stop()
    for i, nm in enumerate(("FFS", "OFS")):
        x = Disk(part(rdb, i))
        x.walk()
        x.bad = [w for w in x.bad if "block order" not in w]
        check(f"{nm}: valid after AmigaOS's validator", not x.bad and x.walked, x.bad)
        o = edit_part(rdb, i, "mount rw\nopen /x wc\nclose %1\nsync\numount\n")
        check(f"{nm}: and ffsd writes it again", "mount -> 0\n" in o and not re.search(r"-> -\d", o), o)
    # the validator as a judge, the numbers: the blocks AmigaOS's rebuilt bitmap calls free against ffsd's
    for i, nm in enumerate(("FFS", "OFS")):
        x = Disk(part(rdb, i))
        x.walk()
        after = set(b for b in range(x.res, x.n) if x.bit_free(b))
        taken = set(before[i]) - after
        given = after - set(before[i])
        written = {x.files[b"/x"][2], x.files[b"/validated.bin"][2]} | set(x.files[b"/validated.bin"][0])
        check(f"{nm}: AmigaOS's validator agrees with ffsd's bitmap, block for block (but what was written after it)", taken == written and not given, f"took {sorted(taken)} gave {sorted(given)} wrote {sorted(written)}")
    if "32" in MACHINE:
        test_made_by_amigaos()
    print(f"ffs-aos-test: {passes} passed, {fails} failed")
    return 1 if fails else 0


signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
try:
    rc = main()
finally:
    amiga_stop()
    shutil.rmtree(T, ignore_errors=True)
sys.exit(rc)
