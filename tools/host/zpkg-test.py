#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""zpkg's PC-side tests (DESIGN D22 8.1), run by tools/host/zpkg-test.sh:

  zpkg-test.py repo OUT CORETEST           tools/mkrepo.py and tools/mkzup.py's D22 additions on synthetic builds:
                                           3.4's version rules, serials, latest, promote, snapshot, prune, check,
                                           installdb, the test key refused for release channels, two runs byte for
                                           byte; every list also through the C parser (CORETEST index)
  zpkg-test.py differential OUT CORETEST N the C list parser against mkrepo's Python one on N damaged lists
  zpkg-test.py slot OUT CORETEST           the update slot's walk and write on tools/mkffs.py images (DOS\\1, DOS\\3)
  zpkg-test.py net OUT CLIENT              (inside unshare -rnm, lo up) the HTTP core and the -Sy/-Sw order against
                                           tools/host/http-test-server.py: needs OUT/repo from the repo part
Work files in OUT. Every check prints PASS or FAIL; the exit status is the number of FAILs (capped at 100).
"""
import hashlib
import os
import random
import shutil
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
sys.path.insert(0, TOOLS)
fails = 0


def ok(name, cond, detail=""):
    global fails
    if cond:
        print("PASS zpkg-test.py: %s" % name)
    else:
        fails += 1
        print("FAIL zpkg-test.py: %s%s" % (name, (": " + str(detail)[-600:]) if detail else ""))


def run(*a, env=None, cwd=None):
    r = subprocess.run([str(x) for x in a], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, cwd=cwd)
    return r.returncode, r.stdout.decode(errors="replace")


def py(tool, *a, env=None):
    return run(sys.executable, "-I", os.path.join(TOOLS, tool), *a, env=env)


# ---- synthetic builds ----
def mkbuild(d, vnum, kernel=b"kernel one", files=None, extra=()):
    """a build directory as the Makefile leaves it: kernel.bin (with its ZVER record) and pkg/ from tools/mkzup.py.
    files: package -> content of its one program; extra: (name, mkzup pkg arguments) added after man"""
    files = dict({"zup": b"zup 1", "base": b"init 1", "awk": b"awk 1", "man": b"page 1"}, **(files or {}))
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(os.path.join(d, "src"))
    open(os.path.join(d, "kernel.bin"), "wb").write(
        b"\x60\x00" + kernel + b"\0" * 6 + b"ZVER" + struct.pack(">I", vnum) + b"0.3".ljust(16, b"\0") + b"tail" * 40)
    for n, c in files.items():
        open(os.path.join(d, "src", n), "wb").write(c)
    p = os.path.join(d, "pkg")
    s = lambda n: os.path.join(d, "src", n)
    v = str(vnum)
    calls = [["zup", "--group", "base", "f:755:0:0:%s:/sbin/zup" % s("zup")],
             ["base", "--group", "base", "--dep", "zup:" + v, "d:755:0:0:/bin", "f:755:0:0:%s:/bin/init" % s("base")],
             ["awk", "--dep", "base:" + v, "--desc", "pattern scanning and text processing (awk)",
              "f:755:0:0:%s:/bin/awk" % s("awk")],
             ["man", "f:644:0:0:%s:/usr/man/awk.txt" % s("man")]]
    names = [c[0] for c in calls]
    for n, args in extra:
        open(os.path.join(d, "src", n), "wb").write(files.get(n, n.encode()))
        calls.append([n] + [x.replace("@", s(n)).replace("VNUM", v) for x in args])
        names.append(n)
    for c in calls:
        rc, out = py("mkzup.py", "pkg", p, c[0], v, "0.3", *c[1:])
        if rc:
            raise SystemExit("mkbuild: %s" % out)
    open(os.path.join(p, "list"), "w").write(" ".join(names) + "\n")
    return d


def tree(d):
    out = {}
    for root, _, fs in os.walk(d):
        for f in fs:
            p = os.path.join(root, f)
            out[os.path.relpath(p, d)] = open(p, "rb").read()
    return out


def latest(repo, ch):
    return int(open(os.path.join(repo, ch, "latest")).read().split()[2])


def index(repo, ch):
    import mkrepo
    return mkrepo.parse_index(open(os.path.join(repo, ch, "index-%d" % latest(repo, ch)), "rb").read())


def part_repo(out, core):
    import mkzup
    keys = os.path.join(out, "sign-test")
    env = dict(os.environ, ZSIGN_TEST_DIR=keys)
    rc, msg = py("zsign.py", "testkey", env=env)
    ok("zsign.py testkey makes the TEST pair in the work folder (never in git)", rc == 0, msg)
    tkey, tpub = os.path.join(keys, "zenix-test.key"), os.path.join(keys, "zenix-test.pub")
    rkey, rpub = os.path.join(out, "release.key"), os.path.join(out, "release.pub")
    if not os.path.exists(rkey):
        subprocess.run(["openssl", "genpkey", "-algorithm", "ed25519", "-out", rkey], check=True, stderr=subprocess.PIPE)
        os.chmod(rkey, 0o600)
        subprocess.run(["openssl", "pkey", "-in", rkey, "-pubout", "-out", rpub], check=True)

    def mr(*a, key=tkey):
        return py("mkrepo.py", *(list(a) + ["--key", key]), env=env)

    def check(repo, *extra):
        return py("mkrepo.py", "check", repo, "--test-pub", tpub, "--pub", rpub, *extra, env=env)

    b = os.path.join(out, "builds")
    repo = os.path.join(out, "repo")
    shutil.rmtree(repo, ignore_errors=True)
    B1, B2, B3, B4, B5 = 2026102600, 2026102700, 2026102800, 2026102900, 2026103000
    mkbuild(b + "/1", B1)
    rc, msg = mr("add", repo, "test", b + "/1", "--date", "20261026")
    ix = index(repo, "test") if rc == 0 else None
    ok("add: a first build into the test channel", rc == 0 and ix["serial"] == 2026102600 and ix["system"]["vnum"] == B1
       and all(p["vnum"] == B1 for p in ix["pkgs"].values()), msg)
    if not ix:
        return
    ok("the list: channel, date, expires 60 days later, system zenix-VNUM.zup min 0",
       ix["channel"] == "test" and ix["date"] == 20261026 and ix["expires"] == 20261225 and
       ix["system"]["file"] == "zenix-%d.zup" % B1 and ix["system"]["min"] == 0)
    ok("latest is the note's 24-byte line", open(os.path.join(repo, "test", "latest"), "rb").read() ==
       b"zpkglatest 1 2026102600\n")
    pool = sorted(os.listdir(os.path.join(repo, "pool")))
    ok("the pool: one package bundle per non-base package and the system bundle, each with an 80-byte .sig",
       pool == ["awk-%d.zup" % B1, "awk-%d.zup.sig" % B1, "man-%d.zup" % B1, "man-%d.zup.sig" % B1,
                "zenix-%d.zup" % B1, "zenix-%d.zup.sig" % B1] and
       all(os.path.getsize(os.path.join(repo, "pool", f)) == 80 for f in pool if f.endswith(".sig")), pool)
    hdr, parts = mkzup.read_bundle(open(os.path.join(repo, "pool", "awk-%d.zup" % B1), "rb").read())
    ok("a package bundle: no kernel, exactly its manifest and tgz, version = the package's, min = its base",
       [n for n, _ in parts] == ["pkg/awk.manifest", "pkg/awk.tgz"] and hdr["vnum"] == B1 and hdr["min"] == B1 and
       hdr["channel"] == "test")
    hdr, parts = mkzup.read_bundle(open(os.path.join(repo, "pool", "zenix-%d.zup" % B1), "rb").read())
    ok("the system bundle carries the kernel and the base group only (zup, base)",
       [n for n, _ in parts] == ["boot/zenix", "pkg/zup.manifest", "pkg/zup.tgz", "pkg/base.manifest", "pkg/base.tgz"])
    ok("base-group blocks are listed (group base, no file), the others with file lines",
       ix["pkgs"]["base"]["group"] == "base" and ix["pkgs"]["base"]["file"] is None and
       ix["pkgs"]["awk"]["file"]["name"] == "awk-%d.zup" % B1 and
       ix["pkgs"]["awk"]["desc"] == "pattern scanning and text processing (awk)")
    rc, msg = check(repo, "--today", "20261026")
    ok("check: the new repository is sound (signatures by the test key, sizes, SHA-256s, bundles)", rc == 0, msg)

    # an unchanged rebuild (a new build number, the same contents): no new versions, no system update
    mkbuild(b + "/2", B2)
    before = tree(repo)
    rc, msg = mr("add", repo, "test", b + "/2", "--date", "20261027")
    ok("an unchanged rebuild (new number, ZVER masked): nothing changed, nothing written",
       rc == 0 and "nothing changed" in msg and tree(repo) == before, msg)

    # only awk changed: awk gets B3, its depends base and minimum the channel's base (B1)
    mkbuild(b + "/3", B3, files={"awk": b"awk 2"})
    rc, msg = mr("add", repo, "test", b + "/3", "--date", "20261028")
    ix = index(repo, "test")
    hdr, parts = mkzup.read_bundle(open(os.path.join(repo, "pool", "awk-%d.zup" % B3), "rb").read())
    man = mkzup.parse_manifest(dict(parts)["pkg/awk.manifest"])
    ok("a changed package: the build's version, depends base and header minimum rewritten to the base (3.4)",
       rc == 0 and ix["pkgs"]["awk"]["vnum"] == B3 and ix["pkgs"]["awk"]["deps"] == [("base", B1)] and
       hdr["min"] == B1 and man["vnum"] == B3 and man["deps"] == [("base", B1)], msg)
    ok("... the system and the unchanged packages keep their versions and pool files",
       ix["system"]["vnum"] == B1 and ix["pkgs"]["man"]["vnum"] == B1 and ix["pkgs"]["base"]["vnum"] == B1 and
       ix["serial"] == 2026102800)

    # the kernel changed (beyond ZVER): a system update; base-group packages carry B4; awk keeps B3
    mkbuild(b + "/4", B4, kernel=b"kernel two", files={"awk": b"awk 2"})
    rc, msg = mr("add", repo, "test", b + "/4", "--date", "20261029")
    ix = index(repo, "test")
    ok("a changed kernel: the system and every base-group package get the build's version",
       rc == 0 and ix["system"]["vnum"] == B4 and ix["pkgs"]["base"]["vnum"] == B4 and ix["pkgs"]["zup"]["vnum"] == B4
       and ix["pkgs"]["awk"]["vnum"] == B3 and ix["pkgs"]["awk"]["deps"] == [("base", B1)], msg)
    mkbuild(b + "/5", B5, kernel=b"kernel two", files={"awk": b"awk 3"})
    rc, msg = mr("add", repo, "test", b + "/5", "--date", "20261030")
    ix = index(repo, "test")
    ok("a package changed after a system update depends on the new base", rc == 0 and
       ix["pkgs"]["awk"]["deps"] == [("base", B4)] and ix["system"]["vnum"] == B4, msg)
    # only base's own files changed: the system again
    mkbuild(b + "/6", 2026103001, kernel=b"kernel two", files={"awk": b"awk 3", "base": b"init 2"})
    rc, msg = mr("add", repo, "test", b + "/6", "--date", "20261030")
    ix = index(repo, "test")
    ok("a changed base-group package alone is a system update; a second list the same day: serial + 1",
       rc == 0 and ix["system"]["vnum"] == 2026103001 and ix["serial"] == 2026103001 and
       ix["pkgs"]["awk"]["vnum"] == B5, msg)
    rc, msg = check(repo, "--today", "20261030")
    ok("check after six builds", rc == 0, msg)

    # the C parser reads every list mkrepo wrote, with the same answers
    good = True
    for f in sorted(os.listdir(os.path.join(repo, "test"))):
        if f.startswith("index-") and not f.endswith(".sig"):
            import mkrepo
            pix = mkrepo.parse_index(open(os.path.join(repo, "test", f), "rb").read())
            rc, o = run(core, "index", os.path.join(repo, "test", f))
            good &= o.strip() == "ok test %d %d" % (pix["serial"], len(pix["pkgs"]))
    ok("every list mkrepo wrote parses in C with the same serial and package count", good)
    rc, o = run(core, "latest", os.path.join(repo, "test", "latest"))
    ok("latest parses in C", o.strip() == "ok %d" % latest(repo, "test"), o)

    # two runs of the same builds give the same bytes
    repo2 = os.path.join(out, "repo2")
    shutil.rmtree(repo2, ignore_errors=True)
    for i, dt in ((1, "20261026"), (2, "20261027"), (3, "20261028"), (4, "20261029"), (5, "20261030"), (6, "20261030")):
        mr("add", repo2, "test", b + "/%d" % i, "--date", dt)
    ok("two repositories built from the same builds are byte for byte the same", tree(repo) == tree(repo2))

    # dates and serials
    rc, msg = mr("resign", repo, "test", "--date", "20261029")
    ok("a date before the previous list's is refused (a PC with a wrong clock)", rc != 0 and "before" in msg, msg)
    rc, msg = mr("resign", repo, "test", "--date", "20271231")
    ok("a date more than 400 days after the previous list's is refused", rc != 0 and "max-date-gap" in msg, msg)
    rc, msg = mr("resign", repo, "test", "--date", "20271231", "--max-date-gap", "500")
    ok("... unless --max-date-gap allows it", rc == 0, msg)
    shutil.rmtree(repo2)
    for i, dt in ((1, "20261026"), (2, "20261027"), (3, "20261028")):
        mr("add", repo2, "test", b + "/%d" % i, "--date", dt)
    rc, msg = mr("resign", repo2, "test", "--date", "20261029", "--expires-days", "30")
    ix = index(repo2, "test")
    ok("resign: the same files under a new serial, date and expiry (--expires-days 30)",
       rc == 0 and ix["serial"] == 2026102900 and ix["expires"] == 20261128 and ix["pkgs"]["awk"]["vnum"] == B3, msg)
    # a cut run: a list written, latest not; the next one goes above it and leaves it alone
    leftover = os.path.join(repo2, "test", "index-2026102901")
    open(leftover, "wb").write(b"never published\n")
    rc, msg = mr("resign", repo2, "test", "--date", "20261029")
    ok("a list a cut run left unpublished is never overwritten: the next serial is above it",
       rc == 0 and latest(repo2, "test") == 2026102902 and open(leftover, "rb").read() == b"never published\n", msg)
    os.unlink(leftover)
    n = 0
    while n < 101:
        rc, msg = mr("resign", repo2, "test", "--date", "20261029")
        if rc:
            break
        n += 1
    ok("at most 100 lists a day: the serial never exceeds date x 100 + 99",
       rc != 0 and "no serial left" in msg and latest(repo2, "test") == 2026102999, (n, msg))
    ok("every list's .sig sits beside it before latest names it",
       all(os.path.exists(os.path.join(repo2, "test", f + ".sig")) for f in os.listdir(os.path.join(repo2, "test"))
           if f.startswith("index-") and not f.endswith(".sig")))

    # release channels: the test key refused; the pool is shared, promote names the same files
    rel = os.path.join(out, "release-repo")
    shutil.rmtree(rel, ignore_errors=True)
    rc, msg = mr("add", rel, "testing", b + "/1", "--date", "20261026")
    ok("add to testing with the test key is refused, nothing written", rc != 0 and "test key" in msg and
       not os.path.exists(os.path.join(rel, "pool")), msg)
    rc, msg = mr("add", rel, "testing", b + "/1", "--date", "20261026", key=rkey)
    rc2, msg2 = mr("add", rel, "testing", b + "/3", "--date", "20261028", key=rkey)
    tix = index(rel, "testing") if rc == 0 else None
    ok("add to testing with the release key", rc == 0 and rc2 == 0 and tix["channel"] == "testing", msg + msg2)
    rc, msg = mr("promote", rel, "testing", "stable", "--date", "20261101", key=rkey)
    six = index(rel, "stable") if rc == 0 else None
    ok("promote testing -> stable: a stable list of its own serial naming exactly testing's pool files",
       rc == 0 and six["channel"] == "stable" and six["serial"] == 2026110100 and six["system"] == tix["system"] and
       six["pkgs"] == tix["pkgs"], msg)
    rc, msg = mr("promote", rel, "testing", "stable", "--date", "20261101")
    ok("promote with the test key is refused", rc != 0 and "test key" in msg, msg)
    rc, msg = mr("promote", rel, "testing", "test", "--date", "20261101", key=rkey)
    ok("promote across key classes (testing -> test) is refused", rc != 0, msg)
    rc, msg = check(rel, "--today", "20261101")
    ok("check: a stable list naming files whose header says testing is sound (the class rule)", rc == 0, msg)
    rc, msg = py("mkrepo.py", "snapshot", rel, "stable", "2026.11", env=env)
    sd = os.path.join(rel, "snapshots", "2026.11")
    ok("snapshot: a copy of stable's list, its .sig and latest",
       rc == 0 and sorted(os.listdir(sd)) == sorted(["latest", "index-%d" % six["serial"], "index-%d.sig" % six["serial"]])
       and open(os.path.join(sd, "index-%d" % six["serial"]), "rb").read() ==
       open(os.path.join(rel, "stable", "index-%d" % six["serial"]), "rb").read(), msg)
    rc, msg = py("mkrepo.py", "snapshot", rel, "stable", "2026.11", env=env)
    ok("a snapshot name is never reused", rc != 0, msg)
    rc, msg = check(rel, "--today", "20270601")
    ok("check: expired channel lists are named; a snapshot's expiry is not checked",
       rc != 0 and "testing: expired" in msg and "stable: expired" in msg and "snapshots/2026.11: expired" not in msg, msg)
    # the same build in test and testing of one root: the pool names clash, refused
    rc, msg = mr("add", rel, "test", b + "/1", "--date", "20261102")
    ok("a pool file that exists with other bytes is never replaced (test and testing in one root)",
       rc != 0 and "never changed" in msg, msg)

    # prune
    pr = os.path.join(out, "prune-repo")
    shutil.rmtree(pr, ignore_errors=True)
    for i, dt in ((1, "20261026"), (3, "20261028"), (5, "20261030")):
        mr("add", pr, "test", b + "/%d" % i, "--date", dt)
    for i in range(4):
        mr("resign", pr, "test", "--date", "20261031")
    mr("add", pr, "test", b + "/6", "--date", "20261031")
    rc, msg = py("mkrepo.py", "prune", pr, env=env)
    left = sorted(f for f in os.listdir(os.path.join(pr, "pool")) if f.endswith(".zup"))
    lists = sorted(f for f in os.listdir(os.path.join(pr, "test")) if f.startswith("index-") and not f.endswith(".sig"))
    ok("prune keeps what the last two lists name plus every package's previous version, and the last two lists",
       rc == 0 and left == ["awk-2026102800.zup", "awk-2026103000.zup", "man-2026102600.zup", "zenix-2026103000.zup",
                            "zenix-2026103001.zup"] and len(lists) == 2, (left, lists, msg))
    rc, msg = check(pr, "--today", "20261031")
    ok("check after prune", rc == 0, msg)

    # check finds damage
    dmg = os.path.join(out, "damage")

    def damaged(fn):
        shutil.rmtree(dmg, ignore_errors=True)
        shutil.copytree(repo, dmg)
        fn(dmg)
        return check(dmg, "--today", "20261030")

    def flip(path, at=None):
        d = bytearray(open(path, "rb").read())
        d[len(d) // 2 if at is None else at] ^= 1
        open(path, "wb").write(bytes(d))
    cur = "index-%d" % latest(repo, "test")
    for what, fn, want in (
            ("a changed pool file", lambda d: flip(os.path.join(d, "pool", "awk-%d.zup" % B5)), "SHA-256"),
            ("a missing pool file", lambda d: os.unlink(os.path.join(d, "pool", "man-%d.zup" % B1)), "missing"),
            ("a missing .sig", lambda d: os.unlink(os.path.join(d, "pool", "man-%d.zup.sig" % B1)), "80-byte"),
            ("a changed list", lambda d: flip(os.path.join(d, "test", cur), 30), "signature"),
            ("a list signed by an unknown key", lambda d: subprocess.run(
                [sys.executable, "-I", os.path.join(TOOLS, "zsign.py"), "sign", "--key", rkey, "--kind", "index",
                 os.path.join(d, "test", cur)], stdout=subprocess.PIPE, env=env), "signature"),
            ("latest naming a missing list", lambda d: open(os.path.join(d, "test", "latest"), "w").write(
                "zpkglatest 1 2026103099\n"), "No such file"),
            ("a bad latest", lambda d: open(os.path.join(d, "test", "latest"), "w").write("zpkglatest 2 1\n"), "latest")):
        rc, msg = damaged(fn)
        ok("check finds %s" % what, rc != 0 and want in msg, msg)
    rc, msg = check(repo, "--today", "20290101")
    ok("check finds an expired list", rc != 0 and "expired" in msg, msg)

    # mkzup's D22 additions
    pk = os.path.join(out, "pk")
    shutil.rmtree(pk, ignore_errors=True)
    rc, msg = py("mkzup.py", "bundle", os.path.join(pk, "x.zup"), "--no-kernel", os.path.join(b, "6", "pkg"), "awk",
                 "--channel", "testing")
    os.makedirs(pk, exist_ok=True)
    rc, msg = py("mkzup.py", "bundle", os.path.join(pk, "x.zup"), "--no-kernel", "--channel", "testing",
                 os.path.join(b, "6", "pkg"), "awk")
    rc2, msg2 = py("mkzup.py", "list", os.path.join(pk, "x.zup"))
    ok("mkzup bundle --no-kernel: one package, its version, channel, minimum its depends base",
       rc == 0 and rc2 == 0 and "for base 2026103001 or newer" in msg2 and "channel testing" in msg2 and
       "boot/zenix" not in msg2 and msg2.count("pkg/") == 2, msg + msg2)
    rc, msg = py("mkzup.py", "bundle", os.path.join(pk, "y.zup"), "--no-kernel", os.path.join(b, "6", "pkg"), "awk", "man")
    ok("mkzup bundle --no-kernel refuses two packages", rc != 0 and "exactly one" in msg, msg)
    rc, msg = py("mkzup.py", "bundle", os.path.join(pk, "z.zup"), "--kernel", os.path.join(b, "6", "kernel.bin"),
                 "--base-only", os.path.join(b, "6", "pkg"), "zup", "base", "awk")
    ok("mkzup bundle --base-only refuses a non-base package in a system bundle", rc != 0 and "base group only" in msg, msg)
    rc, msg = py("mkzup.py", "bundle", os.path.join(pk, "z.zup"), "--kernel", os.path.join(b, "6", "kernel.bin"),
                 "--base-only", os.path.join(b, "6", "pkg"), "zup", "base")
    ok("... and takes the base group", rc == 0, msg)
    rc, msg = py("mkzup.py", "pkg", pk, "fam", "1", "x", "--desc", "x" * 73, "f:755:0:0:%s:/bin/x" % os.path.join(b, "6", "src", "awk"))
    ok("a desc over 72 characters is refused", rc != 0 and "desc" in msg, msg)
    rc, msg = py("mkzup.py", "pkg", pk, "fam", "1", "x", "--group", "base", "--takes", "utils",
                 "f:755:0:0:%s:/bin/x" % os.path.join(b, "6", "src", "awk"))
    ok("a base-group package never takes another's files", rc != 0 and "takes" in msg, msg)
    rc, msg = py("mkzup.py", "pkg", pk, "fam", "1", "x", "--desc", "the family", "--takes", "utils", "--takes", "man",
                 "f:755:0:0:%s:/bin/x" % os.path.join(b, "6", "src", "awk"))
    m = mkzup.parse_manifest(open(os.path.join(pk, "fam.manifest"), "rb").read()) if rc == 0 else {}
    ok("desc and takes lines are written and read back", rc == 0 and m["desc"] == "the family" and
       m["takes"] == ["utils", "man"], msg)
    rc, msg = py("mkzup.py", "repack", pk, "fam", "/bin/y=%s" % os.path.join(b, "6", "src", "man"))
    m = mkzup.parse_manifest(open(os.path.join(pk, "fam.manifest"), "rb").read())
    ok("repack keeps desc and takes", rc == 0 and m["desc"] == "the family" and m["takes"] == ["utils", "man"], msg)

    # another dependency naming a version the repository did not give: refused, not guessed
    mkbuild(b + "/7", 2026103100, kernel=b"kernel two", files={"awk": b"awk 3", "base": b"init 2"},
            extra=[("tool", ["--dep", "base:VNUM", "--dep", "awk:VNUM", "f:755:0:0:@:/bin/tool"])])
    r3 = os.path.join(out, "repo3")
    shutil.rmtree(r3, ignore_errors=True)
    shutil.copytree(repo, r3)
    rc, msg = mr("add", r3, "test", b + "/7", "--date", "20271231")
    ok("a dependency on an unchanged package at the build's number is refused (D22 3.4 is silent; said)",
       rc != 0 and "tool needs awk 2026103100" in msg, msg)

    # installdb: the repository's manifests for an image
    db = os.path.join(out, "installdb")
    shutil.rmtree(db, ignore_errors=True)
    rc, msg = py("mkrepo.py", "installdb", repo, "test", db, env=env)
    stage = os.path.join(out, "stage")
    shutil.rmtree(stage, ignore_errors=True)
    for path, src in (("/sbin/zup", "6/src/zup"), ("/bin/init", "6/src/base"), ("/bin/awk", "6/src/awk"),
                      ("/usr/man/awk.txt", "6/src/man")):
        os.makedirs(os.path.dirname(stage + path), exist_ok=True)
        shutil.copy(os.path.join(b, src), stage + path)
    rc2, msg2 = py("mkzup.py", "installdb", stage, db, os.path.join(out, "stage-db"))
    am = mkzup.parse_manifest(open(os.path.join(db, "awk.manifest"), "rb").read()) if rc == 0 else {}
    ok("installdb: the repository's manifests (its versions) for an image; mkzup installdb takes them",
       rc == 0 and rc2 == 0 and am["vnum"] == B5 and am["deps"] == [("base", B4)] and
       "awk base man zup" in msg2.replace("package database: ", ""), msg + msg2)


# ---- the C parser against the Python one ----
def part_differential(out, core, n):
    import mkrepo
    repo = os.path.join(out, "repo")
    seeds = [open(os.path.join(repo, "test", f), "rb").read() for f in sorted(os.listdir(os.path.join(repo, "test")))
             if f.startswith("index-") and not f.endswith(".sig")]
    if not seeds:
        ok("differential: lists from the repo part", False, "run the repo part first")
        return
    rnd = random.Random(22)
    words = [b"pkg", b"version", b"file", b"size", b"depends", b"takes", b"desc", b"group", b"base", b"system",
             b"channel", b"serial", b"date", b"expires", b"min", b" ", b"  ", b"\n", b"\r", b"\0", b"0", b"9999999999",
             b"4294967296", b"zenix-1.zup", b"-", b"zpkgindex 1", b"20260229", b"a" * 73, b"\xff"]
    path = os.path.join(out, "diff.index")
    bad = []
    accepted = 0
    for i in range(n):
        d = bytearray(rnd.choice(seeds))
        for _ in range(rnd.randint(1, 4)):
            k = rnd.randrange(len(d) + 1)
            r = rnd.random()
            if r < 0.3:
                d[k:k + rnd.randint(1, 8)] = b""
            elif r < 0.6:
                d[k:k] = rnd.choice(words)
            elif r < 0.8 and k < len(d):
                d[k] = rnd.randrange(256)
            else:
                lines = bytes(d).split(b"\n")
                a, c = rnd.randrange(len(lines)), rnd.randrange(len(lines))
                lines[a], lines[c] = lines[c], lines[a]
                d = bytearray(b"\n".join(lines))
        data = bytes(d)
        open(path, "wb").write(data)
        try:
            p = mkrepo.parse_index(data)
            want = "ok %s %d %d" % (p["channel"], p["serial"], len(p["pkgs"]))
        except mkrepo.Bad:
            want = "refused"
        rc, o = run(core, "index", path)
        got = o.strip() if o.startswith("ok") else "refused" if o.startswith("refused") else o.strip()
        if want.startswith("ok"):
            accepted += 1
        if got != want:
            bad.append((i, want, got[:100]))
            shutil.copy(path, os.path.join(out, "diff-%d.index" % i))
    ok("differential: the C and Python list parsers agree on %d damaged lists (%d accepted by both)" % (n, accepted),
       not bad, bad[:5])


# ---- the slot ----
BS = 512


def lget(img, blk, i):
    return struct.unpack(">I", img[blk * BS + 4 * i:blk * BS + 4 * i + 4])[0]


def lset(img, blk, i, v, fix=5):
    img[blk * BS + 4 * i:blk * BS + 4 * i + 4] = struct.pack(">I", v & 0xFFFFFFFF)
    if fix is not None:                        # the checksum long (5 for headers, 0 for bitmap blocks)
        img[blk * BS + 4 * fix:blk * BS + 4 * fix + 4] = b"\0" * 4
        s = sum(struct.unpack(">128I", bytes(img[blk * BS:(blk + 1) * BS])))
        img[blk * BS + 4 * fix:blk * BS + 4 * fix + 4] = struct.pack(">I", (-s) & 0xFFFFFFFF)


def ffs_hash(name, intl):
    h = len(name)
    for c in name.encode("latin-1"):
        u = c - 32 if (97 <= c <= 122 or (intl and 224 <= c <= 254 and c != 247)) else c
        h = (h * 13 + u) & 0x7FF
    return h % 72


def part_slot(out, core):
    d = os.path.join(out, "slot")
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    rnd = random.Random(45)
    SLOT = "zenix-update.slot"
    for intl in (False, True):
        dt = "DOS3" if intl else "DOS1"
        h = ffs_hash(SLOT, intl)
        # names in the slot's own hash chain, one before and one after it
        same = []
        i = 0
        while len(same) < 2:
            nm = "f%d" % i
            if ffs_hash(nm, intl) == h:
                same.append(nm)
            i += 1
        files = []
        for nm, size in (("ss", 300), ("before", 5000)):
            p = os.path.join(d, nm)
            open(p, "wb").write(os.urandom(size))
            files.append("%s=%s" % (p, "S/Startup-Sequence" if nm == "ss" else "Zenix/" + same[0]))
        slotdata = os.path.join(d, "slotdata")
        open(slotdata, "wb").write(bytes(200 * 1024))            # 400 blocks: a header and five extension blocks
        files.append("%s=Zenix/%s" % (slotdata, SLOT))
        p = os.path.join(d, "after")
        open(p, "wb").write(os.urandom(3000))
        files.append("%s=Zenix/%s" % (p, same[1]))
        img = os.path.join(d, "boot-%s.img" % dt)
        rc, msg = run(sys.executable, os.path.join(TOOLS, "mkffs.py"), img, "2M", "BOOT", *(["--intl"] if intl else []),
                      *files, env=dict(os.environ, SOURCE_DATE_EPOCH="1761600000"))
        if rc:
            ok("mkffs.py %s" % dt, False, msg)
            continue
        rc, o = run(core, "slot", img, "Zenix", SLOT, dt)
        w = o.split()
        ok("%s: the slot is found through its hash chain (a file before and after it in the chain)" % dt,
           rc == 0 and w[0] == "ok" and int(w[1]) == 200 * 1024 and int(w[2]) == 400, o)
        if rc:
            continue
        blocks = [int(x) for x in w[4:]]
        hdrblk = int(w[3])
        orig = open(img, "rb").read()
        data = os.urandom(200 * 1024)
        open(os.path.join(d, "data"), "wb").write(data)
        rc, o = run(core, "slotwrite", img, "Zenix", SLOT, dt, "0", os.path.join(d, "data"))
        new = open(img, "rb").read()
        changed = {i for i in range(len(orig) // BS) if orig[i * BS:(i + 1) * BS] != new[i * BS:(i + 1) * BS]}
        ok("%s: a write changes only the slot's data blocks, byte for byte everything else" % dt,
           rc == 0 and changed <= set(blocks) and len(changed) > 390, (o, len(changed)))
        ok("%s: the slot's bytes read back in file order" % dt,
           b"".join(new[b * BS:(b + 1) * BS] for b in blocks) == data)
        before = new
        part = os.urandom(1000)
        open(os.path.join(d, "part"), "wb").write(part)
        rc, o = run(core, "slotwrite", img, "Zenix", SLOT, dt, "700", os.path.join(d, "part"))
        new = open(img, "rb").read()
        changed = {i for i in range(len(new) // BS) if before[i * BS:(i + 1) * BS] != new[i * BS:(i + 1) * BS]}
        got = b"".join(new[b * BS:(b + 1) * BS] for b in blocks)
        ok("%s: a write in the middle (700, 1000 bytes) touches blocks 1-3 of the slot only" % dt,
           rc == 0 and changed <= {blocks[1], blocks[2], blocks[3]} and got[700:1700] == part and
           got[:700] == data[:700] and got[1700:] == data[1700:], o)
        rc, o = run(core, "slotwrite", img, "Zenix", SLOT, dt, str(200 * 1024 - 10), os.path.join(d, "part"))
        ok("%s: a write past the slot's end is refused" % dt, rc != 0 and open(img, "rb").read() == new, o)

        # refusals: on a damaged copy each; the image is never written by the walk
        base = bytearray(open(img, "rb").read())
        nb = len(base) // BS
        root = (2 + nb - 1) // 2
        bm0 = lget(base, root, 79)
        ext1 = lget(base, hdrblk, 126)
        other = [x for x in range(nb) if lget(base, x, 0) == 2 and lget(base, x, 127) == 0xFFFFFFFD and x != hdrblk][0]

        def bit_free(img_, blk):
            k = blk - 2
            li = 1 + k // 32
            v = lget(img_, bm0, li) | (1 << (k % 32))
            lset(img_, bm0, li, v, fix=0)
        cases = [
            ("a device that cannot flush", None, ["noflush"], "flush"),
            ("1024-byte blocks", None, ["1024", "2"], "512-byte"),
            ("4 reserved blocks", None, ["512", "4"], "reserved"),
            ("the bitmap not valid", lambda m: lset(m, root, 78, 0), [], "validate"),
            ("a bitmap extension block", lambda m: lset(m, root, 104, 7), [], "extension"),
            ("a damaged root checksum", lambda m: lset(m, root, 20, 1, fix=None), [], "root"),
            ("a damaged bitmap checksum", lambda m: lset(m, bm0, 5, 1, fix=None), [], "bitmap"),
            ("a slot block marked free in the bitmap", lambda m: bit_free(m, blocks[200]), [], "free"),
            ("the slot is a hard link (ST_LINKFILE)", lambda m: lset(m, hdrblk, 127, -4), [], "plain file"),
            ("the slot has a hard link to it", lambda m: lset(m, hdrblk, 118, other), [], "hard link"),
            ("a damaged file header checksum", lambda m: lset(m, hdrblk, 81, 1, fix=None), [], "damaged"),
            ("a damaged extension block", lambda m: lset(m, ext1, 30, 1, fix=None), [], "damaged"),
            ("an extension block with another parent", lambda m: lset(m, ext1, 125, root), [], "damaged"),
            ("a data pointer outside the partition", lambda m: lset(m, hdrblk, 70, nb + 5), [], "outside"),
            ("a data pointer to the root", lambda m: lset(m, hdrblk, 70, root), [], "metadata"),
            ("a data pointer to a bitmap block", lambda m: lset(m, hdrblk, 70, bm0), [], "metadata"),
            ("a data pointer to the slot's own header", lambda m: lset(m, hdrblk, 70, hdrblk), [], "metadata"),
            ("a data block named twice", lambda m: lset(m, hdrblk, 70, lget(m, hdrblk, 71)), [], "twice"),
            ("a byte size that does not match the blocks", lambda m: lset(m, hdrblk, 81, 100 * 1024), [], "count"),
            ("a byte size not in whole blocks", lambda m: lset(m, hdrblk, 81, 200 * 1024 - 1), [], "whole blocks"),
            ("the first data block not the first pointer", lambda m: lset(m, hdrblk, 4, blocks[5]), [], "first"),
            ("the boot block's dostype not the partition's", lambda m: m.__setitem__(slice(0, 4), b"DOS\x07"), [], "dostype"),
        ]
        for what, fn, extra, want in cases:
            m = bytearray(base)
            if fn:
                fn(m)
            p = os.path.join(d, "case.img")
            open(p, "wb").write(bytes(m))
            rc, o = run(core, "slot", p, "Zenix", SLOT, dt, *extra)
            ok("%s: refused: %s" % (dt, what), rc != 0 and o.startswith("refused") and want in o and
               open(p, "rb").read() == bytes(m), o)
        # a hash chain that goes round: the last entry of the slot's chain pointing back to its head; a name that
        # hashes there but is not on the volume walks it
        m = bytearray(base)
        zdir = lget(m, root, 6 + ffs_hash("Zenix", intl))
        while bytes(m[zdir * BS + 433:zdir * BS + 433 + m[zdir * BS + 432]]).decode() != "Zenix":
            zdir = lget(m, zdir, 124)
        head = last = lget(m, zdir, 6 + h)
        while lget(m, last, 124):
            last = lget(m, last, 124)
        lset(m, last, 124, head)
        absent = next("g%d" % i for i in range(100000) if ffs_hash("g%d" % i, intl) == h)
        p = os.path.join(d, "case.img")
        open(p, "wb").write(bytes(m))
        rc, o = run(core, "slot", p, "Zenix", absent, dt)
        ok("%s: refused: a hash chain that goes round" % dt, rc != 0 and "goes round" in o, o)
        rc, o = run(core, "slot", p, "Zenix", SLOT, dt)
        ok("%s: ... (the slot itself, before the loop, is still found)" % dt, rc == 0, o)
        rc, o = run(core, "slot", img, "Zenix", SLOT, "DOS0")
        ok("%s: OFS (DOS\\0) is refused" % dt, rc != 0 and "DOS" in o, o)
        rc, o = run(core, "slot", img, "Zenix", "nothere", dt)
        ok("%s: no slot file" % dt, rc != 0 and "no slot file" in o, o)
        rc, o = run(core, "slot", img, "NoDir", SLOT, dt)
        ok("%s: no slot directory" % dt, rc != 0 and "no slot directory" in o, o)
        rc, o = run(core, "slot", img, "Zenix", same[0], dt)
        ok("%s: a one-block file is refused as a slot (too small)" % dt, rc != 0 and "size" in o, o)
        rc, o = run(core, "slot", img, "S", "Startup-Sequence", dt)
        ok("%s: names are matched without case (as AmigaDOS)" % dt, rc != 0 and "size" in o, o)
        up = SLOT.upper()
        rc, o = run(core, "slot", img, "ZENIX", up, dt)
        ok("%s: ZENIX/ZENIX-UPDATE.SLOT finds the slot" % dt, rc == 0 and o.startswith("ok"), o)
    # an empty 1 KB slot (2 blocks) on a small volume
    p = os.path.join(d, "small")
    open(p, "wb").write(bytes(1024))
    img = os.path.join(d, "small.img")
    run(sys.executable, os.path.join(TOOLS, "mkffs.py"), img, "64K", "B", "%s=Zenix/%s" % (p, SLOT))
    rc, o = run(core, "slot", img, "Zenix", SLOT, "DOS1")
    ok("a 1 KB slot on a 64 KB volume", rc == 0 and o.split()[1:3] == ["1024", "2"], o)


# ---- the network part (inside a namespace) ----
def part_net(out, client):
    repo = os.path.join(out, "repo")
    if not os.path.exists(os.path.join(repo, "test", "latest")):
        ok("net: the repository from the repo part", False, "run the repo part first")
        return
    root = os.path.join(out, "www")
    shutil.rmtree(root, ignore_errors=True)
    os.makedirs(root)
    shutil.copytree(repo, os.path.join(root, "m1"))
    # an older mirror: the repository as it was after build 3 (repo2's first three adds)
    older = os.path.join(root, "old")
    shutil.copytree(repo, older)
    ser_now = latest(repo, "test")
    oldser = sorted(int(f[6:]) for f in os.listdir(os.path.join(older, "test"))
                    if f.startswith("index-") and not f.endswith(".sig"))[0]
    open(os.path.join(older, "test", "latest"), "w").write("zpkglatest 1 %d\n" % oldser)
    # a mirror whose list was changed on the way
    shutil.copytree(repo, os.path.join(root, "bad"))
    p = os.path.join(root, "bad", "test", "index-%d" % ser_now)
    dd = bytearray(open(p, "rb").read())
    dd[dd.index(b"expires ") + 8] ^= 1
    open(p, "wb").write(bytes(dd))
    big = os.path.join(root, "big.bin")
    blob = os.urandom(300 * 1024)
    open(big, "wb").write(blob)
    bigsha = hashlib.sha256(blob).hexdigest()
    small = os.path.join(root, "small.bin")
    open(small, "wb").write(b"0123456789" * 3)
    pf = os.path.join(out, "port")
    for f in (pf, pf + ".conns"):
        if os.path.exists(f):
            os.unlink(f)
    srv = subprocess.Popen([sys.executable, "-I", os.path.join(HERE, "http-test-server.py"), root, pf],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            if os.path.exists(pf):
                break
            time.sleep(0.05)
        port = int(open(pf).read())
        U = "http://127.0.0.1:%d" % port
        dl = os.path.join(out, "dl")
        shutil.rmtree(dl, ignore_errors=True)
        os.makedirs(dl)

        def get(path, *opts, name="f"):
            o = os.path.join(dl, name)
            rc, msg = run(client, "get", *opts, U + path, o)
            return rc, msg.strip(), o

        rc, msg, o = get("/big.bin", "--repeat", "3")
        ok("keep-alive: the same file three times over one connection", rc == 0 and "conns=1" in msg and
           msg.endswith(bigsha), msg)
        rc, msg, o = get("/chunked/big.bin")
        ok("chunked, random chunk sizes", rc == 0 and msg.endswith(bigsha), msg)
        rc, msg, o = get("/hugechunk")
        ok("a chunk size of 16 hex digits is refused", rc != 0 and "more than expected" in msg, msg)
        rc, msg, o = get("/endless", "--limit", "100000")
        ok("an endless body is cut off at the limit", rc != 0 and "more than expected" in msg, msg)
        rc, msg, o = get("/endless-chunked", "--limit", "100000")
        ok("endless chunks are cut off at the limit", rc != 0 and "more than expected" in msg, msg)
        rc, msg, o = get("/big.bin", "--limit", "1000")
        ok("a Content-Length over the limit is refused", rc != 0 and "more than expected" in msg, msg)
        rc, msg, o = get("/bighdr")
        ok("a header over 4 KB is refused", rc != 0, msg)
        rc, msg, o = get("/loop")
        ok("a redirect loop stops after 3", rc != 0 and "more than 3 redirects" in msg, msg)
        rc, msg, o = get("/redir/3/small.bin")
        ok("3 redirects are followed", rc == 0 and msg.startswith("ok 30 "), msg)
        rc, msg, o = get("/redir/4/small.bin")
        ok("4 redirects are not", rc != 0 and "more than 3" in msg, msg)
        rc, msg, o = get("/nothere")
        ok("404", rc != 0 and "not found" in msg, msg)
        rc, msg, o = get("/short/big.bin", name="short")
        ok("a body shorter than announced (then closed) is an error", rc != 0 and "ended before" in msg, msg)
        rc, msg, o = get("/long/small.bin", name="long")
        ok("a body longer than announced: exactly Content-Length bytes are taken", rc == 0 and msg.startswith("ok 30 ") and
           open(o, "rb").read() == b"0123456789" * 3, msg)
        rc, msg, o = get("/stall", "--idle-ms", "500")
        ok("a stalled server times out", rc != 0 and "stalled" in msg, msg)
        rc, msg, o = get("/drip/small.bin", "--file-ms", "1500")
        ok("a slow drip is stopped by the total time per file", rc != 0 and "total time" in msg, msg)
        # resume: a cut download, then the rest by Range
        rc, msg, o = get("/cut/big.bin", "--total", str(len(blob)), name="cut")
        ok("a cut connection leaves the .part", rc != 0 and os.path.getsize(o + ".part") == len(blob) // 2, msg)
        rc, msg, o = get("/cut/big.bin", "--total", str(len(blob)), name="cut")
        ok("... and the next run asks for the rest (206) and appends", rc == 0 and "resumed" in msg and msg.endswith(bigsha), msg)
        open(os.path.join(dl, "nr.part"), "wb").write(blob[:1000])
        rc, msg, o = get("/norange/big.bin", "--total", str(len(blob)), name="nr")
        ok("a server that ignores Range (200): the file again from the start", rc == 0 and "restarted" in msg and
           msg.endswith(bigsha), msg)
        open(os.path.join(dl, "br.part"), "wb").write(blob[:1000])
        rc, msg, o = get("/badrange/big.bin", "--total", str(len(blob)), name="br")
        ok("a wrong Content-Range is refused", rc != 0 and "Content-Range" in msg, msg)
        rc, msg, o = get("/tohttps/small.bin")
        ok("a redirect to https (this test client has no TLS) is not followed silently", rc != 0, msg)

        # -Sy and -Sw (4.3, 5.1)
        pub = subprocess.run(["openssl", "pkey", "-pubin", "-in", os.path.join(out, "sign-test", "zenix-test.pub"),
                              "-outform", "DER"], stdout=subprocess.PIPE, check=True).stdout[12:].hex()
        rpub = subprocess.run(["openssl", "pkey", "-pubin", "-in", os.path.join(out, "release.pub"), "-outform", "DER"],
                              stdout=subprocess.PIPE, check=True).stdout[12:].hex()
        st = os.path.join(out, "state")
        shutil.rmtree(st, ignore_errors=True)
        today = str(ser_now // 100)

        def sync(*mirrors, opts=(), names=(), cls="test", keys=None, state=st):
            a = ["sync", "--clock", today, "--clockfloor", "20261026"] + list(opts) + \
                [state, "test", cls, keys or pub] + [U + "/" + m for m in mirrors]
            if names:
                a += ["--"] + list(names)
            rc, msg = run(client, *a)
            return rc, msg.strip()

        rc, msg = sync("m1")
        ok("-Sy on a fresh machine: the list, its signature (zc_sig_verify, test key), stored",
           rc == 0 and msg == "new %d" % ser_now and os.path.exists(os.path.join(st, "sync", "test.index")) and
           os.path.getsize(os.path.join(st, "sync", "test.index.sig")) == 80, msg)
        rc, msg = sync("m1")
        ok("-Sy again: latest is the stored serial, the stored list is used", rc == 0 and msg == "same %d" % ser_now, msg)
        rc, msg = sync("old", "m1")
        ok("a mirror that is behind is passed over", rc == 0 and "mirror 1: this mirror is behind" in msg and
           "same %d" % ser_now in msg, msg)
        rc, msg = sync("old")
        ok("only a mirror that is behind: nothing usable", rc != 0 and "behind" in msg, msg)
        rc, msg = sync("m1", opts=("--clock", "20990101"))
        ok("the clock ahead: the stored list is refused at use as expired", rc != 0 and "expired" in msg, msg)
        rc, msg = sync("m1", opts=("--clock", "20990101", "--ignore-expiry"))
        ok("... --ignore-expiry goes on", rc == 0 and "same" in msg, msg)
        st2 = os.path.join(out, "state2")
        shutil.rmtree(st2, ignore_errors=True)
        rc, msg = sync("bad", "m1", state=st2)
        ok("a list changed on the way: its signature fails, the next mirror is used",
           rc == 0 and "mirror 1: the list's signature does not verify" in msg and "new %d" % ser_now in msg, msg)
        shutil.rmtree(st2)
        rc, msg = sync("m1", keys=rpub, state=st2)
        ok("a list signed by a key not among zpkg's is refused", rc != 0 and "does not verify" in msg, msg)
        shutil.rmtree(st2, ignore_errors=True)
        rc, msg = sync("m1", cls="release", state=st2)
        ok("a test-channel list taken by a release build (class) is refused", rc != 0 and "does not fit" in msg, msg)
        shutil.rmtree(st2, ignore_errors=True)
        rc, msg = sync("m1", opts=("--floor", "%d" % (ser_now + 100)), state=st2)
        ok("a fresh machine refuses a list older than its floor", rc != 0 and "older package list" in msg, msg)
        shutil.rmtree(st2, ignore_errors=True)
        rc, msg = sync("m1", opts=("--clock", str(int(today) - 3)), state=st2)
        ok("a clock behind the list's date by more than a day is said", rc == 0 and "the clock is behind" in msg, msg)
        # a replayed older list: the stored one is newer; the old mirror's list is refused by the serial even when
        # its latest were made to look new (a list with an old serial under a new latest)
        shutil.rmtree(st2, ignore_errors=True)
        rp = os.path.join(root, "replay")
        shutil.copytree(older, rp)
        open(os.path.join(rp, "test", "latest"), "w").write("zpkglatest 1 %d\n" % (ser_now + 1))
        shutil.copy(os.path.join(rp, "test", "index-%d" % oldser), os.path.join(rp, "test", "index-%d" % (ser_now + 1)))
        shutil.copy(os.path.join(rp, "test", "index-%d.sig" % oldser),
                    os.path.join(rp, "test", "index-%d.sig" % (ser_now + 1)))
        rc, msg = sync("replay")
        ok("a replayed older signed list under a new name is refused (its own serial is not latest's)",
           rc != 0 and "not latest's" in msg, msg)

        ix = index(repo, "test")
        awk = ix["pkgs"]["awk"]["file"]["name"]
        rc, msg = sync("m1", names=("awk", "man", "system"))
        ok("-Sw: package and system bundles fetched, size and SHA-256 against the list, the header checked, .sig beside",
           rc == 0 and "%s: ok" % awk in msg and "%s: ok" % ix["system"]["file"] in msg and
           os.path.getsize(os.path.join(st, "cache", awk + ".sig")) == 80, msg)
        rc, msg = sync("m1", names=("awk",))
        ok("a cached file is checked again and used", rc == 0 and "%s: cached" % awk in msg, msg)
        os.unlink(os.path.join(st, "cache", awk))
        rc, msg = sync("damaged/m1", "m1", names=("awk",))
        ok("a mirror serving a damaged file: refused by its SHA-256, the next mirror", rc == 0 and
           "size or SHA-256 is not the list's" in msg and "%s: fetched whole" % awk in msg, msg)
        os.unlink(os.path.join(st, "cache", awk))
        whole = open(os.path.join(repo, "pool", awk), "rb").read()
        open(os.path.join(st, "cache", awk + ".part"), "wb").write(whole[:100])
        rc, msg = sync("m1", names=("awk",))
        ok("a cut download resumes (Range) and is checked whole", rc == 0 and "fetched resumed" in msg, msg)
        os.unlink(os.path.join(st, "cache", awk))
        open(os.path.join(st, "cache", awk + ".part"), "wb").write(b"\x55" * 100)
        rc, msg = sync("m1", names=("awk",))
        ok("a wrong prefix: found at the end, deleted, fetched once from the start",
           rc == 0 and "after a resume" in msg and "fetched whole" in msg, msg)
        rc, msg = sync("m1", names=("nothere", "base"))
        ok("names the list does not have as files are refused (base comes only with the system)",
           rc != 0 and "nothere: not a package file" in msg and "base: not a package file" in msg, msg)
        # a file whose header is of another class than the key (a test build's bundle in a list a test key signed is
        # fine; make the list name a release-channel file: the release repo's awk, re-listed and re-signed here)
        lie = os.path.join(root, "lie")
        shutil.copytree(repo, lie)
        rel = os.path.join(out, "release-repo")
        rix = index(rel, "testing")
        rf = rix["pkgs"]["awk"]["file"]["name"]
        shutil.copy(os.path.join(rel, "pool", rf), os.path.join(lie, "pool", rf))
        import mkrepo
        lix = index(lie, "test")
        lix["pkgs"]["awk"] = dict(rix["pkgs"]["awk"])
        lix["serial"] += 1
        data = mkrepo.index_text(lix)
        lp = os.path.join(lie, "test", "index-%d" % lix["serial"])
        open(lp, "wb").write(data)
        subprocess.run([sys.executable, "-I", os.path.join(TOOLS, "zsign.py"), "sign", "--key",
                        os.path.join(out, "sign-test", "zenix-test.key"), "--kind", "index", lp], stdout=subprocess.PIPE,
                       env=dict(os.environ, ZSIGN_TEST_DIR=os.path.join(out, "sign-test")))
        open(os.path.join(lie, "test", "latest"), "w").write("zpkglatest 1 %d\n" % lix["serial"])
        shutil.rmtree(st2, ignore_errors=True)
        rc, msg = sync("lie", names=("awk",), state=st2)
        ok("a downloaded bundle whose header channel is not of the key's class is refused",
           rc != 0 and "channel does not fit" in msg, msg)
        # a snapshot: the stable channel of the release repository, its expiry not checked
        shutil.copytree(rel, os.path.join(root, "rel"))
        st3 = os.path.join(out, "state3")
        shutil.rmtree(st3, ignore_errors=True)
        rc, msg = run(client, "sync", "--clock", "20990101", "--snapshot", "2026.11", st3, "stable", "release", rpub,
                      U + "/rel")
        ok("--snapshot: the class is checked, serial and expiry are not (said), stored apart",
           rc == 0 and "a snapshot" in msg and os.path.exists(os.path.join(st3, "sync", "snapshot-2026.11.index")), msg)
    finally:
        srv.kill()
        srv.wait()


def main(a):
    if len(a) == 3 and a[0] == "repo":
        part_repo(a[1], a[2])
    elif len(a) == 4 and a[0] == "differential":
        part_differential(a[1], a[2], int(a[3]))
    elif len(a) == 3 and a[0] == "slot":
        part_slot(a[1], a[2])
    elif len(a) == 3 and a[0] == "net":
        part_net(a[1], a[2])
    else:
        sys.exit(__doc__)
    sys.exit(min(fails, 100))


if __name__ == "__main__":
    main(sys.argv[1:])
