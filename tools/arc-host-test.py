#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# The LhA and LZX readers and their commands on the PC, under ASAN and UBSAN (DESIGN D33): each method and level byte for byte, from archives made with
# LHa as a program and the Amiga's LhA (tools/tests/archives/) and with tools/mkarc.py; the refusals; a fuzz run, which prints its result.
# ZENIX_ARCHIVES=FOLDER: every .lzx, .lha, .lzh under it too, checked by their own checksums (real archives; a line says SKIP without it).
# usage: tools/arc-host-test.py [fuzz-rounds]
import hashlib, importlib.util, os, shutil, stat, subprocess, sys, tempfile
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
spec = importlib.util.spec_from_file_location("mkarc", "tools/mkarc.py")
mk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mk)
ROUNDS = sys.argv[1] if len(sys.argv) > 1 else "4000"
T = tempfile.mkdtemp(prefix="zenix-arc-host.")
ok = True
CF = "-m32 -D_FILE_OFFSET_BITS=64 -DZHOST -g -O1 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -fsanitize=address,undefined -fno-sanitize-recover=undefined -Iports/ahiru/upstream/core -Iuser".split()
SRC = ["user/arcread.c", "user/arclha.c", "user/arclzx.c", "ports/ahiru/upstream/core/arc.c"]


def check(name, good, detail=""):
    global ok
    print(("PASS " if good else "FAIL ") + name + ("" if good else " (%s)" % (detail,)))
    if not good:
        ok = False


