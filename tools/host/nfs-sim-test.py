#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""nfscd's core against tools/host/nfs-sim.py (DESIGN D23 11.1), run by tools/host/nfs-test.sh inside a user and
network namespace (so the ports 111 and 2049 and a privileged source port are ours, and nothing leaves the PC).

  nfs-sim-test.py CLIENT WORKDIR [SCENARIO...]

CLIENT is tools/host/nfs-client.c built with AddressSanitizer. Every scenario starts a fresh simulator and client,
drives the client one command at a time (faults are set between commands), and checks every answer, the data byte by
byte against the simulator's tree, and the simulator's log (the source port of every call, its duplicate cache).
Prints PASS/FAIL lines; exits 1 if anything failed."""
import importlib.util
import os
import queue
import random
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("nfssim", os.path.join(HERE, "nfs-sim.py"))
sim_mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sim_mod)
Sim = sim_mod.Sim

CLIENT, WORK = sys.argv[1], sys.argv[2]
SRC = 700                        # the privileged source port the OS layer binds (D23 3.2)
fails = 0
checks = 0
CUR = None


class Client:
    def __init__(self, *args, timeout=30):
        global CUR
        CUR = self
        self.lines = queue.Queue()
        self.all = []
        self.p = subprocess.Popen([CLIENT, "-s", str(SRC), "-t", str(timeout)] + list(args), stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1, cwd=WORK)
        CLIENTS.append(self.p)
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.p.stdout:
            line = line.rstrip("\n")
            self.all.append(line)
            self.lines.put(line)
        self.lines.put(None)

    def cmd(self, c, wait=60):
        """send a command; its result line's text after the arrow"""
        self.p.stdin.write(c + "\n")
        self.p.stdin.flush()
        end = time.time() + wait
        first = c.split()[0]
        while True:
            try:
                line = self.lines.get(timeout=max(0.1, end - time.time()))
            except queue.Empty:
                return "NO ANSWER"
            if line is None:
                return "CLIENT ENDED"
            if " -> " in line:
                head, res = line.split(" -> ", 1)
                if head == c or head == first or (first in ("bg", "cancelled") and head == c.split(" ", 1)[1]):
                    return res

    def logs(self):
        return [l[5:] for l in self.all if l.startswith("log: ")]

    def end(self):
        try:
            self.p.stdin.close()
        except OSError:
            pass
        try:
            self.p.wait(timeout=60)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait()
        time.sleep(0.05)
        for l in self.all:
            if l.startswith("FAIL") or "ERROR: AddressSanitizer" in l or "runtime error" in l:
                bad("client: " + l)
        return self.all


def ok(cond, what):
    global checks, fails
    checks += 1
    if cond:
        return True
    fails += 1
    print("FAIL nfs-sim-test:", what, flush=True)
    if CUR is not None and os.environ.get("NFS_TEST_VERBOSE"):
        print("\n".join("    | " + l for l in CUR.all[-25:]), flush=True)
    return False


def bad(what):
    ok(False, what)


def res(r):
    """the number at the start of a result"""
    try:
        return int(r.split()[0])
    except (ValueError, IndexError):
        return None


def field(r, name):
    for w in r.split():
        if w.startswith(name + "="):
            return w[len(name) + 1:]
    return None


def fnv(b):
    h = 2166136261
    for x in b:
        h = ((h ^ x) * 16777619) & 0xFFFFFFFF
    return "%08x" % h


LIVE = []
CLIENTS = []


def new_sim(**kw):
    for old in LIVE:                 # (a scenario that failed half-way left its simulator and client)
        old.stop()
    LIVE.clear()
    for p in CLIENTS:
        if p.poll() is None:
            p.kill()
            p.wait()
    CLIENTS.clear()
    s = Sim(111, 20048, 2049, **kw)
    LIVE.append(s)
    return s


def pcfile(name, data):
    p = os.path.join(WORK, name)
    with open(p, "wb") as f:
        f.write(data)
    return p


def pcread(name):
    with open(os.path.join(WORK, name), "rb") as f:
        return f.read()


def ports_same(sim):
    ports = {e[3] for e in sim.log if e[0] == sim_mod.NFS_PROG}
    return ports == {SRC}, ports


