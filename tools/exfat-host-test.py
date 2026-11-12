#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The exFAT server (user/exfatd.c, DESIGN D47) built for the PC, against volumes the PC's mkfs.exfat (exfatprogs) formats
and tools/exfat-img.py fills from the specification (no exFAT writer runs here without root), each checked by the PC's
fsck.exfat first: contiguous and FAT-chained files and directories (fragmented on purpose), a valid data length below the
size, names in Swedish, Greek, Cyrillic, Japanese and past the BMP, long names, read-only files, UTC offsets; sector
sizes 512 and 4096, clusters from 512 bytes to 32 MB; a 128 GB volume with a file at its far end, files 32 GB apart, a
file of 4 GB - 1 and one of 5 GB (its size capped, its open refused); every file read in odd pieces and compared; a 64 KB read of a contiguous file as one
request; every change refused; volumes refused at the mount; damage aimed at the boot region, the FAT, the entry sets;
and a fuzz run with ASAN and UBSAN (no crash, no hang).
usage: tools/exfat-host-test.py [fuzz-rounds]   (default 600). SKIP without mkfs.exfat and fsck.exfat."""
import importlib.util, os, random, re, shutil, struct, subprocess, sys, tempfile, zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
_spec = importlib.util.spec_from_file_location("exfat_img", os.path.join(ROOT, "tools", "exfat-img.py"))
ei = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ei)
CF = "-m32 -D_FILE_OFFSET_BITS=64 -DZHOST -DZFSD -DEXFATD -Wall -Wextra -Wno-builtin-declaration-mismatch -Wno-unused-function -O1 -g -Iuser".split()
SAN = "-fsanitize=address,undefined -fno-sanitize-recover=undefined".split()
SRC = ["tools/host/fsd-host.c", "tools/host/hostdisk.c"]
ROUNDS = int(sys.argv[1]) if len(sys.argv) > 1 else 600
T = tempfile.mkdtemp(prefix="zenix-exfathost.")
EXE = os.path.join(T, "exfatd")
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


def mkfs(img, size, *opts):
    if os.path.exists(img):
        os.unlink(img)
    with open(img, "wb") as f:
        f.truncate(size)
    r = subprocess.run(["mkfs.exfat", "-q", "-P", "none"] + list(opts) + [img], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode:
        raise SystemExit("mkfs.exfat failed: " + r.stdout.decode(errors="replace"))


def fsck(img):
    r = subprocess.run(["fsck.exfat", "-n", img], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return r.returncode == 0, r.stdout.decode(errors="replace")


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


def build():
    r = subprocess.run(["gcc"] + CF + SAN + SRC + ["-o", EXE], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode:
        print(r.stdout.decode())
        raise SystemExit("FAIL build")


NAMES = {"räksmörgås.txt": b"svenska\n", "Ελληνικά.txt": b"greek\n", "Кириллица.txt": b"cyrillic\n",
         "日本語のファイル.txt": b"japanese\n", "emoji-\U0001F600.txt": b"astral\n",
         "A long name with spaces and more words in it, past sixty-three bytes.txt": b"long\n"}


def items_main(cb):
    """the main tree; path -> data (None: a directory), options"""
    it = []
    for n in (0, 1, 511, 512, 513, 4095, 4096, 4097, 65536, 300001):
        it.append((f"size{n}.bin", pat(n, n), {}))
    it.append(("chained.bin", pat(70000, 7), {"nofat": False}))                      # a chain, one run
    it.append(("frag.bin", pat(max(200000, 9 * cb + 77), 8), {"nofat": False, "fragment": True}))
    it.append(("vdl.bin", pat(3 * cb + 100, 9), {"vdl": cb + 33}))                    # zeros past the valid length
    it.append(("ro.txt", b"read-only\n", {"ro": True}))
    it.append(("utc.txt", b"utc\n", {"mtime": 1700000000, "utc": -20}))             # UTC - 5 h
    it.append(("Mapp", None, {"nofat": False, "fragment": True}))
    it.append(("Mapp/inner.txt", b"inner\n", {"vendor": True, "deleted_before": True}))
    it.append(("Mapp/Under/deeper.txt", b"deeper\n", {}))
    it.append(("Tom", None, {}))
    for n, d in NAMES.items():
        it.append(("Namn/" + n, d, {}))
    for i in range(300):
        it.append((f"Lista/fil{i:03}.txt", b"file %d\n" % i, {}))
    it.append(("Kedja", None, {"nofat": False, "fragment": True}))
    for i in range(200):
        it.append((f"Kedja/k{i:03}.txt", pat(100 + i, i), {"nofat": i % 2 == 0, "fragment": True}))
    return it


def expect_of(items):
    exp = {}
    for p, d, o in items:
        if d is not None:
            exp["/" + p] = d[:o.get("vdl", len(d))] + bytes(len(d) - o.get("vdl", len(d)))
    return exp


def compare(name, img, items, extra_rawnames=()):
    rc, out = run(img, "mount\ntree /\nstatfs\nquit\n", timeout=120)
    got, errs = tree(out)
    check(f"{name}: mounts", "mount -> 0 readonly" in out, out[:400])
    check(f"{name}: no error in the tree", not errs and rc == 0, errs[:5] or out[-800:])
    check(f"{name}: nothing written", "hostdisk-writes 0" in out, out[-200:])
    exp = expect_of(items)
    for p in list(exp):
        if p.endswith("/bad"):
            exp[p.replace("/bad", "/a_b_c")] = exp.pop(p)
    extra_ok = set(extra_rawnames)
    for p in list(exp):                                # over a udirent's 63 bytes: listed as its start, ~ and a number
        if len(os.path.basename(p).encode()) > 63:
            alias = [k for k in got if k.startswith(p[:40]) and re.search(r"~[0-9a-f]+$", k)]
            if len(alias) == 1:
                got[p] = got.pop(alias[0])
    bad = [p for p, d in exp.items() if p not in got or got[p][0] != "f" or int(got[p][2]) != len(d) or int(got[p][5], 16) != zlib.crc32(d)]
    check(f"{name}: every file there, its size and CRC (read in odd pieces)", not bad, bad[:5])
    extra = [p for p in got if got[p][0] == "f" and p not in exp and p not in extra_ok]
    check(f"{name}: nothing else listed (deleted sets not shown)", not extra, extra[:5])
    return got, out


def main():
    for t in ("mkfs.exfat", "fsck.exfat"):
        if not shutil.which(t):
            print(f"SKIP exfat-host-test: {t} (exfatprogs) is not installed")
            return 0
    build()

    # ---- the main volume: 64 MB, 4 KB clusters, 512-byte sectors ----
    img = os.path.join(T, "main.img")
    mkfs(img, 64 << 20, "-L", "ZENIX")
    items = items_main(4096)
    ei.build(img, items)
    ok, why = fsck(img)
    check("main: fsck.exfat finds the written volume clean (the writer is right)", ok, why)
    got, out = compare("main", img, items)
    check("main: the 300-entry directory and the chained one", sum(1 for p in got if p.startswith("/Lista/")) == 300 and
          sum(1 for p in got if p.startswith("/Kedja/")) == 200)
    check("main: modes (644, read-only 444, directories 755)", got["/size1.bin"][1] == "644" and got["/ro.txt"][1] == "444" and got["/Mapp"][1] == "755", [got.get(k) for k in ("/size1.bin", "/ro.txt", "/Mapp")])
    check("main: the time is UTC (the entry's offset taken off)", got["/utc.txt"][4] == "1700000000" and got["/size1.bin"][4] == "1700000000", got["/utc.txt"])
    m = re.search(r"statfs -> 0 bsize 4096 blocks (\d+) bfree (\d+)", out)
    used = None
    if m:
        bm = bytearray(open(img, "rb").read())       # the bitmap's own count, from the image
        v = ei.Volume(img)
        used = sum(bin(b).count("1") for b in v.bitmap)
        v.f.close()
    check("main: statfs: clusters and the free ones from the bitmap", m and int(m.group(2)) == int(m.group(1)) - used, (m and m.groups(), used))

    # lookups in any case; Swedish, Greek and Cyrillic folded, Japanese exact; the ~ form; '_' for what was listed so
    rc, out = run(img, "mount\nopen /SIZE1.BIN r\nopen /Namn/RÄKSMÖRGÅS.TXT r\nread %2 0 20\nopen /Namn/ΕΛΛΗΝΙΚΆ.txt r\n"
                       "open /Namn/ελληνικά.TXT r\nopen /Namn/КИРИЛЛИЦА.txt r\nopen /Namn/日本語のファイル.TXT r\n"
                       "open /Namn/emoji-\U0001F600.txt r\nopen /mapp/under/DEEPER.txt r\nquit\n")
    lines = [l for l in out.splitlines() if l.startswith("open -> ") or l.startswith("read -> ")]
    ok = lambda i: i < len(lines) and re.match(r"open -> \d+$", lines[i])
    check("lookups: ASCII in any case", ok(0), lines)
    check("... Swedish letters in any case", ok(1) and lines[2] == "read -> 8 svenska", lines)
    check("... Greek and Cyrillic in any case (accented capitals are another letter: not found)", lines[3] == "open -> -2" and ok(4) and ok(5), lines)
    check("... Japanese exact, a character past the BMP", ok(6) and ok(7), lines)
    check("... a path in any case", ok(8), lines)
    # a name a Unix name cannot carry ('/', a control character: fsck.exfat would call it damage, so a volume of its own)
    odd = os.path.join(T, "odd.img")
    mkfs(odd, 8 << 20)
    ei.build(odd, [("bad", b"slash\n", {"rawname": "a/b\x01c"}), ("good.txt", b"g\n", {})])
    rc, out = run(odd, "mount\nls /\nopen /a_b_c r\nread %1 0 10\nopen /a_b_d r\nopen /A_B_C r\nquit\n")
    check("a name with '/' and a control character: listed with '_', found so, in any case, not by another",
          "ls -> a_b_c good.txt (2, 0)" in out and "read -> 6 slash" in out and out.count("open -> -2") == 1 and len(re.findall(r"open -> \d+", out)) == 2, out)
    rc, out = run(img, "mount\nls /Namn\nquit\n")
    alias = None
    for token in re.findall(r"A long name.*?~[0-9a-f]+", out):
        alias = token
    check("a long name is listed by its start, ~ and its number", alias and len(alias.encode()) <= 63, out[:300])
    if alias:
        rc, o2 = run(img, "mount\nstat /Namn/" + alias.replace(" ", "+") + "\nstat /Namn/A+long+name+with+spaces+and+more+words+in+it,+past+sixty-three+bytes.txt\nquit\n")
        check("... which opens it, as does its whole name", o2.count("stat -> 0 ino") == 2, o2)

    # read-only answers (the answer a writable volume would give first), options, permissions
    rc, out = run(img, "mount\nopen /size1.bin w\nopen /new c\nopen /size1.bin cx\nmkdir /Mapp/x\nmkdir /Mapp\nunlink /size1.bin\n"
                       "unlink /nosuch\nrename /size1.bin /y\nchmod /size1.bin 777\nsymlink a /z\nlink /size1.bin /w\n"
                       "readlink /size1.bin\nrmdir /Tom\nutimes /size1.bin now\nchown /size1.bin 1 1\n"
                       "stat /nosuch\nstat /size1.bin/x\nls /size1.bin\nopen /Mapp w\nquit\n")
    lines = out.splitlines()
    def ans(cmd, n=0):
        hits = [l for l in lines if l.startswith(cmd + " -> ")]
        return hits[n] if len(hits) > n else ""
    check("open for writing, creating: read-only; an existing name with O_EXCL: EEXIST", ans("open") == "open -> -30" and ans("open", 1) == "open -> -30" and ans("open", 2) == "open -> -17", lines)
    check("mkdir, unlink, rename, chmod, symlink, link, rmdir, utimes, chown: read-only",
          all(ans(c) == c + " -> -30" for c in ("unlink", "rename", "chmod", "symlink", "link", "rmdir", "utimes", "chown")) and ans("mkdir") == "mkdir -> -30", lines)
    check("... mkdir of an existing name EEXIST, unlink of none ENOENT, readlink EINVAL", ans("mkdir", 1) == "mkdir -> -17" and ans("unlink", 1) == "unlink -> -2" and ans("readlink") == "readlink -> -22", lines)
    check("not found, not a directory, a directory for writing", ans("stat") == "stat -> -2" and ans("stat", 1) == "stat -> -20" and ans("ls") == "ls -> (0, -20)" and ans("open", 3) == "open -> -21", lines)
    rc, out = run(img, "mount rw\nquit\n")
    check("-o rw refused by the server", "-o rw is refused" in out and "mount -> bad option" in out, out)
    rc, out = run(img, "mount uid=5,gid=6,fmode=640,dmode=750\nstat /size1.bin\nstat /ro.txt\nstat /Mapp\nas 7 7\nopen /size1.bin r\nls /Mapp\n"
                       "as 7 6\nopen /size1.bin r\nls /Mapp\nas 5 1\nopen /ro.txt r\nquit\n")
    check("options: owner, group and modes", re.search(r"mode 100640 size 1 links 1 uid 5 gid 6", out) and re.search(r"mode 100440 ", out) and re.search(r"mode 40750 ", out), out)
    lines = out.splitlines()
    o = [l for l in lines if l.startswith("open") or l.startswith("ls")]
    check("... others: EACCES; the group: reads; the owner: reads", o[0] == "open -> -13" and o[1] == "ls -> -13" and re.match(r"open -> \d+", o[2]) and o[3].startswith("ls -> inner.txt Under") and re.match(r"open -> \d+", o[4]), o)
    rc, out = run(img, "mount\nas 7 7\nraw on\nstat /size1.bin\nquit\n")
    check("a user's own IPC is refused", "stat -> -13" in out, out)

    # inode numbers: the same for stat, open and the listing; two open files differ; fstat; close
    rc, out = run(img, "mount\nstat /size1.bin\nopen /size1.bin r\nfstat %1\nopen /size4096.bin r\nopen /Mapp r\nfstat %3\nclose %1\nclose %2\nclose %3\nquit\n")
    inos = re.findall(r"ino (\d+)", out)
    opens = re.findall(r"open -> (\d+)", out)
    check("inode numbers: stat, open and fstat agree; two files differ", len(inos) == 3 and inos[0] == inos[1] == opens[0] and opens[0] != opens[1], out)
    check("... a directory opened: fstat says directory", re.search(r"fstat -> 0 ino \d+ mode 40755", out), out)

    # speed: whole sectors straight to the caller, a contiguous run as one request
    rc, out = run(img, "mount\nopen /size300001.bin r\nreads\nread %1 0 65536\nreads\nread %1 65536 65536\nreads\n"
                       "open /chained.bin r\nread %2 0 4096\nreads\nread %2 4096 65536\nreads\nopen /frag.bin r\nread %3 0 512\nreads\nread %3 512 65536\nreads\n"
                       "read %1 100 300\nreads\nread %1 400 300\nreads\nquit\n")
    r = [tuple(map(int, x)) for x in re.findall(r"reads -> (\d+) (\d+)", out)]
    d = [(r[i + 1][0] - r[i][0], r[i + 1][1] - r[i][1]) for i in range(len(r) - 1)]
    check("speed: a 64 KB read of a contiguous file is one request of 128 sectors", len(d) >= 2 and d[0] == (1, 128) and d[1] == (1, 128), d)
    check("... of a FAT chain that is one run: one request (its FAT sector cached)", len(d) >= 4 and d[3] == (1, 128), d)
    check("... of a fragmented chain: one request per run (and one for a FAT sector)", len(d) >= 6 and 1 < d[5][0] <= 16 and d[5][1] in (128, 129), d)
    check("... small reads through the read-ahead window: the second from memory", len(d) >= 8 and d[6][0] == 1 and d[7] == (0, 0), d)

    rc, out = run(img, "mount\nlsat /Lista 0\nlsat /Lista 9716\nlsat /Lista 10236\nlsat /Lista 2147483647\nlsat /Lista 32\nlsat /Lista 2147483616\nquit\n")
    lsl = [l for l in out.splitlines() if l.startswith("lsat")]
    check("a listing's place a program seeks to: not a multiple of 32 EINVAL (never read inside an entry); on a set's "
          "secondary entry the next set; past the end the end", len(lsl) == 6 and lsl[0] == "lsat -> 1 fil000.txt" and
          lsl[1:4] == ["lsat -> -22 "] * 3 and lsl[4] == "lsat -> 1 fil001.txt" and lsl[5] == "lsat -> 0 ", lsl)
    longc = os.path.join(T, "long.img")
    mkfs(longc, 64 << 20)
    ldata = pat(40 << 20, 40)
    ei.build(longc, [("long.bin", ldata, {"nofat": False, "fragment": True})])
    sc = "mount\nopen /long.bin r\nread %1 31457280 5000\nreads\n" + "".join(f"read %1 {31457280 + 5000 * i} 5000\n" for i in range(1, 21)) + "reads\ndump %1 " + os.path.join(T, "long.out") + "\nquit\n"
    rc, out = run(longc, sc, timeout=300)
    r = [tuple(map(int, x)) for x in re.findall(r"reads -> (\d+) (\d+)", out)]
    check("a 40 MB fragmented chain: 20 small reads in order near its end cost a few requests each, not a walk from the start",
          len(r) == 2 and r[1][0] - r[0][0] <= 60, r)
    check("... and the whole file reads right", os.path.exists(os.path.join(T, "long.out")) and open(os.path.join(T, "long.out"), "rb").read() == ldata)

    # ---- other geometries: 512-byte clusters, 32 MB clusters, 4096-byte sectors ----
    for name, size, opts in (("512-byte clusters", 32 << 20, ("-c", "512")), ("32 MB clusters", 1 << 30, ("-c", "32M")),
                             ("4096-byte sectors", 64 << 20, ("-s", "4096"))):
        g = os.path.join(T, "geo.img")
        mkfs(g, size, *opts)
        v = ei.Volume(g); cb = v.cb; v.f.close()
        it = [("a.bin", pat(3 * cb + 5, 1), {}), ("b.bin", pat(2 * cb + 7, 2), {"nofat": False, "fragment": True}),
              ("D", None, {"nofat": False}), ("D/c.txt", b"c\n", {}), ("D/d.bin", pat(cb + 1, 3), {"vdl": 100})]
        if cb <= 4096:
            it += [(f"L/f{i}.txt", b"%d\n" % i, {}) for i in range(60)]
        ei.build(g, it)
        ok, why = fsck(g)
        check(f"{name}: fsck.exfat clean", ok, why)
        compare(name, g, it)

    # ---- a 128 GB volume (sparse): 128 KB clusters, files at its far end and 32 GB apart, 4 GB - 1 and 5 GB ----
    big = os.path.join(T, "big.img")
    mkfs(big, 128 << 30)
    v = ei.Volume(big); nclus = v.nclus; cb = v.cb; v.f.close()
    per32 = (32 << 30) // cb
    tail = pat(70000, 99)
    LN = "a long name that is well past the sixty-three bytes a listing can hold.txt"
    it = [("Near", None, {"at": 1000}), ("Near/near.txt", b"near\n", {}), ("Near/" + LN, b"n\n", {}),
          ("Far", None, {"at": 1000 + per32}), ("Far/far.txt", b"far\n", {}), ("Far/" + LN, b"f\n", {}),
          ("end.bin", tail, {"at": nclus - 2}),
          ("max.bin", b"", {"size": 0xFFFFFFFF, "at": 3000}), ("huge.bin", b"", {"size": 5 << 30, "at": 50000})]
    ei.build(big, it)
    ok, why = fsck(big)
    check("128 GB: fsck.exfat clean", ok, why)
    # the last bytes of max.bin: written straight into the image (its data is otherwise sparse: zeros)
    v = ei.Volume(big)
    rootd = b"".join(v.rd(v.cl(c), v.cb) for c in v.root_chain)
    def first_of(name):
        u = name.encode("utf-16-le")
        i = rootd.index(b"\xC1\x00" + u[:30])
        return struct.unpack_from("<I", rootd, i - 32 + 20)[0]
    mfirst = first_of("max.bin")
    v.wr(v.cl(mfirst) + 0xFFFFFFFF - 16, b"the last bytes!!")
    v.wr(v.cl(mfirst), b"HELLO-START-OF-MAX")
    v.f.close()
    rc, out = run(big, "mount\nstat /end.bin\nopen /end.bin r\ndump %1 " + os.path.join(T, "end.out") + "\nstat /max.bin\nopen /max.bin r\n"
                       "read %2 4294967279 100\nread %2 4294967295 10\nstat /huge.bin\nopen /huge.bin r\nls /\n"
                       "stat /Near/near.txt\nstat /Far/far.txt\nopen /Near/near.txt r\nopen /Far/far.txt r\nstatfs\nquit\n", timeout=300)
    check("128 GB: mounts", "mount -> 0 readonly" in out, out[:300])
    check("... a file in its last clusters reads right", os.path.exists(os.path.join(T, "end.out")) and open(os.path.join(T, "end.out"), "rb").read() == tail, out[:400])
    rc, o2 = run(big, "mount\nopen /end.bin r\nread %1 1 30\nopen /max.bin r\nread %2 0 18\nread %2 18 10\nquit\n")
    check("... a file of 4 GB - 1 read from its start in small pieces after another file's (the window's length right)",
          "read -> 18 HELLO-START-OF-MAX" in o2, o2)
    check("... a file of 4 GB - 1: its size, and its last bytes at offset 4 GB - 17", "size 4294967295 " in out and "read -> 16 the last bytes!!" in out and "read -> 0" in out, out)
    check("... a file of 5 GB: listed, stat says 4294967295 (capped, as pfs3d and NFS), open answers EFBIG",
          "huge.bin" in out and out.count("size 4294967295 ") == 2 and "open -> -27" in out, out)
    sts = re.findall(r"stat -> 0 ino (\d+)", out)
    ops = re.findall(r"open -> (\d+)", out)
    check("... two files 32 GB apart: their places wrap to one number for stat, two numbers when open", len(sts) >= 5 and sts[3] == sts[4] and len(ops) >= 4 and ops[2] != ops[3], (sts, ops))
    rc, o3 = run(big, "mount\nopen /Near/" + LN.replace(" ", "+") + " r\nls /Far\nls /Far\nquit\n")
    al = re.findall(r"(a long name that.*?~[0-9a-f]+)", o3)
    ok3 = len(al) == 2 and al[0] == al[1]
    if ok3:
        rc, o4 = run(big, "mount\nopen /Near/" + LN.replace(" ", "+") + " r\nstat /Far/" + al[0].replace(" ", "+") + "\nopen /Far/" + al[0].replace(" ", "+") + " r\nread %2 0 5\nquit\n")
        ok3 = "stat -> 0 ino" in o4 and "read -> 2 f" in o4
    check("... a long name whose place number an open file holds: its ~ name stays the same and opens it", ok3, (al, o3[-400:]))
    m = re.search(r"statfs -> 0 bsize 131072 blocks (\d+) bfree (\d+)", out)
    check("... statfs over a million clusters", m and int(m.group(1)) == nclus and nclus - int(m.group(2)) > 41000, m and m.groups())

    # ---- refused at the mount ----
    base = bytearray(open(img, "rb").read())
    def refuse(name, data, why, ok=False):
        p = os.path.join(T, "refuse.img")
        open(p, "wb").write(data)
        rc, out = run(p, "mount\nls /\nquit\n")
        if ok:
            check(f"mounts: {name}", "mount -> 0" in out and why in out and rc == 0, out[-400:])
        else:
            check(f"refused: {name}", "mount -> -1" in out and why in out and rc == 0, out[-400:])
    def bootsum(e, base_s=0):
        s = 0
        for i in range(11 * 512):
            if i in (106, 107, 112):
                continue
            s = ((s >> 1) | ((s & 1) << 31)) + e[base_s * 512 + i] & 0xFFFFFFFF
        e[(base_s + 11) * 512:(base_s + 12) * 512] = struct.pack("<I", s) * 128
    def patched(fn):
        """FN applied to both boot regions (else the backup is used, rightly), each with its checksum made to match"""
        e = bytearray(base)
        for b0 in (0, 12):
            piece = bytearray(e[b0 * 512:(b0 + 1) * 512]); fn(piece); e[b0 * 512:(b0 + 1) * 512] = piece
            bootsum(e, b0)
        return e
    refuse("zeros (not exFAT)", bytes(len(base)), "not an exFAT volume")
    refuse("too small", base[:512 * 1000], "too small")
    refuse("revision 2.0", patched(lambda e: e.__setitem__(105, 2)), "revision")
    refuse("4096 << 1 byte sectors", patched(lambda e: e.__setitem__(108, 13)), "out of range")
    refuse("clusters over 32 MB", patched(lambda e: e.__setitem__(109, 17)), "out of range")
    refuse("a FAT running into the clusters", patched(lambda e: struct.pack_into("<I", e, 84, 0x10000)), "FAT")
    refuse("more clusters than the volume holds", patched(lambda e: struct.pack_into("<I", e, 92, 16000)), "run past its end")
    refuse("longer than the device", patched(lambda e: struct.pack_into("<Q", e, 72, len(base) // 512 + 1)), "past the device")
    refuse("a root cluster out of range", patched(lambda e: struct.pack_into("<I", e, 96, 1)), "root directory cluster")
    refuse("non-zero must-be-zero bytes", patched(lambda e: e.__setitem__(20, 1)), "must-be-zero")
    e = bytearray(base); e[100] ^= 1                      # the serial changed: the checksum no longer matches; backup good
    refuse("a bad main boot checksum, the backup good: mounts from the backup", e, "backup boot region is used", ok=True)
    e[12 * 512 + 100] ^= 1
    refuse("both boot regions bad", e, "checksum")
    v = ei.Volume(img); rootoff = v.cl(v.root_chain[0]); fatoff = v.fat; v.f.close()
    def root_type(t):
        def f(e):
            for o in range(rootoff, rootoff + 4096, 32):
                if e[o] == t:
                    e[o] = 0x84 if t else 0
                    return
        return f
    e = bytearray(base); root_type(0x81)(e); refuse("an unknown critical entry in the root (spec 8.2)", e, "unknown")
    e = bytearray(base)
    for o in range(rootoff, rootoff + 4096, 32):
        if e[o] == 0x81:
            e[o] = 0x01                                   # (unused: the bitmap is gone)
    refuse("no allocation bitmap", e, "no allocation bitmap")
    rc0 = struct.unpack_from("<I", base, 96)[0]
    e = bytearray(base); struct.pack_into("<I", e, fatoff + rc0 * 4, rc0); refuse("a root directory chain that loops", e, "root directory's chain")

    # ---- aimed damage: errors, never a crash, never another file's data ----
    def find_set(e, dirclus_list, name):
        u = name.encode("utf-16-le")[:30]
        for c in dirclus_list:
            o0 = rootoff if c is None else c
            for o in range(o0, o0 + 4096 * 8, 32):
                if e[o] == 0xC1 and e[o + 2:o + 2 + len(u)] == u and e[o - 64] == 0x85:
                    return o - 64
        return None
    s300 = find_set(base, [None], "size300001.bin")
    sfrag = find_set(base, [None], "frag.bin")
    fragdata = expect_of(items)["/frag.bin"]
    def damaged(name, patch, script=None, expect=r"(-5|! )", datacheck=True):
        e = bytearray(base)
        patch(e)
        p = os.path.join(T, "dmg.img")
        open(p, "wb").write(e)
        dout = os.path.join(T, "dmg.out")
        if os.path.exists(dout):
            os.unlink(dout)
        try:
            rc, out = run(p, (script or "mount\ntree /\nls /\nopen /size300001.bin r\nread %1 0 70000\nopen /frag.bin r\nread %2 0 200000\nquit\n").replace(
                "quit\n", ("dumpp /frag.bin " + dout + "\n" if datacheck else "") + "quit\n"), timeout=60)
        except subprocess.TimeoutExpired:
            check(f"damage: {name}: no hang", False)
            return ""
        bad = re.search(r"(AddressSanitizer|runtime error|SUMMARY)", out)
        check(f"damage: {name}: no crash, an error answered", rc == 0 and not bad and re.search(expect, out), out[-600:])
        if datacheck and os.path.exists(dout):
            got = open(dout, "rb").read()
            check(f"damage: {name}: what frag.bin gives is its own data, never another file's", fragdata.startswith(got), len(got))
        return out
    out = damaged("a set's checksum wrong", lambda e: e.__setitem__(s300 + 2, e[s300 + 2] ^ 1),
                  "mount\nls /\nopen /size300001.bin r\nstat /size1.bin\nquit\n")
    check("... the set is not listed or opened, the rest is (the listing ends EIO)", "size300001.bin" not in out.split("ls ->")[1].split("\n")[0] and
          "size1.bin" in out and re.search(r"ls -> .*\(\d+, -5\)", out) and "open -> -5" in out and "stat -> 0" in out, out[-500:])
    damaged("a file's first cluster out of range", lambda e: struct.pack_into("<I", e, s300 + 52, 0x7FFFFFFF) or fix_sum(e, s300))
    damaged("a contiguous file past the heap", lambda e: struct.pack_into("<Q", e, s300 + 56, 0xFFFFF000) or struct.pack_into("<Q", e, s300 + 40, 100) or fix_sum(e, s300))
    damaged("a valid data length past the size", lambda e: struct.pack_into("<Q", e, s300 + 40, 0x7FFFFFFF) or fix_sum(e, s300))
    damaged("a name length of 0", lambda e: e.__setitem__(s300 + 35, 0) or fix_sum(e, s300))
    damaged("a name longer than its entries", lambda e: e.__setitem__(s300 + 35, 200) or fix_sum(e, s300))
    damaged("a secondary count of 1", lambda e: e.__setitem__(s300 + 1, 1) or fix_sum(e, s300))
    damaged("no stream entry", lambda e: e.__setitem__(s300 + 32, 0xC1) or fix_sum(e, s300))
    damaged("a set cut by a primary", lambda e: e.__setitem__(s300 + 64, 0x85) or fix_sum(e, s300))
    fc = struct.unpack_from("<I", base, sfrag + 52)[0]
    nxt = struct.unpack_from("<I", base, fatoff + fc * 4)[0]
    for what, val in (("free", 0), ("bad", 0xFFFFFFF7), ("past the heap", 0x0FFFFFFF), ("cut short (an end mark)", 0xFFFFFFFF)):
        damaged(f"a chain {what}", lambda e, val=val: struct.pack_into("<I", e, fatoff + nxt * 4, val),
                "mount\nopen /frag.bin r\nread %1 0 200000\nread %1 100000 100\nquit\n")
    out = damaged("a chain that loops", lambda e: struct.pack_into("<I", e, fatoff + nxt * 4, fc),
                  "mount\nopen /frag.bin r\nread %1 0 200000\nquit\n", expect=r"read -> \d+", datacheck=False)
    sd = find_set(base, [None], "Kedja")
    damaged("a directory's length not a multiple of 32", lambda e: struct.pack_into("<Q", e, sd + 56, 4097) or struct.pack_into("<Q", e, sd + 40, 4097) or fix_sum(e, sd),
            "mount\nls /Kedja\nopen /Kedja/k001.txt r\nquit\n", expect=r"-5")
    damaged("a directory over 256 MB", lambda e: struct.pack_into("<Q", e, sd + 56, 512 << 20) or struct.pack_into("<Q", e, sd + 40, 512 << 20) or fix_sum(e, sd),
            "mount\nls /Kedja\nquit\n", expect=r"-5")
    dfirst = struct.unpack_from("<I", base, sd + 52)[0]
    damaged("a directory's chain broken", lambda e: struct.pack_into("<I", e, fatoff + dfirst * 4, 0),
            "mount\nls /Kedja\nopen /Kedja/k199.txt r\nquit\n", expect=r"-5")

    # ---- fuzz: random bytes over the boot sector, the FAT in use, the root and the directories ----
    rnd = random.Random(47)
    v = ei.Volume(img)
    hot = [0, 512 * 11, fatoff, fatoff + 512] + [v.cl(c) for c in v.root_chain]
    for p in ("Lista", "Kedja", "Mapp", "Namn"):
        s = find_set(base, [None], p)
        if s is not None:
            hot.append(v.cl(struct.unpack_from("<I", base, s + 52)[0]))
    v.f.close()
    hangs = crashes = 0
    for i in range(ROUNDS):
        e = bytearray(base)
        for _ in range(rnd.randint(1, 12)):
            at = rnd.choice(hot) + rnd.randrange(0, 4096) if rnd.random() < 0.85 else rnd.randrange(0, len(e))
            if at < len(e):
                e[at] = rnd.randrange(256) if rnd.random() < 0.7 else rnd.choice((0, 1, 0xFF, 0x80, 0x85, 0xC0, 0xC1))
        if rnd.random() < 0.5:                            # the boot checksum made to match: the damage gets past it
            bootsum(e)
        if rnd.random() < 0.5:                            # and the sets' checksums: the damage gets inside them
            for o in range(0, len(e) - 32, 32):
                if e[o] == 0x85 and rnd.random() < 0.5:
                    fix_sum(e, o)
        p = os.path.join(T, "fz.img")
        open(p, "wb").write(e)
        try:
            rc, out = run(p, "mount\ntree /\nopen /size300001.bin r\nread %1 1000 70000\nstatfs\nquit\n", timeout=30)
        except subprocess.TimeoutExpired:
            hangs += 1
            shutil.copy(p, os.path.join(T, f"hang{i}.img"))
            continue
        if rc != 0 or re.search(r"(AddressSanitizer|runtime error)", out):
            crashes += 1
            shutil.copy(p, os.path.join(T, f"crash{i}.img"))
            if crashes == 1:
                print(out[-1500:])
    check(f"fuzz: {ROUNDS} damaged volumes, no crash", crashes == 0, crashes)
    check(f"fuzz: ... no hang", hangs == 0, hangs)

    print(f"exfat-host-test: {passes} passed, {fails} failed")
    if fails:
        print("kept for a look:", T)
    else:
        shutil.rmtree(T, ignore_errors=True)
    return 1 if fails else 0


def fix_sum(e, o):
    """the set at byte O of E given a checksum that matches it again (damage aimed past the checksum)"""
    sc = e[o + 1]
    s = ei._sum16(bytes(e[o:o + 32 * (sc + 1)]))
    struct.pack_into("<H", e, o + 2, s)


if __name__ == "__main__":
    sys.exit(main())
