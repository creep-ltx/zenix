#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""zpkg's package repository on the PC (DESIGN D22, docs/d22-zpkg.md 3 and 6): plain files for a static web server.

  mkrepo.py add REPO CHANNEL BUILD [OPTIONS]     the packages of a build into a channel: versions only for what
                                                 changed (3.4), the pool files, a new list, everything signed
  mkrepo.py promote REPO FROM TO [OPTIONS]       a new TO list naming exactly FROM's pool files (testing -> stable)
  mkrepo.py resign REPO CHANNEL [OPTIONS]        the same files under a new serial, date and expiry (the weekly run)
  mkrepo.py snapshot REPO CHANNEL NAME           snapshots/NAME/: a copy of the channel's list, its .sig and latest
  mkrepo.py prune REPO                           the pool down to what a list or snapshot names plus the previous
                                                 version of every package; each channel's lists but the last two
  mkrepo.py check REPO [--pub PUB]... [--test-pub PUB]... [--today YYYYMMDD]
                                                 every list, signature, size, SHA-256 and bundle as zpkg would check
                                                 them; exit 1 with every problem said
  mkrepo.py installdb REPO CHANNEL OUTDIR        the channel's manifests (the repository's versions) for an image's
                                                 package database (3.4; tools/mkzup.py installdb takes OUTDIR as PKGDIR)

