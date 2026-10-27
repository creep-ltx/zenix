#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""A misbehaving HTTP/1.1 server for zpkg's HTTP core (DESIGN D22 8.1; tools/host/zpkg-test.sh, in unshare -rnm).

  http-test-server.py ROOT PORTFILE     serves ROOT on 127.0.0.1 (a free port, written to PORTFILE)

Paths (each prefix changes how the file under ROOT is served; the rest is the file's path):
  /PATH               plain: Content-Length, Range honoured (206), keep-alive
  /chunked/PATH       chunked, random chunk sizes          /norange/PATH   Range ignored (200, the whole file)
  /badrange/PATH      206 with a wrong Content-Range        /long/PATH      100 bytes more than Content-Length, close
  /short/PATH         half the body, then close             /cut/PATH       the first request: half, then close
  /drip/PATH          one byte every 0.2 s                  /damaged/PATH   one byte changed
  /close/PATH         "Connection: close"                    /redir/N/PATH   N redirects (302), then PATH
  /loop               302 to itself                         /stall          reads the request, never answers
  /hugechunk          a chunk size of 16 hex digits         /endless        no length, a body without end
  /endless-chunked    chunks without end                    /bighdr         a 5 KB header
  /tohttps/PATH       302 to https://
Each accepted connection is counted in PORTFILE.conns (one line each).
"""
import os
import random
import socket
import sys
import threading
import time

ROOT, PORTFILE = sys.argv[1], sys.argv[2]
cut_seen = set()
lock = threading.Lock()


def send(c, data):
    try:
        c.sendall(data)
        return True
    except OSError:
        return False


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


def file_of(p):
    full = os.path.normpath(os.path.join(ROOT, p.lstrip("/")))
    if not full.startswith(os.path.normpath(ROOT)) or not os.path.isfile(full):
        return None
    return open(full, "rb").read()


def handle(c):
    buf = b""
    while True:
        req, buf = read_request(c, buf)
        if req is None:
            return
        method, path, hdrs = req
        rng = hdrs.get("range")
        mode, _, rest = path.lstrip("/").partition("/")
        keep = True

        def reply(status, body=b"", extra=(), length=True):
            h = "HTTP/1.1 %s\r\n" % status
            for k, v in extra:
                h += "%s: %s\r\n" % (k, v)
            if length:
                h += "Content-Length: %d\r\n" % len(body)
            return send(c, h.encode() + b"\r\n" + body)

        if path == "/stall":
            time.sleep(30)
            return
        if path == "/loop":
            reply("302 Found", b"", [("Location", "/loop")])
            continue
        if path == "/bighdr":
            reply("200 OK", b"x", [("X-Big", "a" * 5000)])
            continue
        if path == "/hugechunk":
            send(c, b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n00000000FFFFFFFFFF\r\nabc")
            return
        if path == "/endless":
            send(c, b"HTTP/1.1 200 OK\r\n\r\n")
            while send(c, b"x" * 4096):
                pass
            return
        if path == "/endless-chunked":
            send(c, b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n")
            while send(c, b"1000\r\n" + b"y" * 4096 + b"\r\n"):
                pass
            return
        if mode == "redir":
            n, _, p = rest.partition("/")
            n = int(n)
            reply("302 Found", b"moved", [("Location", "/redir/%d/%s" % (n - 1, p) if n > 1 else "/" + p)])
            continue
        if mode == "tohttps":
            reply("301 Moved", b"", [("Location", "https://127.0.0.1/" + rest)])
            continue
        if mode in ("chunked", "norange", "badrange", "long", "short", "cut", "drip", "damaged", "close"):
            data = file_of(rest)
        else:
            mode, data = "plain", file_of(path)
        if data is None:
            reply("404 Not Found", b"no such file\n")
            continue
        if mode == "damaged" and data:
            i = len(data) // 2
            data = data[:i] + bytes([data[i] ^ 0x40]) + data[i + 1:]
        first = 0
        if rng and mode not in ("norange", "chunked") and rng.startswith("bytes=") and rng.endswith("-"):
            first = int(rng[6:-1])
        if mode == "cut":
            with lock:
                again = rest in cut_seen
                cut_seen.add(rest)
            if not again:
                send(c, b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n" % len(data) + data[:len(data) // 2])
                return
        if mode == "chunked":
            out = b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
            i = 0
            while i < len(data):
                k = random.randint(1, 3000)
                out += b"%x\r\n" % len(data[i:i + k]) + data[i:i + k] + b"\r\n"
                i += k
            send(c, out + b"0\r\n\r\n")
            continue
        if first:
            body = data[first:]
            cr = "bytes %d-%d/%d" % (first, len(data) - 1, len(data))
            if mode == "badrange":
                cr = "bytes %d-%d/%d" % (first - 1 if first else 0, len(data) - 1, len(data))
            if not reply("206 Partial Content", body, [("Content-Range", cr)]):
                return
            continue
        if mode == "long":
            send(c, b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\nConnection: close\r\n\r\n" % len(data) + data + b"z" * 100)
            return
        if mode == "short":
            send(c, b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n" % len(data) + data[:len(data) // 2])
            return
        if mode == "drip":
            if not send(c, b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n" % len(data)):
                return
            for b in data:
                time.sleep(0.2)
                if not send(c, bytes([b])):
                    return
            continue
        if mode == "close":
            reply("200 OK", data, [("Connection", "close")])
            return
        if not reply("200 OK", data):
            return
        if not keep:
            return


def serve(c):
    try:
        handle(c)
    except (OSError, ValueError):
        pass
    finally:
        try:
            c.close()
        except OSError:
            pass


def main():
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", 0))
    s.listen(64)
    open(PORTFILE + ".part", "w").write("%d\n" % s.getsockname()[1])
    os.replace(PORTFILE + ".part", PORTFILE)
    while True:
        c, _ = s.accept()
        with open(PORTFILE + ".conns", "a") as f:
            f.write("1\n")
        threading.Thread(target=serve, args=(c,), daemon=True).start()


main()
