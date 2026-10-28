#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""wasabid's conformance client (DESIGN D24 12.1), written from Wasabi's PROTOCOL.md: the core (net/wasabi/) on the PC
through tools/host/wasabi-host.c, every command Zenix offers, as the real wasabi client sends them (64 KB DATA frames,
the flags it sets), and the rules of D24 3.2, 5 and 9: wrong keys and the pause, short and oversized frames, unknown
tags, a transfer that stops moving, one connection over the cap, the address filter, discovery.
    wasabi-check.py HOST_BINARY WORKDIR PORT_OFFSET [WASABI_CHECKOUT]
Prints PASS/FAIL lines; exit status 0 only when every check passed. Standard library only. Its frames and checks are
also tools/host/wasabi-remote.py's (the same against Zenix in FS-UAE, tools/remote-test.sh)."""
import os, socket, stat, struct, subprocess, sys, time

if __name__ == "__main__":
    HOSTBIN, WORK, OFF = sys.argv[1], os.path.abspath(sys.argv[2]), int(sys.argv[3])
    REAL = sys.argv[4] if len(sys.argv) > 4 else ""
else:                                           # imported by wasabi-remote.py, which sets OFF (the forward's port)
    HOSTBIN, WORK, OFF, REAL = "", "", 0, ""
KEY = "k3y-for-the-host-test-0123"
CAPS = ("ping,info,ls,put,get,run,del,mkdir,debug,reboot,rebootto,restart,ps,kill,speed,speedfile,quit,grab,"
        "screen,windows,hb,key,health,detach,slots,sum,inputs,zenix,text")
SCREEN = {"grab", "screen", "windows", "key", "sum", "inputs", "text"}
HELLO, WELCOME, ERR, OK, PING, PONG = 1, 2, 3, 4, 5, 6
PUT, GET, DATA, END, LS, DEL, MKDIR = 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16
RUN, STDOUT, STDERR, EXIT = 0x20, 0x21, 0x22, 0x23
DEBUG, SNOOP, LOG = 0x30, 0x31, 0x32
REBOOT, INFO, RESTART, PS, KILL, SPEED, QUIT = 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46
GRAB, SCREENT, KEYT, WINDOWS, HEALTH, SLOTS, FREE, SLOT, SUM, INPUTS, TEXT = (0x48, 0x49, 0x4B, 0x4C, 0x4D, 0x50,
                                                                           0x51, 0x52, 0x57, 0x58, 0x59)
fails = 0
passes = 0


def check(ok, what):
    global fails, passes
    if ok:
        passes += 1
    else:
        fails += 1
        print("FAIL wasabi-check: " + what, flush=True)


def S(s):
    b = s.encode("latin-1") if isinstance(s, str) else s
    return struct.pack(">H", len(b)) + b


def errtext(p):
    code, = struct.unpack_from(">I", p, 0)
    n, = struct.unpack_from(">H", p, 4)
    return code, p[6:6 + n].decode("latin-1")


class Conn:
    def __init__(self, port=1236, key=KEY, hello=True, timeout=8):
        self.s = socket.create_connection(("127.0.0.1", port + OFF), timeout=timeout)
        self.buf = b""
        if hello:
            self.send(HELLO, struct.pack(">H", 1) + S(key))
            t, p = self.recv()
            if t != WELCOME:
                raise RuntimeError("no WELCOME: %r %r" % (t, p))
            self.welcome = p

    def send(self, tag, payload=b""):
        self.s.sendall(struct.pack(">BI", tag, len(payload)) + payload)

    def recv(self):
        while len(self.buf) < 5:
            c = self.s.recv(65536)
            if not c:
                return None, None
            self.buf += c
        t, n = struct.unpack(">BI", self.buf[:5])
        while len(self.buf) < 5 + n:
            c = self.s.recv(65536)
            if not c:
                return None, None
            self.buf += c
        p = self.buf[5:5 + n]
        self.buf = self.buf[5 + n:]
        return t, p

    def closed(self, wait=3):
        """True when the daemon closes the connection (nothing more comes) within wait seconds."""
        self.s.settimeout(wait)
        try:
            while True:
                c = self.s.recv(65536)
                if not c:
                    return True
        except (socket.timeout, TimeoutError):
            return False
        except OSError:
            return True

    def data(self, tag, payload=b""):
        """A command answered by DATA frames and END: (bytes, None) or (bytes so far, ERR words)."""
        self.send(tag, payload)
        out = []
        while True:
            t, p = self.recv()
            if t == DATA:
                out.append(p)
            elif t == END:
                return b"".join(out), None
            elif t == ERR:
                return b"".join(out), errtext(p)
            else:
                return b"".join(out), ("unexpected", t)

    def simple(self, tag, payload=b""):
        self.send(tag, payload)
        return self.recv()

    def close(self):
        self.s.close()


def run(c, cmd, flags=0):
    """RUN: (stdout, stderr, rc, slot) as the client collects them."""
    c.send(RUN, struct.pack(">I", flags) + S(cmd))
    out, err, slot = b"", b"", None
    while True:
        t, p = c.recv()
        if t == STDOUT:
            out += p
        elif t == STDERR:
            err += p
        elif t == SLOT:
            slot, = struct.unpack(">I", p)
        elif t == EXIT:
            return out, err, struct.unpack(">II", p)[0], slot
        elif t == ERR:
            return out, err, ("ERR",) + errtext(p), slot
        else:
            return out, err, ("tag", t), slot


def put(c, path, data, prot=0xFFFFFFFF, cut=False):
    c.send(PUT, struct.pack(">II", len(data), prot) + S(path))
    for i in range(0, len(data), 65536):
        if cut and i >= len(data) // 2:
            c.close()
            return None
        c.send(DATA, data[i:i + 65536])
    c.send(END)
    t, p = c.recv()
    return "OK" if t == OK else errtext(p) if t == ERR else t


# ---- the pretend Zenix root ----
ROOT = os.path.join(WORK, "root")
procs = []


