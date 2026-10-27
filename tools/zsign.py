#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Detached Ed25519 signatures for update bundles and zpkg's package list (DESIGN D21 6, docs/d21-crypto-ssh-https.md;
the format is frozen once shipped, section 12). The signing is the PC's OpenSSL (openssl pkeyutl -rawin, Ed25519
since 1.1.1); the key never enters the build or the repository.

  zsign.py sign [--key KEY] [--kind bundle|index] FILE        FILE.sig, 80 bytes
  zsign.py verify --pub PUB [--pub PUB]... [--kind K] FILE [SIG]  checked with OpenSSL against the given keys
  zsign.py keyid PUB                                            the key id (8 bytes, hex) the .sig names
  zsign.py testkey                                              the TEST key pair, made when missing (below)

KEY: a PKCS#8 PEM Ed25519 private key; by default ~/.config/zenix-signing/zenix-release.key (D15: mode 600, outside
every repository, with an offline backup). PUB: the public half (SubjectPublicKeyInfo PEM), as in tools/keys/ and
by zsign.py testkey (the TEST pair, below). A new release key: openssl genpkey -algorithm ed25519 -out KEY && chmod 600 KEY, and the
public half with openssl pkey -in KEY -pubout -out PUB.

The kind: a file starting with "ZUP1" is a bundle, anything else needs --kind. What is signed (6.1) is the label, a
NUL and the 32-byte SHA-256 the verifier computes itself:
  bundle  "Zenix bundle signature v1": SHA-256 over the header's bytes 0-63 and the table of contents (the value the
          header holds in bytes 64-95; the bundle is checked first, as zup and zenixboot check it)
  index   "Zenix package index v1": SHA-256 of the whole file
The .sig (6.2): "ZSG1", format 1, algorithm 1 (Ed25519), two zero bytes, the key id (the first 8 bytes of SHA-256 of
the 32-byte public key), the 64-byte signature.

The TEST key pair (6.3) is never in the repository: it is made at build or test time, when missing, as
TESTDIR/zenix-test.key (mode 600) and TESTDIR/zenix-test.pub; TESTDIR is $ZSIGN_TEST_DIR, else build/sign-test in the
repository. Test builds trust that public half, so a test bundle made in one build tree verifies in that tree.
Channels (decided 2026-10-27): test bundles carry the channel "test"; "testing" and "stable" are release channels,
and a release bundle is never signed with the test key; nor is a package list ("zpkgindex", D22 5.1) whose header
names a release channel (a "zpkgindex" list without a channel line is refused).
"""
import hashlib
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mkzup  # noqa: E402  (the bundle checks, shared with the bundle builder)

LABELS = {"bundle": b"Zenix bundle signature v1", "index": b"Zenix package index v1"}
RELEASE_CHANNELS = ("stable", "testing")
TEST_DIR = os.environ.get("ZSIGN_TEST_DIR") or os.path.join(os.path.dirname(HERE), "build", "sign-test")
TEST_KEY = os.path.join(TEST_DIR, "zenix-test.key")
TEST_PUB = os.path.join(TEST_DIR, "zenix-test.pub")
DEFAULT_KEY = os.path.join(os.path.expanduser("~"), ".config", "zenix-signing", "zenix-release.key")
ED25519_SPKI = bytes.fromhex("302a300506032b6570032100")      # the DER prefix of an Ed25519 public key


def die(msg):
    sys.stderr.write("zsign: %s\n" % msg)
    sys.exit(1)


def openssl(*args, data=None):
    r = subprocess.run(["openssl"] + list(args), input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return r.returncode, r.stdout, r.stderr.decode(errors="replace")


def raw_public(pem_path, private=False):
    """the 32 raw bytes of an Ed25519 key's public half"""
    a = ["pkey", "-in", pem_path, "-pubout", "-outform", "DER"] if private else \
        ["pkey", "-pubin", "-in", pem_path, "-outform", "DER"]
    rc, out, err = openssl(*a)
    if rc != 0 or len(out) != 44 or not out.startswith(ED25519_SPKI):
        die("%s: not an Ed25519 %s key (%s)" % (pem_path, "private" if private else "public", err.strip()[-200:]))
    return out[12:]


def testkey():
    """the TEST pair in TEST_DIR, made when missing; returns (key, pub)"""
    if not (os.path.exists(TEST_KEY) and os.path.exists(TEST_PUB)):
        os.makedirs(TEST_DIR, exist_ok=True)
        tmp = TEST_KEY + ".part"
        old = os.umask(0o077)
        try:
            rc, _, err = openssl("genpkey", "-algorithm", "ed25519", "-out", tmp)
        finally:
            os.umask(old)
        if rc != 0:
            die("openssl genpkey: %s" % err.strip()[-200:])
        os.chmod(tmp, 0o600)
        rc, _, err = openssl("pkey", "-in", tmp, "-pubout", "-out", TEST_PUB + ".part")
        if rc != 0:
            die("openssl pkey: %s" % err.strip()[-200:])
        os.replace(tmp, TEST_KEY)
        os.replace(TEST_PUB + ".part", TEST_PUB)
    return TEST_KEY, TEST_PUB


