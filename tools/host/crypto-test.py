#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The crypto library against outside answers (DESIGN D21 10 step 0), through `crypto-test oracle`:

  crypto-test.py ORACLE wycheproof DIR    Wycheproof's vectors (Apache-2.0 data, never committed) from DIR, fetched
                                          there by crypto-test.sh the first time: X25519 (the all-zero results must be
                                          refused), Ed25519, ChaCha20-Poly1305 (both implementations), AES-GCM, ECDSA
                                          P-256 (DER and raw) and P-384, RSA PKCS#1 v1.5 2048 and 4096, HMAC-SHA-256
  crypto-test.py ORACLE anchors X509TEST OUTDIR
                                          tools/mkanchors.py and the X.509 wrapper (tools/host/x509-test.c) with a
                                          test CA made here, and every Mozilla root looked up in the anchors file
  crypto-test.py ORACLE seeds FUZZDIR [ANCHORS]  the seed inputs of tools/host/crypto-fuzz.c's harnesses
  crypto-test.py ORACLE sign OUTDIR       tools/zsign.py (OpenSSL) and the library's .sig check both ways (D21 6)
  crypto-test.py ORACLE openssl [N]       differential runs against the PC's OpenSSL (Python's cryptography module and
                                          hashlib, which use it): N random cases of each (default 200)