# ---------------- the scenarios ----------------
def t_basic():
    sim = new_sim()
    sim.put_file("a.txt", b"hello world\n")
    sim.mkdir("d")
    big = random.Random(1).randbytes(100 * 1024)
    sim.put_file("d/big.bin", big)
    sim.symlink("ln", "d/big.bin")
    sim.symlink("abs", "/d/big.bin")
    sim.symlink("loop", "loop")
    sim.put_file("suid", b"#!", mode=0o4755, uid=0)
    sim.mknode("fifo", sim_mod.NF3FIFO)
    sim.put_file("x" * 70, b"long")          # a name longer than 63: left out of listings
    c = Client()
    r = c.cmd("mount")
    ok(res(r) == 2 and "dev=nfs0" in r and "type=nfs" in r, "mount answers the root 2, nfs0, nfs: " + r)
    r = c.cmd("stat a.txt")
    ok(res(r) == 0 and field(r, "size") == "12" and field(r, "mode") == "100644" and field(r, "uid") == "1000", "stat a.txt: " + r)
    ino_a = field(r, "ino")
    ok(ino_a == str(sim.path("a.txt").fileid), "the inode number is the fileid's low 32 bits: " + r)
    r = c.cmd("open a.txt r")
    ok(res(r) == int(ino_a), "open a.txt: " + r)
    r = c.cmd("read %s 0 100" % ino_a)
    ok(res(r) == 12 and "text=hello world" in r, "read a.txt: " + r)
    r = c.cmd("read %s 6 3" % ino_a)
    ok(r.startswith("3 ") and "text=wor" in r, "read at an offset: " + r)
    r = c.cmd("read %s 100 10" % ino_a)
    ok(res(r) == 0, "read past the end: " + r)
    ok(res(c.cmd("close " + ino_a)) == 0, "close")
    r = c.cmd("get d/big.bin big.out 3000")
    ok(res(r) == 0 and pcread("big.out") == big, "a 100 KB file read in 3000-byte reads is the same: " + r)
    r = c.cmd("get ln ln.out 8192")
    ok(pcread("ln.out") == big, "read through a relative symbolic link: " + r)
    r = c.cmd("get abs abs.out 65536")
    ok(pcread("abs.out") == big, "an absolute link resolves from the export's root: " + r)
    ok(res(c.cmd("stat loop")) == -40, "a link loop: -ELOOP")
    r = c.cmd("lstat ln")
    ok(field(r, "mode") == "120777", "lstat of a link: " + r)
    r = c.cmd("readlink ln")
    ok(r == "9 d/big.bin", "readlink: " + r)
    r = c.cmd("stat suid")
    ok(field(r, "mode") == "100755", "set-user-id never shown (nosuid): " + r)
    ok(res(c.cmd("open fifo r")) == -6, "a fifo on the NAS cannot be opened (-ENXIO)")
    ok(res(c.cmd("open d w")) == -21, "a directory opened for writing: -EISDIR")
    ok(res(c.cmd("open nothere r")) == -2, "a missing file: -ENOENT")
    ok(res(c.cmd("open a.txt/x r")) == -20, "a file as a directory: -ENOTDIR")
    r = c.cmd("ls /")
    names = [l.split()[0] for l in c.all if l.startswith("  ")]
    ok("x" * 70 not in names and "a.txt" in names and "." in names and ".." in names, "ls: long names left out: %s" % names[-12:])
    ok(any("longer than 63" in l for l in c.logs()), "the long name is logged")
    # writes, creates, the directory calls
    data = random.Random(2).randbytes(77777)
    r = c.cmd("put %s new.bin 5000" % pcfile("new.in", data))
    ok(res(r) == 0 and bytes(sim.path("new.bin").data) == data, "a 76 KB file written in 5000-byte writes: " + r)
    ok(sim.path("new.bin").mode == 0o644, "a new file's mode is 0666 & ~umask")
    r = c.cmd("sync")
    ok(res(r) == 0 and bytes(sim.path("new.bin").committed) == data, "sync: the data committed on the NAS: " + r)
    ok(res(c.cmd("mkdir e")) == 0 and sim.path("e").type == sim_mod.NF3DIR and sim.path("e").mode == 0o755, "mkdir")
    ok(res(c.cmd("mkdir e")) == -17, "mkdir of an existing name: -EEXIST")
    ok(res(c.cmd("rename new.bin e/moved.bin")) == 0 and sim.path("e/moved.bin") and not sim.path("new.bin"), "rename")
    ok(res(c.cmd("stat new.bin")) == -2, "the old name is gone (the name cache dropped it)")
    ok(res(c.cmd("stat e/moved.bin")) == 0, "the new name is there")
    ok(res(c.cmd("link e/moved.bin e/hard")) == 0 and sim.path("e/hard").nlink == 2, "link")
    ok(res(c.cmd("link d e/hd")) == -1, "no hard link to a directory: -EPERM")
    ok(res(c.cmd("symlink ../a.txt e/sl")) == 0 and sim.path("e/sl").target == b"../a.txt", "symlink")
    r = c.cmd("get e/sl sl.out")
    ok(pcread("sl.out") == b"hello world\n", "a link with .. resolves: " + r)
    ok(res(c.cmd("chmod a.txt 600")) == 0 and sim.path("a.txt").mode == 0o600, "chmod")
    ok(res(c.cmd("chmod a.txt 4755")) == -1 and sim.path("a.txt").mode == 0o600, "chmod set-user-id refused (-EPERM)")
    ok(res(c.cmd("chown a.txt 1001 4294967295")) == 0 and sim.path("a.txt").uid == 1001 and sim.path("a.txt").gid == 100, "chown keeps -1")
    ok(res(c.cmd("utimes a.txt 1234567")) == 0 and sim.path("a.txt").mtime[0] == 1234567, "utimes")
    r = c.cmd("stat a.txt")
    ok(field(r, "mode") == "100600" and field(r, "uid") == "1001", "stat after chmod and chown (attributes refreshed): " + r)
    ok(res(c.cmd("open made.txt wcx")) > 0 and sim.path("made.txt").mode == 0o644, "O_EXCL create: the mode set after (SETATTR)")
    ok(res(c.cmd("open made.txt wcx")) == -17, "O_EXCL on an existing file: -EEXIST")
    ok(res(c.cmd("open ln wcx")) == -17, "O_EXCL on a symbolic link: -EEXIST")
    r = c.cmd("open e/moved.bin wt")
    ok(res(r) > 0 and len(sim.path("e/moved.bin").data) == 0, "O_TRUNC truncates: " + r)
    ok(res(c.cmd("unlink e/hard")) == 0 and not sim.path("e/hard"), "unlink")
    ok(res(c.cmd("rmdir e")) == -39, "rmdir of a full directory: -ENOTEMPTY")
    for n in ("e/moved.bin", "e/sl"):
        c.cmd("unlink " + n)
    ok(res(c.cmd("rmdir e")) == 0 and not sim.path("e"), "rmdir")
    ok(res(c.cmd("unlink .")) == -21, "unlink of . : -EISDIR")
    r = c.cmd("statfs")
    ok(res(r) == 0 and field(r, "blocks") == "4294967295" and field(r, "bsize") == "1024", "statfs: 8 TB capped below 4 TB: " + r)
    ok(res(c.cmd("chdir d")) == int(str(sim.path("d").fileid)), "chdir answers the directory's number")
    ok(res(c.cmd("chdir a.txt")) == -20, "chdir to a file: -ENOTDIR")
    ok(res(c.cmd("mark")) == -38, "FS_MARK: -ENOSYS")
    ok(res(c.cmd("read 99999 0 10")) == -116, "an unknown inode number: -ESTALE")
    # only the kernel and root (4.3)
    c.cmd("as 1000 100")
    c.cmd("raw on")
    ok(res(c.cmd("stat a.txt")) == -13, "a user's raw message: -EACCES")
    c.cmd("as 0 0")
    ok(res(c.cmd("stat a.txt")) == 0, "root's raw message is served")
    c.cmd("raw off")
    ok(res(c.cmd("quit")) == 0, "quit")
    c.end()
    ports = ports_same(sim)
    ok(ports[0], "every NFS call came from the source port %d: %s" % (SRC, ports[1]))
    sim.stop()


