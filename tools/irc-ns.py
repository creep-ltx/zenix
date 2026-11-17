#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The PC's side of tools/irc-test.sh (DESIGN D58 section 8): in this user, mount and network namespace (run under
unshare --map-root-user --map-auto -nm, as tools/wget-ns.py: no root, nothing leaves the PC) a test CA made at this
run (tools/host/tls-test.py's make_pki: its keys only in DIR, never in git) and tools/host/irc-fake.py twice: plain
on 127.0.0.1:6667 and TLS 1.2 on :6697 with rsa.test's certificate; then COMMAND (the image set-up and the FS-UAE
run, which reaches them as 10.0.2.2), then all stopped.

  irc-ns.py DIR -- COMMAND...

DIR gets tls/ (the CA, the certificates, test.anchors) and irc.log (every line both ways). Exits with the command's
status."""
import importlib.util
import os
import ssl
import subprocess
import sys

D = os.path.abspath(sys.argv[1])
cmd = sys.argv[sys.argv.index("--") + 1:]
here = os.path.dirname(os.path.abspath(__file__))


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    saved = sys.argv
    sys.argv = [saved[0]]
    spec.loader.exec_module(m)
    sys.argv = saved
    return m


tt = load("tlstest", os.path.join(here, "host", "tls-test.py"))
fake = load("fake", os.path.join(here, "host", "irc-fake.py"))
subprocess.run(["ip", "link", "set", "lo", "up"], check=True)
tls = os.path.join(D, "tls")
os.makedirs(tls, exist_ok=True)
sites, anchors, now = tt.make_pki(tls)
if tt.fails:
    sys.exit("irc-ns: the test CA could not be made")
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.minimum_version = ssl.TLSVersion.TLSv1_2
ctx.maximum_version = ssl.TLSVersion.TLSv1_2
ctx.load_cert_chain(*sites["rsa.test"][0])
log = os.path.join(D, "irc.log")
fake.serve("127.0.0.1", 6667, log)
fake.serve("127.0.0.1", 6697, log, ctx)
sys.exit(subprocess.call(cmd))
