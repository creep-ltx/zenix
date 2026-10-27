#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""zenix-slirp alone (docs/d19-network.md 5.3, 8.1): this script plays the Amiga on a socketpair, with SLIP frames
it builds itself. Run by tools/host/net-test.sh inside a user and network namespace (unshare -rnm: no root on the
PC, no traffic leaves it) with /etc/resolv.conf naming 127.0.0.1, where this script's resolver listens.
    zenix-slirp-test.py ZENIX-SLIRP LOGDIR
Checks: console text before and after frames reaches the log; a damaged frame is dropped and counted; UDP to an
echo server on the PC (10.0.2.2); ICMP echo to the gateway; DNS through slirp's 10.0.2.3; a TCP connection to an
echo server on the PC (handshake, data, its echo); the counts zenix-slirp prints at the end."""
import os, socket, struct, subprocess, sys, threading, time

zs, logdir = sys.argv[1], sys.argv[2]
fails = 0
AMIGA, GW, DNS = "10.0.2.15", "10.0.2.2", "10.0.2.3"

def ok(cond, what):
    global fails
    print(("ok " if cond else "FAIL zenix-slirp-test: ") + what, flush=True)
    if not cond:
        fails += 1

def csum(b):
    if len(b) % 2:
        b += b"\0"
    s = sum(struct.unpack("!%dH" % (len(b) // 2), b))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return ~s & 0xFFFF

def ip4(proto, src, dst, payload, ident=1):
    h = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(payload), ident, 0, 64, proto, 0,
                    socket.inet_aton(src), socket.inet_aton(dst))
    h = h[:10] + struct.pack("!H", csum(h)) + h[12:]
    return h + payload

def udp(src, sport, dst, dport, data):
    u = struct.pack("!HHHH", sport, dport, 8 + len(data), 0) + data
    ph = socket.inet_aton(src) + socket.inet_aton(dst) + struct.pack("!BBH", 0, 17, len(u))
    c = csum(ph + u) or 0xFFFF
    return ip4(17, src, dst, u[:6] + struct.pack("!H", c) + u[8:])

def tcp(src, sport, dst, dport, seq, ack, flags, data=b""):
    t = struct.pack("!HHIIBBHHH", sport, dport, seq, ack, 5 << 4, flags, 8192, 0, 0) + data
    ph = socket.inet_aton(src) + socket.inet_aton(dst) + struct.pack("!BBH", 0, 6, len(t))
    return ip4(6, src, dst, t[:16] + struct.pack("!H", csum(ph + t)) + t[18:])

def slip(pkt):
    return b"\xc0" + pkt.replace(b"\xdb", b"\xdb\xdd").replace(b"\xc0", b"\xdb\xdc") + b"\xc0"

class Line:
    """the Amiga's end: frames out, frames in (SLIP decoded)"""
    def __init__(self, s):
        self.s, self.buf = s, b""
    def send(self, raw):
        self.s.sendall(raw)
    def frames(self, timeout):
        end = time.time() + timeout
        out = []
        while time.time() < end:
            self.s.settimeout(max(0.01, end - time.time()))
            try:
                d = self.s.recv(65536)
            except socket.timeout:
                break
            if not d:
                break
            self.buf += d
            while b"\xc0" in self.buf:
                f, self.buf = self.buf.split(b"\xc0", 1)
                if f:
                    out.append(f.replace(b"\xdb\xdc", b"\xc0").replace(b"\xdb\xdd", b"\xdb"))
            if out:
                break
        return out
    def wait(self, pred, timeout=5.0):
        end = time.time() + timeout
        while time.time() < end:
            for f in self.frames(end - time.time()):
                if pred(f):
                    return f
        return None