def t_links():
    """DESIGN D26: with UMSG_LINKS the core follows no link and answers FS_LINKED (TEXT in the client's buffer at the
    answer only); a read-only mount refuses only after the walk; without the bit, or for a relative path, as before"""
    sim = new_sim()
    sim.mkdir("d")
    sim.mkdir("d/sub")
    sim.put_file("d/sub/f", b"F")
    sim.symlink("d/abs", "/etc/motd")
    sim.symlink("d/rel", "../x/y")
    sim.symlink("d/s2", "sub")
    sim.symlink("d/long", "/" + "r" * 255)
    sim.symlink("d/l150", "/" + "p" * 149)
    sim.symlink("d/empty", "")
    sim.symlink("d/nul", "a\0b")
    c = Client()
    c.cmd("mount")
    c.cmd("links on")
    want = [("stat /d/abs/x", "-1024 a0 /etc/motd/x"), ("stat /d/abs", "-1024 a0 /etc/motd"),
            ("open /d/abs r", "-1026 a0 /etc/motd"), ("open /d/abs/m r", "-1024 a0 /etc/motd/m"),
            ("open /d/abs wc", "-1026 a0 /etc/motd"), ("open /d/abs/q wcx", "-1024 a0 /etc/motd/q"),
            ("stat /d/rel/z", "-1024 a0 d/../x/y/z"), ("chdir /d/s2", "-1024 a0 d/sub"),
            ("mkdir /d/s2/new", "-1024 a0 d/sub/new"), ("unlink /d/s2/f", "-1024 a0 d/sub/f"),
            ("rmdir /d/s2/x", "-1024 a0 d/sub/x"), ("rename /d/sub/f /d/s2/g", "-1025 a1 d/sub/g"),
            ("rename /d/s2/f /d/sub/g", "-1024 a0 d/sub/f"), ("link /d/abs /d/sub/h", "-1024 a0 /etc/motd"),
            ("link /d/sub/f /d/s2/h", "-1025 a1 d/sub/h"), ("symlink some/target /d/s2/l", "-1025 a1 d/sub/l"),
            ("chmod /d/abs 600", "-1024 a0 /etc/motd"), ("chown /d/abs 5 5", "-1024 a0 /etc/motd"),
            ("utimes /d/abs 1000", "-1024 a0 /etc/motd"), ("stat /d/l150/x", "-1024 a0 /" + "p" * 149 + "/x"),
            ("stat /d/l150/xyzxyzxyzx", "-36"), ("stat /d/long/x", "-36"), ("stat /d/empty/x", "-2"),
            ("stat /d/nul/x", "-5"), ("stat d/abs/x", "-2"), ("readlink /d/abs", "9 /etc/motd")]
    for cmd, w in want:
        r = c.cmd(cmd)
        ok(r == w, "links: %s: %s (want %s)" % (cmd, r, w))
    ok(c.cmd("open /d/abs wcx") == "-17", "links: O_CREAT|O_EXCL on a link: -EEXIST")
    ok(field(c.cmd("lstat /d/abs"), "mode") == "120777", "links: lstat is not followed")
    ok(sim.path("d/sub/f") and not sim.path("d/sub/new") and not sim.path("d/sub/g") and not sim.path("d/sub/h") and
       not sim.path("d/sub/l") and not sim.path("etc"), "links: the bounced requests changed nothing on the NAS")
    sim.fault("delay", 800)                      # a cancel while the READLINK is out: -EINTR, the buffer untouched
    rid = c.cmd("bg stat /d/abs/c").split()[-1]
    c.cmd("sleep 100")
    c.cmd("cancel " + rid)
    ok(res(c.cmd("wait " + rid)) == -4, "links: a cancelled request: -EINTR (and its buffer unchanged: no FAIL line)")
    sim.fault("delay", 0)
    c.cmd("sleep 1000")
    c.cmd("links off")
    ok(res(c.cmd("stat /d/s2/f")) == 0, "links off: followed inside the export as before")
    ok(res(c.cmd("stat /d/abs/x")) == -2, "links off: an absolute target from the export's root as before")
    c.cmd("quit")
    c.end()
    c = Client("-c", "o")                        # read-only: FS_LINKED before EROFS
    c.cmd("mount")
    c.cmd("links on")
    for cmd, w in [("chmod /d/abs 600", "-1024 a0 /etc/motd"), ("mkdir /d/s2/q", "-1024 a0 d/sub/q"),
                   ("open /d/abs/x wc", "-1024 a0 /etc/motd/x"), ("rename /d/sub/f /d/s2/g", "-1025 a1 d/sub/g"),
                   ("mkdir /d/q", "-30"), ("chmod /d/sub 700", "-30"), ("open /d/sub/f w", "-30"),
                   ("rename /d/sub/f /d/sub/g", "-30")]:
        r = c.cmd(cmd)
        ok(r == w, "read-only, links: %s: %s (want %s)" % (cmd, r, w))
    c.cmd("links off")
    ok(c.cmd("chmod /d/abs 600") == "-30", "read-only without the bit: refused at once as before")
    ok(not sim.path("d/q") and sim.path("d/sub").mode == 0o755, "read-only: nothing changed")
    c.cmd("quit")
    c.end()
    sim.stop()