def setup(conf="key %s\nname testbox\n" % KEY, mode=0o600):
    subprocess.run(["rm", "-rf", ROOT])
    for d in ("etc", "var/lib", "tmp", "bin", "sbin", "dev", "nas", "data/sub", "data/empty"):
        os.makedirs(os.path.join(ROOT, d), exist_ok=True)
    with open(os.path.join(ROOT, "etc/wasabid.conf"), "w") as f:
        f.write(conf)
    os.chmod(os.path.join(ROOT, "etc/wasabid.conf"), mode)
    with open(os.path.join(ROOT, "sbin/wasabid"), "wb") as f:
        f.write(b"ZEXE....")
    os.mkfifo(os.path.join(ROOT, "dev/zero"))
    with open(os.path.join(ROOT, "data/a.txt"), "w") as f:
        f.write("hello\n")
    os.symlink("a.txt", os.path.join(ROOT, "data/link"))
    with open(os.path.join(ROOT, "data/sub/x"), "w") as f:
        f.write("x")
    with open(os.path.join(ROOT, "bin/reboot"), "w") as f:      # the pretend reboot: says why it came back
        f.write("#!/bin/sh\necho \"$@\" > .reboot-args\necho 'reboot: no room for the boot note' >&2\nexit 1\n")
    os.chmod(os.path.join(ROOT, "bin/reboot"), 0o755)
    with open(os.path.join(ROOT, ".klog"), "w") as f:
        f.write("cpu: 68030 (attnflags 7)\npi: not running under Emu68 (no device tree)\n"
                "klog: boot 4, log kept at 0x1fbe00 (top of chip RAM) for the next boot\n"
                "=== previous boot (3) ended here with a kernel panic; its last lines (all of it: dmesg -p): ===\n"
                "> fsd: started\n> panic: the test's own\n=== end of the previous boot's log ===\n")


def start(*args, offset_extra=0, env=None):
    log = open(os.path.join(WORK, "daemon-%d.log" % len(procs)), "w")
    p = subprocess.Popen([HOSTBIN, "--root", ROOT, "--offset", str(OFF + offset_extra)] + list(args),
                         stdout=log, stderr=subprocess.STDOUT, env=env)
    procs.append(p)
    for _ in range(100):                        # until it listens (or ends)
        if p.poll() is not None:
            return p
        try:
            socket.create_connection(("127.0.0.1", 1236 + OFF + offset_extra), timeout=1).close()
            return p
        except OSError:
            time.sleep(0.05)
    return p


def stop(p):
    if p.poll() is None:
        p.terminate()
        try:
            p.wait(5)
        except subprocess.TimeoutExpired:
            p.kill()


def rootfile(name):
    return os.path.join(ROOT, name.lstrip("/"))


# ---- the checks ----
def t_hello(d):
    c = Conn()
    v, = struct.unpack_from(">H", c.welcome, 0)
    n, = struct.unpack_from(">H", c.welcome, 2)
    banner = c.welcome[4:4 + n].decode()
    m, = struct.unpack_from(">H", c.welcome, 4 + n)
    caps = c.welcome[6 + n:6 + n + m].decode()
    refused, = struct.unpack_from(">I", c.welcome, 6 + n + m)
    check(v == 1 and banner == "Zenix 0.3.1", "WELCOME: version 1, banner 'Zenix 0.3.1' (%d %r)" % (v, banner))
    check(caps == CAPS, "WELCOME: the exact caps of D24 4 (%s)" % caps)
    check(len(c.welcome) == 10 + n + m and refused == 0, "WELCOME: refused 0 at the end")
    check(c.simple(PING)[0] == PONG, "PING -> PONG")
    t, p = c.simple(SNOOP, struct.pack(">I", 0) + S(""))
    check(t == ERR and errtext(p)[1] == "not on Zenix", "SNOOP: ERR 'not on Zenix'")
    t, p = c.simple(0x7E)
    check(t == ERR and errtext(p)[1] == "not on Zenix", "an unknown tag: ERR 'not on Zenix'")
    check(c.simple(PING)[0] == PONG, "the connection goes on after an unknown tag")
    c.close()
    # the first byte 0x16 (a TLS ClientHello): closed at once, nothing said
    c = Conn(hello=False)
    c.s.sendall(b"\x16\x03\x01\x00\x05hello")
    check(c.closed() and not c.buf, "a TLS ClientHello is closed at once")
    c = Conn(hello=False)
    t, p = c.simple(PING)
    check(t == ERR and errtext(p)[1] == "expected HELLO" and c.closed(), "a command before HELLO: ERR, closed")
    c = Conn(hello=False)
    t, p = c.simple(HELLO, struct.pack(">H", 2) + S(KEY))
    check(t == ERR and "version" in errtext(p)[1] and c.closed(), "HELLO version 2: ERR, closed")
    # a wrong key, then the pause
    c = Conn(hello=False)
    t, p = c.simple(HELLO, struct.pack(">H", 1) + S("wrong-key-0123456789"))
    check(t == ERR and errtext(p) == (0, "bad key") and c.closed(), "a wrong key: ERR 'bad key', closed")
    t0 = time.time()
    c = Conn()
    dt = time.time() - t0
    check(0.9 <= dt < 3, "the next HELLO from that address waits about 1 s (%.2f s)" % dt)
    n, = struct.unpack_from(">H", c.welcome, 2)
    m, = struct.unpack_from(">H", c.welcome, 4 + n)
    refused, = struct.unpack_from(">I", c.welcome, 6 + n + m)
    check(refused == 1, "the wrong key is counted in WELCOME's refused (%d)" % refused)
    c.close()
    t0 = time.time()
    Conn().close()
    check(time.time() - t0 < 0.5, "after the right key no more pause")
    # frames too long
    c = Conn()
    c.s.sendall(struct.pack(">BI", INFO, 70000))
    t, p = c.recv()
    check(t == ERR and c.closed(), "a length over 64 KB: ERR, closed")
    c = Conn()
    c.s.sendall(struct.pack(">BI", RUN, 5000) + b"\0" * 5000)
    t, p = c.recv()
    check(t == ERR and c.closed(), "a command frame over 4 KB: ERR, closed")


