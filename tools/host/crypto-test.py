#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The crypto library against outside answers (DESIGN D21 10 step 0), through `crypto-test oracle`:

  crypto-test.py ORACLE wycheproof DIR    Wycheproof's vectors (Apache-2.0 data, never committed) from DIR, fetched
                                          there by crypto-test.sh the first time: X25519 (the all-zero results must be
                                          refused), Ed25519, ChaCha20-Poly1305 (both implementations), AES-GCM, ECDSA
                                          P-256 (DER and raw) and P-384, RSA PKCS#1 v1.5 2048 and 4096, HMAC-SHA-256
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
    tkey = os.path.join(here, "..", "testdata", "sign", "zenix-test.key")
    tpub = os.path.join(here, "..", "testdata", "sign", "zenix-test.pub")
    d = os.path.join(out, "sign")
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)

    def zs(*a):
        r = subprocess.run([sys.executable, "-I", zsign] + list(a), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        return r.returncode, (r.stdout + r.stderr).decode(errors="replace")

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


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    o = Oracle(sys.argv[1])
    if sys.argv[2] == "wycheproof":
        wycheproof(o, sys.argv[3])
    elif sys.argv[2] == "sign":
        sign(o, sys.argv[3])
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