BUILD is a build directory: BUILD/kernel.bin and BUILD/pkg/ (tools/mkzup.py packages: NAME.manifest, NAME.tgz and
the list). OPTIONS: --key KEY (default tools/zsign.py's release key), --date YYYYMMDD (the signing day; default
today), --expires-days N (default 60, owner question 3), --max-date-gap DAYS (default 400: a list's date may not be
before the previous list's nor more than this after it, a PC with a wrong clock). The test channel is signed with
the TEST key (tools/zsign.py testkey); a release channel (testing, stable) never is.

The layout (3.2): pool/NAME-VNUM.zup and .sig (a package bundle: one package, no kernel), pool/zenix-VNUM.zup and
.sig (the system bundle: the kernel and the base group only), CHANNEL/latest ("zpkglatest 1 SERIAL", written last),
CHANNEL/index-SERIAL and .sig (the list, 3.3). Every published file but latest is immutable: a pool file that exists
with other bytes is refused, never replaced.

Versions (3.4): a package is changed when its manifest differs in anything but its own version line and its
"depends base" line (in a base-group package also its dependencies on other base-group packages, which always carry
the group's one version); the base group is changed when the kernel differs with its ZVER record masked or any
base-group package changed, and then the system and every base-group package get the build's version. A changed
package gets the build's version; its "depends base" line and its bundle header's minimum become the channel's
current base version. An unchanged package keeps its pool file. Any other dependency must name a version the new
list has, or add refuses (the note does not say how to rewrite those).
"""
import datetime
import hashlib
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mkzup  # noqa: E402
import zsign  # noqa: E402

CHANNELS = ("test", "testing", "stable")
RELEASE = ("testing", "stable")
SYSNAME = "zenix"
INDEX_MAX = 256 * 1024                    # zpkg refuses a bigger list (3.3)
LATEST_MAX = 64
SIG_SIZE = 80
MAXDEP = 8                                # zup's manifest limit (user/zup.c MAXDEP)
MAXTAKES = 8
NAME_RE = re.compile(r'^[a-z0-9][a-z0-9.+-]{0,23}$')
VSTR_RE = re.compile(r'^[\x21-\x7e]{1,15}$')
SHA_RE = re.compile(r'^[0-9a-f]{64}$')
NUM_RE = re.compile(r'^[0-9]{1,10}$')
SNAP_RE = re.compile(r'^[A-Za-z0-9][A-Za-z0-9._-]{0,31}$')


def die(msg):
    sys.stderr.write("mkrepo: %s\n" % msg)
    sys.exit(1)


class Bad(Exception):
    pass


# ---- dates ----
def ymd(n):
    """YYYYMMDD -> date, or Bad"""
    try:
        if n // 10000 < 1970:
            raise ValueError
        return datetime.date(n // 10000, n // 100 % 100, n % 100)
    except ValueError:
        raise Bad("%d is not a date" % n)


def num(s, what):
    if not NUM_RE.match(s) or int(s) > 0xFFFFFFFF:
        raise Bad("%s: %r is not a number" % (what, s))
    return int(s)


# ---- the list (3.3) ----
def parse_latest(data):
    if len(data) > LATEST_MAX:
        raise Bad("latest is longer than %d bytes" % LATEST_MAX)
    t = data.decode("ascii", errors="replace")
    if t.endswith("\n"):
        t = t[:-1]
    w = t.split(" ")
    if len(w) != 3 or w[0] != "zpkglatest" or w[1] != "1":
        raise Bad("latest is not a \"zpkglatest 1 SERIAL\" line")
    return num(w[2], "latest")


def latest_text(serial):
    return ("zpkglatest 1 %d\n" % serial).encode()


def parse_index(data):
    """the list's grammar, as zpkg's parser (user/zpkg/index.c) takes it -> dict; Bad with the reason"""
    if len(data) > INDEX_MAX:
        raise Bad("the list is longer than %d bytes" % INDEX_MAX)
    if any(b != 10 and not 32 <= b < 127 for b in data):
        raise Bad("the list has a byte that is not printable ASCII")
    lines = data.decode("ascii").split("\n")
    if lines[0] != "zpkgindex 1":
        raise Bad("not a \"zpkgindex 1\" list")
    ix = {"pkgs": {}, "order": []}
    cur = None
    for no, ln in enumerate(lines[1:], 2):
        if not ln:
            continue
        w = ln.split(" ")
        if "" in w and w[0] != "desc":
            raise Bad("line %d: an empty word" % no)
        k, a = w[0], w[1:]
        where = "line %d (%s)" % (no, k)
        if k == "pkg":
            if len(a) != 1 or not NAME_RE.match(a[0]):
                raise Bad("%s: a bad package name" % where)
            if a[0] in ix["pkgs"]:
                raise Bad("%s: %s twice" % (where, a[0]))
            cur = {"name": a[0], "deps": [], "takes": [], "group": None, "file": None, "desc": None}
            ix["pkgs"][a[0]] = cur
            ix["order"].append(a[0])
            continue
        if cur is None:
            if k in ("channel", "serial", "date", "expires", "system") and k in ix:
                raise Bad("%s: twice" % where)
            if k == "channel":
                if len(a) != 1:
                    raise Bad(where)
                ix["channel"] = a[0]
            elif k in ("serial", "date", "expires"):
                if len(a) != 1:
                    raise Bad(where)
                ix[k] = num(a[0], where)
                if k != "serial":
                    ymd(ix[k])
            elif k == "system":
                if len(a) != 7 or a[5] != "min":
                    raise Bad("%s: system VNUM VSTR FILE SIZE SHA256 min MIN" % where)
                v = num(a[0], where)
                if not VSTR_RE.match(a[1]) or a[2] != "%s-%d.zup" % (SYSNAME, v) or not SHA_RE.match(a[4]):
                    raise Bad(where)
                ix["system"] = {"vnum": v, "vstr": a[1], "file": a[2], "fsize": num(a[3], where), "sha": a[4],
                                "min": num(a[6], where)}
                if not 96 < ix["system"]["fsize"] <= mkzup.MAXSIZE:
                    raise Bad("%s: a bad size" % where)
            continue                                   # other header words: a later list's (growth), ignored
        if k == "version":
            if len(a) != 2 or "vnum" in cur or not VSTR_RE.match(a[1]):
                raise Bad(where)
            cur["vnum"], cur["vstr"] = num(a[0], where), a[1]
        elif k == "group":
            if len(a) != 1 or a[0] != "base" or cur["group"]:
                raise Bad("%s: only \"group base\", once" % where)
            cur["group"] = a[0]
        elif k == "file":
            if len(a) != 3 or cur["file"] or not SHA_RE.match(a[2]):
                raise Bad(where)
            cur["file"] = {"name": a[0], "fsize": num(a[1], where), "sha": a[2]}
            if not 96 < cur["file"]["fsize"] <= mkzup.MAXSIZE:
                raise Bad("%s: a bad size" % where)
        elif k == "size":
            if len(a) != 1 or "size" in cur:
                raise Bad(where)
            cur["size"] = num(a[0], where)
        elif k == "depends":
            if len(a) != 2 or not NAME_RE.match(a[0]) or len(cur["deps"]) == MAXDEP:
                raise Bad("%s: NAME VERSION, at most %d" % (where, MAXDEP))
            cur["deps"].append((a[0], num(a[1], where)))
        elif k == "takes":
            if len(a) != 1 or not NAME_RE.match(a[0]) or len(cur["takes"]) == MAXTAKES:
                raise Bad("%s: NAME, at most %d" % (where, MAXTAKES))
            cur["takes"].append(a[0])
        elif k == "desc":
            d = ln[5:]
            if cur["desc"] is not None or not 1 <= len(d) <= 72:
                raise Bad("%s: 1-72 characters" % where)
            cur["desc"] = d
        # other words in a block: growth, ignored
    for k in ("channel", "serial", "date", "expires", "system"):
        if k not in ix:
            raise Bad("the list has no %s line" % k)
    if ix["expires"] < ix["date"]:
        raise Bad("the list expires before its date")
    for n, p in ix["pkgs"].items():
        if "vnum" not in p or "size" not in p:
            raise Bad("package %s: no version or size line" % n)
        if p["file"] is None and p["group"] != "base":
            raise Bad("package %s: no file line" % n)
        if p["file"] is not None and p["file"]["name"] != "%s-%d.zup" % (n, p["vnum"]):
            raise Bad("package %s: its file is not %s-%d.zup" % (n, n, p["vnum"]))
    return ix


def index_text(ix):
    s = ix["system"]
    out = ["zpkgindex 1", "channel %s" % ix["channel"], "serial %d" % ix["serial"], "date %d" % ix["date"],
           "expires %d" % ix["expires"],
           "system %d %s %s %d %s min %d" % (s["vnum"], s["vstr"], s["file"], s["fsize"], s["sha"], s["min"])]
    for n in sorted(ix["pkgs"]):
        p = ix["pkgs"][n]
        out.append("pkg %s" % n)
        out.append("version %d %s" % (p["vnum"], p["vstr"]))
        if p["group"]:
            out.append("group %s" % p["group"])
        if p["file"]:
            out.append("file %s %d %s" % (p["file"]["name"], p["file"]["fsize"], p["file"]["sha"]))
        out.append("size %d" % p["size"])
        out += ["depends %s %d" % d for d in p["deps"]]
        out += ["takes %s" % t for t in p["takes"]]
        if p["desc"]:
            out.append("desc %s" % p["desc"])
    data = ("\n".join(out) + "\n").encode()
    if len(data) > INDEX_MAX:
        die("the list would be %d bytes, more than zpkg takes (%d)" % (len(data), INDEX_MAX))
    return data


# ---- the repository's state ----
def chdir_of(repo, channel):
    if channel not in CHANNELS:
        die("unknown channel %r (%s)" % (channel, ", ".join(CHANNELS)))
    return os.path.join(repo, channel)


def current(repo, channel, d=None):
    """(serial, index dict) of a channel's current list, or (0, None)"""
    d = d or chdir_of(repo, channel)
    lp = os.path.join(d, "latest")
    if not os.path.exists(lp):
        return 0, None
    try:
        serial = parse_latest(open(lp, "rb").read())
        ix = parse_index(open(os.path.join(d, "index-%d" % serial), "rb").read())
    except (Bad, OSError) as e:
        die("%s: the current list cannot be read: %s" % (d, e))
    if ix["serial"] != serial:
        die("%s: latest says %d, its list says %d" % (d, serial, ix["serial"]))
    return serial, ix


def pool_bundle(repo, fname):
    data = open(os.path.join(repo, "pool", fname), "rb").read()
    try:
        hdr, parts = mkzup.read_bundle(data)
    except ValueError as e:
        die("pool/%s: %s" % (fname, e))
    return hdr, dict(parts)


def write_new(path, data):
    """an immutable file: written once; the same bytes again is fine, other bytes are refused"""
    if os.path.exists(path):
        if open(path, "rb").read() != data:
            die("%s exists with other content: a published file is never changed" % path)
        return False
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path + ".part", "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(path + ".part", path)
    return True


def sign(path, key, kind):
    zsign.sign(path, key, kind)


def check_key(channel, key):
    if not os.path.exists(key):
        die("no signing key %s" % key)
    if channel in RELEASE and os.path.exists(zsign.TEST_PUB) and \
            zsign.raw_public(key, private=True) == zsign.raw_public(zsign.TEST_PUB):
        die("%s is a release channel: refusing to sign it with the test key (D22 5.1)" % channel)


def publish(repo, channel, ix, prev, opts):
    """write CHANNEL/index-SERIAL, its .sig, then latest; ix holds channel, system and pkgs"""
    date = opts["date"]
    if prev is not None:
        if date < prev["date"]:
            die("the date %d is before the previous list's (%d): check the PC's clock" % (date, prev["date"]))
        if (ymd(date) - ymd(prev["date"])).days > opts["gap"]:
            die("the date %d is more than %d days after the previous list's (%d): check the PC's clock "
                "(--max-date-gap)" % (date, opts["gap"], prev["date"]))
    named = [ix["system"]["vnum"]] + [p["vnum"] for p in ix["pkgs"].values()]
    d = chdir_of(repo, channel)
    os.makedirs(d, exist_ok=True)
    # above every list in the directory, also one a cut run wrote but never named in latest (never published)
    serial = max([date * 100, max(named)] + [s + 1 for s in serials(d)] + ([prev["serial"] + 1] if prev else []))
    if serial > date * 100 + 99:
        die("no serial left for %d (the previous is %d, the newest version named %d): a list's serial is at most "
            "its date x 100 + 99" % (date, prev["serial"] if prev else 0, max(named)))
    ix["serial"], ix["date"] = serial, date
    ix["expires"] = int((ymd(date) + datetime.timedelta(days=opts["expires"])).strftime("%Y%m%d"))
    data = index_text(ix)
    parse_index(data)                                   # what is written must be what zpkg reads
    p = os.path.join(d, "index-%d" % serial)
    write_new(p, data)
    sign(p, opts["key"], "index")
    with open(os.path.join(d, "latest.part"), "wb") as f:
        f.write(latest_text(serial))
        f.flush()
        os.fsync(f.fileno())
    os.replace(os.path.join(d, "latest.part"), os.path.join(d, "latest"))
    print("%s/latest: %d (%d packages, system %d, expires %d)" % (channel, serial, len(ix["pkgs"]),
                                                                  ix["system"]["vnum"], ix["expires"]))
    return serial


# ---- add (3.4) ----
def masked_manifest(text, is_base, base_names):
    out = []
    for ln in text.decode().split("\n"):
        w = ln.split(" ")
        if w[0] == "version":
            continue
        if w[0] == "depends" and len(w) == 3 and (w[1] == "base" or (is_base and w[1] in base_names)):
            out.append("depends %s *" % w[1])
            continue
        out.append(ln)
    return "\n".join(out)


def masked_kernel(img):
    i = img.find(b"ZVER")
    while i >= 0 and i + 24 > len(img):
        i = img.find(b"ZVER", i + 1)
    if i < 0:
        die("the kernel has no ZVER record")
    return img[:i + 4] + bytes(20) + img[i + 24:]


def installed_size(m):
    return sum(e[5] for e in m["entries"] if e[0] in "fc")


def rewrite_base_dep(text, base):
    out = []
    for ln in text.decode().split("\n"):
        w = ln.split(" ")
        if w[0] == "depends" and len(w) == 3 and w[1] == "base":
            ln = "depends base %d" % base
        out.append(ln)
    return "\n".join(out).encode()


def block_of(m, fname=None, blob=None):
    return {"name": m["name"], "vnum": m["vnum"], "vstr": m["vstr"], "group": m["group"],
            "file": {"name": fname, "fsize": len(blob), "sha": hashlib.sha256(blob).hexdigest()} if fname else None,
            "size": installed_size(m), "deps": list(m["deps"]), "takes": list(m["takes"]), "desc": m["desc"]}


def add(repo, channel, build, opts):
    check_key(channel, opts["key"])
    pkgdir = os.path.join(build, "pkg")
    try:
        names = open(os.path.join(pkgdir, "list")).read().split()
        kimg = open(os.path.join(build, "kernel.bin"), "rb").read()
    except OSError as e:
        die("%s: not a build (%s)" % (build, e))
    bv, bvstr = mkzup.kernel_version(kimg)
    if bv is None:
        die("the kernel has no ZVER record")
    man, tgz, m = {}, {}, {}
    for n in names:
        man[n] = open(os.path.join(pkgdir, n + ".manifest"), "rb").read()
        tgz[n] = open(os.path.join(pkgdir, n + ".tgz"), "rb").read()
        m[n] = mkzup.parse_manifest(man[n])
        if m[n]["name"] != n or m[n]["vnum"] != bv:
            die("%s: the manifest says %s %d, the build is %d" % (n, m[n]["name"], m[n]["vnum"], bv))
        if n == SYSNAME:
            die("a package may not be called %s (the system bundle's pool name)" % SYSNAME)
    base_names = [n for n in names if m[n]["group"] == mkzup.BASE_GROUP]
    if not base_names:
        die("the build has no base-group package")
    serial, old = current(repo, channel)
    os.makedirs(os.path.join(repo, "pool"), exist_ok=True)

    # the base group: changed?
    base_changed = old is None
    old_sys = None
    if old is not None:
        _, old_sys = pool_bundle(repo, old["system"]["file"])
        oldk = old_sys.get("boot/zenix")
        old_base = sorted(k[4:-9] for k in old_sys if k.endswith(".manifest"))
        if oldk is None or masked_kernel(oldk) != masked_kernel(kimg) or old_base != sorted(base_names):
            base_changed = True
        else:
            for n in base_names:
                if masked_manifest(old_sys["pkg/%s.manifest" % n], True, base_names) != \
                        masked_manifest(man[n], True, base_names):
                    base_changed = True
    pkgs = {}
    if base_changed:
        if old is not None and bv <= old["system"]["vnum"]:
            die("the base group changed, but the build's version %d is not above the channel's system %d" %
                (bv, old["system"]["vnum"]))
        sysv = bv
        parts = [(b"boot/zenix", kimg)]
        for n in base_names:
            parts += [(("pkg/%s.manifest" % n).encode(), man[n]), (("pkg/%s.tgz" % n).encode(), tgz[n])]
        blob = mkzup.bundle_bytes(parts, bv, bvstr, bv // 100, 0, channel)
        fname = "%s-%d.zup" % (SYSNAME, bv)
        if write_new(os.path.join(repo, "pool", fname), blob) or not os.path.exists(os.path.join(repo, "pool", fname + ".sig")):
            sign(os.path.join(repo, "pool", fname), opts["key"], "bundle")
        system = {"vnum": bv, "vstr": bvstr, "file": fname, "fsize": len(blob),
                  "sha": hashlib.sha256(blob).hexdigest(), "min": 0}
        for n in base_names:
            pkgs[n] = block_of(m[n])
    else:
        system = dict(old["system"])
        sysv = system["vnum"]
        for n in base_names:
            pkgs[n] = dict(old["pkgs"][n]) if n in old["pkgs"] else block_of(mkzup.parse_manifest(old_sys["pkg/%s.manifest" % n]))

    # the other packages
    changed = []
    for n in names:
        if n in base_names:
            continue
        ob = old["pkgs"].get(n) if old else None
        if ob is not None and ob["group"] is None and ob["file"]:
            _, op = pool_bundle(repo, ob["file"]["name"])
            if masked_manifest(op["pkg/%s.manifest" % n], False, base_names) == masked_manifest(man[n], False, base_names):
                pkgs[n] = dict(ob)
                continue
            if bv <= ob["vnum"]:
                die("%s changed, but the build's version %d is not above the channel's %d" % (n, bv, ob["vnum"]))
        text = rewrite_base_dep(man[n], sysv)
        mm = mkzup.parse_manifest(text)
        parts = [(("pkg/%s.manifest" % n).encode(), text), (("pkg/%s.tgz" % n).encode(), tgz[n])]
        blob = mkzup.bundle_bytes(parts, bv, bvstr, bv // 100, sysv, channel)
        fname = "%s-%d.zup" % (n, bv)
        if write_new(os.path.join(repo, "pool", fname), blob) or not os.path.exists(os.path.join(repo, "pool", fname + ".sig")):
            sign(os.path.join(repo, "pool", fname), opts["key"], "bundle")
        pkgs[n] = block_of(mm, fname, blob)
        changed.append(n)

    # every dependency must be met by the new list
    for n, p in pkgs.items():
        for d, v in p["deps"]:
            if d not in pkgs or pkgs[d]["vnum"] < v:
                die("%s needs %s %d, and the list would have %s: the build's other dependencies name versions the "
                    "repository did not give (D22 3.4 rewrites only \"depends base\")" %
                    (n, d, v, ("%s %d" % (d, pkgs[d]["vnum"])) if d in pkgs else "none"))
    gone = sorted(set(old["pkgs"]) - set(pkgs)) if old else []
    if old is not None and not base_changed and not changed and not gone:
        print("%s: nothing changed; the list stays %d" % (channel, serial))
        return
    print("%s: system %s%s; changed: %s%s" % (channel, sysv, " (new)" if base_changed else "",
                                             " ".join(changed) or "none", "; gone: " + " ".join(gone) if gone else ""))
    publish(repo, channel, {"channel": channel, "system": system, "pkgs": pkgs}, old, opts)


def promote(repo, src, dst, opts):
    check_key(dst, opts["key"])
    if (src in RELEASE) != (dst in RELEASE):
        die("%s and %s are of different key classes" % (src, dst))
    _, ix = current(repo, src)
    if ix is None:
        die("%s has no list" % src)
    _, old = current(repo, dst)
    publish(repo, dst, {"channel": dst, "system": ix["system"], "pkgs": ix["pkgs"]}, old, opts)


def resign(repo, channel, opts):
    check_key(channel, opts["key"])
    _, ix = current(repo, channel)
    if ix is None:
        die("%s has no list" % channel)
    publish(repo, channel, {"channel": channel, "system": ix["system"], "pkgs": ix["pkgs"]}, ix, opts)


def snapshot(repo, channel, name):
    if not SNAP_RE.match(name):
        die("bad snapshot name %r" % name)
    serial, ix = current(repo, channel)
    if ix is None:
        die("%s has no list" % channel)
    d = os.path.join(repo, "snapshots", name)
    if os.path.exists(d):
        die("snapshots/%s exists" % name)
    os.makedirs(d + ".part")
    src = chdir_of(repo, channel)
    for f in ("index-%d" % serial, "index-%d.sig" % serial, "latest"):
        shutil.copyfile(os.path.join(src, f), os.path.join(d + ".part", f))
    os.rename(d + ".part", d)
    print("snapshots/%s: %s's list %d" % (name, channel, serial))


def lists(repo):
    """every list prune and check consider: (where, dir, is_snapshot)"""
    out = [(c, os.path.join(repo, c), False) for c in CHANNELS if os.path.isdir(os.path.join(repo, c))]
    sd = os.path.join(repo, "snapshots")
    if os.path.isdir(sd):
        out += [("snapshots/" + s, os.path.join(sd, s), True) for s in sorted(os.listdir(sd)) if not s.endswith(".part")]
    return out


def serials(d):
    return sorted(int(f[6:]) for f in os.listdir(d) if re.match(r'^index-[0-9]+$', f))


def prune(repo):
    keep = set()
    current_pkgs = []
    for where, d, snap in lists(repo):
        ss = serials(d)
        cur, _ = current(repo, None, d) if not snap else (parse_latest(open(os.path.join(d, "latest"), "rb").read()), None)
        mine = [s for s in ss if s <= cur][-2:] if not snap else [cur]
        for s in mine:
            ix = parse_index(open(os.path.join(d, "index-%d" % s), "rb").read())
            keep.add(ix["system"]["file"])
            keep.update(p["file"]["name"] for p in ix["pkgs"].values() if p["file"])
            if s == cur and not snap:
                current_pkgs.append(ix)
        if not snap:
            for s in ss:
                if s not in mine and s < cur:
                    for f in ("index-%d" % s, "index-%d.sig" % s):
                        if os.path.exists(os.path.join(d, f)):
                            os.unlink(os.path.join(d, f))
                            print("pruned %s/%s" % (where, f))
    pool = os.path.join(repo, "pool")
    files = [f for f in os.listdir(pool) if f.endswith(".zup")] if os.path.isdir(pool) else []
    vers = {}
    for f in files:
        mt = re.match(r'^(.+)-([0-9]+)\.zup$', f)
        if mt:
            vers.setdefault(mt.group(1), []).append(int(mt.group(2)))
    for ix in current_pkgs:                             # the previous version of every package (and the system)
        named = [(SYSNAME, ix["system"]["vnum"])] + [(n, p["vnum"]) for n, p in ix["pkgs"].items() if p["file"]]
        for n, v in named:
            older = [x for x in vers.get(n, []) if x < v]
            if older:
                keep.add("%s-%d.zup" % (n, max(older)))
    for f in sorted(files):
        if f not in keep:
            for g in (f, f + ".sig"):
                if os.path.exists(os.path.join(pool, g)):
                    os.unlink(os.path.join(pool, g))
            print("pruned pool/%s" % f)


# ---- check: as zpkg would ----
def sig_ok(path, kind, pubs):
    import contextlib
    import io
    if not os.path.exists(path + ".sig") or os.path.getsize(path + ".sig") != SIG_SIZE:
        return "no 80-byte .sig"
    if not pubs:
        return "no key of this class given"
    buf = io.StringIO()
    try:
        with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(buf):
            zsign.verify(path, path + ".sig", pubs, kind)
        return None
    except SystemExit:
        return buf.getvalue().strip().replace("zsign: ", "")


def check(repo, pubs, tpubs, today):
    probs = []
    say = lambda m: probs.append(m)
    pool = os.path.join(repo, "pool")
    seen = {}
    for where, d, snap in lists(repo):
        chan = os.path.basename(d) if not snap else None
        try:
            serial = parse_latest(open(os.path.join(d, "latest"), "rb").read())
            data = open(os.path.join(d, "index-%d" % serial), "rb").read()
        except (OSError, Bad) as e:
            say("%s: %s" % (where, e))
            continue
        lp = os.path.join(d, "index-%d" % serial)
        # the signature first, before a line is parsed (as zpkg, 5.1): by the key class the directory says (a
        # snapshot: a copy of a release channel's list)
        keys = tpubs if chan == "test" else pubs
        e = sig_ok(lp, "index", keys)
        if e:
            say("%s/index-%d: signature: %s" % (where, serial, e))
            continue
        try:
            ix = parse_index(data)
        except Bad as e:
            say("%s/index-%d: %s" % (where, serial, e))
            continue
        release = ix["channel"] in RELEASE
        if ix["channel"] not in CHANNELS or (chan is None and not release):
            say("%s: channel %s" % (where, ix["channel"]))
        if chan is not None and ix["channel"] != chan:
            say("%s: the list says channel %s" % (where, ix["channel"]))
        if ix["serial"] != serial:
            say("%s: latest says %d, the list %d" % (where, serial, ix["serial"]))
        if ix["serial"] > ix["date"] * 100 + 99:
            say("%s: the serial %d is above its date's" % (where, ix["serial"]))
        named = [ix["system"]["vnum"]] + [p["vnum"] for p in ix["pkgs"].values()]
        if ix["serial"] < max(named):
            say("%s: the serial is below a version the list names" % where)
        if not snap and today > ix["expires"]:
            say("%s: expired on %d" % (where, ix["expires"]))
        files = [(SYSNAME, ix["system"], True)] + [(n, p, False) for n, p in ix["pkgs"].items() if p["file"]]
        for n, ent, is_sys in files:
            fe = ent if is_sys else ent["file"]
            fname = fe["file"] if is_sys else fe["name"]
            fp = os.path.join(pool, fname)
            if not os.path.exists(fp):
                say("%s: pool/%s is missing" % (where, fname))
                continue
            blob = open(fp, "rb").read()
            if len(blob) != fe["fsize"] or hashlib.sha256(blob).hexdigest() != fe["sha"]:
                say("%s: pool/%s: size or SHA-256 is not the list's" % (where, fname))
                continue
            try:
                hdr, parts = mkzup.read_bundle(blob)
            except ValueError as ex:
                say("pool/%s: %s" % (fname, ex))
                continue
            parts = dict(parts)
            if hdr["vnum"] != ent["vnum"]:
                say("pool/%s: the header's version %d is not the list's" % (fname, hdr["vnum"]))
            if (hdr["channel"] in RELEASE) != release or hdr["channel"] not in CHANNELS:
                say("pool/%s: channel %s is not of the list's key class" % (fname, hdr["channel"]))
            if fname not in seen:
                seen[fname] = sig_ok(fp, "bundle", keys)
            if seen[fname]:
                say("pool/%s: signature: %s" % (fname, seen[fname]))
            if is_sys:
                k = parts.get("boot/zenix")
                if k is None or mkzup.kernel_version(k)[0] != ent["vnum"]:
                    say("pool/%s: no kernel of version %d" % (fname, ent["vnum"]))
                for pn in (x[4:-9] for x in parts if x.endswith(".manifest")):
                    mm = mkzup.parse_manifest(parts["pkg/%s.manifest" % pn])
                    if mm["group"] != mkzup.BASE_GROUP:
                        say("pool/%s: %s is not in the base group" % (fname, pn))
                    lb = ix["pkgs"].get(pn)
                    if lb is None or lb["vnum"] != mm["vnum"] or lb["group"] != mkzup.BASE_GROUP:
                        say("%s: the system's %s is not listed as base-group %d" % (where, pn, mm["vnum"]))
            else:
                if "boot/zenix" in parts or sorted(parts) != ["pkg/%s.manifest" % n, "pkg/%s.tgz" % n]:
                    say("pool/%s: not a package bundle of %s alone" % (fname, n))
                    continue
                mm = mkzup.parse_manifest(parts["pkg/%s.manifest" % n])
                if mm["name"] != n or mm["vnum"] != ent["vnum"] or mm["vstr"] != ent["vstr"]:
                    say("pool/%s: its manifest is %s %d" % (fname, mm.get("name"), mm.get("vnum", 0)))
                if sorted(mm["deps"]) != sorted(ent["deps"]) or mm["takes"] != ent["takes"] or \
                        (mm["group"] or None) != ent["group"]:
                    say("pool/%s: its manifest's dependencies, takes or group are not the list's" % fname)
                if hdr["min"] != dict(mm["deps"]).get("base", hdr["min"]):
                    say("pool/%s: its minimum base %d is not its depends base" % (fname, hdr["min"]))
                if installed_size(mm) != ent["size"]:
                    say("%s: %s's size is not its files'" % (where, n))
        for n, p in ix["pkgs"].items():
            for dn, dv in p["deps"]:
                if dn not in ix["pkgs"] or ix["pkgs"][dn]["vnum"] < dv:
                    say("%s: %s needs %s %d, which the list does not have" % (where, n, dn, dv))
    for p in probs:
        print("check: %s" % p)
    print("check: %s" % ("%d problems" % len(probs) if probs else "ok (%d lists)" % len(lists(repo))))
    return 1 if probs else 0


def installdb(repo, channel, out):
    _, ix = current(repo, channel)
    if ix is None:
        die("%s has no list" % channel)
    os.makedirs(out, exist_ok=True)
    _, sysparts = pool_bundle(repo, ix["system"]["file"])
    for n, p in sorted(ix["pkgs"].items()):
        if p["file"]:
            _, parts = pool_bundle(repo, p["file"]["name"])
        else:
            parts = sysparts
        open(os.path.join(out, n + ".manifest"), "wb").write(parts["pkg/%s.manifest" % n])
    print("%s: %d manifests of %s's list %d" % (out, len(ix["pkgs"]), channel, ix["serial"]))


def main(a):
    opts = {"key": zsign.DEFAULT_KEY, "date": int(datetime.date.today().strftime("%Y%m%d")), "expires": 60,
            "gap": 400}
    pubs, tpubs, rest, today = [], [], [], None
    i = 0
    while i < len(a):
        o = a[i]
        if o in ("--key", "--date", "--expires-days", "--max-date-gap", "--pub", "--test-pub", "--today"):
            if i + 1 >= len(a):
                die("%s needs a value" % o)
            v = a[i + 1]
            if o == "--key":
                opts["key"] = v
            elif o == "--pub":
                pubs.append(v)
            elif o == "--test-pub":
                tpubs.append(v)
            else:
                try:
                    n = int(v)
                    if o in ("--date", "--today"):
                        ymd(n)
                except (ValueError, Bad):
                    die("%s: %r" % (o, v))
                if o == "--date":
                    opts["date"] = n
                elif o == "--today":
                    today = n
                elif o == "--expires-days":
                    opts["expires"] = n
                else:
                    opts["gap"] = n
            i += 2
        else:
            rest.append(o)
            i += 1
    if opts["expires"] < 1 or opts["gap"] < 0:
        die("--expires-days must be at least 1, --max-date-gap not negative")
    c = rest[0] if rest else ""
    if c == "add" and len(rest) == 4:
        add(rest[1], rest[2], rest[3], opts)
    elif c == "promote" and len(rest) == 4:
        chdir_of(rest[1], rest[2])
        promote(rest[1], rest[2], rest[3], opts)
    elif c == "resign" and len(rest) == 3:
        resign(rest[1], rest[2], opts)
    elif c == "snapshot" and len(rest) == 4:
        snapshot(rest[1], rest[2], rest[3])
    elif c == "prune" and len(rest) == 2:
        prune(rest[1])
    elif c == "check" and len(rest) == 2:
        sys.exit(check(rest[1], pubs, tpubs, today or opts["date"]))
    elif c == "installdb" and len(rest) == 4:
        installdb(rest[1], rest[2], rest[3])
    else:
        sys.stderr.write(__doc__)
        sys.exit(2)


if __name__ == "__main__":
    main(sys.argv[1:])
