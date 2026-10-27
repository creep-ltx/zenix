#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The PC's side of the SLIP test (tools/slip-test.sh, DESIGN D19 8.2 "SLIP"), run inside a user and network
namespace (unshare -rnm: no root, nothing leaves the PC) whose /etc/resolv.conf names 127.0.0.1:
    slip-host.py ZENIX-SLIRP SERIAL-LINK CONSOLE-LOG READY-FILE [ZENIX-SLIRP-OPTION...]
It starts the servers the Amiga reaches through zenix-slirp's gateway 10.0.2.2 (which is this namespace's
127.0.0.1): a TCP echo server on port 7100, a UDP echo server on 7101, a TCP source on 7102 (a line "N\\n" asks for
N bytes of a known pattern, then it closes), and a DNS server on 127.0.0.1:53 (slirp's 10.0.2.3 asks it) that knows
amiga.zenix.test = 10.1.2.3 and never answers slow.zenix.test, and a flood control on TCP 7103: a line "flood PORT PORTS N" makes it open N connections,
spread over PORTS ports from PORT on, to this PC's 127.0.0.1 (zenix-slirp's forwards to the Amiga, -f, among the
options: each connection becomes a SYN to the Amiga), hold them 20 s, close them and answer "done N". Then zenix-slirp on a new pty linked at SERIAL-LINK (FS-UAE's serial port), the bytes
that are not SLIP (Zenix's console) appended to CONSOLE-LOG. It writes READY-FILE when all of that is up and runs
until it is killed (zenix-slirp with it). ZENIX-SLIRP "-": the servers alone (SERIAL-LINK and CONSOLE-LOG unused), for
tools/eth-test.sh, where FS-UAE's A2065 and its own slirp reach them."""
import os, signal, socket, struct, subprocess, sys, threading, time

zs, link, log, ready = sys.argv[1:5]
extra = sys.argv[5:]


def pattern(i):
    return ((i * 2654435761 + 11) & 0xFFFFFFFF) >> 13 & 0xFF


def tcp_server(port, handle):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", port))
    s.listen(8)

    def run():
        while True:
            c, _ = s.accept()
            threading.Thread(target=handle, args=(c,), daemon=True).start()
    threading.Thread(target=run, daemon=True).start()


def echo(c):
    with c:
        while True:
            d = c.recv(4096)
            if not d:
                return
            c.sendall(d)


def source(c):
    with c:
        line = b""
        while not line.endswith(b"\n"):
            d = c.recv(64)
            if not d:
                return
            line += d
        n = int(line.strip() or b"0")
        c.sendall(bytes(pattern(i) for i in range(n)))


def udp_echo(port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", port))

    def run():
        while True:
            d, a = s.recvfrom(2048)
            s.sendto(d, a)
    threading.Thread(target=run, daemon=True).start()


def resolver(names):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 53))

    def run():
        while True:
            q, a = s.recvfrom(512)
            qid = struct.unpack("!H", q[:2])[0]
            i, labels = 12, []
            while q[i]:
                labels.append(q[i + 1:i + 1 + q[i]].decode())
                i += 1 + q[i]
            question = q[12:i + 5]
            name = ".".join(labels).lower()
            if name == "slow.zenix.test":
                continue                    # never answered: the Amiga's resolver must give up (no answer)
            if name in names:
                ans = b"\xc0\x0c" + struct.pack("!HHIH", 1, 1, 60, 4) + socket.inet_aton(names[name])
                r = struct.pack("!HHHHHH", qid, 0x8180, 1, 1, 0, 0) + question + ans
            else:
                r = struct.pack("!HHHHHH", qid, 0x8183, 1, 0, 0, 0) + question
            s.sendto(r, a)
    threading.Thread(target=run, daemon=True).start()


def flood(c):
    with c:
        line = b""
        while not line.endswith(b"\n"):
            d = c.recv(64)
            if not d:
                return
            line += d
        _, port, ports, n = line.split()
        port, ports, n = int(port), int(ports), int(n)
        socks = []
        for i in range(n):
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.setblocking(False)
            try:
                s.connect_ex(("127.0.0.1", port + i % ports))
            except OSError:
                pass
            socks.append(s)
            time.sleep(0.005)
        time.sleep(20)
        for s in socks:
            s.close()
        c.sendall(b"done %d\n" % len(socks))


tcp_server(7100, echo)
tcp_server(7103, flood)
tcp_server(7102, source)
udp_echo(7101)
resolver({"amiga.zenix.test": "10.1.2.3"})
if zs == "-":                           # the servers only (tools/eth-test.sh: FS-UAE's own slirp is the PC's side)
    open(ready, "w").write("ready\n")
    signal.signal(signal.SIGTERM, lambda *a: sys.exit(0))
    while True:
        time.sleep(3600)
p = subprocess.Popen([zs, "--pty", link, "--log", log] + extra)
signal.signal(signal.SIGTERM, lambda *a: (p.terminate(), sys.exit(0)))
for _ in range(100):
    if os.path.islink(link):
        break
    time.sleep(0.05)
open(ready, "w").write("ready\n")
p.wait()
