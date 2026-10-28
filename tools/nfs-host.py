#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""nfs-host: the PC's NFS server for nfscd's tests in FS-UAE (tools/nfs-emu-test.sh, DESIGN D23 11.2).

tools/host/nfs-sim.py's server (its portmapper on 111, NFSv3 on 2049, MOUNT on a free port) with the export /export
holding the test tree, run inside the test's user and network namespace, where the emulated Amiga reaches it as
10.0.2.2 (FS-UAE's slirp). The Amiga's script steers it by looking up names in the export (each is answered "no such
file", then acted on half a second later):
    .ctl-blackhole-N   nothing is answered for N seconds (the NAS gone without a word), then everything again
    .ctl-down-N        connections refused for N seconds (the NAS off), then served again
    .ctl-prog2         /export/prog is replaced by the second program (a change made on the PC)
    .ctl-rootchange    another export at the same address (its handles change)
    .ctl-restart       the NAS restarts (a new write verifier; what was not committed is lost)
    .ctl-blackrestart-N  nothing is answered for N seconds, then the NAS restarts (a write during the stop)
    .ctl-squash-U-G    every user and root act as user U, group G (DSM's "map all users"); .ctl-nosquash ends it
    .ctl-dump-NAME     the tree into OUTDIR/tree-NAME.txt: one line a file, "path type mode uid gid size md5
                       committed-md5" (directories and links: "-" for the sums; a link's target after "->")
Usage: nfs-host.py OUTDIR PROG1 PROG2 [PROG3] [--nfs 2049]   (PROG3: /export/suidid, mode 4755, root's)
Writes OUTDIR/ready when it serves, OUTDIR/big.bin (the 100 KB file, for the checksums) and OUTDIR/ctl.log (one line
a command).
"""
import hashlib
import importlib.util
import os
import sys
import threading
import time

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("nfs_sim", os.path.join(here, "host", "nfs-sim.py"))
sim_mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sim_mod)


class TestSim(sim_mod.Sim):
    def __init__(self, out, prog2, **kw):
        self.out, self.prog2 = out, prog2
        super().__init__(**kw)

    def nfs(self, proc, r, uid, gid, gids):
        if proc == 3:                                   # LOOKUP: a control name?
            p = r.p
            try:
                r.opaque(64)
                name = r.opaque(255)
            except sim_mod.Short:
                name = b""
            r.p = p
            if name.startswith(b".ctl-"):
                threading.Timer(0.5, self.control, args=(name[5:].decode("latin1"),)).start()
        return super().nfs(proc, r, uid, gid, gids)

    def control(self, cmd):
        with open(os.path.join(self.out, "ctl.log"), "a") as f:
            f.write("%.1f %s\n" % (time.time(), cmd))
        word, _, n = cmd.partition("-")
        secs = float(n) if n.replace(".", "").isdigit() else 0
        if word == "blackhole":
            self.fault("blackhole", 1)
            threading.Timer(secs, self.end_blackhole).start()
        elif word == "blackrestart":
            self.fault("blackhole", 1)
            threading.Timer(secs, self.end_blackhole, args=(True,)).start()
        elif word == "squash":
            u, _, g = n.partition("-")
            self.fault("squash", (int(u), int(g)))
        elif word == "nosquash":
            self.fault("squash", None)
        elif word == "dump":
            self.dump(n)
        elif word == "down":
            self.fault("down")
            threading.Timer(secs, self.fault, args=("up",)).start()
        elif word == "prog2":
            with open(self.prog2, "rb") as f:
                self.put_file("prog", f.read(), mode=0o755)
        elif word == "rootchange":
            self.fault("rootchange")
        elif word == "restart":
            self.fault("restart")

    def end_blackhole(self, restart=False):
        with self.lock:
            self.faults.pop("blackhole", None)
            if restart:
                self.fault("restart")                   # a new verifier, the uncommitted data lost, connections reset
        self.close_all()                                # the half-open connections are reset: the client reconnects

    def dump(self, name):
        lines = []
        with self.lock:
            def walk(ino, path):
                for n in sorted(ino.entries):
                    c = self.inodes[ino.entries[n]]
                    p = path + "/" + n.decode("utf-8", "replace")
                    if c.type == sim_mod.NF3REG:
                        sums = "%s %s" % (hashlib.md5(bytes(c.data)).hexdigest(), hashlib.md5(bytes(c.committed)).hexdigest())
                    elif c.type == sim_mod.NF3LNK:
                        sums = "- - -> " + c.target.decode("utf-8", "replace")
                    else:
                        sums = "- -"
                    lines.append("%s %d %o %d %d %d %s" % (p, c.type, c.mode, c.uid, c.gid,
                                                           len(c.data) if c.type == sim_mod.NF3REG else 0, sums))
                    if c.type == sim_mod.NF3DIR:
                        walk(c, p)
            walk(self.inodes[self.root], "")
        with open(os.path.join(self.out, "tree-%s.txt" % name), "w") as f:
            f.write("\n".join(lines) + "\n")


def main():
    a = sys.argv[1:]
    if len(a) < 3:
        print(__doc__)
        sys.exit(2)
    out, prog1, prog2 = a[0], a[1], a[2]
    nfsport = int(a[a.index("--nfs") + 1]) if "--nfs" in a else 2049
    s = TestSim(out, prog2, pmap_port=111, mount_port=0, nfs_port=nfsport, host="127.0.0.1")
    s.put_file("hello.txt", b"hello from the NAS\n")
    big = bytes((i * 7 + (i >> 8)) & 0xFF for i in range(100 * 1024))
    s.put_file("big.bin", big)
    with open(os.path.join(out, "big.bin"), "wb") as f:
        f.write(big)
    s.mkdir("dir")
    for n in ("a", "b", "c"):
        s.put_file("dir/" + n, ("file " + n + "\n").encode())
    s.symlink("link", "hello.txt")
    s.symlink("dir/up", "../hello.txt")
    s.put_file("åäö.txt", "räksmörgås\n".encode())
    with open(prog1, "rb") as f:
        s.put_file("prog", f.read(), mode=0o755)
    if len(a) > 3 and not a[3].startswith("--"):
        with open(a[3], "rb") as f:
            s.put_file("suidid", f.read(), mode=0o4755, uid=0, gid=0)
    open(os.path.join(out, "ready"), "w").close()
    print("nfs-host: serving", s.ports, flush=True)
    while True:
        time.sleep(3600)


if __name__ == "__main__":
    main()