def keyid(pub):
    return hashlib.sha256(pub).digest()[:8]


def digest(path, kind):
    data = open(path, "rb").read()
    if kind is None:
        kind = "bundle" if data[:4] == b"ZUP1" else None
    if kind is None:
        die("%s: not a bundle; say --kind index (or another kind D21 6.1 lists)" % path)
    if kind not in LABELS:
        die("unknown kind %r (D21 6.1: bundle, index)" % kind)
    if kind == "bundle":
        try:
            hdr, parts = mkzup.read_bundle(data)
        except ValueError as e:
            die("%s: %s" % (path, e))
        ntoc = len(parts)
        d = hashlib.sha256(data[:64] + data[mkzup.HDR:mkzup.HDR + mkzup.TOCE * ntoc]).digest()
        assert d == data[64:96]          # (read_bundle compared it)
        return kind, d, hdr["channel"]
    return kind, hashlib.sha256(data).digest(), index_channels(data)


def index_channels(data):
    """the channel(s) a zpkg package list names in its header (D22 3.3: the lines before the first "pkg"); a file
    that is not a "zpkgindex" list names none"""
    lines = data.split(b"\n")
    if not lines[0].startswith(b"zpkgindex "):
        return None
    chans = []
    for ln in lines[1:]:
        if ln.startswith(b"pkg ") or ln == b"pkg":
            break
        if ln.startswith(b"channel "):
            chans.append(ln[8:].decode(errors="replace"))
    if not chans:
        die("a zpkgindex list without a channel line (D22 3.3)")
    return chans


def message(kind, d):
    return LABELS[kind] + b"\0" + d


def sign(path, key, kind):
    if not os.path.exists(key):
        die("no signing key %s (D15: kept outside every repository)" % key)
    kind, d, channel = digest(path, kind)
    pub = raw_public(key, private=True)
    chans = channel if isinstance(channel, list) else [channel]
    for c in chans:
        if c in RELEASE_CHANNELS and os.path.exists(TEST_PUB) and pub == raw_public(TEST_PUB):
            die("%s is a release %s (channel %s): refusing to sign it with the test key" % (path, kind, c))
    with tempfile.TemporaryDirectory(prefix="zsign.") as t:
        m = os.path.join(t, "msg")
        open(m, "wb").write(message(kind, d))
        rc, sig, err = openssl("pkeyutl", "-sign", "-inkey", key, "-rawin", "-in", m)
    if rc != 0 or len(sig) != 64:
        die("openssl could not sign: %s" % err.strip()[-300:])
    blob = b"ZSG1" + bytes([1, 1, 0, 0]) + keyid(pub) + sig
    assert len(blob) == 80
    with open(path + ".sig.part", "wb") as f:
        f.write(blob)
    os.replace(path + ".sig.part", path + ".sig")
    print("%s.sig: %s, key %s" % (path, kind, keyid(pub).hex()))


def verify(path, sigpath, pubs, kind):
    kind, d, _ = digest(path, kind)
    s = open(sigpath, "rb").read()
    if len(s) != 80 or s[:4] != b"ZSG1" or s[4:8] != bytes([1, 1, 0, 0]):
        die("%s: not a ZSG1 format 1 Ed25519 signature file" % sigpath)
    tried = False
    with tempfile.TemporaryDirectory(prefix="zsign.") as t:
        m, sg = os.path.join(t, "msg"), os.path.join(t, "sig")
        open(m, "wb").write(message(kind, d))
        open(sg, "wb").write(s[16:])
        for p in pubs:
            if keyid(raw_public(p)) != s[8:16]:
                continue
            tried = True
            rc, out, err = openssl("pkeyutl", "-verify", "-pubin", "-inkey", p, "-rawin", "-in", m, "-sigfile", sg)
            if rc == 0:
                print("%s: good signature (%s) by %s, key %s" % (path, kind, p, s[8:16].hex()))
                return 0
    die("%s: %s" % (path, "BAD signature" if tried else "signed by key %s, which is not among the given ones" % s[8:16].hex()))


def main(a):
    if not a:
        sys.exit(__doc__)
    cmd, rest, key, kind, pubs = a[0], [], DEFAULT_KEY, None, []
    i = 1
    while i < len(a):
        if a[i] in ("--key", "--kind", "--pub") and i + 1 < len(a):
            if a[i] == "--key":
                key = a[i + 1]
            elif a[i] == "--kind":
                kind = a[i + 1]
            else:
                pubs.append(a[i + 1])
            i += 2
        else:
            rest.append(a[i])
            i += 1
    if cmd == "sign" and len(rest) == 1:
        sign(rest[0], key, kind)
    elif cmd == "verify" and len(rest) in (1, 2) and pubs:
        sys.exit(verify(rest[0], rest[1] if len(rest) == 2 else rest[0] + ".sig", pubs, kind))
    elif cmd == "testkey" and not rest:
        k, p = testkey()
        print("%s %s (key %s)" % (k, p, keyid(raw_public(p)).hex()))
    elif cmd == "keyid" and len(rest) == 1:
        print(keyid(raw_public(rest[0])).hex())
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
