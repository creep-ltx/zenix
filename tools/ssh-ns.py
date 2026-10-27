#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The PC's side of tools/ssh-test.sh (DESIGN D21 10 step 3): two real OpenSSH sshds in this user, mount and network
namespace (run under unshare --map-root-user --map-auto -nm, as tools/host/ssh-test.py: no root, nothing leaves the
PC), then a command (the FS-UAE run), then both stopped.

  ssh-ns.py DIR -- COMMAND...

DIR holds hostkey and hostkey2 (made by ssh-test.sh) and gets the namespace's test files. sshd "main" on
127.0.0.1:2222 (the Amiga's 10.0.2.2:2222 through slirp) with hostkey, RekeyLimit 1M, password logins for root
(password zenix-test-pw) and DIR/home/.ssh/authorized_keys; sshd "other" on 2223 with hostkey2 (the Amiga's
known_hosts names hostkey for it: a changed key). Exits with the command's status."""
import os, socket, subprocess, sys, time

T = os.path.abspath(sys.argv[1])
cmd = sys.argv[sys.argv.index("--") + 1:]
PW = "zenix-test-pw"


def run(*a):
    r = subprocess.run(list(a), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if r.returncode:
        sys.exit("ssh-ns: %s: %s" % (" ".join(a), r.stderr.decode(errors="replace")[-400:]))
    return r.stdout.decode()


for d in ("home/.ssh", "empty", "etc"):
    os.makedirs(os.path.join(T, d), exist_ok=True)
os.chmod(os.path.join(T, "empty"), 0o755)
run("ip", "link", "set", "lo", "up")
h = run("openssl", "passwd", "-6", PW).strip()
w = lambda n, s: open(os.path.join(T, "etc", n), "w").write(s)
w("passwd", "root:x:0:0:root:%s/home:/bin/sh\nsshd:x:0:0:sshd:/var/empty:/usr/bin/nologin\n" % T)
w("shadow", "root:%s:19000:0:99999:7:::\nsshd:!:19000::::::\n" % h)
w("group", "root:x:0:\nsshd:x:0:\n")
for src, dst in (("passwd", "/etc/passwd"), ("shadow", "/etc/shadow"), ("group", "/etc/group")):
    run("mount", "--bind", os.path.join(T, "etc", src), dst)
for d in ("/usr/share/empty.sshd", "/var/empty", "/run/sshd", "/var/lib/sshd"):
    if os.path.isdir(d):
        run("mount", "--bind", os.path.join(T, "empty"), d)
ak = os.path.join(T, "home/.ssh/authorized_keys")
open(ak, "a").close()
procs = []
for name, port, key, extra in (("main", 2222, "hostkey", "RekeyLimit 1M\n"), ("other", 2223, "hostkey2", "")):
    conf = os.path.join(T, name + ".conf")
    with open(conf, "w") as f:
        f.write("%sPort %d\nListenAddress 127.0.0.1\nHostKey %s\nPidFile %s/%s.pid\nUsePAM no\nPermitRootLogin yes\n"
                "StrictModes no\nPasswordAuthentication yes\nKbdInteractiveAuthentication no\n"
                "AuthorizedKeysFile %s\nLogLevel VERBOSE\n" % (extra, port, os.path.join(T, key), T, name, ak))
    procs.append(subprocess.Popen(["/usr/bin/sshd", "-f", conf, "-E", os.path.join(T, name + ".log"), "-D"]))
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", port), 0.2).close()
            break
        except OSError:
            time.sleep(0.05)
    else:
        sys.exit("ssh-ns: sshd %s did not start: %s" % (name, open(os.path.join(T, name + ".log")).read()[-600:]))
try:
    rc = subprocess.call(cmd)
finally:
    for p in procs:
        p.terminate()
        p.wait()
sys.exit(rc)
