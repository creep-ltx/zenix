#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""D21 step 7's network tests on the PC (docs/d21-crypto-ssh-https.md 10 step 7), run by tools/host/tls-test.sh
inside unshare --map-root-user --map-auto -nm with lo up (nothing leaves the PC):

  tls-test.py https OUT WGETCLIENT   a test CA made here (keys in OUT only, never in git), HTTPS servers on 443 (by
                                     SNI: one certificate case per name), 8443 (TLS 1.3 only), 8444 (a slow
                                     handshake) and plain HTTP on 80, all on 127.0.0.1; tools/host/wget-client.c
                                     against them: certificates, SNI, the time rules, keep-alive, resumption,
                                     redirects, resume, truncation, slowness. Also writes the fuzzing seeds.
  tls-test.py sntp OUT SNTPCLIENT    an SNTP server on 127.0.0.2-127.0.0.9:123 with true, false and hostile answers;
                                     tools/host/sntp-client.c against them, the clock file checked
Every check prints PASS or FAIL; the exit status is the number of FAILs (capped at 100).
"""
import datetime
import hashlib
import os
import random
import shutil
import socket
import ssl
import struct
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
fails = 0


def ok(name, cond, detail=""):
    global fails
    if cond:
        print("PASS tls-test.py: %s" % name)
    else:
        fails += 1
        print("FAIL tls-test.py: %s%s" % (name, (": " + str(detail)[-900:]) if detail else ""))
    sys.stdout.flush()


def run(*a, cwd=None, timeout=120):
    try:
        r = subprocess.run([str(x) for x in a], stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=cwd,
                           timeout=timeout)
    except subprocess.TimeoutExpired:
        return -999, b"", "timeout"
    return r.returncode, r.stdout, r.stderr.decode(errors="replace")


# ---------------------------------------------------------------------------------------------------------------
# the test CA
def make_pki(d):
    from cryptography import x509
    from cryptography.x509.oid import NameOID, ExtendedKeyUsageOID
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa, ec

    now = datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0)

    def name(cn):
        return x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, cn), x509.NameAttribute(NameOID.ORGANIZATION_NAME, "Zenix test")])

    def cert(subj, key, issuer, ikey, ca, frm=-1, to=60, san=None, eku="server", h=None):
        h = h or hashes.SHA256()
        b = (x509.CertificateBuilder().subject_name(name(subj)).issuer_name(name(issuer)).public_key(key.public_key())
             .serial_number(x509.random_serial_number()).not_valid_before(now + datetime.timedelta(days=frm))
             .not_valid_after(now + datetime.timedelta(days=to))
             .add_extension(x509.BasicConstraints(ca=ca, path_length=None), critical=True))
        if ca:
            b = b.add_extension(x509.KeyUsage(False, False, False, False, False, True, True, False, False), critical=True)
        else:
            b = b.add_extension(x509.KeyUsage(True, False, True, False, False, False, False, False, False), critical=True)
        if san:
            b = b.add_extension(x509.SubjectAlternativeName([x509.DNSName(n) for n in san]), critical=False)
        if eku == "server":
            b = b.add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
        elif eku == "client":
            b = b.add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.CLIENT_AUTH]), critical=False)
        return b.sign(ikey, h)

    def keyfile(k, nm):
        p = os.path.join(d, nm + ".key")
        fd = os.open(p, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        os.write(fd, k.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                     serialization.NoEncryption()))
        os.close(fd)
        return p

    def pem(*certs):
        return b"".join(c.public_bytes(serialization.Encoding.PEM) for c in certs)

    def chainfile(nm, *certs):
        p = os.path.join(d, nm + ".crt")
        open(p, "wb").write(pem(*certs))
        return p

    R = lambda bits=2048: rsa.generate_private_key(65537, bits)
    E = lambda c=ec.SECP256R1: ec.generate_private_key(c())
    rk = R()
    root = cert("Zenix Test Root R", rk, "Zenix Test Root R", rk, True, -10, 3650)
    ik = R()
    inter = cert("Zenix Test Inter R", ik, "Zenix Test Root R", rk, True, -10, 1000)
    erk = E()
    eroot = cert("Zenix Test Root E", erk, "Zenix Test Root E", erk, True, -10, 3650)
    eik = E()
    einter = cert("Zenix Test Inter E", eik, "Zenix Test Root E", erk, True, -10, 1000)
    r4k = R(4096)
    root4k = cert("Zenix Test Root R4096", r4k, "Zenix Test Root R4096", r4k, True, -10, 3650)
    i4k = R()
    inter4k = cert("Zenix Test Inter R4096", i4k, "Zenix Test Root R4096", r4k, True, -10, 1000)
    p384k = E(ec.SECP384R1)
    root384 = cert("Zenix Test Root P384", p384k, "Zenix Test Root P384", p384k, True, -10, 3650, h=hashes.SHA384())
    i384 = E(ec.SECP384R1)
    inter384 = cert("Zenix Test Inter P384", i384, "Zenix Test Root P384", p384k, True, -10, 1000, h=hashes.SHA384())
    ok_ = R()
    oroot = cert("Zenix Other Root", ok_, "Zenix Other Root", ok_, True, -10, 3650)
    oik = R()
    ointer = cert("Zenix Other Inter", oik, "Zenix Other Root", ok_, True, -10, 1000)
    wk = R()
    owner = cert("Owner Root", wk, "Owner Root", wk, True, -10, 3650)
    open(os.path.join(d, "owner.pem"), "wb").write(pem(owner))
    for nm, c in (("root", root), ("eroot", eroot), ("root4k", root4k), ("root384", root384)):
        open(os.path.join(d, nm + ".pem"), "wb").write(pem(c))
    lk, elk = R(), E()
    sites = {}

    def site(host, key, *chain):
        sites[host] = sites.get(host, []) + [(chainfile(host + "-" + str(len(sites.get(host, []))), *chain), keyfile(key, host + "-" + str(len(sites.get(host, [])))))]

    site("rsa.test", lk, cert("rsa.test", lk, "Zenix Test Inter R", ik, False, san=["rsa.test", "*.wild.test"]), inter)
    site("ecdsa.test", elk, cert("ecdsa.test", elk, "Zenix Test Inter E", eik, False, san=["ecdsa.test"]), einter)
    site("rsa4096.test", lk, cert("rsa4096.test", lk, "Zenix Test Inter R4096", i4k, False, san=["rsa4096.test"]), inter4k)
    site("p384.test", elk, cert("p384.test", elk, "Zenix Test Inter P384", i384, False, san=["p384.test"],
                                h=hashes.SHA384()), inter384)
    site("expired.test", lk, cert("expired.test", lk, "Zenix Test Inter R", ik, False, -30, -2, san=["expired.test"]), inter)
    site("future.test", lk, cert("future.test", lk, "Zenix Test Inter R", ik, False, 3, 60, san=["future.test"]), inter)
    site("wrong.test", lk, cert("other.test", lk, "Zenix Test Inter R", ik, False, san=["other.test"]), inter)
    site("unknown.test", lk, cert("unknown.test", lk, "Zenix Other Inter", oik, False, san=["unknown.test"]), ointer)
    site("nointer.test", lk, cert("nointer.test", lk, "Zenix Test Inter R", ik, False, san=["nointer.test"]))
    site("noeku.test", lk, cert("noeku.test", lk, "Zenix Test Inter R", ik, False, san=["noeku.test"], eku="client"), inter)
    site("local.test", lk, cert("local.test", lk, "Owner Root", wk, False, san=["local.test"]))
    # nine intermediates under the root: ten certificates sent (the wrapper takes at most 8)
    prev_k, prev_n, chain = rk, "Zenix Test Root R", []
    for i in range(9):
        k = R()
        c = cert("Zenix Long %d" % i, k, prev_n, prev_k, True, -10, 1000)
        chain.insert(0, c)
        prev_k, prev_n = k, "Zenix Long %d" % i
    site("long.test", lk, cert("long.test", lk, prev_n, prev_k, False, san=["long.test"]), *chain)
    # both an RSA and an ECDSA certificate: the client's suite order decides (RSA first, D21 3.3)
    site("dual.test", lk, cert("dual.test", lk, "Zenix Test Inter R", ik, False, san=["dual.test"]), inter)
    site("dual.test", elk, cert("dual.test", elk, "Zenix Test Inter E", eik, False, san=["dual.test"]), einter)
    site("default.invalid", lk, cert("default.invalid", lk, "Zenix Test Inter R", ik, False, san=["default.invalid"]), inter)
    an = os.path.join(d, "test.anchors")
    r = subprocess.run([sys.executable, "-I", os.path.join(TOOLS, "mkanchors.py"), "--no-certdata"] +
                       sum((["--pem", os.path.join(d, n + ".pem")] for n in ("root", "eroot", "root4k", "root384")), []) + [an],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    ok("mkanchors.py with the test roots (RSA-2048, P-256, RSA-4096, P-384)", r.returncode == 0 and b"4 anchors" in r.stdout,
       r.stdout)
    return sites, an, now


# ---------------------------------------------------------------------------------------------------------------
# the servers
LOG = {"sni": [], "reused": [], "cipher": [], "alpn": [], "conns": 0}
LOCK = threading.Lock()
ROOT = None


def read_request(c, buf):
    while b"\r\n\r\n" not in buf:
        d = c.recv(4096)
        if not d:
            return None, b""
        buf += d
    head, rest = buf.split(b"\r\n\r\n", 1)
    lines = head.decode("latin-1").split("\r\n")
    method, path, _ = lines[0].split(" ", 2)
    hdrs = {}
    for ln in lines[1:]:
        k, _, v = ln.partition(":")
        hdrs[k.strip().lower()] = v.strip()
    return (method, path, hdrs), rest


def handle(c, tls):
    buf = b""
    while True:
        req, buf = read_request(c, buf)
        if req is None:
            return "eof"
        method, path, hdrs = req
        rng = hdrs.get("range")
        mode, _, rest = path.lstrip("/").partition("/")
        host = hdrs.get("host", "")

        def reply(status, body=b"", extra=(), length=True):
            h = "HTTP/1.1 %s\r\n" % status
            for k, v in extra:
                h += "%s: %s\r\n" % (k, v)
            if length:
                h += "Content-Length: %d\r\n" % len(body)
            c.sendall(h.encode() + b"\r\n" + body)

        def data_of(p):
            if p.endswith("/") or not p:
                p += "index.html"
            full = os.path.normpath(os.path.join(ROOT, p.lstrip("/")))
            if not full.startswith(os.path.normpath(ROOT)) or not os.path.isfile(full):
                return None
            return open(full, "rb").read()

        if path == "/stall":
            time.sleep(20)
            return "stall"
        if mode == "redir":
            n, _, p = rest.partition("/")
            n = int(n)
            reply("302 Found", b"moved", [("Location", "/redir/%d/%s" % (n - 1, p) if n > 1 else "/" + p)])
            continue
        if mode == "tohttp":
            reply("301 Moved", b"", [("Location", "http://%s:80/%s" % (host.split(":")[0], rest))])
            continue
        if mode == "tohttps":
            reply("301 Moved", b"", [("Location", "https://%s/%s" % (host.split(":")[0], rest))])
            continue
        if mode == "rel":                   # /rel/PATH: a relative Location, "x/../../PATH" (RFC 3986 5.2)
            reply("302 Found", b"", [("Location", "x/../" + "../" * (rest.count("/") + 1) + rest)])
            continue
        if mode == "schemerel":             # /schemerel/HOST/PATH: "//HOST/PATH", the same scheme
            h2, _, p = rest.partition("/")
            reply("302 Found", b"", [("Location", "//%s/%s" % (h2, p))])
            continue
        if mode == "tohost":                # /tohost/HOST/PATH: an absolute https Location on another name
            h2, _, p = rest.partition("/")
            reply("302 Found", b"", [("Location", "https://%s/%s" % (h2, p))])
            continue
        special = ("chunked", "norange", "badrange", "close", "nolen", "nolen-trunc", "cut", "drip")
        data = data_of(rest) if mode in special else data_of(path)
        if data is None:
            reply("404 Not Found", b"no such file\n")
            continue
        first = 0
        if rng and mode not in ("norange", "chunked") and rng.startswith("bytes=") and rng.endswith("-"):
            first = int(rng[6:-1])
        if first and first >= len(data):
            reply("416 Range Not Satisfiable", b"", [("Content-Range", "bytes */%d" % len(data))])
            continue
        if mode == "chunked":
            out = b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
            i = 0
            while i < len(data):
                k = random.randint(1, 20000)
                out += b"%x\r\n" % len(data[i:i + k]) + data[i:i + k] + b"\r\n"
                i += k
            c.sendall(out + b"0\r\n\r\n")
            continue
        if first:
            cr = "bytes %d-%d/%d" % (first, len(data) - 1, len(data))
            if mode == "badrange":
                cr = "bytes %d-%d/%d" % (first - 1, len(data) - 1, len(data))
            reply("206 Partial Content", data[first:], [("Content-Range", cr)])
            continue
        if mode == "close":
            reply("200 OK", data, [("Connection", "close")])
            return "close"
        if mode == "nolen":                 # a body that ends with the connection, closed with close_notify
            c.sendall(b"HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n" + data)
            return "close"
        if mode in ("nolen-trunc", "cut"):  # the connection cut without close_notify
            if mode == "cut":
                c.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n" % len(data) + data[:len(data) // 2])
            else:
                c.sendall(b"HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n" + data)
            return "cut"
        if mode == "drip":                  # a slow server: one byte a write (a TLS record each)
            c.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n" % len(data))
            for b in data:
                time.sleep(0.002)
                c.sendall(bytes([b]))
            continue
        reply("200 OK", data)


def serve_conn(raw, ctx, slow=0):
    how = "error"
    c = raw
    try:
        if ctx:
            if slow:
                time.sleep(slow)
            c = ctx.wrap_socket(raw, server_side=True)
            with LOCK:
                LOG["reused"].append(c.session_reused)
                LOG["cipher"].append(c.cipher()[0] if c.cipher() else "")
                LOG["alpn"].append(c.selected_alpn_protocol())
        how = handle(c, ctx is not None)
    except (OSError, ValueError, ssl.SSLError):
        pass
    finally:
        try:
            if ctx and how in ("close", "eof") and isinstance(c, ssl.SSLSocket):
                try:
                    c.unwrap()              # close_notify
                except (OSError, ssl.SSLError):
                    pass
            elif isinstance(c, ssl.SSLSocket) and how == "cut":
                c.shutdown(socket.SHUT_RDWR)    # without close_notify
            raw.close()
        except OSError:
            pass


def listener(port, ctx, slow=0):
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", port))
    s.listen(64)

    def loop():
        while True:
            c, _ = s.accept()
            with LOCK:
                LOG["conns"] += 1
            threading.Thread(target=serve_conn, args=(c, ctx, slow), daemon=True).start()
    threading.Thread(target=loop, daemon=True).start()


def contexts(sites, tls13=False):
    def mk(host):
        x = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        if tls13:
            x.minimum_version = ssl.TLSVersion.TLSv1_3
        else:
            x.minimum_version = x.maximum_version = ssl.TLSVersion.TLSv1_2
        x.options |= ssl.OP_NO_TICKET
        x.options &= ~ssl.OP_CIPHER_SERVER_PREFERENCE     # the client's order decides
        x.set_alpn_protocols(["http/1.1"])
        for crt, key in sites[host]:
            x.load_cert_chain(crt, key)
        return x
    per = {h: mk(h) for h in sites}
    base = per["default.invalid"]

    def sni(sock, name, _ctx):
        with LOCK:
            LOG["sni"].append(name)
        if name and name.endswith(".wild.test"):
            name = "rsa.test"               # its certificate names *.wild.test too
        if name in per:
            sock.context = per[name]
    base.sni_callback = sni
    return base


# ---------------------------------------------------------------------------------------------------------------
def https(out, client):
    global ROOT
    d = os.path.join(out, "tls")
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    sites, anchors, now = make_pki(d)
    ROOT = os.path.join(d, "www")
    os.makedirs(os.path.join(ROOT, "dir"))
    big = os.urandom(1 << 20)
    small = b"hello, Zenix\n" * 100
    open(os.path.join(ROOT, "big.bin"), "wb").write(big)
    open(os.path.join(ROOT, "small.txt"), "wb").write(small)
    open(os.path.join(ROOT, "dir", "f.bin"), "wb").write(small)
    open(os.path.join(ROOT, "index.html"), "wb").write(b"<p>index</p>\n")
    listener(443, contexts(sites))
    listener(8443, contexts(sites, tls13=True))
    listener(8444, contexts(sites), slow=2.5)
    listener(80, None)
    time.sleep(0.3)
    work = os.path.join(d, "cwd")
    os.makedirs(work)
    nowt = int(now.timestamp())

    def wget(*a, opts=(), cwd=work):
        args = [client, "--connect", "127.0.0.1", "--anchors", anchors, "--idle-ms", "8000"] + list(opts) + list(a)
        rc, so, se = run(*args, cwd=cwd)
        return rc, so, se

    def tail(se):
        return [l for l in se.splitlines() if l.startswith("wget-client: exit")][-1:] or ["?"]

    def counts(se):
        t = tail(se)[0].split()
        try:
            return dict(zip(t[3::2], (int(x) for x in t[4::2])))
        except ValueError:
            return {}

    def fresh():
        for f in os.listdir(work):
            os.unlink(os.path.join(work, f))
        with LOCK:
            for k in ("sni", "reused", "cipher", "alpn"):
                LOG[k].clear()

    # -- certificates --
    cases = [
        ("an RSA chain (RSA-2048 under RSA-2048)", "rsa.test", 0, ""),
        ("an ECDSA chain (P-256)", "ecdsa.test", 0, ""),
        ("an RSA-4096 root", "rsa4096.test", 0, ""),
        ("a P-384 chain", "p384.test", 0, ""),
        ("a wildcard name", "a.wild.test", 0, ""),
        ("expired", "expired.test", 5, "expired or is not yet valid"),
        ("not yet valid", "future.test", 5, "expired or is not yet valid"),
        ("the wrong name", "wrong.test", 5, "for another name"),
        ("an unknown root", "unknown.test", 5, "not trusted"),
        ("a missing intermediate", "nointer.test", 5, "not trusted"),
        ("no serverAuth", "noeku.test", 5, "not for TLS servers"),
        ("a chain too long (ten certificates)", "long.test", 5, "chain is too long"),
        ("the owner's root without --local", "local.test", 5, "not trusted"),
    ]
    for nm, host, want, msg in cases:
        fresh()
        rc, so, se = wget("-O", "-", "https://%s/small.txt" % host)
        good = rc == want and (so == small if want == 0 else so == b"") and msg in se
        ok("certificate: %s -> exit %d" % (nm, want), good, (rc, se))
        ok("SNI: %s sent" % host, host in LOG["sni"], LOG["sni"])
    fresh()
    rc, so, se = wget("-O", "-", "https://local.test/small.txt", opts=["--local", os.path.join(d, "owner.pem")])
    ok("certificate: the owner's root from /etc/ssl/local's PEM", rc == 0 and so == small, (rc, se))
    fresh()
    rc, so, se = wget("-O", "-", "https://dual.test/small.txt")
    ok("RSA preferred: a server with RSA and ECDSA certificates answers with RSA (%s)" % LOG["cipher"],
       rc == 0 and so == small and LOG["cipher"] and "RSA" in LOG["cipher"][0] and "ECDSA" not in LOG["cipher"][0], (rc, se))
    ok("ALPN http/1.1 both ways", LOG["alpn"] == ["http/1.1"] and "alpn=http/1.1" in se, (LOG["alpn"], se))
    rc, so, se = wget("-O", "-", "https://rsa.test:8443/small.txt")
    ok("a TLS 1.3-only server: refused with the TLS 1.3 message", rc != 0 and "needs TLS 1.3" in se, (rc, se))
    # -- the time rules (8.3) --
    day = 86400
    rc, so, se = wget("-O", "-", "https://rsa.test/small.txt", opts=["--clock", "1000", "--floor", str(nowt)])
    ok("time: a 1970 clock is never used, the floor stands in", rc == 0 and so == small and "clock is not set" in se, (rc, se))
    rc, so, se = wget("-O", "-", "https://rsa.test/small.txt", opts=["--untrusted", "--floor", str(nowt)])
    ok("time: no trusted clock: the floor, said once", rc == 0 and se.count("clock is not set") == 1, (rc, se))
    rc, so, se = wget("-O", "-", "https://future.test/small.txt", opts=["--untrusted", "--floor", str(nowt)])
    ok("time: no trusted clock: \"not before\" skipped (future.test accepted)", rc == 0 and so == small, (rc, se))
    rc, so, se = wget("-O", "-", "https://expired.test/small.txt", opts=["--untrusted", "--floor", str(nowt - 300 * day)])
    ok("time: no trusted clock, an older floor: an expired certificate accepted (said)", rc == 0 and "clock is not set" in se, (rc, se))
    rc, so, se = wget("-O", "-", "https://rsa.test/small.txt", opts=["--untrusted", "--floor", str(nowt + 90 * day)])
    ok("time: a certificate that expired before the floor is refused", rc == 5 and "expired" in se, (rc, se))
    rc, so, se = wget("-O", "-", "https://rsa.test/small.txt", opts=["--clock", str(nowt + 90 * day)])
    ok("time: a trusted clock after the expiry refuses", rc == 5, (rc, se))
    # -- keep-alive and resumption --
    fresh()
    with LOCK:
        LOG["conns"] = 0
    rc, so, se = wget("-O", "-", *["https://rsa.test/small.txt"] * 3 + ["https://rsa.test/chunked/big.bin"])
    c = counts(se)
    ok("keep-alive: four requests over one TLS connection", rc == 0 and so == small * 3 + big and c.get("conns") == 1 and
       c.get("handshakes") == 1 and LOG["conns"] == 1, (rc, c, LOG["conns"], se[-500:]))
    fresh()
    rc, so, se = wget("-O", "-", "https://rsa.test/close/small.txt", "https://rsa.test/small.txt", "https://ecdsa.test/small.txt",
                      "https://rsa.test/small.txt")
    c = counts(se)
    ok("resumption: the second and fourth connections to rsa.test resume (counted on both sides)",
       rc == 0 and so == small * 4 and c.get("handshakes") == 4 and c.get("resumed") == 2 and LOG["reused"] == [False, True, False, True],
       (rc, c, LOG["reused"], se[-600:]))
    fresh()
    rc, so, se = wget("-O", "-", "https://rsa.test/close/small.txt", "https://rsa.test/small.txt", opts=["--no-cache"])
    ok("resumption: none without the cache", rc == 0 and counts(se).get("resumed") == 0 and LOG["reused"] == [False, False], (rc, se))
    # -- big bodies, chunked, names --
    fresh()
    rc, so, se = wget("https://rsa.test/big.bin")
    ok("1 MB over TLS, byte for byte, named big.bin", rc == 0 and open(os.path.join(work, "big.bin"), "rb").read() == big, (rc, se))
    rc, so, se = wget("https://rsa.test/big.bin")
    ok("an existing file: big.bin.1 (GNU wget's numbering)", rc == 0 and os.path.exists(os.path.join(work, "big.bin.1")), (rc, se))
    rc, so, se = wget("-q", "https://rsa.test/")
    ok("a directory: index.html", rc == 0 and open(os.path.join(work, "index.html"), "rb").read() == b"<p>index</p>\n", (rc, se))
    rc, so, se = wget("https://rsa.test/chunked/dir/f.bin")
    ok("chunked over TLS: f.bin", rc == 0 and open(os.path.join(work, "f.bin"), "rb").read() == small, (rc, se))
    rc, so, se = wget("https://rsa.test/missing")
    ok("404: exit 8, nothing written", rc == 8 and not os.path.exists(os.path.join(work, "missing")), (rc, se))
    # -- redirects --
    fresh()
    rc, so, se = wget("-O", "-", "https://rsa.test/redir/3/small.txt")
    ok("redirects: 3 followed (one connection)", rc == 0 and so == small and "hops=3" in se and counts(se).get("conns") == 1, (rc, se))
    rc, so, se = wget("-O", "-", "https://rsa.test/redir/4/small.txt")
    ok("redirects: a fourth refused (exit 7)", rc == 7 and "more than 3" in se, (rc, se))
    rc, so, se = wget("-O", "-", "https://rsa.test/tohttp/small.txt")
    ok("redirects: https to http never followed", rc == 7 and "https to http" in se and so == b"", (rc, se))
    rc, so, se = wget("-O", "-", "http://rsa.test/tohttps/small.txt")
    ok("redirects: http to https followed (a new connection, TLS)", rc == 0 and so == small and counts(se).get("handshakes") == 1, (rc, se))
    fresh()
    rc, so, se = wget("https://rsa.test/tohost/ecdsa.test/dir/f.bin")
    ok("redirects: another host, the name from the URL given (f.bin)", rc == 0 and
       os.path.exists(os.path.join(work, "f.bin")) and "ecdsa.test" in LOG["sni"], (rc, se, os.listdir(work)))
    rc, so, se = wget("-O", "-", "https://rsa.test/rel/dir/f.bin")
    ok("redirects: a relative Location followed (x/../../dir/f.bin from /rel/)", rc == 0 and so == small and "hops=1" in se,
       (rc, se))
    rc, so, se = wget("-O", "-", "https://rsa.test/schemerel/ecdsa.test/small.txt")
    ok("redirects: //host followed with the same scheme (TLS to ecdsa.test)", rc == 0 and so == small and
       "ecdsa.test" in LOG["sni"], (rc, se))
    rc, so, se = wget("-O", "-", "https://rsa.test/tohost/wrong.test/small.txt")
    ok("redirects: the next host's certificate is checked too", rc == 5 and "another name" in se, (rc, se))
    # -- resume (-c, D22 7.5's rule) --
    fresh()
    part = os.path.join(work, "big.bin")
    open(part, "wb").write(big[:300000])
    rc, so, se = wget("-c", "https://rsa.test/big.bin")
    ok("resume: 206 appended, the file whole", rc == 0 and open(part, "rb").read() == big and "resumed 206" in se, (rc, se))
    rc, so, se = wget("-c", "https://rsa.test/big.bin")
    ok("resume: a complete file (416): nothing to do", rc == 0 and "already complete" in se and open(part, "rb").read() == big, (rc, se))
    open(part, "wb").write(b"X" * 1000)
    rc, so, se = wget("-c", "https://rsa.test/norange/big.bin")
    ok("resume: a server without ranges (200): refused, the file untouched, exit 1", rc == 1 and
       open(part, "rb").read() == b"X" * 1000 and "refusing to truncate" in se, (rc, se))
    open(part, "wb").write(big[:1000])
    rc, so, se = wget("-c", "https://rsa.test/badrange/big.bin")
    ok("resume: a wrong Content-Range refused, the file untouched", rc == 7 and open(part, "rb").read() == big[:1000], (rc, se))
    open(part, "wb").write(big[:1000])
    rc, so, se = wget("-c", "-O", "big.bin", "http://rsa.test/big.bin")
    ok("resume over plain HTTP with -O", rc == 0 and open(part, "rb").read() == big, (rc, se))
    fresh()
    rc, so, se = wget("https://rsa.test/cut/big.bin")
    ok("a cut connection: exit 4", rc == 4, (rc, se))
    rc, so, se = wget("-c", "https://rsa.test/big.bin")
    ok("... then resumed with -c", rc == 0 and open(os.path.join(work, "big.bin"), "rb").read() == big, (rc, se))
    # -- truncation (8.2) --
    rc, so, se = wget("-O", "-", "https://rsa.test/nolen/small.txt")
    ok("a body without a length, ended by close_notify: complete", rc == 0 and so == small, (rc, se))
    rc, so, se = wget("-O", "-", "https://rsa.test/nolen-trunc/small.txt")
    ok("a body without a length, cut without close_notify: truncated (exit 4)", rc == 4 and "truncated" in se, (rc, se))
    # -- slowness --
    rc, so, se = wget("-O", "-", "https://rsa.test/drip/small.txt")
    ok("a slow server: one byte a TLS record, 1,300 records", rc == 0 and so == small, (rc, se))
    rc, so, se = wget("-O", "-", "https://rsa.test:8444/small.txt")
    ok("a slow handshake (2.5 s) within the limit", rc == 0 and so == small, (rc, se))
    rc, so, se = run(client, "--connect", "127.0.0.1", "--anchors", anchors, "--idle-ms", "1000", "-O", "-",
                     "https://rsa.test:8444/small.txt", cwd=work)
    ok("a slow handshake past the socket layer's limit: exit 4", rc == 4, (rc, se))
    rc, so, se = run(client, "--connect", "127.0.0.1", "--anchors", anchors, "--idle-ms", "1000", "-O", "-",
                     "https://rsa.test/stall", cwd=work)
    ok("a stalled server: exit 4", rc == 4 and "stalled" in se, (rc, se))
    rc, so, se = wget("-x", "https://rsa.test/")
    ok("a bad option: exit 2", rc == 2, (rc, se))
    # -- the fuzzing seeds: one recorded server flight, with the clock and seed the harness uses --
    fs = os.path.join(out, "fuzz-seeds")
    shutil.rmtree(fs, ignore_errors=True)
    for t in ("tls", "url", "plan", "sntp"):
        os.makedirs(os.path.join(fs, t))
    shutil.copy(anchors, os.path.join(fs, "anchors"))
    for host, nm in (("rsa.test", "flight"), ("ecdsa.test", "flight-ecdsa")):
        recf = os.path.join(d, "rec-" + nm)
        rc, so, se = wget("-O", "-", "https://%s/close/small.txt" % host,
                          opts=["--seed", "42" * 32, "--clock", str(nowt), "--record", recf, "--no-cache"])
        if rc == 0 and os.path.exists(recf):
            open(os.path.join(fs, "tls", nm), "wb").write(struct.pack(">I", nowt) + host.encode() + b"\0" +
                                                          open(recf, "rb").read())
    ok("fuzzing seeds: recorded server flights", os.path.exists(os.path.join(fs, "tls", "flight")), "")
    for i, u in enumerate([b"http://h/a/b?c=%2F#f", b"https://h:8443/", b"http://h/%2e%2E", b"https://a.b/x/y.tar.gz"]):
        open(os.path.join(fs, "url", "u%d" % i), "wb").write(u + b"\n/redir#x\n" + b"https://x/y")
    for i, r in enumerate([b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello",
                           b"HTTP/1.1 206 P\r\nContent-Range: bytes 100-199/200\r\nContent-Length: 100\r\n\r\n",
                           b"HTTP/1.1 302 F\r\nLocation: https://k/z\r\n\r\n",
                           b"HTTP/1.1 416 R\r\nContent-Range: bytes */200\r\n\r\n",
                           b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n"]):
        open(os.path.join(fs, "plan", "r%d" % i), "wb").write(bytes([i & 3, 100 if i == 1 else 0]) + r)
    nonce = bytes(range(1, 9))
    for i, (b0, st) in enumerate([(0x24, 2), (0x1c, 1), (0x24, 0), (0xe4, 1)]):
        p = bytes([b0, st, 6, 0xec]) + bytes(8) + b"DENY" + bytes(8) + nonce + struct.pack(">IIII", 3900000000, 1 << 30, 3900000001, 1 << 31)
        open(os.path.join(fs, "sntp", "p%d" % i), "wb").write(p + b"server a.b\n# x\nserver c\n")


# ---------------------------------------------------------------------------------------------------------------
NTP_UNIX = 2208988800


def ntp(t):
    s = int(t)
    return struct.pack(">II", (s + NTP_UNIX) & 0xFFFFFFFF, int((t - s) * (1 << 32)) & 0xFFFFFFFF)


BEHAVIOUR = {}


def sntp_server(addr, kind, now_fn):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind((addr, 123))

    def loop():
        while True:
            q, peer = s.recvfrom(512)
            if len(q) < 48:
                continue
            origin = q[40:48]
            t = now_fn()
            k = BEHAVIOUR.get(addr, kind)
            li, vn, mode, stratum, refid = 0, 4, 4, 2, b"GPS\0"
            tx = t
            if k == "liar-future":
                tx = t + 500 * 86400
            elif k == "liar-past":
                tx = t - 20 * 365 * 86400
            elif k == "liar-near":
                tx = t + 3600
            elif k == "slow":
                time.sleep(1.5)
                tx = t = now_fn()
            elif k == "silent":
                continue
            org = origin
            pkts = []
            if k == "wrong-origin":
                org = bytes(8)
            if k == "spoof-first":          # a forged answer (another nonce) first, then the true one
                pkts.append(bytes([0x24, 1, 6, 0xec]) + bytes(8) + refid + ntp(t + 9e6) + bytes(8) + ntp(t + 9e6) + ntp(t + 9e6))
            if k == "kod":
                stratum, refid = 0, b"DENY"
            if k == "stratum16":
                stratum = 16
            if k == "leap3":
                li = 3
            if k == "client-mode":
                mode = 3
            if k == "v2":
                vn = 2
            hdr = bytes([li << 6 | vn << 3 | mode, stratum, 6, 0xec]) + bytes(8) + refid
            body = ntp(tx - 0.001) + org + ntp(tx - 0.0005) + ntp(tx)
            if k == "zero-tx":
                body = body[:24] + bytes(8)
            if k == "short":
                body = body[:20]
            pkts.append(hdr + body)
            for p in pkts:
                s.sendto(p, peer)
    threading.Thread(target=loop, daemon=True).start()


def sntp(out, client):
    d = os.path.join(out, "sntp")
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    kinds = ["good", "liar-future", "liar-past", "liar-near", "slow", "silent", "wrong-origin", "spoof-first", "kod",
             "stratum16", "leap3", "client-mode", "v2", "zero-tx", "short"]
    addr = {}
    for i, k in enumerate(kinds):
        a = "127.0.0.%d" % (i + 2)
        addr[k] = a
        sntp_server(a, k, time.time)
    time.sleep(0.2)
    now = int(time.time())
    build = now - 30 * 86400
    clockf = os.path.join(d, "clock")
    conf = os.path.join(d, "sntp.conf")

    def ask(servers, rtc=None, floorfile=None, extra=""):
        open(conf, "w").write("# test servers\n" + "".join("server %s\n" % addr[s] for s in servers) + extra)
        if floorfile is None:
            if os.path.exists(clockf):
                os.unlink(clockf)
        else:
            open(clockf, "w").write(floorfile)
        a = [client, "--conf", conf, "--clockfile", clockf, "--build", str(build), "--wait-ms", "4000"]
        if rtc is not None:
            a += ["--rtc", str(rtc)]
        rc, so, se = run(*a)
        so = so.decode()
        res = [l for l in so.splitlines() if l.startswith("result")]
        fl = open(clockf).read() if os.path.exists(clockf) else None
        return rc, so, res[0] if res else "", fl

    def near(res, t, slack=3):
        t += int(time.time()) - now         # the checks' times are relative to now: the run takes a while
        try:
            v = int(res.split()[2])
        except (IndexError, ValueError):
            return False
        return abs(v - t) <= slack

    rc, so, res, fl = ask(["good"])
    ok("sntp: a true answer, no battery clock: set and the floor raised", near(res, now) and res.endswith("raise 1") and
       fl and fl.startswith("zenix-clock 1 ") and abs(int(fl.split()[2]) - now) <= 3, (so, fl))
    rc, so, res, fl = ask(["good"], rtc=now)
    ok("sntp: a true answer agreeing with the battery clock raises", near(res, now) and res.endswith("raise 1"), so)
    rc, so, res, fl = ask(["liar-future"])
    ok("sntp: 500 days ahead, no battery clock: set, the floor NOT raised", near(res, now + 500 * 86400) and
       res.endswith("raise 0") and fl is None, (so, fl))
    rc, so, res, fl = ask(["liar-near"], rtc=now)
    ok("sntp: an hour off the battery clock: set, the floor NOT raised", near(res, now + 3600) and res.endswith("raise 0") and
       fl is None, (so, fl))
    rc, so, res, fl = ask(["liar-past", "good"])
    ok("sntp: an answer before the floor refused, the next server used", "refused before-the-floor" in so and near(res, now) and
       res.endswith("raise 1"), so)
    old = "zenix-clock 1 %d\n" % (now - 5 * 86400)
    rc, so, res, fl = ask(["liar-future"], floorfile=old)
    ok("sntp: a lying server leaves the clock file as it was", fl == old and res.endswith("raise 0"), (so, fl))
    rc, so, res, fl = ask(["liar-future"], floorfile="zenix-clock 1 %d\n" % (now + 200 * 86400))
    ok("sntp: the floor from the clock file (200 days ahead) lets 500 days through as a raise",
       res.endswith("raise 1") and near(res, now + 500 * 86400), (so, fl))
    rc, so, res, fl = ask(["kod", "good"])
    ok("sntp: a kiss of death obeyed, the next server asked", "kod DENY" in so and near(res, now), so)
    rc, so, res, fl = ask(["wrong-origin", "good"])
    ok("sntp: not our nonce: dropped, then the wait times out, the next server", "dropped -4" in so and near(res, now), so)
    rc, so, res, fl = ask(["spoof-first"])
    ok("sntp: a forged answer before the true one is dropped", "dropped -4" in so and near(res, now) and res.endswith("raise 1"), so)
    for k, code in (("stratum16", -6), ("leap3", -7), ("client-mode", -2), ("v2", -3), ("zero-tx", -8)):
        rc, so, res, fl = ask([k, "good"])
        ok("sntp: %s refused (%d), the next server" % (k, code), ("refused %d" % code) in so and near(res, now) and fl, so)
    rc, so, res, fl = ask(["short", "good"])
    ok("sntp: a short packet dropped", "dropped -1" in so, so)
    rc, so, res, fl = ask(["slow"])
    ok("sntp: a slow server (1.5 s) answers within the wait, the round trip halved", near(res, now, 4), so)
    t0 = time.time()
    rc, so, res, fl = ask(["silent", "silent"])
    ok("sntp: silent servers: nothing set, the whole wait bounded (4 s)", res == "result none" and rc == 1 and
       time.time() - t0 < 6 and fl is None, (so, time.time() - t0))
    rc, so, res, fl = ask(["good"], extra="server 127.0.0.99\nserver x\nserver y\nserver z\nfoo\n")
    ok("sntp: /etc/sntp's warnings (a fifth server, an unknown word)", "line 6: more than four" in so and "line 7: unknown word" in so, so)


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    if sys.argv[1] == "https":
        https(sys.argv[2], sys.argv[3])
    elif sys.argv[1] == "sntp":
        sntp(sys.argv[2], sys.argv[3])
    else:
        sys.exit(__doc__)
    sys.exit(min(fails, 100))


if __name__ == "__main__":       # tools/wget-ns.py imports the CA and the servers
    main()
