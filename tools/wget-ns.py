#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The PC's side of tools/wget-test.sh (DESIGN D21 10 step 7, the programs): in this user, mount and network
namespace (run under unshare --map-root-user --map-auto -nm, as tools/host/tls-test.sh: no root, nothing leaves the
PC) a test CA made at this run (tools/host/tls-test.py's make_pki: its keys only in DIR, never in git), its HTTPS
servers on 127.0.0.1:443 (one certificate case per SNI name) and plain HTTP on :80, the same file tree, an SNTP server
on :123 whose answers a control page switches (http://127.0.0.1:8080/mode/KIND: good, liar-future, liar-past, kod...,
tls-test.py's kinds), then COMMAND (the image set-up and the FS-UAE run, which reaches these as 10.0.2.2), then all
stopped.

  wget-ns.py DIR -- COMMAND...

DIR gets tls/ (the CA, the certificates, test.anchors, owner.pem), www/ (small.txt, big.bin of 1 MB, dir/f.bin,
index.html), sntp.log (one line "KIND UNIXTIME" per SNTP answer) and http.log. Exits with the command's status."""
import http.server
import importlib.util
import os
import subprocess
import sys
import threading
import time

D = os.path.abspath(sys.argv[1])
cmd = sys.argv[sys.argv.index("--") + 1:]
here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("tlstest", os.path.join(here, "host", "tls-test.py"))
tt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tt)

subprocess.run(["ip", "link", "set", "lo", "up"], check=True)
tls = os.path.join(D, "tls")
www = os.path.join(D, "www")
os.makedirs(tls, exist_ok=True)
os.makedirs(os.path.join(www, "dir"), exist_ok=True)
sites, anchors, now = tt.make_pki(tls)
if tt.fails:
    sys.exit("wget-ns: the test CA could not be made")
open(os.path.join(www, "big.bin"), "wb").write(os.urandom(1 << 20))
open(os.path.join(www, "small.txt"), "wb").write(b"hello, Zenix\n" * 100)
open(os.path.join(www, "dir", "f.bin"), "wb").write(os.urandom(5000))
open(os.path.join(www, "index.html"), "wb").write(b"<p>index</p>\n")
tt.ROOT = www
tt.listener(443, tt.contexts(sites))
tt.listener(80, None)

log = open(os.path.join(D, "sntp.log"), "a", buffering=1)
MODE = {"kind": "good"}


def now_fn():
    t = time.time()
    k = MODE["kind"]
    off = {"liar-future": 500 * 86400, "liar-past": -20 * 365 * 86400}.get(k, 0)
    log.write("%s %d\n" % (k, int(t + off)))
    return t


tt.BEHAVIOUR["127.0.0.1"] = "good"
tt.sntp_server("127.0.0.1", "good", now_fn)


class Control(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        k = self.path.rsplit("/", 1)[-1]
        MODE["kind"] = k
        tt.BEHAVIOUR["127.0.0.1"] = k
        b = ("mode %s\n" % k).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def log_message(self, fmt, *a):
        open(os.path.join(D, "http.log"), "a").write((fmt % a) + "\n")


srv = http.server.ThreadingHTTPServer(("127.0.0.1", 8080), Control)
threading.Thread(target=srv.serve_forever, daemon=True).start()
time.sleep(0.3)
rc = subprocess.call(cmd)
srv.shutdown()
open(os.path.join(D, "log.txt"), "w").write(repr({k: v for k, v in tt.LOG.items()}) + "\n")
sys.exit(rc)