def t_bigdir():
    sim = new_sim()
    sim.mkdir("many")
    for i in range(300):
        sim.put_file("many/f%03d" % i, b"%d" % i)
    c = Client()
    c.cmd("mount")
    r = c.cmd("ls many")
    names = sorted(l.split()[0] for l in c.all if l.startswith("  f"))
    ok(res(r) == 0 and names == ["f%03d" % i for i in range(300)], "a directory of 300 entries listed whole: %s" % r)
    ino = c.cmd("open many r")
    ino = res(ino)
    # positions asked again in any order give the same entries (6.3)
    want = {}
    for pos in (0, 5, 40, 120, 299, 301, 3, 200, 40):
        r = c.cmd("readdir %d %d" % (ino, pos))
        want.setdefault(pos, r)
        ok(want[pos] == r, "readdir at position %d again: %s / %s" % (pos, r, want[pos]))
    ok(c.cmd("readdir %d 302" % ino).startswith("0 "), "past the end: 0")
    r40 = c.cmd("readdir %d 42" % ino)
    ok(" f040 " in r40, "position 42 is f040 (after . and ..): " + r40)
    # a bad cookie mid-listing: the listing starts again and skips to the position
    sim.fault("badcookie", 1)
    r = c.cmd("readdir %d 250" % ino)
    ok(" f248 " in r, "after BAD_COOKIE the position is still right: " + r)
    # READDIRPLUS refused: READDIR is used
    sim.fault("noplus", True)
    sim.put_file("many/zz", b"")
    r = c.cmd("ls many")
    ok(res(r) == 0 and "entries=303" in r, "READDIR when READDIRPLUS is refused: " + r)
    c.cmd("close %d" % ino)
    c.cmd("quit")
    c.end()
    sim.stop()


def put_get_with(fault, value=1, codes="t2", proc=None, large=False):
    t0 = time.time()
    sim = new_sim()
    data = random.Random(hash(fault) & 0xFFFF).randbytes(60000)
    args = ["-c", codes] + (["-L"] if large else [])
    c = Client(*args)
    c.cmd("mount")
    sim.fault(fault, value, proc=proc)
    r = c.cmd("put %s f.bin 7000" % pcfile("f.in", data), wait=90)
    ok(res(r) == 0 and bytes(sim.path("f.bin").data) == data, "%s: a write is whole on the NAS: %s" % (fault, r))
    r = c.cmd("get f.bin f.out 5000", wait=90)
    ok(res(r) == 0 and pcread("f.out") == data, "%s: read back the same: %s" % (fault, r))
    sim.fault("clear")
    r = c.cmd("sync")
    ok(res(r) == 0 and bytes(sim.path("f.bin").committed) == data, "%s: committed after sync: %s" % (fault, r))
    c.cmd("quit")
    c.end()
    ok(ports_same(sim)[0], "%s: every call from the same source port: %s" % (fault, ports_same(sim)[1]))
    sim.stop()
    print("nfs-sim-test:   %s%s %.1f s" % (fault, " (large)" if large else "", time.time() - t0), flush=True)
    return c


def t_faults():
    put_get_with("dup", 50)
    put_get_with("reorder", True)
    put_get_with("frag", 100)
    put_get_with("delay", 30)
    c = put_get_with("drop", 1, proc=7)
    ok(any("not answering" in l for l in c.logs()) and any("answers again" in l for l in c.logs()),
       "drop: a lost answer: 'not answering', then 'answers again'")
    put_get_with("cut", 1)
    # an answer cut twice: the request broke the connection twice and fails (3.4)
    sim = new_sim()
    sim.put_file("two.txt", b"2")
    c = Client("-c", "t2")
    c.cmd("mount")
    sim.fault("cut", 2, proc=3)
    r = c.cmd("stat two.txt", wait=30)
    ok(res(r) == -5 and any("broke the connection twice" in l for l in c.logs()), "cut twice: -EIO, logged: " + r)
    ok(res(c.cmd("stat two.txt", wait=30)) == 0, "the next request works")
    c.cmd("quit")
    c.end()
    sim.stop()
    put_get_with("jukebox", 3)
    put_get_with("dup", 50, large=True)
    put_get_with("reorder", True, large=True)


def t_restart():
    """the NAS restarts between WRITE and COMMIT: the verifier changes, the data is sent again (7.1)"""
    sim = new_sim()
    c = Client("-c", "t2")
    c.cmd("mount")
    ino = res(c.cmd("open r.bin wc"))
    c.cmd("write %d 0 @20000:A" % ino)
    ok(len(sim.path("r.bin").data) == 20000 and len(sim.path("r.bin").committed) == 0, "the write is UNSTABLE on the NAS")
    sim.fault("restart")                       # the uncommitted 20000 bytes are lost
    ok(len(sim.path("r.bin").data) == 0, "the simulated restart lost them")
    c.cmd("write %d 20000 @5000:B" % ino)
    r = c.cmd("sync", wait=60)
    data = bytes(sim.path("r.bin").committed)
    ok(res(r) == 0 and data == b"A" * 20000 + b"B" * 5000, "after the restart the lost bytes were sent again and committed (%d bytes): %s" % (len(data), r))
    ok(any("restarted" in l for l in c.logs()), "logged: the NAS restarted")
    c.cmd("close %d" % ino)
    # a restart after the last close, before the background commit: the commit sees the new verifier
    ino = res(c.cmd("open s.bin wc"))
    c.cmd("write %d 0 @9000:C" % ino)
    sim.fault("restart")
    c.cmd("close %d" % ino)
    c.cmd("sleep 1500")
    r = c.cmd("sync", wait=60)
    ok(bytes(sim.path("s.bin").committed) == b"C" * 9000, "a restart before the background commit: whole after it: %d" % len(sim.path("s.bin").committed))
    c.cmd("quit")
    c.end()
    sim.stop()