def build(out, main, extra=()):
    r = subprocess.run(["gcc", *CF, *extra, main, *SRC, "-o", out], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return r.returncode, r.stdout


rc, log = build(T + "/unlha", "user/unlha.c")
if rc:
    # -m32 may be missing: the host's own width
    CF = [c for c in CF if c != "-m32"]
    rc, log = build(T + "/unlha", "user/unlha.c")
check("the command builds with no warning", rc == 0 and "warning" not in log, log[-800:])
if rc:
    sys.exit(1)
os.symlink("unlha", T + "/unlzx")
UN = {".lzx": T + "/unlzx", ".lzh": T + "/unlha", ".lha": T + "/unlha"}


def run(arc, cmd, *args):
    prog = UN[os.path.splitext(arc)[1]]
    r = subprocess.run([prog, cmd, *args, arc], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
    return r.returncode, r.stdout.decode("utf-8", "replace")


def tree(d):
    out = {}
    for base, _, files in os.walk(d):
        for f in files:
            p = os.path.join(base, f)
            out[os.path.relpath(p, d)] = open(p, "rb").read()
    return out


txt, mix, noise = mk.payload("text"), mk.payload("mixed"), mk.payload("noise")
SMALL_TXT, SMALL_NOISE = txt[:6000], noise[:800]
A = "tools/tests/archives"

print("--- what LHa and the Amiga's LhA wrote, every method and level")
for m in ("lh0", "lh1", "lh5", "lh6", "lh7"):
    for lev in (0, 1, 2):
        f = "%s/%s-%d.lzh" % (A, m, lev)
        d = T + "/x"
        shutil.rmtree(d, ignore_errors=True)
        os.mkdir(d)
        rc, o = run(f, "x", "-q", "-d", d)
        t = tree(d)
        check("-%s- level %d: unpacked byte for byte" % (m, lev), rc == 0 and t.get("text.txt") == SMALL_TXT and t.get("sub/noise.bin") == SMALL_NOISE and t.get("näme.txt") == b"svenska\n", (rc, o[-200:], sorted(t)))
rc, o = run(A + "/lh1-rescale.lzh", "t", "-q")
check("-lh1-: past the tree's limit (its counts halved at least once)", rc == 0, (rc, o[-200:]))
for lev in (0, 1, 2):
    f = "%s/amiga-%d.lha" % (A, lev)
    rc, o = run(f, "l")
    check("the Amiga's LhA, level %d: protection bits, a filenote" % lev, rc == 0 and " h---rwed " in o and "hsparwed" in o and " ----r-ed " in o and " ----rwe- " in o and " ----rw-d " in o and ": a note here" in o and "emptyd/" in o and "sub/deep/x.txt" in o, o[-300:])
    rc, o = run(f, "t", "-q")
    check("... its data", rc == 0, o[-200:])

print("--- the test set of tools/mkarc.py")
S = T + "/set"
cases = mk.write_set(S)
EXPECT = {"ok": 0, "refused": 1, "checksum": 1, "damaged": 2, "not supported": 1, "does not read": 2, "over the limit": 2}
for fn, ex in cases:
    arc = S + "/" + fn
    rc, o = run(arc, "t")
    d = T + "/x"
    shutil.rmtree(d, ignore_errors=True)
    os.mkdir(d)
    rx, ox = run(arc, "x", "-q", "-d", d)
    t = tree(d)
    want = EXPECT[ex]
    if fn in ("short.lzh", "regroup.lzx"):
        want = 1            # the member is damaged, the archive's structure is whole
    words = ex == "ok" or ex in o
    if fn == "empties.lzh":
        o = ""
    check("%s: exit %d, %s" % (fn, want, ex), rc == want and rx == want and words, (rc, rx, o[-200:]))
    outside = [p for p in os.listdir(T) if "escaped" in p] + [p for p in t if "escaped" in p]
    check("%s: nothing written outside" % fn, not outside, outside)
    if ex == "refused":
        check("%s: the good member unpacked, the refused one not" % fn, t.get("good.txt") == b"good\n" and "evil" not in str(t.values()) and len(t) == 1, sorted(t))
    if ex == "checksum":
        check("%s: what failed does not stay" % fn, "bad.txt" not in t and t.get("good.txt") == b"good\n", sorted(t))
    if ex == "over the limit":
        check("%s: nothing unpacked" % fn, not t, sorted(t))
d = T + "/x"
shutil.rmtree(d, ignore_errors=True)
os.mkdir(d)
rc, o = run(S + "/all.lzx", "x", "-q", "-d", d)
t = tree(d)
good = t.get("stored.txt") == txt and t.get("normal.txt") == txt and t.get("aligned.bin") == mix and t.get("merged/a.txt") == txt[:9000] and t.get("merged/b.bin") == mix[:16000] and t.get("merged/c.bin") == noise and t.get("second/z.bin") == mk.payload("zeros") and t.get("note.txt") == b"with a note\n" and t.get("räksmörgås.txt") == b"svenska\n"
check("LZX: stored, normal, aligned, merged groups, a Swedish name: byte for byte", rc == 0 and good, (rc, o[-200:], sorted(t)))
check("LZX: a directory", os.path.isdir(d + "/empty"))
rc, o = run(S + "/all.lzx", "l")
check("LZX: the filenote listed", ": a filenote" in o and "räksmörgås.txt" in o, o[-400:])
for m in (4, 5, 6, 7):
    for lev in (0, 1, 2):
        f = "%s/lh%d-%d.lzh" % (S, m, lev)
        d = T + "/x"
        shutil.rmtree(d, ignore_errors=True)
        os.mkdir(d)
        rc, o = run(f, "x", "-q", "-d", d)
        t = tree(d)
        check("-lh%d-, level %d: byte for byte" % (m, lev), rc == 0 and t.get("text.txt") == txt and t.get("mixed.bin") == mix, (rc, o[-200:]))
d = T + "/x"
shutil.rmtree(d, ignore_errors=True)
os.mkdir(d)
rc, o = run(S + "/mixed.lzh", "x", "-q", "-d", d)
t = tree(d)
check("LhA: stored, -lz4-, a directory field, a Latin-1 name to UTF-8, a backslash", rc == 0 and t.get("stored.txt") == txt and t.get("dir/deep/inner.txt") == b"inner\n" and t.get("räksmörgås.txt") == b"svenska\n" and t.get("note.txt") == b"with a note\n" and t.get("win/path.txt") == b"x\n", (rc, o[-200:], sorted(t)))
check("LhA: a -lhd- member is a folder", os.path.isdir(d + "/dir/deep"))
for f, names in ((S + "/mixed.lzh", ("stored.txt", "note.txt")),):
    d2 = T + "/y"
    shutil.rmtree(d2, ignore_errors=True)
    os.mkdir(d2)
    r = subprocess.run([T + "/unlha", "x", "-q", "-d", d2, f, "note.txt", "dir", "nothere"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    t = tree(d2)
    check("names chosen: a file, a folder's members; one not there says so with exit 1", r.returncode == 1 and sorted(t) == ["dir/deep/inner.txt", "note.txt"] and "nothere: not in the archive" in r.stdout, (r.returncode, r.stdout, sorted(t)))

print("--- found by the review")
import time
t0 = time.time()
rc, o = run(S + "/regroup.lzx", "t", "-q")
dt = time.time() - t0
check("a damaged group: the members after the damage answer at once (one decode of 300 KB, not 400; %.1f s under the sanitizers)" % dt, rc == 1 and dt < 8 and o.count("damaged") >= 400, (rc, dt, o[-200:]))
d = T + "/x"
shutil.rmtree(d, ignore_errors=True)
os.mkdir(d)
rc, o = run(S + "/storedgroup.lzx", "x", "-q", "-d", d)
t = tree(d)
check("stored members in a merged group", rc == 0 and t.get("a.txt") == b"first\n" and t.get("b.txt") == b"second\n", (rc, o, sorted(t)))
rc, o = run(S + "/midzero.lzh", "l")
check("a zero byte does not hide the rest: said, exit 2", rc == 2 and "damaged" in o, (rc, o))
rc, o = run(S + "/padded.lzh", "t", "-q")
check("padding after the end mark is fine", rc == 0, (rc, o))
rc, o = run(S + "/c1.lzx", "l")
check("U+009B in a name does not reach the terminal", rc == 0 and "\u009b" not in o and "x_2Jy" in o, ascii(o))
d = T + "/x"
shutil.rmtree(d, ignore_errors=True)
os.mkdir(d)
rc, o = run(S + "/d2040.lzx", "x", "-q", "-d", d)
check("a date in 2040: no overflow, kept", rc == 0 and abs((os.stat(d + "/later.txt").st_mtime % 2**32) - 2222294400) < 86400 * 400, (rc, o, os.path.exists(d + "/later.txt") and os.stat(d + "/later.txt").st_mtime))
for f in ("dotdir.lzh", "dotdir.lzx"):
    d = T + "/x"
    shutil.rmtree(d, ignore_errors=True)
    os.mkdir(d)
    rc, o = run(S + "/" + f, "x", "-q", "-d", d)
    check(f + ': "./" is nothing to do, not a refusal', rc == 0 and tree(d).get("a.txt") == b"hello\n", (rc, o))
d = T + "/x"
shutil.rmtree(d, ignore_errors=True)
os.mkdir(d)
r = subprocess.run([T + "/unlzx", "x", "-q", "-d", d, S + "/all.lzx", "merged/c.bin", "merged", "merged/c.bin"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
check("names that overlap: all found", r.returncode == 0 and "not in the archive" not in r.stdout and tree(d).get("merged/c.bin") == noise, (r.returncode, r.stdout))
d = T + "/x"
shutil.rmtree(d, ignore_errors=True)
os.mkdir(d)
rc, o = run(S + "/prezero.lzx", "x", "-q", "-d", d)
check("a distance before the group's start reads zeros", rc == 0 and tree(d).get("p.bin") == b"A\0\0\0\0", (rc, o))
rc, o = run(S + "/junk.lzh", "l")
check("LhA: junk after the end mark is fine", rc == 0 and "a.txt" in o, (rc, o))
t0 = time.time()
rc, o = run(S + "/empties.lzh", "t", "-q")
check("3000 empty -lh7- members", rc == 0, (rc, o[-100:]))
big = mk.LZX_HEAD + mk.lzx_header(b"huge.bin", 0xE0000000, 0, 2, True, 0) + mk.lzx_header(b"last", 1, 8, 2, True, 0) + bytes(8)
open(S + "/skipcost.lzx", "wb").write(big)
r = subprocess.run([T + "/unlzx", "t", S + "/skipcost.lzx", "last"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60)
check("the limit counts a group's bytes before the member asked for", r.returncode == 2 and "over the limit" in r.stdout, (r.returncode, r.stdout))

print("--- on the disk: overwriting, links, modes, dates")
d = T + "/z"
os.mkdir(d)
a = S + "/m.lzh"
open(a, "wb").write(mk.lha_member(b"f.txt", b"new\n", level=1, attr=0x00) + mk.lha_member(b"via/f2.txt", b"new2\n") + mk.lha_member(b"ro.txt", b"ro\n", attr=0x05) + b"\0")
open(d + "/f.txt", "w").write("old\n")
rc, o = run(a, "x", "-q", "-d", d)
check("nothing overwritten without -f", rc == 1 and open(d + "/f.txt").read() == "old\n" and "already there" in o, (rc, o))
rc, o = run(a, "x", "-q", "-f", "-d", d)
check("-f overwrites", rc == 0 and open(d + "/f.txt").read() == "new\n", (rc, o))
real = T + "/elsewhere"
os.mkdir(real)
d = T + "/z2"
os.mkdir(d)
os.symlink(real, d + "/via")
open(T + "/victim", "w").write("safe\n")
os.symlink(T + "/victim", d + "/f.txt")
rc, o = run(a, "x", "-q", "-d", d)
check("a soft link on the way is not followed", rc == 1 and not os.listdir(real) and "not a folder" in o, (rc, o))
check("a soft link in place is not written through", open(T + "/victim").read() == "safe\n" and "already there" in o, o)
rc, o = run(a, "x", "-q", "-f", "-d", d)
check("... with -f neither: the link goes, a file comes", open(T + "/victim").read() == "safe\n" and os.path.isfile(d + "/f.txt") and not os.path.islink(d + "/f.txt"), (rc, o))
m = stat.S_IMODE(os.stat(d + "/ro.txt").st_mode)
check("the protection bits to a mode: rwed less w, d is r-x", m == 0o555, oct(m))
m = stat.S_IMODE(os.stat(d + "/f.txt").st_mode)
check("... rwed is rwxr-xr-x", m == 0o755, oct(m))
check("the date kept", os.stat(d + "/f.txt").st_mtime > 1.5e9, os.stat(d + "/f.txt").st_mtime)
rc, o = run(S + "/bomb.lzx", "t", "-L", "4000")
check("-L lets a large archive through to the reader, which says it is damaged", rc != 0 and "limit" not in o, (rc, o[-200:]))
r = subprocess.run([T + "/unlha", "q", "x"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
check("a bad command: usage, exit 2", r.returncode == 2 and "usage" in r.stdout, r.stdout)
r = subprocess.run([T + "/unlha", "t", T + "/none.lha"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
check("no such archive: exit 2", r.returncode == 2, r.stdout)

print("--- truncated at every length: never a crash, always an error")
seeds = [S + "/all.lzx", S + "/lh5-1.lzh", S + "/lh7-2.lzh", S + "/mixed.lzh", A + "/lh1-1.lzh", A + "/amiga-0.lha"]
for f in seeds:
    data = open(f, "rb").read()
    step = max(1, len(data) // 150)
    bad = []
    for cut in list(range(0, min(300, len(data)), 7)) + list(range(0, len(data), step)):
        c = T + "/cut" + os.path.splitext(f)[1]
        open(c, "wb").write(data[:cut])
        rc, o = run(c, "t", "-q")
        if rc not in (0, 1, 2) or "Sanitizer" in o or "runtime error" in o:
            bad.append((cut, rc))
    check(os.path.basename(f) + ": cut anywhere", not bad, bad[:5])

print("--- fuzz, ASAN and UBSAN")
rc, log = build(T + "/fuzz", "tools/host/arc-fuzz.c")
check("the fuzzer builds", rc == 0, log[-500:])
fuzzseeds = [S + "/" + f for f, ex in cases if ex in ("ok", "checksum")] + [os.path.join(A, f) for f in sorted(os.listdir(A))]
fuzzseeds = [f for f in fuzzseeds if os.path.getsize(f) < 120000 and not f.endswith("empties.lzh")]
for label, ext in (("LZX", (".lzx",)), ("LhA", (".lzh", ".lha"))):
    sel = [f for f in fuzzseeds if f.endswith(ext)]
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:allocator_may_return_null=1")
    r = subprocess.run([T + "/fuzz", ROUNDS, "1", T + "/hang-" + label, *sel], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env, timeout=1800)
    line = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else ""
    print("    " + line)
    check("fuzz %s: completed, no crash or hang" % label, r.returncode == 0 and "no crash, no hang" in line, r.stdout[-1500:])

print("--- the readers hold no copy of another program")
for f in ("user/arcread.c", "user/arclha.c", "user/arclzx.c", "user/arcread.h", "ports/ahiru/upstream/core/lha.h", "ports/ahiru/upstream/core/lzx.h"):
    check(f + ": GPL-2.0-only", "SPDX-License-Identifier: GPL-2.0-only" in open(f).read())
check("the old readers are gone", not os.path.exists("ports/ahiru/upstream/core/lha.c") and not os.path.exists("ports/ahiru/upstream/core/lzx.c"))

print("--- real archives (ZENIX_ARCHIVES)")
R = os.environ.get("ZENIX_ARCHIVES")
if not R or not os.path.isdir(R):
    print("SKIP no ZENIX_ARCHIVES: real archives not tried")
else:
    n = bad = 0
    fails = []
    for base, _, files in os.walk(R):
        for f in files:
            e = os.path.splitext(f)[1].lower()
            if e not in UN:
                continue
            p = os.path.join(base, f)
            if open(p, "rb").read(3) != b"LZX" and e == ".lzx":
                continue            # a file only named so
            n += 1
            c = T + "/real" + e
            if os.path.lexists(c):
                os.remove(c)
            os.symlink(p, c)
            rc, o = run(c, "t", "-q")
            if rc:
                bad += 1
                fails.append((f, rc, o[-120:]))
    print("    %d archives, %d failed" % (n, bad))
    check("real archives: every checksum right", n > 0 and bad == 0, fails[:6])

shutil.rmtree(T, ignore_errors=True)
print("arc-host-test: " + ("ok" if ok else "FAILED"))
sys.exit(0 if ok else 1)
