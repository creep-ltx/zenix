#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The trust anchors file /etc/ssl/anchors from Mozilla's certdata.txt (DESIGN D21 8.1, docs/d21-crypto-ssh-https.md;
the format is frozen once shipped, section 12).

  mkanchors.py [--certdata FILE | --no-certdata] [--pem FILE]... OUT

--certdata: Mozilla's file (default tools/certdata, NSS's lib/ckfw/builtins/certdata.txt of NSS_3_132_RTM, commit
34716626, kept in the repository with its MPL-2.0 notice at its top: MPL's source-form rule, D21 8.1); --pem: more
roots (PEM certificates: a test CA, the owner's own); --no-certdata: only those.

Kept from certdata: the certificates whose trust object says CKA_TRUST_SERVER_AUTH CKT_NSS_TRUSTED_DELEGATOR (trusted
for TLS servers) and that have no CKA_NSS_SERVER_DISTRUST_AFTER date (BearSSL cannot apply one: such roots are
dropped whole, D21 8.1); RSA keys and EC keys on P-256, P-384, P-521. Everything dropped is counted on the output.

The file, big-endian: a 16-byte header ("ZTA1", format u16 1, count u16, the index's offset u32, the data's offset
u32); the index, one 12-byte entry per anchor sorted by hash (the first 8 bytes of SHA-256 of the DER-encoded subject
name, then the entry's offset u32 from the data's start); each entry: flags u8 (bit 0: a CA), key type u8 (1 RSA,
2 EC), the name's length u16 and the DER name; for RSA the modulus's length u16, the modulus, the exponent's length
u8, the exponent; for EC the curve u8 (BearSSL's ids: 23 P-256, 24 P-384, 25 P-521), the point's length u8 and the
uncompressed point. Several roots may share a name (re-keyed roots): readers load every matching entry.
"""
import base64
import hashlib
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_CERTDATA = os.path.join(HERE, "certdata")
OID_RSA = "1.2.840.113549.1.1.1"
OID_EC = "1.2.840.10045.2.1"
CURVES = {"1.2.840.10045.3.1.7": 23, "1.3.132.0.34": 24, "1.3.132.0.35": 25}
MAXCOUNT = 0xFFFF


def die(msg):
    sys.stderr.write("mkanchors: %s\n" % msg)
    sys.exit(1)


# ---- a small DER reader (certificates are only read here, from Mozilla's file or the owner's PEMs) ----
def tlv(b, i):
    """(tag, start of the value, end of the value) of the element at i"""
    if i + 2 > len(b):
        raise ValueError("truncated")
    tag, n = b[i], b[i + 1]
    i += 2
    if n & 0x80:
        k = n & 0x7F
        if k == 0 or k > 3 or i + k > len(b):
            raise ValueError("bad length")
        n = int.from_bytes(b[i:i + k], "big")
        i += k
    if i + n > len(b):
        raise ValueError("truncated value")
    return tag, i, i + n


def children(b, start, end):
    out, i = [], start
    while i < end:
        t, s, e = tlv(b, i)
        out.append((t, s, e, i))
        i = e
    return out


def oid(b):
    v = []
    n = 0
    for x in b:
        n = (n << 7) | (x & 0x7F)
        if not x & 0x80:
            v.append(n)
            n = 0
    first = [0, v[0]] if v[0] < 40 else [1, v[0] - 40] if v[0] < 80 else [2, v[0] - 80]
    return ".".join(str(x) for x in first + v[1:])


def cert_info(der):
    """(subject DER, key type, key fields) of a certificate"""
    t, s, e = tlv(der, 0)
    cert = children(der, s, e)
    tbs = children(der, cert[0][1], cert[0][2])
    k = 1 if tbs[0][0] == 0xA0 else 0              # [0] version
    subject = tbs[k + 4]
    spki = tbs[k + 5]
    subj_der = der[subject[3]:subject[2]]
    alg, bits = children(der, spki[1], spki[2])
    algs = children(der, alg[1], alg[2])
    aoid = oid(der[algs[0][1]:algs[0][2]])
    key = der[bits[1] + 1:bits[2]]                  # the BIT STRING without its unused-bits byte
    if der[bits[1]] != 0:
        raise ValueError("a public key that is not whole bytes")
    if aoid == OID_RSA:
        _, s2, e2 = tlv(key, 0)
        n, ex = children(key, s2, e2)[:2]
        mod = key[n[1]:n[2]].lstrip(b"\0")
        exp = key[ex[1]:ex[2]].lstrip(b"\0")
        if len(mod) > 0xFFFF or len(exp) > 0xFF or not mod or not exp:
            raise ValueError("an RSA key out of range")
        return subj_der, 1, (mod, exp)
    if aoid == OID_EC:
        c = oid(der[algs[1][1]:algs[1][2]]) if len(algs) > 1 and algs[1][0] == 6 else None
        if c not in CURVES:
            raise ValueError("an EC key on an unsupported curve (%s)" % c)
        if not key or key[0] != 4 or len(key) > 0xFF:
            raise ValueError("an EC point that is not uncompressed")
        return subj_der, 2, (CURVES[c], key)
    raise ValueError("an unsupported key type (%s)" % aoid)


# ---- certdata.txt ----
def octal(lines):
    return bytes(int(x, 8) for x in re.findall(r"\\([0-7]{3})", "".join(lines)))


def parse_certdata(text):
    """the objects of certdata.txt: dicts of attribute -> value (bytes for MULTILINE_OCTAL, else the word)"""
    objs, cur, i = [], None, 0
    lines = text.split("\n")
    while i < len(lines):
        ln = lines[i].strip()
        i += 1
        if not ln or ln.startswith("#"):
            continue
        if ln.startswith("BEGINDATA"):
            continue
        w = ln.split(None, 2)
        if w[0] == "CKA_CLASS":
            cur = {}
            objs.append(cur)
        if cur is None:
            continue
        if len(w) >= 2 and w[1] == "MULTILINE_OCTAL":
            body = []
            while lines[i].strip() != "END":
                body.append(lines[i].strip())
                i += 1
            i += 1
            cur[w[0]] = octal(body)
        elif len(w) == 3:
            cur[w[0]] = w[2]
    return objs


def from_certdata(path, stats):
    text = open(path, encoding="utf-8").read()
    if "Mozilla Public" not in text[:400]:
        die("%s: no MPL notice at the top: not Mozilla's certdata.txt?" % path)
    objs = parse_certdata(text)
    trust = {}
    for o in objs:
        if o.get("CKA_CLASS") in ("CKO_NSS_TRUST", "CKO_TRUST") and "CKA_ISSUER" in o:
            trust[(o["CKA_ISSUER"], o.get("CKA_SERIAL_NUMBER"))] = o.get("CKA_TRUST_SERVER_AUTH")
    out = []
    for o in objs:
        if o.get("CKA_CLASS") != "CKO_CERTIFICATE":
            continue
        stats["certificates"] += 1
        t = trust.get((o.get("CKA_ISSUER"), o.get("CKA_SERIAL_NUMBER")))
        if t != "CKT_NSS_TRUSTED_DELEGATOR":
            stats["not trusted for TLS servers"] += 1
            continue
        if isinstance(o.get("CKA_NSS_SERVER_DISTRUST_AFTER"), bytes):
            stats["dropped: a distrust-after date"] += 1
            continue
        out.append((o.get("CKA_LABEL", "?").strip('"'), o["CKA_VALUE"]))
    return out


def from_pem(path):
    t = open(path).read()
    out = []
    for m in re.finditer(r"-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----", t, re.S):
        out.append((os.path.basename(path), base64.b64decode("".join(m.group(1).split()))))
    if not out:
        die("%s: no PEM certificate" % path)
    return out


def build(certs, stats):
    entries = []
    seen = set()
    for label, der in certs:
        try:
            subj, kt, key = cert_info(der)
        except (ValueError, IndexError) as e:
            stats["dropped: %s" % re.sub(r" \(.*", "", str(e))] += 1
            continue
        body = struct.pack(">BBH", 1, kt, len(subj)) + subj
        if kt == 1:
            body += struct.pack(">H", len(key[0])) + key[0] + struct.pack(">B", len(key[1])) + key[1]
        else:
            body += struct.pack(">BB", key[0], len(key[1])) + key[1]
        if body in seen:
            stats["duplicates"] += 1
            continue
        seen.add(body)
        entries.append((hashlib.sha256(subj).digest()[:8], body))
    if len(entries) > MAXCOUNT:
        die("more than %d anchors" % MAXCOUNT)
    entries.sort(key=lambda x: x[0])
    index, data = b"", b""
    for h, body in entries:
        index += h + struct.pack(">I", len(data))
        data += body
    head = b"ZTA1" + struct.pack(">HHII", 1, len(entries), 16, 16 + len(index))
    return head + index + data, len(entries)


def main(a):
    certdata, pems, out, i = DEFAULT_CERTDATA, [], None, 0
    while i < len(a):
        if a[i] == "--certdata" and i + 1 < len(a):
            certdata = a[i + 1]
            i += 2
        elif a[i] == "--no-certdata":
            certdata = None
            i += 1
        elif a[i] == "--pem" and i + 1 < len(a):
            pems.append(a[i + 1])
            i += 2
        elif out is None and not a[i].startswith("--"):
            out = a[i]
            i += 1
        else:
            sys.exit(__doc__)
    if not out:
        sys.exit(__doc__)
    stats = {"certificates": 0, "not trusted for TLS servers": 0, "dropped: a distrust-after date": 0, "duplicates": 0}
    stats = __import__("collections").defaultdict(int, stats)
    certs = from_certdata(certdata, stats) if certdata else []
    for p in pems:
        certs += from_pem(p)
    blob, n = build(certs, stats)
    with open(out + ".part", "wb") as f:
        f.write(blob)
    os.replace(out + ".part", out)
    print("%s: %d anchors, %d bytes (%s)" % (out, n, len(blob), ", ".join("%s %d" % kv for kv in stats.items() if kv[1])))


if __name__ == "__main__":
    main(sys.argv[1:])