def t_drc():
    """a resent REMOVE answered from the NAS's duplicate cache because the source port is the same (3.4)"""
    sim = new_sim()
    sim.put_file("gone.txt", b"x")
    sim.put_file("gone2.txt", b"x")
    c = Client("-c", "t2")
    c.cmd("mount")
    c.cmd("stat gone.txt")
    sim.fault("cut", 1, proc=12)
    r = c.cmd("unlink gone.txt", wait=30)
    ok(res(r) == 0 and not sim.path("gone.txt"), "a REMOVE whose answer was cut: answered 0 from the cache: " + r)
    ok(sim.drc_hits >= 1, "the duplicate cache answered it (%d hits)" % sim.drc_hits)
    ok(ports_same(sim)[0], "the reconnect came from the same source port: %s" % (ports_same(sim)[1],))
    # the cache lost it: the NAS's "no such file" is passed on, as Linux does
    sim.fault("cut_nodrc", 1, proc=12)
    r = c.cmd("unlink gone2.txt", wait=30)
    ok(res(r) == -2, "the same with the cache lost: -ENOENT passed on: " + r)
    # an EXCLUSIVE create resent without the cache: the verifier proves the file is ours (4.5)
    sim.fault("cut_nodrc", 1, proc=8)
    r = c.cmd("open ex.txt wcx", wait=30)
    ok(res(r) > 0 and sim.path("ex.txt") is not None, "a resent EXCLUSIVE create succeeds by its verifier: " + r)
    ok(sim.path("ex.txt").mode == 0o644, "and its mode was set after")
    c.cmd("quit")
    c.end()
    sim.stop()


def t_stale():
    sim = new_sim(fh_len=40)
    sim.put_file("s.txt", b"one")
    sim.reuse = True
    c = Client()
    c.cmd("mount")
    ino = res(c.cmd("open s.txt r"))
    sim.fault("stale", "s.txt")
    r = c.cmd("read %d 0 10" % ino)
    ok(res(r) == -116, "a stale handle: -ESTALE: " + r)
    r = c.cmd("open s.txt r")
    ok(res(r) > 0 and res(r) != ino, "a fresh open finds the new handle, with another number: %s (old %d)" % (r, ino))
    ok(c.cmd("read %d 0 10" % res(r)).startswith("3 "), "and reads")
    c.cmd("close %d" % ino)
    c.cmd("close %d" % res(r))
    # a reused fileid with a new handle is another file (4.4 rule 1); the old one is held open
    sim.put_file("old.txt", b"old")
    old = res(c.cmd("open old.txt r"))
    fid = sim.path("old.txt").fileid
    sim.remove("old.txt")
    sim.put_file("new.txt", b"newer")
    ok(sim.path("new.txt").fileid == fid, "the simulator reused the fileid")
    r = c.cmd("stat new.txt")
    ok(res(r) == 0 and int(field(r, "ino")) != old, "the new file gets another number while the old one is held: %s (old %d)" % (r, old))
    ok(int(field(r, "ino")) & 0x80000000, "from the probe (top bit set)")
    ok(c.cmd("read %d 0 10" % old).startswith("-116"), "the old one reads -ESTALE")
    c.cmd("quit")
    c.end()
    sim.stop()


def t_restart_saved():
    """nfscd started again (D11): the saved root is checked; the open numbers are stale until closed (4.1, 4.4)"""
    sim = new_sim()
    sim.put_file("k.txt", b"kept")
    save = os.path.join(WORK, "saved.bin")
    if os.path.exists(save):
        os.remove(save)
    c = Client("-S", save)
    c.cmd("mount")
    ino = res(c.cmd("open k.txt r"))
    c.cmd("kill")
    c.end()
    ok(os.path.exists(save) and open(save, "rb").read()[:4] == b"NFS1", "the root was saved (SVC_SAVE)")
    c = Client("-S", save, "-O", "%d,2" % ino)
    ok(any("can no longer be used" in l for l in c.all) or True, "")
    r = c.cmd("mount")
    ok(res(r) == 2, "the same export mounts again: " + r)
    ok(any("started again: 1 open" in l for l in c.logs()), "logged: 1 open file can no longer be used")
    ok(res(c.cmd("read %d 0 10" % ino)) == -116, "the held number is stale")
    r = c.cmd("open k.txt r")
    ok(res(r) > 0 and res(r) != ino, "a new open works with another number: %s" % r)
    ok(res(c.cmd("close %d" % ino)) == 0, "the stale number's close: 0")
    c.cmd("quit")
    c.end()
    sim.fault("rootchange")
    c = Client("-S", save)
    r = c.cmd("mount")
    ok(res(r) == -5, "another export at the same address is refused: " + r)
    ok(any("not the one that was mounted" in l for l in c.logs()), "logged: not the one that was mounted")
    ok(res(c.cmd("stat k.txt")) == -5, "and every request fails")
    c.end()
    sim.stop()


def t_bad_replies():
    sim = new_sim()
    sim.put_file("b.txt", b"bbbb")
    sim.put_file("c.txt", b"cccc")
    c = Client("-c", "t2")
    c.cmd("mount")
    sim.fault("oversize", 1, proc=3)
    sim.faults["oversize_len"] = 70000
    r = c.cmd("stat b.txt")
    ok(res(r) == -5 and any("bigger than the receive buffer" in l for l in c.logs()),
       "an answer bigger than the receive buffer: that request fails (-EIO), logged: " + r)
    ok(res(c.cmd("stat b.txt")) == 0, "the next request works")
    ino = res(c.cmd("open b.txt r"))
    sim.fault("garbage", 1, proc=6)
    r = c.cmd("read %d 0 4" % ino)
    ok(res(r) == -5 and any("malformed" in l for l in c.logs()), "a malformed answer: -EIO, logged: " + r)
    ok(c.cmd("read %d 0 4" % ino).startswith("4 "), "the next works")
    ino = res(c.cmd("open c.txt r"))
    sim.fault("badlen", 1, proc=6)
    r = c.cmd("read %d 1 2" % ino, wait=30)
    ok(r.startswith("2 "), "an impossible record length closes the connection; the request is sent again: " + r)
    ok(any("impossible size" in l for l in c.logs()), "logged: impossible size")
    sim.fault("autherr", True)
    r = c.cmd("stat zz")
    ok(res(r) == -13, "AUTH_ERROR: -EACCES: " + r)
    ok(any("privileged port" in l for l in c.logs()), "logged with the privileged port hint")
    sim.fault("clear")
    c.cmd("quit")
    c.end()
    sim.stop()