def udp_echo(port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", port))
    def run():
        while True:
            d, a = s.recvfrom(2048)
            s.sendto(d, a)
    threading.Thread(target=run, daemon=True).start()

def tcp_echo(port):
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", port))
    s.listen(4)
    def serve(c):
        while True:
            d = c.recv(4096)
            if not d:
                c.close()
                return
            c.sendall(d)
    def run():
        while True:
            c, _ = s.accept()
            threading.Thread(target=serve, args=(c,), daemon=True).start()
    threading.Thread(target=run, daemon=True).start()

def resolver(names):
    """a tiny DNS server on 127.0.0.1:53: A records for the names given, NXDOMAIN for the rest"""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 53))
    def run():
        while True:
            q, a = s.recvfrom(512)
            qid, flags, qd = struct.unpack("!HHH", q[:6])
            i, labels = 12, []
            while q[i]:
                labels.append(q[i + 1:i + 1 + q[i]].decode())
                i += 1 + q[i]
            question = q[12:i + 5]
            name = ".".join(labels).lower()
            if name in names:
                ans = b"\xc0\x0c" + struct.pack("!HHIH", 1, 1, 60, 4) + socket.inet_aton(names[name])
                r = struct.pack("!HHHHHH", qid, 0x8180, 1, 1, 0, 0) + question + ans
            else:
                r = struct.pack("!HHHHHH", qid, 0x8183, 1, 0, 0, 0) + question
            s.sendto(r, a)
    threading.Thread(target=run, daemon=True).start()

def dnsq(qid, name):
    q = b"".join(bytes([len(p)]) + p.encode() for p in name.split(".")) + b"\0"
    return struct.pack("!HHHHHH", qid, 0x0100, 1, 0, 0, 0) + q + struct.pack("!HH", 1, 1)