def t_files(d):
    c = Conn()
    big = os.urandom(100 * 1024 + 17)
    check(put(c, "/data/big.bin", big) == "OK", "PUT 100 KB in 64 KB DATA frames")
    c.close()
    check(open(rootfile("/data/big.bin"), "rb").read() == big, "PUT: the file's bytes")
    check(stat.S_IMODE(os.stat(rootfile("/data/big.bin")).st_mode) == 0o644, "PUT: a new file is 0644")
    c = Conn()
    got, e = c.data(GET, S("/data/big.bin"))
    check(e is None and got == big, "GET: the same 100 KB back")
    c.close()
    leftovers = [n for n in os.listdir(rootfile("/data")) if n.startswith(".wsb-")]
    check(not leftovers, "no temporary .wsb- file left (%s)" % leftovers)
    c = Conn()
    check(put(c, "/bin/prog", b"ZEXE" + b"\0" * 60) == "OK", "PUT of a Zenix program")
    check(stat.S_IMODE(os.stat(rootfile("/bin/prog")).st_mode) == 0o755, "PUT: a new Zenix program is 0755")
    os.chmod(rootfile("/data/a.txt"), 0o600)
    check(put(c, "/data/a.txt", b"new\n") == "OK", "PUT over an existing file")
    check(stat.S_IMODE(os.stat(rootfile("/data/a.txt")).st_mode) == 0o600, "PUT: the old file's mode is kept")
    check(put(c, "/data/m", b"m", prot=0x80000000 | 0o750) == "OK" and
          stat.S_IMODE(os.stat(rootfile("/data/m")).st_mode) == 0o750, "PUT with bit 31: a Unix mode (caps zenix)")
    check(put(c, "/data/am", b"m", prot=0) == "OK" and
          stat.S_IMODE(os.stat(rootfile("/data/am")).st_mode) == 0o755, "PUT with AmigaDOS bits rwed: 0755")
    r = put(c, "/sbin/wasabid", b"evil")
    check(isinstance(r, tuple) and "running daemon" in r[1] and
          open(rootfile("/sbin/wasabid"), "rb").read() == b"ZEXE....", "PUT of /sbin/wasabid refused (%r)" % (r,))
    r = put(c, "/nas/x", b"x")
    check(isinstance(r, tuple) and "network mount" in r[1], "PUT on an nfs mount refused (%r)" % (r,))
    r = put(c, "/data/sub", b"x")
    check(isinstance(r, tuple) and r[0] == 212, "PUT over a directory: ERR code 212 (%r)" % (r,))
    c.send(PUT, struct.pack(">II", 10, 0xFFFFFFFF) + S("/data/short"))
    c.send(DATA, b"12345")
    c.send(END)
    t, p = c.recv()
    check(t == ERR and errtext(p)[1] == "size mismatch" and not os.path.exists(rootfile("/data/short")),
          "PUT short of its size: ERR 'size mismatch', no file")
    r = put(c, "rel/path", b"x")
    check(isinstance(r, tuple) and "starts with /" in r[1], "PUT of a relative path refused")
    r = put(c, "/" + "a" * 70, b"x")
    check(isinstance(r, tuple) and "63" in r[1], "PUT of a path over 63 bytes refused")
    c.close()
    # cut half-way: the old file stays, no temporary left
    with open(rootfile("/data/keep"), "wb") as f:
        f.write(b"old contents")
    c = Conn()
    put(c, "/data/keep", os.urandom(200 * 1024), cut=True)
    time.sleep(0.5)
    check(open(rootfile("/data/keep"), "rb").read() == b"old contents", "a PUT cut half-way leaves the old file")
    check(not [n for n in os.listdir(rootfile("/data")) if n.startswith(".wsb-")], "... and no temporary")
    c = Conn()
    _, e = c.data(GET, S("/dev/zero"))
    check(e is not None and "regular" in e[1], "GET of /dev/zero (not a regular file) refused (%r)" % (e,))
    _, e = c.data(GET, S("/nope"))
    check(e is not None and e[0] == 205, "GET of a missing file: code 205 (%r)" % (e,))
    _, e = c.data(GET, S("/nas/file"))
    check(e is not None and "network mount" in e[1], "GET on an nfs mount refused")
    # LS against the files
    got, e = c.data(LS, S("/data"))
    lines = {ln.split(" ", 6)[6]: ln.split(" ")[:6] for ln in got.decode().splitlines()}
    want = sorted(n for n in os.listdir(rootfile("/data")))
    check(e is None and sorted(lines) == want, "LS: every name (%s vs %s)" % (sorted(lines), want))
    for n in want:
        st = os.stat(os.path.join(rootfile("/data"), n))
        m = st.st_mode
        p = (0 if m & 0o400 else 8) | (0 if m & 0o200 else 5) | (0 if m & 0o100 else 2)
        p |= (m & 0o40 and 1 << 11) | (m & 0o20 and (1 << 10 | 1 << 8)) | (m & 0o10 and 1 << 9)
        p |= (m & 0o4 and 1 << 15) | (m & 0o2 and (1 << 14 | 1 << 12)) | (m & 0o1 and 1 << 13)
        t = max(int(st.st_mtime) - 252460800, 0)
        exp = ["d" if stat.S_ISDIR(m) else "f", str(st.st_size), str(p), str(t // 86400), str(t % 86400 // 60),
               str(t % 60 * 50)]
        if stat.S_ISDIR(m):
            exp[1] = lines.get(n, ["", ""])[1]  # a directory's size is the filesystem's
        check(lines.get(n) == exp, "LS line of %s: %s, want %s" % (n, lines.get(n), exp))
    check(lines.get("link", [""])[1] == str(os.path.getsize(rootfile("/data/a.txt"))), "LS: a link shows its target")
    got, e = c.data(LS, S(""))
    check(e is None and b" data\n" in got, "LS of '' lists /")
    _, e = c.data(LS, S("/data/a.txt"))
    check(e is not None and e[0] == 212, "LS of a file: code 212")
    # DEL, MKDIR and their codes
    check(c.simple(DEL, S("/data/m"))[0] == OK and not os.path.exists(rootfile("/data/m")), "DEL of a file")
    check(c.simple(DEL, S("/data/empty"))[0] == OK and not os.path.exists(rootfile("/data/empty")),
          "DEL of an empty directory")
    t, p = c.simple(DEL, S("/data/sub"))
    check(t == ERR and errtext(p)[0] == 216, "DEL of a full directory: code 216")
    t, p = c.simple(DEL, S("/data/none"))
    check(t == ERR and errtext(p)[0] == 205, "DEL of a missing file: code 205")
    check(c.simple(MKDIR, S("/data/new"))[0] == OK and os.path.isdir(rootfile("/data/new")), "MKDIR")
    t, p = c.simple(MKDIR, S("/data/new"))
    check(t == ERR and errtext(p)[0] == 203, "MKDIR of an existing name: code 203")
    os.chmod(rootfile("/data/new"), 0o500)
    t, p = c.simple(MKDIR, S("/data/new/inside"))
    os.chmod(rootfile("/data/new"), 0o755)
    check(t == ERR and errtext(p)[0] in (223, 0) or os.getuid() == 0, "MKDIR without the right: code 223")
    c.close()


def t_speed(d):
    c = Conn()
    blob = bytes(range(256)) * 800                      # 200 KB
    c.send(SPEED, struct.pack(">II", 0, len(blob)) + S(""))
    for i in range(0, len(blob), 65536):
        c.send(DATA, blob[i:i + 65536])
    c.send(END)
    check(c.recv()[0] == OK, "SPEED to Zenix over the network")
    got, e = c.data(SPEED, struct.pack(">II", 1, 100000) + S(""))
    check(e is None and len(got) == 100000, "SPEED from Zenix: 100000 bytes")
    c.send(SPEED, struct.pack(">II", 0, len(blob)) + S("/tmp"))
    for i in range(0, len(blob), 65536):
        c.send(DATA, blob[i:i + 65536])
    c.send(END)
    check(c.recv()[0] == OK and os.path.getsize(rootfile("/tmp/wasabi-speed.tmp")) == len(blob),
          "SPEED through /tmp: the test file written")
    got, e = c.data(SPEED, struct.pack(">II", 1, len(blob)) + S("/tmp"))
    check(e is None and got == blob and not os.path.exists(rootfile("/tmp/wasabi-speed.tmp")),
          "SPEED's read-back half: the same bytes, the file removed")
    t, p = c.simple(SPEED, struct.pack(">II", 1, 0) + S(""))
    check(t == ERR and "256 MB" in errtext(p)[1], "SPEED of 0 bytes refused")
    c.close()
    p2 = start(offset_extra=100, env=dict(os.environ, WASABI_HOST_FULL="1"))
    c = Conn(port=1236 + 100)
    c.send(SPEED, struct.pack(">II", 0, 200000) + S("/tmp"))
    c.send(DATA, b"x" * 1000)
    c.send(END)
    t, p = c.recv()
    check(t == ERR and "room" in errtext(p)[1], "SPEED through a full disk refused (size plus 64 KB)")
    c.close()
    stop(p2)


def t_run(d):
    c = Conn()
    o, e, rc, slot = run(c, "echo out; echo err >&2; exit 3", 4)
    check((o, e, rc) == (b"out\n", b"err\n", 3) and slot in (1, 2), "RUN: stdout, stderr apart, exit 3, the slot")
    o, e, rc, _ = run(c, "echo out; echo err >&2", 1)
    check(o.count(b"out") == 1 and b"err" in o and e == b"" and rc == 0, "RUN merged: both on STDOUT")
    o, e, rc, _ = run(c, "kill -SEGV $$")
    check(rc == 139, "RUN: a fault is 139 (%r)" % (rc,))
    t0 = time.time()
    o, e, rc, _ = run(c, "sleep 30 > /dev/null & echo started")
    check(o == b"started\n" and rc == 0 and time.time() - t0 < 3, "RUN 'sleep 30 &': EXIT at once")
    o, e, rc, _ = run(c, "x=0; while [ $x -lt 3000 ]; do echo line $x; x=$((x+1)); done")
    check(rc == 0 and len(o.splitlines()) == 3000 and o.splitlines()[-1] == b"line 2999",
          "RUN with 3000 lines of output, every one")
    o, e, rc, _ = run(c, "echo $HOME $USER $TERM $PATH")
    check(o == b"/root root dumb /bin:/sbin\n", "RUN's environment (%r)" % o)
    c.close()
    # the slots: two at once, a third refused
    a, b = Conn(), Conn()
    a.send(RUN, struct.pack(">I", 4) + S("sleep 30"))
    b.send(RUN, struct.pack(">I", 4) + S("sleep 30"))
    sa, sb = a.recv(), b.recv()
    c = Conn()
    _, _, rc, _ = run(c, "echo no")
    check(rc[:1] == ("ERR",) and rc[2] == "all run slots are busy", "a third RUN: 'all run slots are busy'")
    got, e = c.data(SLOTS)
    ls = got.decode().splitlines()
    check(e is None and len(ls) == 2 and all(" running " in x and x.endswith(" 1 sleep 30") for x in ls),
          "SLOTS: the two (%r)" % ls)
    na = struct.unpack(">I", sa[1])[0]
    got, e = c.data(FREE, struct.pack(">II", na, 0))
    check(e is None and got == b"stopped", "FREE: SIGINT, 'stopped' (%r %r)" % (got, e))
    t, p = a.recv()
    check(t == EXIT and struct.unpack(">I", p[:4])[0] == 130, "the freed command's client gets EXIT 130")
    nb = struct.unpack(">I", sb[1])[0]
    got, e = c.data(FREE, struct.pack(">II", nb, 1))
    check(e is None and got == b"asked", "FREE bit 0: 'asked'")
    t, p = b.recv()
    check(t == EXIT, "... and the command ends")
    _, e = c.data(FREE, struct.pack(">II", 2, 0))
    check(e is not None and "no command" in e[1], "FREE of an empty slot: ERR")
    # SIGINT ignored: SIGKILL after 3 s, still 'stopped'
    a = Conn()
    a.send(RUN, struct.pack(">I", 4) + S("trap '' INT; sleep 30"))
    n = struct.unpack(">I", a.recv()[1])[0]
    time.sleep(0.5)                             # the shell's trap is set before the SIGINT
    t0 = time.time()
    got, e = c.data(FREE, struct.pack(">II", n, 0))
    dt = time.time() - t0
    check(e is None and got == b"stopped" and 2.8 < dt < 5, "FREE of a command that ignores SIGINT: SIGKILL after "
          "3 s (%.1f s, %r)" % (dt, got))
    t, p = a.recv()
    check(t == EXIT and struct.unpack(">I", p[:4])[0] == 137, "... EXIT 137")
    # a client that goes away: its command is stopped
    a = Conn()
    a.send(RUN, struct.pack(">I", 0) + S("sleep 30; echo late > .late"))
    time.sleep(0.3)
    a.close()
    time.sleep(0.5)
    got, _ = c.data(SLOTS)
    check(got == b"", "a command whose client went is stopped, its slot free (%r)" % got)
    # detached
    o, e, rc, _ = run(c, "sleep 0.3; echo bg-done", 2)
    check(rc == 0 and e.startswith(b"[started in the background; its output goes to /tmp/wasabi-bg-"),
          "RUN detached: the note, EXIT 0 (%r)" % e)
    name = e.decode().split("goes to ")[1].rstrip("]\n")
    time.sleep(1)
    check(open(rootfile(name)).read() == "bg-done\n", "the detached command's output file")
    got, _ = c.data(SLOTS)
    check(got == b"", "a detached command takes no slot")
    c.close()


def t_procs(d):
    c = Conn()
    got, e = c.data(PS)
    rows = [ln.split(" ", 6) for ln in got.decode().splitlines()]
    names = [r[6].split("\t")[0] for r in rows]
    check(e is None and names[:5] == ["boot", "init", "netd", "fsd", "wasabid"], "PS: the processes (%s)" % names)
    check(all(r[0].startswith("0x") and len(r[0]) == 10 and r[1] == "p" and r[3] in ("run", "ready", "wait", "stop")
              and r[4] == "0" and r[5] == "-1" and r[6].split("\t")[0] == r[6].split("\t")[1] for r in rows),
          "PS: Wasabi's line, the name twice")
    check([r[3] for r in rows if r[6].startswith("wasabid")] == ["run"], "PS: wasabid runs")
    kill = lambda t, f=0: c.simple(KILL, struct.pack(">I", f) + S(t))
    t, p = kill("init")
    check(t == ERR and "never" in errtext(p)[1], "KILL init refused")
    t, p = kill("0x00000001", 1)
    check(t == ERR and "never" in errtext(p)[1], "KILL boot by its number refused, even forced")
    t, p = kill("wasabid")
    check(t == ERR and "itself" in errtext(p)[1], "KILL of the daemon refused")
    t, p = kill("netd")
    check(t == ERR and "--force" in errtext(p)[1], "KILL netd without the flag refused")
    t, p = kill("netd", 1)
    check(t == OK and open(rootfile(".kills")).read() == "3 9\n", "KILL netd --force: SIGKILL")
    t, p = kill("nosuch")
    check(t == ERR and "no process" in errtext(p)[1], "KILL of nothing")
    with open(rootfile(".procs"), "w") as f:
        f.write("40 2 twin\n41 2 twin\n")
    t, p = kill("twin")
    check(t == ERR and "several" in errtext(p)[1], "KILL of a name two processes have")
    t, p = kill("0x00000029")
    check(t == OK and open(rootfile(".kills")).read().endswith("41 2\n"), "KILL by 0x number: SIGINT")
    os.unlink(rootfile(".procs"))
    c.close()


def t_info(d):
    c = Conn()
    got, e = c.data(INFO)
    t = got.decode()
    check(e is None and t.startswith("Zenix 0.3.1 (12) m68k") and "cpu: 68030" in t and "Emu68: no" in t and
          "chip free 1160 KB, fast free 0 KB" in t and "sl0 10.0.2.15 rx 100 tx 200 bad 1" in t and
          "port 1236, name testbox, started by hand" in t and KEY not in t, "INFO's lines (%r)" % t)
    got, e = c.data(HEALTH)
    kv = dict(ln.split(" ", 1) for ln in got.decode().splitlines())
    check(e is None and kv.get("mem.chip.free") == "1187840" and kv.get("mem.fast.free") == "0" and
          kv.get("emu68") == "no" and kv.get("zenix.version") == "0.3.1" and kv.get("net.sl0.rx") == "100" and
          kv.get("net.sl0.bad") == "1" and int(kv.get("procs", 0)) >= 5 and "load.ready" in kv and
          "uptime" in kv and int(kv.get("disk.root.free.kb", -1)) > 0, "HEALTH's keys (%r)" % kv)
    c.close()


def t_debug(d):
    c = Conn()
    c.send(DEBUG, struct.pack(">I", 0))
    seen = []
    for _ in range(3):
        t, p = c.recv()
        st, seq = struct.unpack(">II", p[:8])
        seen.append((t, st, seq, p[10:].decode()))
    check([s[3] for s in seen] == ["=== previous boot (3) ended here with a kernel panic; its last lines (all of it: "
                                   "dmesg -p): ===\n", "> fsd: started\n", "> panic: the test's own\n"] and
          [s[2] for s in seen] == [1, 2, 3] and all(s[1] == 0 and s[0] == LOG for s in seen),
          "DEBUG: the previous boot's panic replayed first (%r)" % seen)
    c.recv()                                    # the "=== end" line
    with open(rootfile(".klog"), "a") as f:
        f.write("mount: the test's line\n")
    t, p = c.recv()
    check(t == LOG and p[10:] == b"mount: the test's line\n", "DEBUG: a new kernel log line (%r)" % p)
    size = os.path.getsize(rootfile(".klog"))
    with open(rootfile(".klog"), "a") as f:
        f.write("lost one\nlost two\nkept\n")
    with open(rootfile(".klog-oldest"), "w") as f:
        f.write(str(size + 18))
    t, p = c.recv()
    check(t == LOG and p[10:] == b"[wasabi: 18 bytes of the kernel log were lost]\n", "a wrapped log: the loss said "
          "(%r)" % p)
    t, p = c.recv()
    check(t == LOG and p[10:] == b"kept\n", "... and the stream goes on from the oldest held")
    t0 = time.time()
    t, p = c.recv()
    check(t == LOG and p[10:] == b"" and 4 < time.time() - t0 < 7, "a heartbeat (an empty LOG) after 5 s")
    c2 = Conn()
    t, p = c2.simple(DEBUG, struct.pack(">I", 0))
    check(t == ERR and "in use" in errtext(p)[1], "a second DEBUG: in use")
    c2.close()
    c.send(SNOOP, struct.pack(">I", 0) + S(""))
    t, p = c.recv()
    check(t == ERR and errtext(p)[1] == "not on Zenix", "SNOOP beside the stream: 'not on Zenix', the stream stays")
    c.close()
    time.sleep(0.2)
    c = Conn()
    c.send(DEBUG, struct.pack(">I", 0))
    check(c.recv()[0] == LOG, "the stream is free again when its client went")
    c.close()


def t_screen(d):
    c = Conn()

    def text(flags=0, con=0, tmo=0, pat=""):
        return c.data(TEXT, struct.pack(">III", flags, con, tmo) + S(pat))

    got, e = text()
    rows, cols, crow, ccol, ch = struct.unpack(">HHHHI", got[:12])
    grid = got[16:]
    check(e is None and (rows, cols) == (32, 80) and len(grid) == 80 * 32 and got[12] == 1 and got[14] == 1,
          "TEXT: the header and a glyph a cell")
    check(grid[:16] == b"Zenix 0.3.1 tty1" and grid[160:167] == b"login: " and (crow, ccol) == (2, 7),
          "TEXT: the console's text and cursor")
    check(c.simple(KEYT, struct.pack(">H", 1) + b"echo hello\n")[0] == OK, "KEY text")
    got, e = text(0, 0, 5, "^hello$")
    check(e is None and b"hello" + b" " * 75 in got[16:], "TEXT waiting for a pattern that is there")
    t0 = time.time()
    with open(rootfile(".conwrite.tmp"), "w") as f:
        f.write("\nlater\n")
    c2 = Conn()
    c2.send(RUN, struct.pack(">I", 0) + S("sleep 0.6; mv .conwrite.tmp .conwrite"))
    got, e = text(0, 1, 5, "^lat[e]r$")
    dt = time.time() - t0
    check(e is None and 0.5 < dt < 3, "TEXT waits until the pattern appears (%.2f s)" % dt)
    c2.close()
    t0 = time.time()
    got, e = text(0, 0, 1, "never there")
    check(e is not None and e[1] == "timeout" and len(got) == 16 + 2560 and 0.9 < time.time() - t0 < 3,
          "TEXT's timeout: the grid, then ERR 'timeout'")
    _, e = text(0, 5)
    check(e is not None and e[0] == 0 and "never used" in e[1], "TEXT of a console never used: ERR (-ENXIO)")
    _, e = text(0, 0, 0, "(")
    check(e is not None and "regular expression" in e[1], "TEXT with a bad pattern")
    cells, e = text(1)
    check(e is None and len(cells) == 16 + 3 * 2560, "TEXT with the cells: 3 bytes each")
    cl = cells[16:]
    # GRAB: the picture from the pretend font, pixel by pixel
    got, e = c.data(GRAB, S(""))
    w, h, bpp = struct.unpack(">III", got[:12])
    pal = [(i * 32 & 255, (255 - i * 32) & 255, i * 85 & 255) for i in range(16)]

    def pen(x, ln):
        g = cl[3 * ((ln // 8) * 80 + x // 8):]
        return g[2] & 7 if ((g[0] + ln) >> (x & 7)) & 1 else 0
    want = bytearray()
    for ln in range(256):
        for x in range(640):
            want += bytes(pal[pen(x, ln)])
    check(e is None and (w, h, bpp) == (640, 256, 3) and got[12:] == bytes(want), "GRAB: every pixel")
    # GRAB's note (flags bit 0, Wasabi 0.11b3): a STDERR frame among the DATA only when asked, the picture still whole
    def grab_frames(payload):
        c.send(GRAB, payload)
        tags, size = [], 0
        while True:
            t, p = c.recv()
            tags.append(t)
            if t == DATA:
                size += len(p)
            if t in (END, ERR, None):
                return tags, size, p
    open(rootfile(".moving"), "w").close()
    tags, size, _ = grab_frames(S("") + struct.pack(">I", 1))
    check(tags.count(STDERR) == 1 and tags[-1] == END and size == 12 + 640 * 256 * 3,
          "GRAB with flags bit 0 while the display changes: one STDERR note, the whole picture")
    tags, size, _ = grab_frames(S("") + struct.pack(">I", 0))
    check(STDERR not in tags and tags[-1] == END and size == 12 + 640 * 256 * 3,
          "GRAB without bit 0 while the display changes: no note frame (it is only logged)")
    tags, size, _ = grab_frames(S(""))
    check(STDERR not in tags and tags[-1] == END, "GRAB without a flags word (an older client): no note frame")
    os.unlink(rootfile(".moving"))
    tags, size, _ = grab_frames(S("") + struct.pack(">I", 1))
    check(STDERR not in tags and tags[-1] == END, "GRAB with bit 0 of a quiet display: no note")
    _, e = c.data(GRAB, S("tty2"))
    check(e is not None and "display" in e[1], "GRAB of a console not shown refused")
    got, e = c.data(SUM, struct.pack(">hhhh", 0, 0, 0, 0))
    s = 2166136261
    for i in range(8):
        for k in pal[i]:
            s = ((s ^ k) * 16777619) & 0xFFFFFFFF
    for ln in range(256):
        for x in range(640):
            s = ((s ^ pen(x, ln)) * 16777619) & 0xFFFFFFFF
    check(e is None and struct.unpack(">II", got) == (s, 640 * 256), "SUM of the screen")
    got2, e = c.data(SUM, struct.pack(">hhhh", 8, 16, 100, 8))
    check(e is None and struct.unpack(">II", got2)[1] == 800, "SUM of a region: its pixels")
    _, e = c.data(SUM, struct.pack(">hhhh", 700, 0, 10, 10))
    check(e is not None and "off the screen" in e[1], "SUM off the screen: ERR")
    got, e = c.data(WINDOWS)
    check(e is None and got == b"S\ttty1\t0\t0\t640\t256\t3\ttty1\n", "WINDOWS: one S line (%r)" % got)
    got, e = c.data(SCREENT, struct.pack(">I", 0) + S(""))
    check(e is None and got == b"tty1 640 256 3 tty1\ntty2 640 256 3 tty2\n", "SCREEN: the consoles, front "
          "first (%r)" % got)
    got, e = c.data(SCREENT, struct.pack(">I", 0) + S("tty2"))
    check(e is None and text()[0][14] == 2, "SCREEN tty2: Amiga+F2 brings it to the front")
    keys = open(rootfile(".keys")).read().split()
    check(keys[-4:] == ["66", "51", "d1", "e6"], "... as raw keys (%s)" % keys[-4:])
    got, e = c.data(SCREENT, struct.pack(">I", 0) + S("nosuch"))
    check(e is not None and "no screen" in e[1] and got.startswith(b"tty2 "), "SCREEN of a title not there")
    check(c.simple(KEYT, struct.pack(">HHHHH", 0, 0x66, 0x80, 0x50, 0))[0] == OK, "KEY raw codes")
    check(open(rootfile(".keys")).read().split()[-2:] == ["66", "50"], "... into the keyboard path")
    t, p = c.simple(KEYT, struct.pack(">HHH", 0, 0x100, 0))
    check(t == ERR, "KEY of a code over 255: ERR")
    t, p = c.simple(KEYT, struct.pack(">H", 1) + b"x" * 2049)
    check(t == ERR, "KEY text over 2048 bytes: ERR")
    rec = lambda k, wait, a: struct.pack(">BBHHHhh", k, 0, wait, a, 0, 0, 0)
    t0 = time.time()
    t, p = c.simple(INPUTS, rec(1, 10, ord("a")) + rec(1, 10, ord("b")) + rec(0, 0, 0x66))
    check(t == OK and 0.35 < time.time() - t0 < 2, "INPUTS: keys timed by the daemon (%.2f s)" % (time.time() - t0))
    got, _ = text(0, 1)
    check(b"echo hellab" in got or b"ab" in got[16:], "INPUTS: the characters typed")
    t, p = c.simple(INPUTS, rec(2, 0, 0))
    check(t == ERR and "mouse" in errtext(p)[1], "INPUTS of a mouse event: ERR before anything")
    t, p = c.simple(INPUTS, rec(1, 200, 65) + rec(1, 60, 66))
    check(t == ERR and "250" in errtext(p)[1], "INPUTS waits over 5 s: ERR")
    c.close()


def t_limits(d):
    # the connection cap: four, the fifth 'busy'
    cs = [Conn() for _ in range(4)]
    c5 = Conn(hello=False)
    t, p = c5.recv()
    check(t == ERR and errtext(p)[1] == "busy" and c5.closed(), "a fifth connection: ERR 'busy', closed")
    for c in cs:
        c.close()
    time.sleep(0.2)
    # a transfer that stops moving: dropped after 10 s
    c = Conn()
    c.s.sendall(struct.pack(">BI", INFO, 20)[:3])
    t0 = time.time()
    check(c.closed(14) and 9.5 < time.time() - t0 < 13, "half a frame: dropped after 10 s (%.1f s)" %
          (time.time() - t0))
    c = Conn(hello=False)
    t0 = time.time()
    check(c.closed(14) and 9.5 < time.time() - t0 < 13, "no HELLO: dropped after 10 s")
    # discovery on both ports
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.settimeout(2)
    for port in (1234, 1236):
        u.sendto(b"WASABI?1\n", ("127.0.0.1", port + OFF))
        try:
            r = u.recv(200)
        except socket.timeout:
            r = b""
        check(r == b"WASABI!1 testbox 1236 Zenix 0.3.1\n", "discovery on UDP %d (%r)" % (port, r))
    u.close()


def t_filter(d):
    stop_all()
    # a caller from outside the home network: closed before HELLO, no discovery answer, counted
    setup()
    p = start("--peer", "8.8.8.8", offset_extra=200)
    c = Conn(port=1236 + 200, hello=False)
    c.send(HELLO, struct.pack(">H", 1) + S(KEY))
    check(c.closed() and not c.buf, "a caller outside the ranges: closed, no word")
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.settimeout(1)
    u.sendto(b"WASABI?1\n", ("127.0.0.1", 1236 + 200 + OFF))
    try:
        r = u.recv(200)
    except socket.timeout:
        r = b""
    check(r == b"", "no discovery answer to it")
    stop(p)
    # the first refusal is saved at once, later ones at most once a minute and at a clean end
    saved = open(rootfile("/var/lib/wasabid.refused")).read()
    check(saved in ("1\n", "2\n"), "the refusals counted in /var/lib/wasabid.refused (%r)" % saved)
    with open(rootfile("/etc/wasabid.conf"), "w") as f:
        f.write("key %s\nallow 8.8.8.0/24\n" % KEY)
    p = start("--peer", "8.8.8.8", offset_extra=200)
    c = Conn(port=1236 + 200)
    n, = struct.unpack_from(">H", c.welcome, 2)
    m, = struct.unpack_from(">H", c.welcome, 4 + n)
    check(struct.unpack_from(">I", c.welcome, 6 + n + m)[0] == int(saved), "the next start reads the count back")
    check(c.simple(PING)[0] == PONG, "allow 8.8.8.0/24 lets it in")
    t, q = c.simple(QUIT)
    check(t == OK and p.wait(5) == 0, "QUIT: status 0")
    c.close()
    stop(p)


def t_lifecycle(d):
    stop_all()
    for conf, mode, words in (("key short\n", 0o600, "shorter than 16"), ("key %s\n" % KEY, 0o644, "mode 600"),
                              ("key %s\nfrobnicate 1\n" % KEY, 0o600, "line 2"),
                              ("key %s\nallow 1.2.3.4/33\n" % KEY, 0o600, "CIDR")):
        setup(conf, mode)
        p = start(offset_extra=300)
        rc = p.wait(5)
        log = open(os.path.join(WORK, "daemon-%d.log" % (len(procs) - 1))).read()
        check(rc == 0 and words in log, "a bad configuration (%s): status 0 and a log line (%d %r)" %
              (words, rc, log))
    setup()
    os.unlink(rootfile("/etc/wasabid.conf"))
    p = start(offset_extra=300)
    check(p.wait(5) == 0, "no configuration: status 0")
    setup()
    p = start(offset_extra=300)
    c = Conn(port=1536)
    t, q = c.simple(RESTART)
    check(t == ERR and "not supervised" in errtext(q)[1], "RESTART by hand refused")
    a = Conn(port=1536)
    a.send(RUN, struct.pack(">I", 4) + S("sleep 30"))
    a.recv()
    t, q = c.simple(QUIT)
    check(t == ERR and "still running" in errtext(q)[1], "QUIT with a busy slot refused")
    t0 = time.time()
    t, q = c.simple(QUIT, struct.pack(">I", 1))
    check(t == OK and p.wait(8) == 0 and time.time() - t0 < 6, "QUIT --force: the command stopped, status 0")
    check(a.recv()[0] in (EXIT, None), "... its client got EXIT or the close")
    setup()
    p = start("--init", offset_extra=300)
    c = Conn(port=1536)
    w = Conn(port=1536)
    w.send(DEBUG, struct.pack(">I", 0))
    for _ in range(4):
        w.recv()
    t, q = c.simple(RESTART)
    check(t == OK and p.wait(5) == 75, "RESTART under --init: status 75")
    t, q = w.recv()
    check(t == LOG and q[10:] == b"[wasabi: restarting - closing this stream]\n", "the farewell on the stream")
    # REBOOT: the guard, the farewell, /bin/reboot's answer after OK
    setup()
    p = start(offset_extra=300)
    with open(rootfile(".procs"), "w") as f:
        f.write("50 2 zup\n")
    c = Conn(port=1536)
    t, q = c.simple(REBOOT, struct.pack(">I", 2))
    check(t == ERR and "zup is running (pid 50)" in errtext(q)[1], "REBOOT while zup runs: the guard")
    w = Conn(port=1536)
    w.send(DEBUG, struct.pack(">I", 0))
    for _ in range(4):
        w.recv()
    t, q = c.simple(REBOOT, struct.pack(">I", 2 | 8))
    check(t == OK, "REBOOT with bit 3: OK")
    t, q = w.recv()
    check(t == LOG and b"rebooting - closing this stream" in q and w.closed(), "... the farewell, the stream closed")
    t, q = c.recv()
    check(t == ERR and errtext(q)[1] == "reboot: no room for the boot note" and
          open(rootfile(".reboot-args")).read() == "amigaos\n", "... /bin/reboot amigaos came back: its words as ERR")
    t, q = c.simple(REBOOT, struct.pack(">I", 2 | 4))
    check(t == ERR and "not both" in errtext(q)[1], "REBOOT to both systems refused")
    c.close()
    stop(p)
    # no console operations: caps without the screen commands, which answer ERR
    setup()
    p = start("--noconops", offset_extra=300)
    c = Conn(port=1536)
    n, = struct.unpack_from(">H", c.welcome, 2)
    m, = struct.unpack_from(">H", c.welcome, 4 + n)
    caps = c.welcome[6 + n:6 + n + m].decode()
    check(caps == ",".join(x for x in CAPS.split(",") if x not in SCREEN), "caps without console operations")
    _, e = c.data(TEXT, struct.pack(">III", 0, 0, 0) + S(""))
    check(e is not None and "not in this Zenix yet" in e[1], "TEXT without console operations: ERR")
    c.close()
    stop(p)


def t_real(d):
    """the owner's own wasabi client against the core (plain: loopback)"""
    if not REAL:
        print("wasabi-check: real client skipped (ZENIX_WASABI not set)")
        return
    stop_all()
    setup()
    p = start(offset_extra=400)
    env = dict(os.environ, XDG_CONFIG_HOME=os.path.join(WORK, "xdg"), XDG_CACHE_HOME=os.path.join(WORK, "xdg"),
               HOME=os.path.join(WORK, "xdg"))
    base = [sys.executable, os.path.join(REAL, "wasabi"), "--host", "127.0.0.1", "--port", str(1236 + 400 + OFF),
            "--key", KEY]
    for args, want in ((["ping"], "Zenix 0.3.1"), (["run", "echo from-the-real-client"], "from-the-real-client"),
                       (["ps"], "wasabid"), (["info"], "Zenix 0.3.1")):
        # (health: today's client needs mem.chip.largest, which Zenix has no call for: a D24 section 11 addition)
        r = subprocess.run(base + args, capture_output=True, text=True, env=env, timeout=30)
        check(r.returncode == 0 and want in r.stdout, "the real client: %s (%d %r %r)" %
              (" ".join(args), r.returncode, r.stdout[-200:], r.stderr[-300:]))
    stop(p)


def stop_all():
    for p in procs:
        stop(p)


def main():
    os.makedirs(WORK, exist_ok=True)
    setup()
    start()
    parts = [t_hello, t_files, t_speed, t_run, t_procs, t_info, t_debug, t_screen, t_limits, t_filter, t_lifecycle,
             t_real]
    for t in parts:
        before = fails
        try:
            t(None)
        except Exception as e:                  # a check that crashed is a failure, not a stop
            import traceback
            traceback.print_exc()
            check(False, "%s raised %r" % (t.__name__, e))
        print("wasabi-check: %s %s" % (t.__name__[2:], "PASS" if fails == before else "FAIL"), flush=True)
    stop_all()
    print("wasabi-check: %d checks, %d failed" % (passes + fails, fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