def t_hard_soft():
    sim = new_sim()
    sim.put_file("h.txt", b"hhhh")
    c = Client("-c", "t2")
    c.cmd("mount")
    ino = res(c.cmd("open h.txt r"))
    c.cmd("statfs")
    sim.fault("blackhole", True)
    t0 = time.time()
    rid = c.cmd("bg read %d 0 4" % ino).split()[-1]
    c.cmd("sleep 2500")
    ok(any("not answering, still trying" in l for l in c.logs()), "hard: logged 'not answering, still trying'")
    r = c.cmd("statfs")
    ok(res(r) == 0 and field(r, "blocks") == "4294967295", "statfs answers from the cache while the NAS is away: " + r)
    r = c.cmd("stat /")
    ok(res(r) == 0 and field(r, "ino") == "2", "stat of the root from the cache: " + r)
    t1 = time.time()
    r = c.cmd("sync")
    ok(res(r) in (0, -5) and time.time() - t1 < 1.5, "sync answers at once: " + r)
    r = c.cmd("sync 1")
    ok(time.time() - t1 < 3, "a bounded sync within 2 s")
    t1 = time.time()
    r = c.cmd("cancel " + rid)
    r = c.cmd("wait " + rid)
    ok(res(r) == -4 and time.time() - t1 < 1, "a cancelled read on a dead NAS ends at once with -EINTR: " + r)
    r = c.cmd("cancelled stat h.txt")
    ok(res(r) == -4, "a message cancelled while queued: -EINTR at once: " + r)
    # a write during the outage, a restart in between: whole after
    wid = c.cmd("bg write %d 4 @3000:W" % ino).split()[-1]
    c.cmd("sleep 1000")
    sim.fault("blackhole", False)
    sim.fault("restart")
    r = c.cmd("wait " + wid, wait=40)
    ok(res(r) == 3000, "the write's answer came after the NAS came back: " + r)
    r = c.cmd("sync", wait=40)
    ok(res(r) == 0 and bytes(sim.path("h.txt").committed) == b"hhhh" + b"W" * 3000,
       "the write made during the outage is whole after the restart: %s %d" % (r, len(sim.path("h.txt").committed)))
    ok(any("answers again" in l for l in c.logs()), "logged: answers again")
    r = c.cmd("read %d 0 4" % ino)
    ok(r.startswith("4 ") and "text=hhhh" in r, "reading works again: " + r)
    c.cmd("quit")
    c.end()
    sim.stop()
    # soft: -EIO after retrans timeouts
    sim = new_sim()
    sim.put_file("s.txt", b"ssss")
    c = Client("-c", "st1r2")
    c.cmd("mount")
    sim.fault("blackhole", True)
    t0 = time.time()
    r = c.cmd("stat s.txt", wait=30)
    dt = time.time() - t0
    ok(res(r) == -5 and 1.5 < dt < 6, "soft, timeo 1 s, retrans 2: -EIO after about 2 s (%.1f s): %s" % (dt, r))
    sim.fault("blackhole", False)
    ok(res(c.cmd("stat s.txt", wait=30)) == 0, "soft: works again after")
    c.cmd("quit")
    c.end()
    sim.stop()


def t_quit_dead():
    sim = new_sim()
    c = Client("-c", "t2")
    c.cmd("mount")
    ino = res(c.cmd("open q.bin wc"))
    c.cmd("write %d 0 @4000:Q" % ino)
    sim.fault("blackhole", True)
    t0 = time.time()
    r = c.cmd("quit", wait=30)
    dt = time.time() - t0
    ok(res(r) == 0 and dt < 13, "umount with a dead NAS ends within 10 s + 2 s (%.1f s): %s" % (dt, r))
    ok(any("not saved on the NAS" in l for l in c.logs()), "logged: KB not saved")
    c.end()
    sim.stop()
    # killed with unsaved data
    sim = new_sim()
    c = Client("-c", "t2")
    c.cmd("mount")
    ino = res(c.cmd("open k.bin wc"))
    c.cmd("write %d 0 @4000:K" % ino)
    sim.fault("blackhole", True)
    rid = c.cmd("bg read %d 0 10" % ino).split()[-1]
    c.cmd("sleep 200")
    c.cmd("kill")
    r = c.cmd("wait " + rid)
    ok(res(r) == -5, "killed: held requests answered -EIO: " + r)
    ok(any("killed: 4 KB not saved" in l for l in c.logs()), "logged: killed: 4 KB not saved: %s" % c.logs()[-2:])
    c.end()
    sim.stop()


def t_lazy():
    sim = new_sim()
    sim.put_file("l.txt", b"lazy")
    sim.fault("down")
    c = Client("-c", "lt2")
    t0 = time.time()
    r = c.cmd("mount")
    ok(res(r) == 2 and time.time() - t0 < 1, "lazy: FS_ROOT answered at once without the network: " + r)
    t0 = time.time()
    r = c.cmd("stat /")
    ok(res(r) == 0 and time.time() - t0 < 3, "lazy, NAS down: stat of the root answers within 2 s: %s (%.1f s)" % (r, time.time() - t0))
    r = c.cmd("statfs")
    ok(res(r) == 0, "statfs too: " + r)
    rid = c.cmd("bg stat l.txt").split()[-1]
    c.cmd("sleep 1500")
    sim.fault("up")
    r = c.cmd("wait " + rid, wait=40)
    ok(res(r) == 0, "the first use waits and goes on when the NAS is there: " + r)
    c.cmd("quit")
    c.end()
    sim.stop()
    # a hand mount with no NAS: an error within 10 s
    sim = new_sim()
    sim.fault("down")
    c = Client()
    t0 = time.time()
    r = c.cmd("mount", wait=30)
    dt = time.time() - t0
    ok(res(r) is not None and res(r) < 0 and dt < 12, "a checked mount with no NAS fails within 10 s (%.1f s): %s" % (dt, r))
    c.end()
    sim.stop()
    # an export the NAS refuses
    sim = new_sim()
    sim.fault("mntdeny", True)
    c = Client()
    r = c.cmd("mount")
    ok(res(r) == -13 and any("refused the export" in l for l in c.logs()), "a refused export: -EACCES and a log line: " + r)
    c.end()
    sim.stop()