def main():
    udp_echo(7777)
    tcp_echo(7778)
    resolver({"amiga.zenix.test": "10.1.2.3"})
    a, b = socket.socketpair()
    log = os.path.join(logdir, "zenix-slirp-test.console")
    err = open(os.path.join(logdir, "zenix-slirp-test.stderr"), "w")
    if os.path.exists(log):
        os.unlink(log)
    p = subprocess.Popen([zs] + (["-v"] if os.environ.get("ZS_V") else []) +
                         ["-f", "tcp:2323:23", "--fd", str(b.fileno()), "--log", log], pass_fds=[b.fileno()], stderr=err)
    b.close()
    line = Line(a)
    # a frame at once, before libslirp's first turn of its loop: its socket must not expire on the spot (a bug once:
    # libslirp's clock was still 0 when the first frame opened a socket)
    line.send(slip(udp(AMIGA, 39999, GW, 7777, b"first frame")))
    f = line.wait(lambda f: len(f) > 28 and f[9] == 17 and f[28:] == b"first frame")
    ok(f is not None, "UDP echo for the very first frame after the start")
    time.sleep(0.3)

    # console text (before the network starts, and between frames), without a line end at the end (a prompt)
    line.send(b"zenix: kernel line one\r\nlogin: ")
    time.sleep(0.4)
    # a damaged frame (binary, not IPv4) between two ENDs
    line.send(b"\xc0\x01\x02\x03\x99\x88\x00\x00\x10\xc0")

    # UDP to the PC's echo server through the gateway address
    line.send(slip(udp(AMIGA, 40000, GW, 7777, b"hello over slip")))
    f = line.wait(lambda f: len(f) > 28 and f[9] == 17 and f[28:] == b"hello over slip")
    ok(f is not None and socket.inet_ntoa(f[12:16]) == GW and struct.unpack("!H", f[20:22])[0] == 7777
       and struct.unpack("!H", f[22:24])[0] == 40000, "UDP echo through 10.0.2.2")

    # ICMP echo to the gateway
    icmp = struct.pack("!BBHHH", 8, 0, 0, 0x1234, 1) + b"ping-data"
    icmp = icmp[:2] + struct.pack("!H", csum(icmp)) + icmp[4:]
    line.send(slip(ip4(1, AMIGA, GW, icmp)))
    f = line.wait(lambda f: len(f) >= 28 and f[9] == 1 and f[20] == 0)
    ok(f is not None and f[28:] == b"ping-data" and struct.unpack("!H", f[24:26])[0] == 0x1234, "ICMP echo from 10.0.2.2")

    # DNS through slirp's server, which asks this namespace's resolv.conf (127.0.0.1: the resolver above)
    line.send(slip(udp(AMIGA, 40001, DNS, 53, dnsq(0x4242, "amiga.zenix.test"))))
    f = line.wait(lambda f: len(f) > 40 and f[9] == 17 and struct.unpack("!H", f[20:22])[0] == 53)
    ok(f is not None and socket.inet_ntoa(f[12:16]) == DNS and f[28:30] == b"\x42\x42"
       and f[-4:] == socket.inet_aton("10.1.2.3"), "DNS answer through 10.0.2.3 from the test resolver")
    line.send(slip(udp(AMIGA, 40002, DNS, 53, dnsq(0x4343, "nothing.zenix.test"))))
    f = line.wait(lambda f: len(f) > 40 and f[9] == 17 and f[28:30] == b"\x43\x43")
    ok(f is not None and (f[31] & 15) == 3, "DNS: a missing name is NXDOMAIN")

    # TCP: handshake, data, the echo, the end
    isn = 1000
    line.send(slip(tcp(AMIGA, 41000, GW, 7778, isn, 0, 0x02)))
    f = line.wait(lambda f: f[9] == 6 and (f[33] & 0x12) == 0x12)
    ok(f is not None, "TCP: SYN-ACK from 10.0.2.2:7778")
    if f is not None:
        srv = struct.unpack("!I", f[24:28])[0]
        line.send(slip(tcp(AMIGA, 41000, GW, 7778, isn + 1, srv + 1, 0x10)))
        line.send(slip(tcp(AMIGA, 41000, GW, 7778, isn + 1, srv + 1, 0x18, b"tcp through slirp")))
        f = line.wait(lambda f: f[9] == 6 and f[40:] == b"tcp through slirp")
        ok(f is not None, "TCP: the echo server's answer")
        line.send(slip(tcp(AMIGA, 41000, GW, 7778, isn + 18, srv + 18, 0x14)))   # RST: done

    # a port forward: a program on the PC connects to 127.0.0.1:2323, the Amiga sees a SYN for its port 23
    c = socket.socket()
    c.settimeout(5)
    threading.Thread(target=lambda: c.connect_ex(("127.0.0.1", 2323)), daemon=True).start()
    f = line.wait(lambda f: f[9] == 6 and (f[33] & 0x12) == 0x02 and struct.unpack("!H", f[22:24])[0] == 23)
    ok(f is not None and socket.inet_ntoa(f[16:20]) == AMIGA, "-f tcp:2323:23: a connection to 127.0.0.1:2323 reaches the Amiga's port 23")
    c.close()

    # console text after the network (netd closed the port): after a pause, without an END
    time.sleep(0.2)
    line.send(b"zenix: kernel line two\r\n")
    time.sleep(1.0)
    a.close()
    p.wait(timeout=10)
    err.close()
    con = open(log, "rb").read()
    ok(b"kernel line one" in con and b"login: " in con, "console text between frames reaches the log")
    ok(b"kernel line two" in con, "console text after the frames (no END) reaches the log after a pause")
    ok(b"hello over slip" not in con and b"\x99\x88" not in con, "no frame and no damaged frame in the console log")
    st = open(os.path.join(logdir, "zenix-slirp-test.stderr")).read()
    print(st.strip().splitlines()[-1] if st.strip() else "(no output)")
    ok("damaged 1," in st, "the damaged frame was counted")
    ok(p.returncode == 0, "zenix-slirp ended cleanly when the line closed")
    print("zenix-slirp-test: %s" % ("all ok" if not fails else "%d FAILED" % fails))
    sys.exit(1 if fails else 0)

main()
