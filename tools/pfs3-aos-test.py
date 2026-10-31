#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""PFS3 against the real thing (DESIGN D32): AmigaOS 3.2 in FS-UAE, with the public pfs3aio handler, formats volumes and
writes into them: Swedish names, big and fragmented files, deep directories, many files, links, comments, protection,
dates. Then pfs3d (the PC build) reads each image and every file is compared with what was written. Not in `make check`:
it needs this PC's test machines and a handler file the repository does not carry. Without them: SKIP, and it passes.
Emulators only; never the real Amiga.
usage: PFS3AIO=FILE tools/pfs3-aos-test.py [variant ...]    variants: plain super bs4096 (default all three); with
PFS3AIO2=FILE, a handler built from the public source with large files, also pfs2 pfs2k1 pfs2k4 (asked for by name). PFS3_KEEP=DIR keeps the images there."""
import calendar, fcntl, os, random, re, shutil, struct, subprocess, sys, tempfile, time, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
MACHINE = "a1200-32"
NAME, PORT = "zenixpfs3", "1299"
CYL = 128 * 64                                             # sectors of a cylinder of these test disks
fails = passes = 0
started = False
slot = None


def check(name, ok, extra=""):
    global fails, passes
    if ok:
        passes += 1
        print("PASS", name, flush=True)
    else:
        fails += 1
        print("FAIL", name, str(extra)[:1500], flush=True)


def run(*a, **k):
    return subprocess.run(a, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, **k)


def wasabi(*a, timeout=600):
    r = run("wasabi", "--host", "127.0.0.1", "--port", PORT, *a, timeout=timeout)
    return r.returncode, r.stdout.decode("latin-1", errors="replace")


def take_slot():
    """one of the emulator slots every FS-UAE on this PC shares (tools/run-test.sh), and a load of 16 or less"""
    global slot
    lock, n = os.environ.get("ZENIX_EMU_LOCK", "/tmp/zenix-emulator.lock"), int(os.environ.get("ZENIX_EMU_SLOTS", "6"))
    while True:
        for i in range(n):
            f = open(lock if i == 0 else f"{lock}.{i}", "a")
            try:
                fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError:
                f.close()
                continue
            while os.getloadavg()[0] > 16:
                time.sleep(30)
            slot = f
            return
        time.sleep(5)


def amiga_start(img, src):
    global started
    take_slot()
    r = run("amiga-test", MACHINE, "start", "--fresh", "--as", NAME, "--port", PORT, "--config", f"hard_drive_1 = {img}", "--config", "hard_drive_1_type = rdb",
            "--config", f"hard_drive_2 = {src}", "--config", "hard_drive_2_label = ZSRC", "--config", "hard_drive_2_priority = -128", timeout=240)
    started = r.returncode == 0
    return started, r.stdout.decode(errors="replace")


def amiga_stop():
    global started, slot
    if started:
        run("amiga-test", MACHINE, "stop", "--as", NAME, timeout=60)
    started = False
    if slot:
        slot.close()
        slot = None


def pat(n, seed):
    """n bytes that differ everywhere and from file to file"""
    r = random.Random(seed)
    block = r.randbytes(4099)
    return (block * (n // 4099 + 1))[:n]


def latin(s):
    return s.encode("latin-1")


def make_source(src):
    """the files AmigaOS copies, with plain names; the script then gives some of them Swedish names. The expected tree:
    a dict path -> dict(data=, mode=, mtime=, links=, link=)"""
    exp = {}

    def put(path, data):
        p = os.path.join(src, path)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, "wb").write(data)
        exp["/" + path[2:]] = dict(data=data)

    for n, name in ((0, "empty"), (1, "one"), (511, "b511"), (512, "b512"), (513, "b513"), (1024, "b1024"), (4096, "b4096"), (65536, "b65536"), (1 << 20, "b1m")):
        put("t/" + name, pat(n, name))
    put("t/big", pat(8 << 20, "big"))
    put("t/Mapp/rak.txt", b"svenska-fran-linux\n")
    put("t/Mapp/o.txt", b"in-the-swedish-directory\n")
    d = "t/Deep"
    for i in range(24):
        d += f"/d{i:02}"
    put(d + "/leaf.txt", b"at-the-bottom\n")
    for i in range(2000):
        put(f"t/Many/f{i:04}", pat(40 + i % 300, i))
    put("t/note.txt", b"has-a-comment\n")
    put("t/r.txt", b"protected\n")
    put("t/dated.txt", b"dated\n")
    put("t/" + "m" * 31, b"thirty-one-characters\n")
    put("t/" + "L" * 100, b"a-long-name\n")
    os.makedirs(os.path.join(src, "t2"))
    a, b = pat(128 << 10, "a"), pat(128 << 10, "b")
    open(os.path.join(src, "t2/a"), "wb").write(a)
    open(os.path.join(src, "t2/b"), "wb").write(b)
    return exp, a, b


def job_script(dev, exp, a, b):
    """the AmigaOS script: copies, renames, links, then two files grown by turns so they come out in pieces"""
    v = dev
    s = ["FailAt 21", f"Copy ZSRC:t {v} ALL CLONE QUIET", "Echo copied $RC"]
    # Swedish names and upper case in a directory name
    s += [f'Rename {v}Mapp {v}ÅÄÖ-mapp', f'Rename {v}ÅÄÖ-mapp/rak.txt "{v}ÅÄÖ-mapp/räksmörgås.txt"', f'Rename {v}ÅÄÖ-mapp/o.txt "{v}ÅÄÖ-mapp/ö é.txt"']
    for a, b in (("/Mapp/rak.txt", "/ÅÄÖ-mapp/räksmörgås.txt"), ("/Mapp/o.txt", "/ÅÄÖ-mapp/ö é.txt")):
        exp[b] = exp.pop(a)
    s += [f'FileNote {v}note.txt "{"k" * 79}"']
    s += [f"Protect {v}r.txt re", f"Protect {v}b511 rw", f"Protect {v}b513 r"]
    exp["/r.txt"]["mode"] = 0o555
    exp["/b511"]["mode"] = 0o644
    exp["/b513"]["mode"] = 0o444
    s += [f"SetDate {v}dated.txt 15-Mar-21 12:34:56"]
    exp["/dated.txt"]["mtime"] = calendar.timegm((2021, 3, 15, 12, 34, 56))
    # hard links: to a file (from two places), to a directory; soft links: relative, from the volume's root, gone
    s += [f"MakeLink {v}hard1 {v}b4096", f"MakeLink {v}ÅÄÖ-mapp/hard2 {v}b4096", f"MakeLink {v}dirlink {v}Deep/d00 FORCE"]
    exp["/hard1"] = exp["/ÅÄÖ-mapp/hard2"] = exp["/b4096"]
    exp["/b4096"]["links"] = 3
    s += [f"MakeLink {v}soft1 b512 SOFT", f'MakeLink {v}ÅÄÖ-mapp/soft2 /b512 SOFT', f"MakeLink {v}soft3 Pfstest:b1024 SOFT", f"MakeLink {v}softdir Deep/d00 SOFT",
          f"MakeLink {v}softgone nothing-here SOFT"]
    exp["/soft1"] = dict(soft="b512")
    exp["/ÅÄÖ-mapp/soft2"] = dict(soft="../b512")
    exp["/soft3"] = dict(soft="b1024")
    exp["/softdir"] = dict(soft="Deep/d00")
    exp["/softgone"] = dict(soft="nothing-here")
    # a file removed goes to the deleted-files directory, which is not listed
    s += [f"Delete {v}Many/f0005 QUIET"]
    del exp["/Many/f0005"]
    for i in range(40):
        s.append(f"Type ZSRC:t2/a >>{v}frag")
        s.append(f"Type ZSRC:t2/b >>{v}frag2")
    exp["/frag"] = dict(data=a * 40)
    exp["/frag2"] = dict(data=b * 40)
    for k in list(exp):                                    # every directory on the way
        while k.rfind("/") > 0:
            k = k[:k.rfind("/")]
            exp.setdefault(k, None)
    exp["/dirlink"] = None
    s += ["Echo done-the-job"]
    return s


def tree_of(exe, img, start, sectors, bsopt=None):
    e = dict(os.environ, HOSTDISK=img, HOSTDISK_RDONLY="1", HOSTDISK_START=str(start), HOSTDISK_SECTORS=str(sectors))
    r = subprocess.run([exe], input=b"mount\nstatfs\ntree /\nwrites\nquit\n", stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=e, timeout=300)
    return r.returncode, r.stdout.decode("utf-8", errors="replace")


def judge(tag, out, exp):
    got = {}
    errs = []
    for l in out.splitlines():
        if l.startswith("!"):
            errs.append(l)
            continue
        m = re.match(r"([fdl]) ([0-7]+) (\d+) (\d+) (\d+) (\w+) (/.*?) #(\d+)(?: -> (.*))?$", l)
        if m:
            got[m.group(7)] = m.groups()
    check(f"{tag}: every file and directory read without an error", not errs, errs[:6])
    check(f"{tag}: nothing written", "writes -> 0" in out, out[-200:])
    missing, wrong = [], []
    for p, x in exp.items():
        g = got.get(p)
        if not g and len(p.encode()) > 64:                 # a name over 63 bytes is listed by its start and its anode
            g = next((v for k, v in got.items() if k.startswith(p[:50]) and "~" in k), None)
        if x is None:
            if not g or g[0] != "d":
                missing.append(p)
            continue
        if not g:
            missing.append(p)
            continue
        if "soft" in x:
            if g[0] != "l" or g[8] != x["soft"]:
                wrong.append((p, g[8], x["soft"]))
            continue
        d = x["data"]
        if g[0] != "f" or int(g[2]) != len(d) or int(g[5], 16) != zlib.crc32(d):
            wrong.append((p, "data", g[2], len(d), g[5]))
        if "mode" in x and int(g[1], 8) != x["mode"]:
            wrong.append((p, "mode", g[1], oct(x["mode"])))
        if "mtime" in x and int(g[4]) != x["mtime"]:
            wrong.append((p, "mtime", g[4], x["mtime"]))
        if "links" in x and int(g[3]) != x["links"]:
            wrong.append((p, "links", g[3], x["links"]))
    check(f"{tag}: all {len(exp)} expected names are there", not missing, missing[:10])
    check(f"{tag}: sizes, checksums, modes, dates, links, soft link texts are what AmigaOS was given", not wrong, wrong[:10])
    files = {k[0]: 0 for k in "fdl"}
    for g in got.values():
        files[g[0]] += 1
    extra = [p for p in got if p not in exp and "~" not in p and not p.startswith("/dirlink/")]
    check(f"{tag}: no name that should not be there (.deldir, the removed file)", not extra, extra[:10])
    print(f"     {files['f']} files, {files['d']} directories, {files['l']} soft links, {sum(int(g[2]) for g in got.values() if g[0] == 'f')} bytes", flush=True)
    return got


def anode_pieces(img, start, bs=512):
    """an independent look at the bytes: how many pieces the largest file's chain has, from the image, for the test's own
    honesty (a contiguous file would prove nothing). A small walker in the format's terms, reserved blocks of 1 KB"""
    f = open(img, "rb")

    def block(b, n=1024):
        f.seek(start * 512 + b * bs)
        return f.read(n)

    root = block(2, 512)
    opts = struct.unpack(">I", root[4:8])[0]
    rb = struct.unpack(">H", root[64:66])[0]
    apb, ipb = (rb - 16) // 12, (rb - 12) // 4
    ext = block(struct.unpack(">I", root[88:92])[0], rb)

    def anode(nr):
        seq, off = (nr >> 16, nr & 0xFFFF) if opts & 2 else (nr // apb, nr % apb)
        ix = seq // ipb
        if opts & 128:
            sb = block(struct.unpack(">I", ext[64 + 4 * (ix // ipb):68 + 4 * (ix // ipb)])[0], rb)
            ib = struct.unpack(">I", sb[12 + 4 * (ix % ipb):16 + 4 * (ix % ipb)])[0]
        else:
            ib = struct.unpack(">I", root[116 + 4 * ix:120 + 4 * ix])[0]
        ibb = block(ib, rb)
        ab = block(struct.unpack(">I", ibb[12 + 4 * (seq % ipb):16 + 4 * (seq % ipb)])[0], rb)
        return struct.unpack(">III", ab[16 + 12 * off:28 + 12 * off])

    def entries(nr):
        while nr:
            c, b, nx = anode(nr)
            for k in range(c):
                d = block(b + k, rb)
                o = 20
                while o + 2 <= rb and d[o]:
                    n = d[o + 17]
                    yield d[o + 18:o + 18 + n], struct.unpack(">I", d[o + 2:o + 6])[0]
                    o += d[o]
            nr = nx

    for name, an in entries(5):
        if name == b"frag":
            n = 0
            while an:
                _, _, an = anode(an)
                n += 1
            return n
    return 0


def variant(tag, T, handler, sectors_cyl, bs, patch_fnsize=True):
    print(f"--- {tag}", flush=True)
    src = os.path.join(T, f"src-{tag}")
    os.makedirs(src)
    shutil.copy(handler, os.path.join(src, "pfs3aio"))
    exp, fa, fb = make_source(src)
    os.chmod(os.path.join(src, "pfs3aio"), 0o755)
    img = os.path.join(T, f"{tag}.hdf")
    cyls = sectors_cyl
    add = ["add", "start=2", f"size={cyls}", "name=ND0", "dostype=0x50465303", "automount=0"] + ([f"bs={bs}"] if bs != 512 else [])
    r = run("rdbtool", "-f", img, "create", f"chs={cyls + 3},128,64", "+", "init", "rdb_cyls=2", "+", *add)
    if r.returncode:
        check(f"{tag}: rdbtool", False, r.stdout)
        return
    spb = bs // 512
    mf = ["FileSystem = ZSRC:pfs3aio", "Device = uaehf.device", "Unit = 1", "Flags = 0", "Surfaces = 128", "BlocksPerTrack = 64", "BlockSize = 512", f"SectorsPerBlock = {spb}", "Reserved = 2",
          "LowCyl = 2", f"HighCyl = {cyls + 1}", "Buffers = 50", "BufMemType = 0", "MaxTransfer = 0xFFFFFF", "Mask = 0x7FFFFFFE", "DosType = 0x50465303", "StackSize = 4096", "GlobVec = -1", "Activate = 1"]
    open(os.path.join(src, "ND0"), "w").write("\n".join(mf) + "\n")
    start = 2 * CYL
    sectors = cyls * CYL
    ok, o = amiga_start(img, src)
    check(f"{tag}: AmigaOS starts", ok, o)
    if not ok:
        return
    wasabi("run", "Mount ZSRC:ND0")
    time.sleep(2)
    rc, o = wasabi("run", "SYS:System/Format <NIL: DRIVE ND0: NAME Pfstest QUICK NOICONS")
    rc2, o2 = wasabi("run", "Info ND0:")
    check(f"{tag}: pfs3aio formats it", "Pfstest" in o2, o + o2)
    if "Pfstest" not in o2:
        amiga_stop()
        return
    time.sleep(12)
    amiga_stop()
    # long names: the file name size lives in the rootblock extension, one number (what the handler's own tool changes)
    d = bytearray(open(img, "r+b").read(start * 512 + 16384))
    unit = bs if d[start * 512 + 2 * bs:start * 512 + 2 * bs + 4] in (b"PFS\x01", b"PFS\x02") else 512     # a PFS\1 volume keeps 512-byte blocks
    root = start * 512 + 2 * unit
    rtype = d[root:root + 4]
    print(f"     root: {bytes(rtype)!r} options {struct.unpack('>I', d[root + 4:root + 8])[0]:#x} reserved block {struct.unpack('>H', d[root + 64:root + 66])[0]} disk size {struct.unpack('>I', d[root + 84:root + 88])[0]}", flush=True)
    ext = struct.unpack(">I", d[root + 88:root + 92])[0]
    if patch_fnsize:
        with open(img, "r+b") as f:
            f.seek(start * 512 + ext * unit + 56)
            f.write(struct.pack(">H", 108))
    ok, o = amiga_start(img, src)
    check(f"{tag}: AmigaOS starts again", ok, o)
    if not ok:
        return
    wasabi("run", "Mount ZSRC:ND0")
    time.sleep(2)
    job = job_script("ND0:", exp, fa, fb)
    open(os.path.join(src, "job"), "wb").write(("\n".join(job) + "\n").encode("latin-1"))
    t0 = time.time()
    rc, o = wasabi("run", "Execute ZSRC:job", timeout=1200)
    check(f"{tag}: the job ran in AmigaOS ({int(time.time() - t0)} s)", "done-the-job" in o and "copied 0" in o, o[-800:])
    bad = [l for l in o.splitlines() if re.search(r"(?i)\b(error|can't|cannot|object not|not found|wrong|failed)\b", l)]
    check(f"{tag}: no complaint from the job", not bad, bad[:8])
    time.sleep(15)
    amiga_stop()
    rc, out = tree_of(os.path.join(T, "pfs3d"), img, start, sectors)
    if os.environ.get("PFS3_KEEP"):
        shutil.copy(img, os.path.join(os.environ["PFS3_KEEP"], tag + ".hdf"))
    check(f"{tag}: pfs3d mounts what AmigaOS made", out.startswith("mount -> 0"), out[:400])
    got = judge(tag, out, exp)
    p = got.get("/hard1"), got.get("/b4096")
    check(f"{tag}: a hard link has its file's inode number", p[0] and p[1] and p[0][7] == p[1][7], p)
    n = anode_pieces(img, start, unit)
    check(f"{tag}: the fragmented file really is in pieces ({n})", n > 3, n)
    e = dict(os.environ, HOSTDISK=img, HOSTDISK_RDONLY="1", HOSTDISK_START=str(start), HOSTDISK_SECTORS=str(sectors))
    script = "mount\nstat /RÄKSMÖRGÅS.TXT\nstat /åäö-MAPP/RÄKSMÖRGÅS.TXT\nstat /dirlink/d01\nstat /softdir/d01\nopen /soft1 r\nfstat %1\nstat /" + "M" * 31 + "\nstat /.deldir\nopen /new wc\nunlink /b512\nmkdir /x\n"
    r = subprocess.run([os.path.join(T, "pfs3d")], input=script.encode(), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=e)
    so = r.stdout.decode(errors="replace")
    ls = so.splitlines()
    check(f"{tag}: names in any case, through both kinds of link", " -> -2" in ls[1] and " -> 0 " in ls[2] and " -> 0 " in ls[3] and " -> 0 " in ls[4], so)
    check(f"{tag}: a soft link opens its target", "size 512" in ls[6], so)
    check(f"{tag}: writes are refused", all(x.endswith("-> -30") for x in ls[9:12]), so)
    os.remove(img)
    shutil.rmtree(src)


def main():
    handler = os.environ.get("PFS3AIO")
    if not handler or not os.path.exists(handler) or not shutil.which("amiga-test") or not shutil.which("wasabi") or not shutil.which("rdbtool"):
        print("SKIP pfs3-aos-test: needs PFS3AIO=the handler file, amiga-test, wasabi and rdbtool")
        return 0
    run("amiga-test", MACHINE, "status")
    T = tempfile.mkdtemp(prefix="zenix-pfs3aos.", dir=os.environ.get("PFS3_DIR"))
    CF = "-m32 -D_FILE_OFFSET_BITS=64 -DZHOST -DZFSD -DPFS3D -O1 -g -Iuser -Wno-builtin-declaration-mismatch".split()
    r = subprocess.run(["gcc", *CF, "tools/host/fsd-host.c", "tools/host/hostdisk.c", "-o", os.path.join(T, "pfs3d")], cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode:
        print(r.stdout.decode())
        return 1
    want = sys.argv[1:] or ["plain", "super", "bs4096"]
    try:
        if "plain" in want:
            variant("plain", T, handler, 25, 512)                # 100 MB, no super blocks
        if "bs4096" in want:
            variant("bs4096", T, handler, 25, 4096)              # the partition table says 4 KB blocks, as two of the owner's partitions
        if "super" in want:
            variant("super", T, handler, 1400, 512)              # 5.6 GB, sparse: over the size where super blocks start
        for tag, b in (("pfs2", 512), ("pfs2k1", 1024), ("pfs2k4", 4096)):   # large files: the newer type; logical blocks over 512
            if tag in want and os.environ.get("PFS3AIO2"):
                variant(tag, T, os.environ["PFS3AIO2"], 25, b)
    finally:
        amiga_stop()
        shutil.rmtree(T, ignore_errors=True)
    print(f"pfs3-aos-test: {passes} passed, {fails} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