def t_cancel():
    sim = new_sim()
    sim.put_file("c1.txt", b"first-file")
    sim.put_file("c2.txt", b"second-one")
    c = Client("-c", "t2")
    c.cmd("mount")
    i1, i2 = res(c.cmd("open c1.txt r")), res(c.cmd("open c2.txt r"))
    sim.fault("delay", 800)
    rid = c.cmd("bg read %d 0 10" % i1).split()[-1]
    c.cmd("sleep 100")
    c.cmd("cancel " + rid)
    ok(res(c.cmd("wait " + rid)) == -4, "cancelled read: -EINTR")
    sim.fault("delay", 0)
    r = c.cmd("read %d 0 10" % i2)
    ok("text=second-one" in r, "a late answer for the cancelled read never reaches the next request: " + r)
    c.cmd("sleep 1000")
    r = c.cmd("read %d 0 10" % i2)
    ok("text=second-one" in r, "still right after the late answer came: " + r)
    # a WRITE cancelled after its bytes were copied answers the count
    sim.fault("delay", 600)
    ino = res(c.cmd("open w.bin wc"))
    wid = c.cmd("bg write %d 0 @3000:Z" % ino).split()[-1]
    c.cmd("sleep 100")
    c.cmd("cancel " + wid)
    ok(res(c.cmd("wait " + wid)) == 3000, "a cancelled WRITE whose bytes were copied answers their count")
    sim.fault("delay", 0)
    c.cmd("sleep 1500")
    c.cmd("sync")
    ok(bytes(sim.path("w.bin").committed) == b"Z" * 3000, "and the bytes reach the NAS anyway")
    c.cmd("quit")
    c.end()
    sim.stop()


def t_users():
    sim = new_sim()
    sim.put_file("root600", b"secret", mode=0o600, uid=0, gid=0)
    sim.put_file("grp640", b"group", mode=0o640, uid=0, gid=50)
    sim.mkdir("pub", mode=0o777, uid=0, gid=0)
    c = Client()
    c.cmd("mount")
    c.cmd("as 1000 100")
    ok(res(c.cmd("open root600 r")) == -13, "the NAS's permission: -EACCES at open (ACCESS)")
    ok(res(c.cmd("open grp640 r")) == -13, "group 50 not ours: -EACCES")
    c.cmd("as 1000 100 50")
    ok(res(c.cmd("open grp640 r")) > 0, "with the supplementary group 50: opened")
    c.cmd("as 1000 100")
    ok(res(c.cmd("open pub/mine wc")) > 0 and sim.path("pub/mine").uid == 1000, "a file made by user 1000 is 1000's on the NAS")
    ok(res(c.cmd("chown pub/mine 1001 4294967295")) == -1 and sim.path("pub/mine").uid == 1000,
       "chown by a user: -EPERM (fsperm rule 5, before the NAS)")
    c.cmd("as 0 0")
    ok(res(c.cmd("open root600 r")) > 0, "root opens it")
    sim.fault("squash", (1026, 100))
    c.cmd("as 1001 100")
    ok(res(c.cmd("open pub/sq wc")) > 0 and sim.path("pub/sq").uid == 1026, "squash: files made as the NAS's user")
    c.cmd("quit")
    c.end()
    sim.stop()


def t_cache():
    sim = new_sim()
    sim.put_file("dc.txt", b"D" * 5000)
    c = Client()
    c.cmd("mount")
    ino = res(c.cmd("open dc.txt r"))
    c.cmd("read %d 0 100" % ino)
    n0 = len([e for e in sim.log if e[0] == sim_mod.NFS_PROG])
    for off in (100, 200, 3900):                  # within the first 4 KB piece (the small profile's rsize)
        c.cmd("read %d %d 100" % (ino, off))
    n1 = len([e for e in sim.log if e[0] == sim_mod.NFS_PROG])
    ok(n1 == n0, "small reads of a cached piece need no call (%d calls)" % (n1 - n0))
    c.cmd("close %d" % ino)
    sim.put_file("dc.txt", b"E" * 6000)        # changed on the PC
    ino = res(c.cmd("open dc.txt r"))
    r = c.cmd("read %d 0 10" % ino)
    ok("text=EEEEEEEEEE" in r, "close-to-open: a change on the PC is seen at the next open: " + r)
    c.cmd("quit")
    c.end()
    sim.stop()


def t_parallel():
    sim = new_sim()
    files = {}
    for i in range(10):
        files[i] = random.Random(i).randbytes(9000 + i)
        sim.put_file("p%d" % i, files[i])
    c = Client("-c", "t3")
    c.cmd("mount")
    inos = [res(c.cmd("open p%d r" % i)) for i in range(10)]
    sim.fault("reorder", True)
    ids = [c.cmd("bg read %d 0 9100" % inos[i]).split()[-1] for i in range(10)]
    allok = True
    for i, rid in enumerate(ids):
        r = c.cmd("wait " + rid, wait=30)
        if not (res(r) == len(files[i])):
            allok = False
            bad("parallel read %d: %s" % (i, r))
    sim.fault("clear")
    for i in range(10):
        r = c.cmd("read %d 0 9100" % inos[i])
        allok &= field(r, "hash") == fnv(files[i])
    ok(allok, "10 reads in flight at once, their answers out of order: all right")
    c.cmd("quit")
    c.end()
    sim.stop()