Prints PASS/FAIL lines; exits 1 on a failure, 3 when what it needs is missing (the caller reports "skipped").
"""
import hashlib
import hmac
import json
import os
import random
import subprocess
import sys


class Oracle:
    def __init__(self, path):
        self.p = subprocess.Popen([path, "oracle"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

    def ask(self, *words):
        line = " ".join((w or "-") if isinstance(w, str) else (w.hex() if w else "-") for w in words)
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()
        r = self.p.stdout.readline()
        if not r:
            raise SystemExit("FAIL crypto-test.py: the oracle stopped (a sanitizer report above?)")
        return r.strip()

    def close(self):
        self.p.stdin.close()
        return self.p.wait()


fails = 0


def result(name, ok, n, bad):
    global fails
    if bad:
        fails += 1
        print("FAIL crypto-test.py: %s: %d of %d wrong, first: %s" % (name, len(bad), n, bad[:5]))
    else:
        print("PASS crypto-test.py: %s (%d cases)" % (name, n))


def unhex(s):
    return bytes.fromhex(s) if s and s != "-" else b""


# ---- Wycheproof ----
def wy(d, name):
    with open(os.path.join(d, name + ".json")) as f:
        return json.load(f)


def wy_x25519(o, d):
    bad, n = [], 0
    for g in wy(d, "x25519_test")["testGroups"]:
        for t in g["tests"]:
            if len(t["public"]) != 64 or len(t["private"]) != 64:
                continue
            for impl in ("bear", "mono"):
                n += 1
                r = o.ask("x25519", impl, t["private"], t["public"])
                if t["shared"] == "00" * 32:
                    if r != "ZERO":
                        bad.append((impl, t["tcId"], "zero not refused"))
                elif t["result"] == "invalid":
                    pass                    # (none with 32-byte inputs in this file but the zero ones)
                elif r != t["shared"]:
                    bad.append((impl, t["tcId"], r))
    result("Wycheproof X25519 (BearSSL and Monocypher; all-zero refused)", True, n, bad)


def wy_ed25519(o, d):
    bad, n = [], 0
    for g in wy(d, "ed25519_test")["testGroups"]:
        pk = g["publicKey"]["pk"]
        for t in g["tests"]:
            if t["result"] == "acceptable":     # small- and mixed-order public keys: RFC 8032 lets either answer
                continue
            n += 1
            r = o.ask("ed25519_verify", pk, t["msg"] or "-", t["sig"] or "-")
            want = "1" if t["result"] == "valid" else "0"
            if r != want:
                bad.append((t["tcId"], t["result"], r))
    result("Wycheproof Ed25519 verification (Monocypher)", True, n, bad)
    # audit 2026-10-28 S19: small-order keys and R refused (the identity as both: S = 0 verified any message)
    ident = "01" + "00" * 31
    sbad = [x for x in (("identity", ident, ident + "00" * 32),
                        ("order 8", "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc05", ident + "00" * 32))
            if o.ask("ed25519_verify", x[1], "6869", x[2]) != "0"]
    result("Ed25519: small-order public keys and R refused", True, 2, sbad)


def wy_aead(o, d, name, cmd):
    bad, n = [], 0
    for g in wy(d, name)["testGroups"]:
        for t in g["tests"]:
            impls = ("bear", "mono") if cmd == "aead" else ("",)
            for impl in impls:
                n += 1
                pre = [cmd + "_open"] + ([impl] if impl else [])
                r = o.ask(*pre, t["key"], t["iv"] or "-", t["aad"] or "-", t["ct"] or "-", t["tag"] or "-")
                if t["result"] == "valid":
                    if r != (t["msg"] or "-"):
                        bad.append((impl, t["tcId"], "open", r[:20]))
                    if cmd == "aead":
                        r = o.ask("aead_seal", impl, t["key"], t["iv"], t["aad"] or "-", t["msg"] or "-")
                    else:
                        r = o.ask("gcm_seal", t["key"], t["iv"], t["aad"] or "-", t["msg"] or "-", str(len(t["tag"]) // 2))
                    if r != "%s %s" % (t["ct"] or "-", t["tag"]):
                        bad.append((impl, t["tcId"], "seal", r[:20]))
                elif r not in ("FAIL", "ERR"):
                    bad.append((impl, t["tcId"], "accepted", t["comment"]))
    result("Wycheproof %s" % name, True, n, bad)


def wy_ecdsa(o, d, name, curve, cmd):
    bad, n = [], 0
    for g in wy(d, name)["testGroups"]:
        q = g["publicKey"]["uncompressed"]
        alg = g["sha"].replace("SHA-", "sha")
        for t in g["tests"]:
            if t["result"] == "acceptable":
                continue
            n += 1
            r = o.ask(cmd, curve, alg, q, t["msg"] or "-", t["sig"] or "-")
            if r != ("1" if t["result"] == "valid" else "0"):
                bad.append((t["tcId"], t["result"], r, t["comment"]))
    result("Wycheproof %s (%s)" % (name, cmd), True, n, bad)


def wy_rsa(o, d, name):
    bad, n = [], 0
    for g in wy(d, name)["testGroups"]:
        nn, e = g["publicKey"]["modulus"], g["publicKey"]["publicExponent"]
        alg = g["sha"].replace("SHA-", "sha")
        for t in g["tests"]:
            if t["result"] == "acceptable":
                continue
            n += 1
            r = o.ask("rsa_vrfy", alg, nn, e, t["msg"] or "-", t["sig"] or "-")
            if r != ("1" if t["result"] == "valid" else "0"):
                bad.append((t["tcId"], t["result"], r, t["comment"]))
    result("Wycheproof %s (rsa_i15 PKCS#1 v1.5)" % name, True, n, bad)


def wy_hmac(o, d, name, alg):
    bad, n = [], 0
    for g in wy(d, name)["testGroups"]:
        for t in g["tests"]:
            n += 1
            r = o.ask("hmac", alg, t["key"] or "-", t["msg"] or "-")
            ok = r[:len(t["tag"])] == t["tag"]
            if ok != (t["result"] == "valid"):
                bad.append((t["tcId"], t["result"]))
    result("Wycheproof %s" % name, True, n, bad)


def wycheproof(o, d):
    wy_x25519(o, d)
    wy_ed25519(o, d)
    wy_aead(o, d, "chacha20_poly1305_test", "aead")
    wy_aead(o, d, "aes_gcm_test", "gcm")
    wy_ecdsa(o, d, "ecdsa_secp256r1_sha256_test", "p256", "ecdsa_vrfy")
    wy_ecdsa(o, d, "ecdsa_secp256r1_sha256_p1363_test", "p256", "ecdsa_vrfy_raw")
    wy_ecdsa(o, d, "ecdsa_secp384r1_sha384_test", "p384", "ecdsa_vrfy")
    wy_rsa(o, d, "rsa_signature_2048_sha256_test")
    wy_rsa(o, d, "rsa_signature_4096_sha512_test")
    wy_hmac(o, d, "hmac_sha256_test", "sha256")


# ---- against OpenSSL ----
def openssl(o, count):
    try:
        from cryptography.hazmat.primitives.asymmetric import x25519, ed25519, rsa, ec, padding, utils
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
        from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305, AESGCM
        from cryptography.hazmat.primitives.poly1305 import Poly1305
        from cryptography.hazmat.backends import default_backend
    except ImportError:
        print("crypto-test.py: Python's cryptography module (OpenSSL) is missing")
        sys.exit(3)
    print("crypto-test.py: against %s" % default_backend().openssl_version_text())
    rnd = random.Random(int.from_bytes(os.urandom(8), "big"))
    rb = lambda n: bytes(rnd.getrandbits(8) for _ in range(n))
    raw = serialization.Encoding.Raw, serialization.PublicFormat.Raw

    bad = []
    for i in range(count):
        a, b = x25519.X25519PrivateKey.generate(), rb(32)
        pub = a.public_key().public_bytes(*raw)
        sk_b = x25519.X25519PrivateKey.from_private_bytes(b)
        want = sk_b.exchange(x25519.X25519PublicKey.from_public_bytes(pub)).hex()
        for impl in ("bear", "mono"):
            r = o.ask("x25519", impl, b, pub)
            if r != want:
                bad.append((impl, b.hex(), pub.hex()))
    result("X25519 against OpenSSL", True, 2 * count, bad)

    bad = []
    for i in range(count):
        seed, msg = rb(32), rb(rnd.randrange(0, 300))
        k = ed25519.Ed25519PrivateKey.from_private_bytes(seed)
        sig = k.sign(msg)
        if o.ask("ed25519_sign", seed, msg) != sig.hex():
            bad.append(("sign", seed.hex()))
        pk = k.public_key().public_bytes(*raw)
        if o.ask("ed25519_verify", pk, msg, sig) != "1":
            bad.append(("verify", seed.hex()))
        bs = bytearray(sig)
        bs[rnd.randrange(64)] ^= 1 << rnd.randrange(8)
        if o.ask("ed25519_verify", pk, msg, bytes(bs)) != "0":
            bad.append(("changed accepted", seed.hex()))
    result("Ed25519 against OpenSSL (sign, verify, a flipped bit)", True, 3 * count, bad)

    bad = []
    for i in range(count):
        k, nn, aad, pt = rb(32), rb(12), rb(rnd.randrange(0, 40)), rb(rnd.randrange(0, 600))
        ct = ChaCha20Poly1305(k).encrypt(nn, pt, aad)
        for impl in ("bear", "mono"):
            if o.ask("aead_seal", impl, k, nn, aad, pt) != "%s %s" % (ct[:-16].hex() or "-", ct[-16:].hex()):
                bad.append((impl, "seal"))
            if o.ask("aead_open", impl, k, nn, aad, ct[:-16], ct[-16:]) != (pt.hex() or "-"):
                bad.append((impl, "open"))
    result("ChaCha20-Poly1305 (RFC 8439) against OpenSSL", True, 4 * count, bad)

    bad = []
    for i in range(count):
        k, iv, aad, pt = rb(rnd.choice((16, 24, 32))), rb(rnd.choice((12, 12, 8, 16))), rb(rnd.randrange(0, 40)), rb(rnd.randrange(0, 600))
        ct = AESGCM(k).encrypt(iv, pt, aad) if len(iv) >= 8 else None
        if o.ask("gcm_seal", k, iv, aad, pt, "16") != "%s %s" % (ct[:-16].hex() or "-", ct[-16:].hex()):
            bad.append(("seal", len(k), len(iv)))
        if o.ask("gcm_open", k, iv, aad, ct[:-16], ct[-16:]) != (pt.hex() or "-"):
            bad.append(("open", len(k), len(iv)))
    result("AES-GCM (aes_ct, ghash_ctmul32) against OpenSSL", True, 2 * count, bad)

    bad = []
    for i in range(count):
        k, iv, data = rb(rnd.choice((16, 32))), rb(16), rb(rnd.randrange(0, 300))
        if i % 3 == 0:
            iv = iv[:8] + b"\xff" * 7 + bytes([rnd.randrange(0xf0, 0x100)])     # the carry through 64 bits
        want = Cipher(algorithms.AES(k), modes.CTR(iv)).encryptor().update(data)
        if o.ask("aes_ctr", k, iv, data) != (want.hex() or "-"):
            bad.append((k.hex(), iv.hex()))
    result("AES-CTR with a 128-bit counter (key files) against OpenSSL", True, count, bad)

    bad = []
    for i in range(count):
        m = rb(rnd.randrange(0, 1000))
        for alg in ("sha1", "sha256", "sha384", "sha512", "sha512m"):
            if o.ask("hash", alg, m) != hashlib.new(alg.rstrip("m"), m).hexdigest():
                bad.append(alg)
        k = rb(rnd.randrange(0, 200))
        for alg in ("sha1", "sha256", "sha512"):
            if o.ask("hmac", alg, k, m) != hmac.new(k, m, alg).hexdigest():
                bad.append("hmac-" + alg)
    result("SHA-1/256/384/512, HMAC against OpenSSL", True, 8 * count, bad)

    # ssh's chacha20-poly1305@openssh.com, built here from OpenSSL's ChaCha20 (16-byte IV: 64-bit little-endian block
    # counter, then the 64-bit nonce = the sequence number, big-endian) and Poly1305
    def ssh_seal(key, seq, pkt):
        n8 = seq.to_bytes(8, "big")
        c = lambda k, ctr, d: Cipher(algorithms.ChaCha20(k, ctr.to_bytes(8, "little") + n8), None).encryptor().update(d)
        enc = c(key[32:], 0, pkt[:4]) + c(key[:32], 1, pkt[4:])
        pk = c(key[:32], 0, bytes(32))
        return enc, Poly1305.generate_tag(pk, enc)
    bad = []
    for i in range(count):
        key, seq = rb(64), rnd.choice((0, 1, 2, rnd.getrandbits(32), 0xFFFFFFFF))
        body = rb(rnd.randrange(1, 200))
        pkt = len(body).to_bytes(4, "big") + body
        enc, tag = ssh_seal(key, seq, pkt)
        if o.ask("ssh_seal", key, str(seq), pkt) != "%s %s" % (enc.hex(), tag.hex()):
            bad.append(("seal", seq))
        if o.ask("ssh_open", key, str(seq), enc, tag) != body.hex():
            bad.append(("open", seq))
        t2 = bytearray(tag)
        t2[rnd.randrange(16)] ^= 1
        if o.ask("ssh_open", key, str(seq), enc, bytes(t2)) != "FAIL":
            bad.append(("bad tag accepted", seq))
    result("chacha20-poly1305@openssh.com against OpenSSL's ChaCha20 and Poly1305", True, 3 * count, bad)

    bad = []
    for i in range(count // 4 + 1):
        k = bytearray(rb(32))
        if i % 4 == 0:
            k[0] |= 0x80            # the mpint's extra zero byte
        if i % 4 == 1:
            k[0] = 0
            k[1] |= 0x80
        h, sid, letter, n = rb(32), rb(32), rnd.choice("ABCDEF"), rnd.choice((16, 32, 64, 65, 100))
        v = bytes(k).lstrip(b"\0")
        if v and v[0] & 0x80:
            v = b"\0" + v
        mp = len(v).to_bytes(4, "big") + v
        out = hashlib.sha256(mp + h + letter.encode() + sid).digest()
        while len(out) < n:
            out += hashlib.sha256(mp + h + out).digest()
        if o.ask("ssh_kdf", bytes(k), h, sid, letter, str(n)) != out[:n].hex():
            bad.append(i)
    result("ssh key derivation (RFC 4253 7.2, key extension, mpint K) against hashlib", True, count // 4 + 1, bad)

    bad = []
    for bits, alg, h in ((2048, "sha256", hashes.SHA256()), (4096, "sha512", hashes.SHA512()), (3072, "sha384", hashes.SHA384())):
        k = rsa.generate_private_key(public_exponent=65537, key_size=bits)
        nums = k.public_key().public_numbers()
        nn, e = nums.n.to_bytes(bits // 8, "big"), nums.e.to_bytes(3, "big")
        for i in range(max(3, count // 20)):
            m = rb(rnd.randrange(0, 200))
            sig = k.sign(m, padding.PKCS1v15(), h)
            if o.ask("rsa_vrfy", alg, nn, e, m, sig) != "1":
                bad.append((bits, "valid refused"))
            s2 = bytearray(sig)
            s2[rnd.randrange(len(s2))] ^= 1
            if o.ask("rsa_vrfy", alg, nn, e, m, bytes(s2)) != "0":
                bad.append((bits, "changed accepted"))
    result("RSA PKCS#1 v1.5 verification (i15) against OpenSSL", True, 6 * max(3, count // 20), bad)

    bad = []
    for curve, cname, alg, h in ((ec.SECP256R1(), "p256", "sha256", hashes.SHA256()), (ec.SECP384R1(), "p384", "sha384", hashes.SHA384()),
                                 (ec.SECP521R1(), "p521", "sha512", hashes.SHA512())):
        k = ec.generate_private_key(curve)
        q = k.public_key().public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
        size = (curve.key_size + 7) // 8
        for i in range(max(3, count // 20)):
            m = rb(rnd.randrange(0, 200))
            der = k.sign(m, ec.ECDSA(h))
            r_, s_ = utils.decode_dss_signature(der)
            rawsig = r_.to_bytes(size, "big") + s_.to_bytes(size, "big")
            if o.ask("ecdsa_vrfy", cname, alg, q, m, der) != "1" or o.ask("ecdsa_vrfy_raw", cname, alg, q, m, rawsig) != "1":
                bad.append((cname, "valid refused"))
            if o.ask("ecdsa_vrfy_raw", cname, alg, q, m + b"x", rawsig) != "0":
                bad.append((cname, "other message accepted"))
    result("ECDSA P-256/P-384/P-521 verification (i15, m15) against OpenSSL", True, 9 * max(3, count // 20), bad)


# ---- tools/zsign.py and the library's .sig check (D21 6.1-6.3) ----
def sign(o, out):
    import struct
    import shutil
    import tempfile
    here = os.path.dirname(os.path.abspath(__file__))
    zsign = os.path.join(here, "..", "zsign.py")
    d = os.path.join(out, "sign")
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    tdir = os.path.join(d, "sign-test")         # the TEST pair, made here by zsign.py testkey (never in git)
    tkey, tpub = os.path.join(tdir, "zenix-test.key"), os.path.join(tdir, "zenix-test.pub")
    env = dict(os.environ, ZSIGN_TEST_DIR=tdir)

    def zs(*a):
        r = subprocess.run([sys.executable, "-I", zsign] + list(a), stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
        return r.returncode, (r.stdout + r.stderr).decode(errors="replace")

    rc, msg = zs("testkey")
    ok0 = rc == 0 and os.path.exists(tkey) and (os.stat(tkey).st_mode & 0o777) == 0o600
    result("zsign testkey makes the TEST pair when missing, the key mode 600", True, 1, [] if ok0 else [msg])
    k1 = open(tkey, "rb").read() if ok0 else b""
    zs("testkey")
    result("zsign testkey keeps an existing pair", True, 1, [] if open(tkey, "rb").read() == k1 else ["remade"])

    def raw(pem, private=False):
        a = ["openssl", "pkey", "-in", pem, "-pubout", "-outform", "DER"] if private else \
            ["openssl", "pkey", "-pubin", "-in", pem, "-outform", "DER"]
        return subprocess.run(a, stdout=subprocess.PIPE, check=True).stdout[12:]

    def bundle(path, channel, part=b"kernel bytes"):
        # a small ZUP1 bundle as tools/mkzup.py lays it out (header, table, one part at a 16-byte boundary)
        toc_end = 96 + 80
        off = (toc_end + 15) & ~15
        toc = struct.pack(">40sII32s", b"boot/zenix", off, len(part), hashlib.sha256(part).digest())
        total = off + len(part) + ((-len(part)) & 15)
        head = struct.pack(">4sIII16sII8sIIII", b"ZUP1", 1, 96, 1001, b"1.0.1", 20261027, 0, channel.encode(), 96, 1,
                           total, 0)
        blob = head + hashlib.sha256(head + toc).digest() + toc
        blob += bytes(off - len(blob)) + part + bytes((-len(part)) & 15)
        open(path, "wb").write(blob)
        return hashlib.sha256(head + toc).digest()

    ok = lambda name, c, detail="": result(name, True, 1, [] if c else [detail])
    tp = raw(tpub)
    rkey, rpub = os.path.join(d, "release.key"), os.path.join(d, "release.pub")
    subprocess.run(["openssl", "genpkey", "-algorithm", "ed25519", "-out", rkey], check=True, stderr=subprocess.PIPE)
    subprocess.run(["openssl", "pkey", "-in", rkey, "-pubout", "-out", rpub], check=True)
    rp = raw(rpub)
    b = os.path.join(d, "test.zup")
    dg = bundle(b, "test")
    rc, msg = zs("sign", "--key", tkey, b)
    s = open(b + ".sig", "rb").read() if rc == 0 else b""
    ok("zsign: a test bundle signed with the test key, 80 bytes, ZSG1 1 1 0 0, its key id", rc == 0 and len(s) == 80 and
       s[:8] == b"ZSG1\1\1\0\0" and s[8:16] == hashlib.sha256(tp).digest()[:8], msg)
    rc, msg = zs("verify", "--pub", tpub, b)
    ok("zsign: OpenSSL verifies it", rc == 0 and "good signature" in msg, msg)
    ok("library: zc_sig_verify takes it (the digest it computed itself, key 0)",
       o.ask("sig_verify", "bundle", dg, s, tp) == "0 0")
    ok("library: another label (index) fails", o.ask("sig_verify", "index", dg, s, tp) == "%d -1" % -3)
    ok("library: another key only: unknown key id", o.ask("sig_verify", "bundle", dg, s, rp) == "%d -1" % -2)
    ok("library: the second of two keys", o.ask("sig_verify", "bundle", dg, s, rp, tp) == "0 1")
    data = bytearray(open(b, "rb").read())
    data[15] ^= 1                                       # the version number in the header
    data[64:96] = hashlib.sha256(bytes(data[:64]) + bytes(data[96:176])).digest()
    open(b + "2", "wb").write(bytes(data))
    shutil.copy(b + ".sig", b + "2.sig")
    rc, msg = zs("verify", "--pub", tpub, b + "2")
    dg2 = hashlib.sha256(bytes(data[:64]) + bytes(data[96:176])).digest()
    ok("a changed version (the header's digest recomputed): OpenSSL and the library refuse", rc != 0 and "BAD" in msg and
       o.ask("sig_verify", "bundle", dg2, s, tp) == "%d -1" % -3, msg)
    data[40] ^= 1                                       # the channel byte, the digest not recomputed
    open(b + "2", "wb").write(bytes(data))
    rc, msg = zs("verify", "--pub", tpub, b + "2")
    ok("a damaged bundle (header digest wrong) is refused before any signature", rc != 0 and "SHA-256" in msg, msg)
    rel = os.path.join(d, "release.zup")
    dg = bundle(rel, "stable")
    rc, msg = zs("sign", "--key", tkey, rel)
    ok("zsign refuses to sign a release bundle (stable) with the test key", rc != 0 and "test key" in msg and
       not os.path.exists(rel + ".sig"), msg)
    bundle(rel, "testing")
    rc, msg = zs("sign", "--key", tkey, rel)
    ok("... and a testing-channel bundle (testing is a release channel, 6.3)", rc != 0 and "test key" in msg, msg)
    dg = bundle(rel, "stable")
    rc, msg = zs("sign", "--key", rkey, rel)
    s = open(rel + ".sig", "rb").read() if rc == 0 else b""
    ok("a release bundle signed with a release key; the library verifies it with the release key, not the test key",
       rc == 0 and o.ask("sig_verify", "bundle", dg, s, rp) == "0 0" and
       o.ask("sig_verify", "bundle", dg, s, tp) == "%d -1" % -2, msg)
    idx = os.path.join(d, "index")
    open(idx, "wb").write(b"zpkg index\nbase 1001 ...\n")
    rc, msg = zs("sign", "--key", tkey, idx)
    ok("an index needs --kind", rc != 0 and "--kind" in msg, msg)
    rc, msg = zs("sign", "--key", tkey, "--kind", "index", idx)
    s = open(idx + ".sig", "rb").read() if rc == 0 else b""
    dg = hashlib.sha256(open(idx, "rb").read()).digest()
    ok("an index: SHA-256 of the whole file under \"Zenix package index v1\"; OpenSSL and the library",
       rc == 0 and zs("verify", "--kind", "index", "--pub", tpub, idx)[0] == 0 and
       o.ask("sig_verify", "index", dg, s, tp) == "0 0", msg)
    # D22 5.1: a package list of a release channel is never signed with the test key (as release bundles)
    for chan in ("testing", "stable"):
        lst = os.path.join(d, "index-" + chan)
        open(lst, "wb").write(b"zpkgindex 1\nchannel %s\nserial 2026110301\npkg awk\nchannel test\n" % chan.encode())
        rc, msg = zs("sign", "--key", tkey, "--kind", "index", lst)
        ok("zsign refuses to sign a %s package list with the test key" % chan, rc != 0 and "test key" in msg and
           not os.path.exists(lst + ".sig"), msg)
        rc, msg = zs("sign", "--key", rkey, "--kind", "index", lst)
        ok("... and signs it with a release key", rc == 0 and os.path.getsize(lst + ".sig") == 80, msg)
    lst = os.path.join(d, "index-test")
    open(lst, "wb").write(b"zpkgindex 1\nchannel test\nserial 2026110301\npkg awk\nchannel stable\n")
    rc, msg = zs("sign", "--key", tkey, "--kind", "index", lst)
    ok("a test-channel package list is signed with the test key (a block's own words are not the header)", rc == 0, msg)
    open(lst, "wb").write(b"zpkgindex 1\nchannel test\nchannel stable\nserial 2026110301\n")
    rc, msg = zs("sign", "--key", tkey, "--kind", "index", lst + "")
    ok("a list naming a release channel in a second channel line is refused with the test key", rc != 0 and "test key" in msg, msg)
    open(lst, "wb").write(b"zpkgindex 1\nserial 2026110301\npkg awk\n")
    rc, msg = zs("sign", "--key", tkey, "--kind", "index", lst)
    ok("a zpkgindex list without a channel line is refused", rc != 0 and "channel" in msg, msg)
    # and back: the library signs (Monocypher), OpenSSL verifies; Ed25519 is deterministic, so both sign alike
    seed = os.urandom(32)
    der = bytes.fromhex("302e020100300506032b657004220420") + seed
    pk = os.path.join(d, "lib.key")
    subprocess.run(["openssl", "pkey", "-inform", "DER", "-out", pk], input=der, check=True)
    subprocess.run(["openssl", "pkey", "-in", pk, "-pubout", "-out", pk + ".pub"], check=True)
    dg = bundle(b, "test", b"other bytes")
    libsig = bytes.fromhex(o.ask("sig_make", seed, "bundle", dg))
    open(b + ".sig", "wb").write(libsig)
    rc, msg = zs("verify", "--pub", pk + ".pub", b)
    ok("the library's .sig verified by OpenSSL", rc == 0, msg)
    zs("sign", "--key", pk, b)
    ok("OpenSSL's and the library's .sig are the same bytes", open(b + ".sig", "rb").read() == libsig)
    shutil.rmtree(d, ignore_errors=True)


# ---- tools/mkanchors.py, the anchors file and the X.509 wrapper (D21 8.1-8.2) ----
def anchors(x509test, out):
    import datetime
    import struct
    import shutil
    from cryptography import x509
    from cryptography.x509.oid import NameOID, ExtendedKeyUsageOID
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa, ec, padding
    here = os.path.dirname(os.path.abspath(__file__))
    mk = os.path.join(here, "..", "mkanchors.py")
    d = os.path.join(out, "anchors")
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    now = datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0)
    days = (now.date() - datetime.date(1970, 1, 1)).days + 719528
    secs = now.hour * 3600 + now.minute * 60 + now.second

    def name(cn):
        return x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, cn), x509.NameAttribute(NameOID.ORGANIZATION_NAME, "Zenix test")])

    def cert(subj, key, issuer, ikey, ca, days_from=-1, days_to=30, san=None, eku="server", h=hashes.SHA256(), pss=False, big=0):
        b = (x509.CertificateBuilder().subject_name(name(subj)).issuer_name(name(issuer)).public_key(key.public_key())
             .serial_number(x509.random_serial_number()).not_valid_before(now + datetime.timedelta(days=days_from))
             .not_valid_after(now + datetime.timedelta(days=days_to))
             .add_extension(x509.BasicConstraints(ca=ca, path_length=None), critical=True))
        if ca:
            b = b.add_extension(x509.KeyUsage(False, False, False, False, False, True, True, False, False), critical=True)
        else:
            b = b.add_extension(x509.KeyUsage(True, False, True, False, False, False, False, False, False), critical=True)
        if san:
            b = b.add_extension(x509.SubjectAlternativeName([x509.DNSName(n) for n in san]), critical=False)
        if eku == "server":
            b = b.add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH, ExtendedKeyUsageOID.CLIENT_AUTH]), critical=False)
        elif eku == "client":
            b = b.add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.CLIENT_AUTH]), critical=False)
        if big:
            b = b.add_extension(x509.UnrecognizedExtension(x509.ObjectIdentifier("1.3.6.1.4.1.99999.1"), os.urandom(big)), critical=False)
        kw = {"rsa_padding": padding.PSS(mgf=padding.MGF1(h), salt_length=32)} if pss else {}
        return b.sign(ikey, h, **kw)

    def save(c, nm):
        p = os.path.join(d, nm + ".der")
        open(p, "wb").write(c.public_bytes(serialization.Encoding.DER))
        return p

    def pem(c, nm):
        p = os.path.join(d, nm + ".pem")
        open(p, "wb").write(c.public_bytes(serialization.Encoding.PEM))
        return p

    rk = rsa.generate_private_key(65537, 4096)
    root = cert("Zenix Test Root R", rk, "Zenix Test Root R", rk, True, -10, 3650)
    ik = rsa.generate_private_key(65537, 2048)
    inter = cert("Zenix Test Inter R", ik, "Zenix Test Root R", rk, True, -10, 1000)
    lk = rsa.generate_private_key(65537, 2048)
    leaf = cert("test.zenix", lk, "Zenix Test Inter R", ik, False, san=["test.zenix", "*.wild.zenix"])
    erk = ec.generate_private_key(ec.SECP384R1())
    eroot = cert("Zenix Test Root E", erk, "Zenix Test Root E", erk, True, -10, 3650, h=hashes.SHA384())
    eik = ec.generate_private_key(ec.SECP384R1())
    einter = cert("Zenix Test Inter E", eik, "Zenix Test Root E", erk, True, -10, 1000, h=hashes.SHA384())
    elk = ec.generate_private_key(ec.SECP256R1())
    eleaf = cert("test.zenix", elk, "Zenix Test Inter E", eik, False, san=["test.zenix"], h=hashes.SHA384())
    other = rsa.generate_private_key(65537, 2048)
    oroot = cert("Zenix Other Root", other, "Zenix Other Root", other, True, -10, 3650)
    oinner = cert("Zenix Test Inter R", ik, "Zenix Other Root", other, True, -10, 1000)
    files = {n: save(c, n) for n, c in (("root", root), ("inter", inter), ("leaf", leaf), ("eroot", eroot),
                                         ("einter", einter), ("eleaf", eleaf), ("oinner", oinner))}
    files["expired"] = save(cert("test.zenix", lk, "Zenix Test Inter R", ik, False, -30, -1, san=["test.zenix"]), "expired")
    files["future"] = save(cert("test.zenix", lk, "Zenix Test Inter R", ik, False, 2, 30, san=["test.zenix"]), "future")
    files["client"] = save(cert("test.zenix", lk, "Zenix Test Inter R", ik, False, san=["test.zenix"], eku="client"), "client")
    files["noeku"] = save(cert("test.zenix", lk, "Zenix Test Inter R", ik, False, san=["test.zenix"], eku=None), "noeku")
    files["pss"] = save(cert("test.zenix", lk, "Zenix Test Inter R", ik, False, san=["test.zenix"], pss=True), "pss")
    # SHA-1: Python's module will not sign with it; OpenSSL's command line still does
    kp, ip_, cp = os.path.join(d, "lk.pem"), os.path.join(d, "ik.pem"), os.path.join(d, "inter.pem")
    open(kp, "wb").write(lk.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
    open(ip_, "wb").write(ik.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
    pem(inter, "inter")
    subprocess.run(["openssl", "req", "-new", "-key", kp, "-subj", "/CN=test.zenix/O=Zenix test", "-out", kp + ".csr"],
                   check=True, stderr=subprocess.PIPE)
    ext = os.path.join(d, "ext")
    open(ext, "w").write("subjectAltName=DNS:test.zenix\nbasicConstraints=critical,CA:FALSE\n")
    r = subprocess.run(["openssl", "x509", "-req", "-in", kp + ".csr", "-CA", cp, "-CAkey", ip_, "-sha1", "-days", "30",
                        "-extfile", ext, "-outform", "DER", "-out", os.path.join(d, "sha1.der")],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    files["sha1"] = os.path.join(d, "sha1.der") if r.returncode == 0 else None
    wk = rsa.generate_private_key(65537, 1024)
    files["weak"] = save(cert("test.zenix", wk, "Zenix Test Inter R", ik, False, san=["test.zenix"]), "weak")
    files["fat"] = save(cert("test.zenix", lk, "Zenix Test Inter R", ik, False, san=["test.zenix"], big=15000), "fat")
    nas = rsa.generate_private_key(65537, 2048)
    files["nas"] = save(cert("nas.lan", nas, "nas.lan", nas, False, san=["nas.lan"]), "nas")
    an = os.path.join(d, "test.anchors")
    r = subprocess.run([sys.executable, "-I", mk, "--no-certdata", "--pem", pem(root, "root"), "--pem", pem(eroot, "eroot"), an],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    result("mkanchors.py with a test CA (RSA-4096 and P-384 roots)", r.returncode == 0 and b"2 anchors" in r.stdout, 1,
           [] if r.returncode == 0 else [r.stdout])

    def chain(names, server="test.zenix", anchorfile=an, local=(), t=(days, secs)):
        a = [x509test, "chain", anchorfile, server, str(t[0]), str(t[1])]
        for l in local:
            a += ["-l", files[l]]
        a += [files[n] for n in names]
        r = subprocess.run(a, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        return r.stdout.decode().strip() if r.returncode == 0 else "crash %d %s" % (r.returncode, r.stderr[-300:])

    cases = [
        ("an RSA chain (RSA-2048 leaf, RSA-2048 intermediate, RSA-4096 root)", ["leaf", "inter"], {}, "0 key"),
        ("an ECDSA chain (P-256 leaf, P-384 intermediate and root)", ["eleaf", "einter"], {}, "0 key"),
        ("the root sent too", ["leaf", "inter", "root"], {}, "0 key"),
        ("a wildcard name", ["leaf", "inter"], {"server": "a.wild.zenix"}, "0 key"),
        ("the wrong name", ["leaf", "inter"], {"server": "other.zenix"}, "56 nokey"),
        ("expired", ["expired", "inter"], {}, "54 nokey"),
        ("not yet valid", ["future", "inter"], {}, "54 nokey"),
        ("an unknown CA", ["leaf", "oinner"], {}, "62 nokey"),
        ("a missing intermediate", ["leaf"], {}, "62 nokey"),
        ("no serverAuth in the Extended Key Usage", ["client", "inter"], {}, "1001 nokey"),
        ("no Extended Key Usage at all", ["noeku", "inter"], {}, "0 key"),
        ("RSA-PSS (BearSSL has none)", ["pss", "inter"], {}, "nokey"),
        ("SHA-1 (never set up)", ["sha1", "inter"], {}, "nokey"),
        ("an RSA-1024 leaf (minrsa 2048)", ["weak", "inter"], {}, "60 nokey"),
        ("a chain over 16 KB", ["fat", "inter"], {}, "1002 nokey"),
        ("nine certificates", ["leaf"] + ["inter"] * 8, {}, "1002 nokey"),
        ("an empty anchors file name: no anchors", ["leaf", "inter"], {"anchorfile": "-"}, "62 nokey"),
        ("the owner's self-signed certificate in /etc/ssl/local (not a CA)", ["nas"],
         {"server": "nas.lan", "anchorfile": "-", "local": ["nas"]}, "0 key"),
        ("a clock long before the certificates (day 700,000)", ["leaf", "inter"], {"t": (700000, 0)}, "54 nokey"),
    ]
    for nm, names, kw, want in cases:
        if any(files.get(n) is None for n in names):
            print("SKIPPED crypto-test.py: x509: %s (OpenSSL would not make the certificate)" % nm)
            continue
        got = chain(names, **kw)
        ok = got == want or (want == "nokey" and got.endswith(" nokey") and not got.startswith("0 "))
        result("x509: %s -> %s" % (nm, want), True, 1, [] if ok else [got])
    # a damaged anchors file: every byte flipped in turn somewhere in the header and index must give an error or the
    # right answer, never a crash (the sanitizers watch)
    blob = open(an, "rb").read()
    bad, crashes = 0, []
    for i in range(0, min(len(blob), 200)):
        b2 = bytearray(blob)
        b2[i] ^= 0xFF
        p = os.path.join(d, "bad.anchors")
        open(p, "wb").write(bytes(b2))
        got = chain(["leaf", "inter"], anchorfile=p)
        if got.startswith("crash"):
            crashes.append((i, got))
        elif not got.startswith("0 "):
            bad += 1
    result("x509: 200 damaged anchors files: errors or the right answer, no crash (%d refused)" % bad, True, 200, crashes)
    seed = b"".join(struct.pack(">H", len(open(files[n], "rb").read())) + open(files[n], "rb").read() for n in ("leaf", "inter"))
    open(os.path.join(out, "chain.seed"), "wb").write(seed)
    shutil.rmtree(d, ignore_errors=True)


def realroots(x509test, out):
    """every root of tools/certdata in the anchors file, found by its name hash, with the key OpenSSL reads"""
    import warnings
    warnings.simplefilter("ignore")             # (a few old roots have serial numbers RFC 5280 now forbids)
    import hashlib as hl
    from cryptography import x509
    from cryptography.hazmat.primitives.asymmetric import rsa, ec
    from cryptography.hazmat.primitives import serialization
    here = os.path.dirname(os.path.abspath(__file__))
    sys.path.insert(0, os.path.join(here, ".."))
    import mkanchors
    import collections
    an = os.path.join(out, "mozilla.anchors")
    r = subprocess.run([sys.executable, "-I", os.path.join(here, "..", "mkanchors.py"), an], stdout=subprocess.PIPE)
    print("crypto-test.py: %s" % r.stdout.decode().strip())
    st = collections.defaultdict(int)
    roots = mkanchors.from_certdata(mkanchors.DEFAULT_CERTDATA, st)
    bad, n = [], 0
    curves = {"secp256r1": 23, "secp384r1": 24, "secp521r1": 25}
    for label, der in roots:
        c = x509.load_der_x509_certificate(der)
        subj = c.subject.public_bytes()
        k = c.public_key()
        if isinstance(k, rsa.RSAPublicKey):
            nums = k.public_numbers()
            want = "rsa %s %s" % (nums.n.to_bytes((nums.n.bit_length() + 7) // 8, "big").hex(),
                                  nums.e.to_bytes((nums.e.bit_length() + 7) // 8, "big").hex())
        elif isinstance(k, ec.EllipticCurvePublicKey) and k.curve.name in curves:
            want = "ec %d %s" % (curves[k.curve.name], k.public_bytes(serialization.Encoding.X962,
                                                                     serialization.PublicFormat.UncompressedPoint).hex())
        else:
            continue
        n += 1
        got = subprocess.run([x509test, "lookup", an, hl.sha256(subj).digest()[:8].hex()], stdout=subprocess.PIPE).stdout.decode()
        if (want + " " + subj.hex() + " 1") not in got.split("\n"):
            bad.append(label)
    result("mkanchors.py on Mozilla's roots: each root found by its name hash, key and name as OpenSSL reads them", True, n, bad)


# ---- seeds for the fuzz harnesses (tools/host/crypto-fuzz.c) ----
def seeds(o, out, anchors_file):
    import struct
    import shutil
    import tempfile
    here = os.path.dirname(os.path.abspath(__file__))

    def put(target, name, data):
        d = os.path.join(out, target, "seeds")
        os.makedirs(d, exist_ok=True)
        open(os.path.join(d, name), "wb").write(data)

    st = lambda b: struct.pack(">I", len(b)) + b
    nl = lambda *names: st(",".join(names).encode())
    def plain(payload):
        pad = 8 - (5 + len(payload)) % 8
        if pad < 4:
            pad += 8
        return struct.pack(">IB", 1 + len(payload) + pad, pad) + payload + bytes(pad)
    kexinit = (bytes([20]) + bytes(16) + nl("mlkem768x25519-sha256", "curve25519-sha256", "ext-info-s",
               "kex-strict-s-v00@openssh.com") + nl("rsa-sha2-512", "ssh-ed25519") + nl("chacha20-poly1305@openssh.com") * 2 +
               nl("umac-64-etm@openssh.com") * 2 + nl("none", "zlib@openssh.com") * 2 + nl() * 2 + bytes(5))
    pk = bytes(range(32))
    blob = st(b"ssh-ed25519") + st(pk)
    reply = bytes([31]) + st(blob) + st(bytes(range(9, 41))) + st(st(b"ssh-ed25519") + st(bytes(64)))
    put("sshkex", "openssh", b"SSH-2.0-OpenSSH_10.6\r\n" + plain(kexinit) + plain(reply) + plain(bytes([21])))
    put("sshkex", "preamble", b"Welcome\r\nto the box\nSSH-2.0-x\r\n" + plain(kexinit))
    put("sshkex", "debug", b"SSH-1.99-y\r\n" + plain(bytes([4, 1]) + st(b"hello") + st(b"")))
    msgs = {
        "service": [bytes([6]) + st(b"ssh-userauth")],
        "failure": [bytes([51]) + nl("publickey", "keyboard-interactive", "password") + b"\0",
                    bytes([53]) + st(b"Banner\r\n\x1b[31m") + st(b"")],
        "pkok": [bytes([60]) + st(b"ssh-ed25519") + st(blob), bytes([52])],
        "info": [bytes([60]) + st(b"name") + st(b"instr") + st(b"") + struct.pack(">I", 2) + st(b"Password: ") + b"\0" +
                 st(b"Code: ") + b"\1"],
        "ext": [bytes([7]) + struct.pack(">I", 1) + st(b"server-sig-algs") + st(b"ssh-ed25519,rsa-sha2-512"), bytes([52])],
        "open": [bytes([91]) + struct.pack(">IIII", 0, 5, 100000, 32768), bytes([99]) + struct.pack(">I", 0),
                 bytes([99]) + struct.pack(">I", 0)],
        "data": [bytes([94]) + struct.pack(">I", 0) + st(b"hello world\r\n"), bytes([95]) + struct.pack(">II", 0, 1) + st(b"err"),
                 bytes([93]) + struct.pack(">II", 0, 1000), bytes([98]) + struct.pack(">I", 0) + st(b"exit-status") + b"\0" +
                 struct.pack(">I", 3), bytes([96]) + struct.pack(">I", 0), bytes([97]) + struct.pack(">I", 0)],
        "global": [bytes([80]) + st(b"keepalive@openssh.com") + b"\1", bytes([98]) + struct.pack(">I", 0) +
                   st(b"keepalive@openssh.com") + b"\1", bytes([2]) + st(b""), bytes([3]) + struct.pack(">I", 7)],
        "rekey": [kexinit, reply, bytes([21])],
        "disconnect": [bytes([1]) + struct.pack(">I", 11) + st(b"bye \x1b[2J") + st(b"")],
    }
    for i, (nm, ps) in enumerate(msgs.items()):
        for where in range(9):
            put("sshpkt", "%s-%d" % (nm, where), bytes([where]) + b"".join(struct.pack(">H", len(p_)) + p_ for p_ in ps))
    put("sshopen", "zeros", bytes(64))
    with tempfile.TemporaryDirectory(prefix="c01-seeds.") as t:
        for nm, extra in (("plain", []), ("pw", ["-N", "pass phrase", "-a", "2"])):
            k = os.path.join(t, nm)
            subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-f", k] + (extra or ["-N", ""]), check=True)
            put("keys", nm, (b"P" if extra else b"-") + open(k, "rb").read())
            put("keys", nm + ".pub", open(k + ".pub", "rb").read())
            if nm == "plain":
                put("kh", "lines", b"pc.lan,10.0.0.1 " + open(k + ".pub", "rb").read() + b"@revoked * " +
                    open(k + ".pub", "rb").read() + b"[pc.lan]:2222 " + open(k + ".pub", "rb").read())
                h = os.path.join(t, "kh")
                open(h, "wb").write(b"pc.lan " + open(k + ".pub", "rb").read())
                subprocess.run(["ssh-keygen", "-q", "-H", "-f", h], check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                put("kh", "hashed", open(h, "rb").read())
    put("config", "sample", b"Port 2200\nHost pc pc.* !x\n HostName=1.2.3.4\n User \"me\"\n IdentityFile ~/.ssh/k\n"
        b"ServerAliveInterval 30\nMatch all\nHost *\n Port 22\n")
    put("clean", "utf8", "plain \t\n\r\n\x1b[31m é € 😀 \u009b".encode() + b"\xc0\xaf\xed\xa0\x80\xff")
    sig = bytes.fromhex(o.ask("sig_make", bytes([5] * 32), "bundle", bytes([0x11] * 32)))
    put("sig", "good", sig)
    put("sig", "der", bytes.fromhex("3006020101020101") + bytes(96))
    if anchors_file and os.path.exists(anchors_file):
        put("anchors", "mozilla", open(anchors_file, "rb").read())
        put("x509", "empty", b"")
    test_chain = os.path.join(out, "chain.seed")
    if os.path.exists(test_chain):
        put("x509", "chain", open(test_chain, "rb").read())
    put("tls", "hello", bytes.fromhex("160303002a020000260303") + bytes(32) + bytes([0]) + bytes.fromhex("cca8") + bytes([0]))
    put("tls", "alert", bytes.fromhex("15030300020228"))


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    o = Oracle(sys.argv[1])
    if sys.argv[2] == "wycheproof":
        wycheproof(o, sys.argv[3])
    elif sys.argv[2] == "anchors":
        anchors(sys.argv[3], sys.argv[4])
        realroots(sys.argv[3], sys.argv[4])
    elif sys.argv[2] == "sign":
        sign(o, sys.argv[3])
    elif sys.argv[2] == "seeds":
        seeds(o, sys.argv[3], sys.argv[4] if len(sys.argv) > 4 else None)
    elif sys.argv[2] == "openssl":
        openssl(o, int(sys.argv[3]) if len(sys.argv) > 3 else 200)
    else:
        sys.exit(__doc__)
    if o.close() != 0:
        print("FAIL crypto-test.py: the oracle exited non-zero")
        sys.exit(1)
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
