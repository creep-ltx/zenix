#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The PC's side of tools/mail-test.sh (DESIGN D59 section 8): in this user, mount and network namespace (run under
unshare --map-root-user --map-auto -nm, as tools/irc-ns.py: no root, nothing leaves the PC) a test CA made at this
run (tools/host/tls-test.py's make_pki: its keys only in DIR, never in git) and tools/host/mail-fake.py twice: IMAP
over TLS on 127.0.0.1:993 and SMTP with STARTTLS on :587, both with rsa.test's certificate; then COMMAND (the image
set-up and the FS-UAE run, which reaches them as 10.0.2.2), then all stopped.

  mail-ns.py DIR -- COMMAND...

DIR gets tls/ (the CA, the certificates, test.anchors), mail.log (every line both ways) and smtp/ (each message
received, byte for byte, and its envelope). Exits with the command's status."""
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
fake = load("fake", os.path.join(here, "host", "mail-fake.py"))
subprocess.run(["ip", "link", "set", "lo", "up"], check=True)
tls = os.path.join(D, "tls")
os.makedirs(tls, exist_ok=True)
smtp = os.path.join(D, "smtp")
os.makedirs(smtp, exist_ok=True)
sites, anchors, now = tt.make_pki(tls)
if tt.fails:
    sys.exit("mail-ns: the test CA could not be made")
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.minimum_version = ssl.TLSVersion.TLSv1_2
ctx.maximum_version = ssl.TLSVersion.TLSv1_2
ctx.load_cert_chain(*sites["rsa.test"][0])
log = os.path.join(D, "mail.log")
fake.serve("imap", "127.0.0.1", 993, log, smtp, ctx, True)
fake.serve("smtp", "127.0.0.1", 587, log, smtp, ctx, False)
sys.exit(subprocess.call(cmd))