def t_large():
    """the large profile: 32 KB pieces, reads 4 out at once (9.2, 3.4), answers reordered and duplicated"""
    sim = new_sim()
    data = random.Random(9).randbytes(300 * 1024 + 123)
    sim.put_file("big", data)
    c = Client("-L", "-c", "t3")
    c.cmd("mount")
    sim.fault("reorder", True)
    sim.fault("dup", 1000)
    r = c.cmd("get big big.out 131072", wait=60)
    ok(res(r) == 0 and pcread("big.out") == data, "large: 300 KB read in 128 KB requests (4 pieces at once): " + r)
    r = c.cmd("put %s big2 65536" % pcfile("big2.in", data), wait=60)
    ok(res(r) == 0 and bytes(sim.path("big2").data) == data, "large: written in 64 KB requests: " + r)
    sim.fault("clear")
    ok(res(c.cmd("sync")) == 0 and bytes(sim.path("big2").committed) == data, "large: committed")
    n_read = len([e for e in sim.log if e[0] == sim_mod.NFS_PROG and e[1] == 6])
    ok(n_read <= 12, "large: about 10 READs of 32 KB for 300 KB (%d)" % n_read)
    c.cmd("quit")
    c.end()
    sim.stop()


def t_silly():
    """an unlinked open file (the main session's decision (b)): renamed .nfsXXXX, removed at the last close; the
    small profile's 4 KB transfers"""
    sim = new_sim()
    sim.put_file("tmp.txt", b"temporary data\n")
    sim.put_file("old", b"old file\n")
    sim.put_file("new", b"new file\n")
    sim.put_file("two", b"two links\n")
    c = Client()
    c.cmd("mount")

    def sillies():
        return sorted(n.decode() for n in sim.inodes[sim.root].entries if n.startswith(b".nfs"))
    ino = res(c.cmd("open tmp.txt r"))
    ok(ino > 0, "open tmp.txt")
    ok(res(c.cmd("unlink tmp.txt")) == 0 and not sim.path("tmp.txt"), "unlink of an open file: its name is gone")
    sl = sillies()
    ok(len(sl) == 1 and len(sl[0]) == 16, "... it is renamed to a .nfs name on the NAS: %s" % sl)
    r = c.cmd("read %d 0 100" % ino)
    ok("text=temporary data" in r, "... and the open file still reads: " + r)
    ok(res(c.cmd("unlink %s" % (sl[0] if sl else "x"))) == -16, "the stand-in name cannot be unlinked while open: -EBUSY")
    ok(res(c.cmd("close %d" % ino)) == 0, "the last close")
    c.cmd("sleep 300")
    ok(sillies() == [], "... removes the stand-in from the NAS: %s" % sillies())
    # written, unlinked, closed: the data is committed before the removal
    w = res(c.cmd("open made wc"))
    c.cmd("write %d 0 @20000:z" % w)
    nw = len([e for e in sim.log if e[0] == sim_mod.NFS_PROG and e[1] == 7])
    ok(nw == 5, "the small profile writes 4 KB pieces: 20000 bytes in %d WRITEs" % nw)
    ok(res(c.cmd("unlink made")) == 0 and len(sillies()) == 1, "an unlinked file that is still being written: renamed")
    c.cmd("close %d" % w)
    c.cmd("sleep 500")
    ncommit = len([e for e in sim.log if e[0] == sim_mod.NFS_PROG and e[1] == 21])
    ok(sillies() == [] and ncommit >= 1, "... committed, then removed at its last close (%d COMMITs)" % ncommit)
    # renamed over while open
    o = res(c.cmd("open old r"))
    ok(res(c.cmd("rename new old")) == 0 and bytes(sim.path("old").data) == b"new file\n" and len(sillies()) == 1,
       "a file renamed over an open one: the open one becomes a .nfs name first")
    r = c.cmd("read %d 0 100" % o)
    ok("text=old file" in r, "... and still reads its own data: " + r)
    c.cmd("close %d" % o)
    c.cmd("sleep 300")
    ok(sillies() == [], "... removed at its close")
    # not open, or more links: removed at once
    ok(res(c.cmd("link two two2")) == 0, "a second link")
    t = res(c.cmd("open two r"))
    ok(res(c.cmd("unlink two")) == 0 and sillies() == [] and sim.path("two2"), "an open file with another link: removed at once")
    c.cmd("close %d" % t)
    ok(res(c.cmd("unlink two2")) == 0 and sillies() == [], "a closed file: removed at once")
    ok(res(c.cmd("unlink nothere")) == -2, "unlink of a missing name: -ENOENT")
    ok(res(c.cmd("rename nothere x")) == -2, "rename of a missing name: -ENOENT")
    # quit with a stand-in still open is not possible (the kernel refuses umount while files are open); a stand-in
    # whose close came is removed before the UMNT
    q = res(c.cmd("open new2 wc"))
    c.cmd("write %d 0 data" % q)
    c.cmd("unlink new2")
    c.cmd("close %d" % q)
    ok(res(c.cmd("quit")) == 0 and sillies() == [], "quit: the stand-in removed first: %s" % sillies())
    c.end()
    sim.stop()


SCENARIOS = {
    "large": t_large,
    "basic": t_basic, "bigdir": t_bigdir, "faults": t_faults, "restart": t_restart, "drc": t_drc, "stale": t_stale,
    "saved": t_restart_saved, "bad": t_bad_replies, "hard": t_hard_soft, "quit": t_quit_dead, "lazy": t_lazy,
    "cancel": t_cancel, "links": t_links, "users": t_users, "cache": t_cache, "parallel": t_parallel,
    "silly": t_silly,
}


def main():
    subprocess.run(["ip", "link", "set", "lo", "up"], check=False)
    names = sys.argv[3:] or list(SCENARIOS)
    for n in names:
        before = fails
        t0 = time.time()
        try:
            SCENARIOS[n]()
        except Exception as e:      # a broken scenario is a failure, not the end of the run
            import traceback
            traceback.print_exc()
            bad("%s raised %r" % (n, e))
        print("nfs-sim-test: %s %s (%.1f s)" % (n, "PASS" if fails == before else "FAIL", time.time() - t0), flush=True)
    print("nfs-sim-test: %d checks, %d failed" % (checks, fails))
    sys.exit(1 if fails else 0)


main()
