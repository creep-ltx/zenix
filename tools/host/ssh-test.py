#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""net/ssh/ against a real OpenSSH sshd (DESIGN D21 10 step 1), run by tools/host/crypto-test.sh inside a user,
mount and network namespace (unshare --map-root-user --map-auto -nm: no root, nothing leaves the PC; the mapped
uid range lets sshd's privilege separation work). The namespace's /etc/passwd, /etc/shadow, /etc/group,
/etc/pam.d/sshd and sshd's empty directory are bind-mounted test files; every key is made here per run.

  ssh-test.py CLIENT OUTDIR

CLIENT is tools/host/ssh-client built over net/ssh/. Prints PASS/FAIL lines; SKIPPED for what cannot run here.
"""
import atexit
import hashlib
import os
import re
import shutil
import socket
import subprocess
import sys
import threading
import time

CLIENT, OUT = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
T = os.path.join(OUT, "ssh")
PW = "zenix-test-pw"
PORT = 2222
fails = 0


def run(*a, check=True, **kw):
    r = subprocess.run(list(a), stdout=subprocess.PIPE, stderr=subprocess.PIPE, **kw)
    if check and r.returncode != 0:
        raise SystemExit("FAIL ssh-test: %s: %s" % (" ".join(a), r.stderr.decode(errors="replace")[-500:]))
    return r


def result(name, ok, detail=""):
    global fails
    if ok:
        print("PASS ssh-test: %s" % name)
    else:
        fails += 1
        print("FAIL ssh-test: %s %s" % (name, detail))
    sys.stdout.flush()


def setup():
    shutil.rmtree(T, ignore_errors=True)
    for d in ("home/.ssh", "empty", "etc"):
        os.makedirs(os.path.join(T, d))
    os.chmod(os.path.join(T, "empty"), 0o755)
    run("ip", "link", "set", "lo", "up")
    h = run("openssl", "passwd", "-6", PW).stdout.decode().strip()
    w = lambda n, s: open(os.path.join(T, "etc", n), "w").write(s)
    w("passwd", "root:x:0:0:root:%s/home:/bin/sh\nsshd:x:0:0:sshd:/var/empty:/usr/bin/nologin\n" % T)
    w("shadow", "root:%s:19000:0:99999:7:::\nsshd:!:19000::::::\n" % h)
    w("group", "root:x:0:\nsshd:x:0:\n")
    w("pam-sshd", "auth required pam_unix.so\naccount required pam_unix.so\npassword required pam_unix.so\n"
                  "session required pam_unix.so\n")
    for src, dst in (("passwd", "/etc/passwd"), ("shadow", "/etc/shadow"), ("group", "/etc/group"),
                     ("pam-sshd", "/etc/pam.d/sshd")):
        run("mount", "--bind", os.path.join(T, "etc", src), dst)
    empty = None
    for d in ("/usr/share/empty.sshd", "/var/empty", "/run/sshd", "/var/lib/sshd"):
        if os.path.isdir(d):
            run("mount", "--bind", os.path.join(T, "empty"), d)
            empty = d
    for k in ("hostkey", "hostkey2", "ukey"):
        run("ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-C", "test-" + k, "-f", os.path.join(T, k))
    run("ssh-keygen", "-q", "-t", "ed25519", "-N", "pass phrase", "-a", "4", "-f", os.path.join(T, "ukey-pw"))


RUNNING = []


@atexit.register
def stop_all():
    for p in RUNNING:
        if p.poll() is None:
            p.kill()
            p.wait()


class Sshd:
    def __init__(self, name, hostkey="hostkey", extra="", authkeys=("ukey",)):
        self.conf = os.path.join(T, name + ".conf")
        self.log = os.path.join(T, name + ".log")
        ak = os.path.join(T, "home/.ssh/authorized_keys")
        with open(ak, "w") as f:
            for k in authkeys:
                f.write(open(os.path.join(T, k + ".pub")).read())
        with open(self.conf, "w") as f:
            # (sshd takes the first value of a keyword: the test's own lines first)
            f.write("%s\nPort %d\nListenAddress 127.0.0.1\nHostKey %s\nPidFile %s/%s.pid\nUsePAM no\n"
                    "PermitRootLogin yes\nStrictModes no\nPasswordAuthentication yes\n"
                    "KbdInteractiveAuthentication yes\nAuthorizedKeysFile %s\nLogLevel DEBUG1\n"
                    % (extra, PORT, os.path.join(T, hostkey), T, name, ak))
        self.p = subprocess.Popen(["/usr/bin/sshd", "-f", self.conf, "-E", self.log, "-D"])
        RUNNING.append(self.p)
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", PORT), 0.2).close()
                return
            except OSError:
                time.sleep(0.05)
        raise SystemExit("FAIL ssh-test: sshd (%s) did not start: %s" % (name, open(self.log).read()[-800:]))

    def stop(self):
        self.p.terminate()
        self.p.wait()
        for _ in range(100):           # the port free again
            try:
                socket.create_connection(("127.0.0.1", PORT), 0.1).close()
                time.sleep(0.05)
            except OSError:
                return


def client(*args, stdin=b"", port=PORT, timeout=60):
    a = [CLIENT, "-v", "-p", str(port), "-l", "root"] + list(args)
    try:
        r = subprocess.run(a, input=stdin, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
    except subprocess.TimeoutExpired:
        return 254, b"", b"(timed out)"
    return r.returncode, r.stdout, r.stderr


def kexes(err):
    m = re.search(rb"ssh-client: kex (\d+)", err)
    return int(m.group(1)) if m else 0


# ---- a proxy that breaks the server's side of the first key exchange (the strict-kex tests) ----
def proxy(mode, lport):
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", lport))
    srv.listen(1)

    def pump(a, b, fix=None):
        buf = b""
        done = fix is None
        try:
            while True:
                d = a.recv(65536)
                if not d:
                    break
                if not done:
                    buf += d
                    out, done, buf = fix(buf)
                    if out:
                        b.sendall(out)
                    continue
                b.sendall(d)
        except OSError:
            pass
        finally:
            for x in (a, b):
                try:
                    x.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass

    ignore = bytes.fromhex("0000000c060200000000") + bytes(6)     # SSH_MSG_IGNORE "" (plaintext, padded to 16)

    def fix(buf):
        """the server's bytes so far -> (bytes to pass on, done, kept)"""
        i = buf.find(b"\r\n")
        if i < 0 or not buf.startswith(b"SSH-"):
            return b"", False, buf
        ver, rest = buf[:i + 2], buf[i + 2:]
        if len(rest) < 4:
            return b"", False, buf
        n = int.from_bytes(rest[:4], "big") + 4
        if len(rest) < n:
            return b"", False, buf
        kexinit, tail = rest[:n], rest[n:]
        if mode == "nostrict":
            kexinit = kexinit.replace(b"kex-strict-s-v00@openssh.com", b"kex-strict-x-v00@openssh.com")
            return ver + kexinit + tail, True, b""
        if mode == "ignore-first":
            return ver + ignore + kexinit + tail, True, b""
        if mode == "ignore-during":
            return ver + kexinit + ignore + tail, True, b""
        return buf, True, b""

    def serve():
        c, _ = srv.accept()
        s = socket.create_connection(("127.0.0.1", PORT))
        t1 = threading.Thread(target=pump, args=(c, s), daemon=True)
        t2 = threading.Thread(target=pump, args=(s, c, fix), daemon=True)
        t1.start()
        t2.start()
        t1.join()
        t2.join()
        srv.close()

    t = threading.Thread(target=serve, daemon=True)
    t.start()
    return t


def main():
    setup()
    kh = os.path.join(T, "known_hosts")
    key = os.path.join(T, "ukey")
    hostpub = open(os.path.join(T, "hostkey.pub")).read().split()[1]

    d = Sshd("main")
    # trust on first use: refused without -y, nothing written; then accepted and written as OpenSSH reads it
    rc, out, err = client("-k", kh, "-i", key, "127.0.0.1", "echo hi")
    result("an unknown host key is refused without -y", rc == 255 and b"not trusted" in err and not os.path.exists(kh),
           err[-300:])
    rc, out, err = client("-k", kh, "-y", "-i", key, "127.0.0.1", "echo KEYLOGIN; exit 7")
    result("key login (Ed25519 from ssh-keygen), the exec's exit status 7", rc == 7 and out == b"KEYLOGIN\n", (rc, out, err[-400:]))
    text = open(kh).read() if os.path.exists(kh) else ""
    result("known_hosts gets [127.0.0.1]:2222 ssh-ed25519 KEY", text == "[127.0.0.1]:2222 ssh-ed25519 %s\n" % hostpub, text)
    f = run("ssh-keygen", "-F", "[127.0.0.1]:2222", "-f", kh, check=False)
    result("OpenSSH's ssh-keygen -F finds our known_hosts line", f.returncode == 0 and hostpub.encode() in f.stdout)
    fp = run("ssh-keygen", "-lf", os.path.join(T, "hostkey.pub")).stdout.split()[1]
    result("the fingerprint shown is ssh-keygen -lf's", b"key " + fp + b": trusted" in err, (fp, err[-300:]))
    rc, out, err = client("-k", kh, "-i", key, "127.0.0.1", "echo again")
    result("a known host goes through without asking", rc == 0 and out == b"again\n" and b"trusted" not in err, err[-300:])
    # passwords
    rc, out, err = client("-k", kh, "-m", "p", "-P", PW, "127.0.0.1", "echo PW")
    result("password login", rc == 0 and out == b"PW\n", err[-300:])
    rc, out, err = client("-k", kh, "-m", "p", "-P", "wrong", "127.0.0.1", "echo PW")
    result("a wrong password: three tries, then refused", rc == 255 and err.count(b"password: refused") == 3 and
           b"authentication failed" in err, err[-400:])
    # keys: protected by OpenSSH, and written by net/ssh/ (read back by OpenSSH)
    run("cp", os.path.join(T, "ukey-pw.pub"), os.path.join(T, "home/.ssh/authorized_keys"))
    rc, out, err = client("-k", kh, "-i", key + "-pw", "-x", "pass phrase", "127.0.0.1", "echo PROTECTED")
    result("a passphrase-protected key from ssh-keygen (bcrypt, aes256-ctr)", rc == 0 and out == b"PROTECTED\n", err[-300:])
    rc, out, err = client("-k", kh, "-i", key + "-pw", "-x", "not it", "127.0.0.1", "true")
    result("the wrong passphrase is told apart", rc == 255 and b"error %d" % -5 in err, err[-200:])
    rc, out, err = client("-k", kh, "-i", key + "-pw", "127.0.0.1", "true")
    result("a protected key without a passphrase is told apart", rc == 255 and b"error %d" % -4 in err, err[-200:])
    for nm, extra in (("zkey", []), ("zkey-pw", ["-x", "z pass", "-a", "4"])):
        p = os.path.join(T, nm)
        r = subprocess.run([CLIENT, "-G", p, "-C", "zenix@test"] + extra, stderr=subprocess.PIPE)
        ok = r.returncode == 0
        y = run("ssh-keygen", "-y", "-P", "z pass" if extra else "", "-f", p, check=False)
        mine = open(p + ".pub").read().split()[:2] if ok else []
        result("a key written by net/ssh/ (%s) reads in OpenSSH's ssh-keygen -y" % ("protected" if extra else "plain"),
               ok and y.returncode == 0 and y.stdout.split()[:2] == [m.encode() for m in mine], (r.stderr, y.stderr))
        if ok:
            shutil.copy(p + ".pub", os.path.join(T, "home/.ssh/authorized_keys"))
            rc, out, err = client("-k", kh, "-i", p, *extra[:2], "127.0.0.1", "echo ZKEY")
            result("login with that key", rc == 0 and out == b"ZKEY\n", err[-300:])
    shutil.copy(os.path.join(T, "ukey.pub"), os.path.join(T, "home/.ssh/authorized_keys"))
    # the config subset
    cfg = os.path.join(T, "config")
    open(cfg, "w").write("# test\nHost other\n  HostName 10.9.9.9\nHost pc !nope\n  HostName 127.0.0.1\n"
                         "  Port 2222\n  User root\n  IdentityFile %s\n  ForwardAgent yes\nHost *\n  Port 9\n" % key)
    rc, out, err = client("-k", kh, "-F", cfg, "pc", "echo CFG")
    result("~/.ssh/config: Host, HostName, Port, User, IdentityFile; an unknown keyword reported",
           rc == 0 and out == b"CFG\n" and b"line 9: an unknown keyword" in err, err[-300:])
    # a pty with the modes of D21 4.2
    rc, out, err = client("-k", kh, "-i", key, "-t", "127.0.0.1", "echo T=$TERM; stty -a")
    if b"pty-req refused" in err:
        print("SKIPPED ssh-test: the namespace gives sshd no pseudo-terminal (pty-req refused); the pty request "
              "itself was answered")
    else:
        o = out.replace(b"\r", b"")
        result("a pty: TERM xterm-16color, 32 rows 80 columns, erase ^?, intr ^C, iutf8",
               rc == 0 and b"T=xterm-16color" in o and b"rows 32" in o and b"columns 80" in o and
               b"erase = ^?" in o and b"intr = ^C" in o and re.search(rb"(^|\s)iutf8", o) is not None, o[-500:])
    # big transfers both ways under the server's RekeyLimit (1 MB): rekeying in the middle, byte for byte
    d.stop()
    d = Sshd("rekey", extra="RekeyLimit 1M")
    rc, out, err = client("-k", kh, "-i", key, "127.0.0.1", "head -c 3000000 /dev/zero | tr '\\0' z", timeout=180)
    result("3 MB down through server rekeys (RekeyLimit 1M)", rc == 0 and out == b"z" * 3000000 and kexes(err) >= 3,
           (rc, len(out), kexes(err), err[-300:]))
    blob = os.urandom(2000000)
    rc, out, err = client("-k", kh, "-i", key, "127.0.0.1", "sha256sum", stdin=blob, timeout=180)
    result("2 MB up through server rekeys, checked by the server's sha256sum",
           rc == 0 and out.split()[:1] == [hashlib.sha256(blob).hexdigest().encode()] and kexes(err) >= 2,
           (rc, out[:80], kexes(err), err[-300:]))
    d.stop()
    d = Sshd("plain")
    rc, out, err = client("-k", kh, "-i", key, "-r", "300000", "127.0.0.1", "head -c 1500000 /dev/zero | tr '\\0' y",
                          timeout=180)
    result("the client's own rekeying (every 300 KB)", rc == 0 and out == b"y" * 1500000 and kexes(err) >= 4,
           (rc, len(out), kexes(err), err[-300:]))
    d.stop()
    # keepalive@openssh.com: sshd drops a client that does not answer within 2 s
    d = Sshd("alive", extra="ClientAliveInterval 1\nClientAliveCountMax 2")
    rc, out, err = client("-k", kh, "-i", key, "127.0.0.1", "sleep 5; echo alive", timeout=60)
    result("ClientAliveInterval: the keepalive requests are answered (5 s session)", rc == 0 and out == b"alive\n",
           (rc, out, err[-300:]))
    # strict key exchange: a server without it is refused; a message slipped in before or during the first
    # exchange ends the connection (Terrapin)
    for mode, want in (("nostrict", b"no strict key exchange"), ("ignore-first", b"first packet is not KEXINIT"),
                       ("ignore-during", b"unexpected message during the first key exchange")):
        t = proxy(mode, PORT + 1)
        rc, out, err = client("-k", kh, "-i", key, "127.0.0.1", "echo NO", port=PORT + 1, timeout=30)
        t.join(5)
        result("strict kex: %s refused" % mode, rc == 255 and want in err and out == b"", err[-300:])
    d.stop()
    # keyboard-interactive through PAM
    d = Sshd("pam", extra="UsePAM yes\nPasswordAuthentication no")
    rc, out, err = client("-k", kh, "-m", "i", "-P", PW, "127.0.0.1", "echo KBD")
    if rc != 0 and b"offers" in err and b"keyboard-interactive" not in err:
        print("SKIPPED ssh-test: sshd with PAM offers no keyboard-interactive here: %s" % err[-200:])
    else:
        result("keyboard-interactive (PAM's password prompt)", rc == 0 and out == b"KBD\n" and b"prompt: Password" in err,
               (rc, err[-400:]))
    d.stop()
    # a changed host key is refused, naming the line; a revoked key is refused
    d = Sshd("changed", hostkey="hostkey2")
    rc, out, err = client("-k", kh, "-y", "-i", key, "127.0.0.1", "echo NO")
    result("a changed host key is refused (even with -y), the known_hosts line named",
           rc == 255 and b"HOST KEY CHANGED" in err and (kh + " line 1").encode() in err and out == b"", err[-300:])
    d.stop()
    d = Sshd("revoked")
    rev = os.path.join(T, "kh-revoked")
    open(rev, "w").write("@revoked * ssh-ed25519 %s\n[127.0.0.1]:2222 ssh-ed25519 %s\n" % (hostpub, hostpub))
    rc, out, err = client("-k", rev, "-i", key, "127.0.0.1", "echo NO")
    result("a @revoked host key is refused", rc == 255 and b"HOST KEY REVOKED" in err, err[-300:])
    hashed = os.path.join(T, "kh-hashed")
    shutil.copy(kh, hashed)
    run("ssh-keygen", "-H", "-f", hashed)
    rc, out, err = client("-k", hashed, "-i", key, "127.0.0.1", "echo HASHED")
    result("a hashed known_hosts line (ssh-keygen -H) is found", rc == 0 and out == b"HASHED\n" and
           b"trusted" not in err and open(hashed).read().startswith("|1|"), err[-300:])
    d.stop()
    if shutil.which("dropbear"):
        print("SKIPPED ssh-test: Dropbear's server is installed but not yet driven by this test")
    else:
        print("SKIPPED ssh-test: no Dropbear server (dropbear) on this PC: the second interop target of D21 11.1")
    print("ssh-test: %d failed" % fails)
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
