#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The PC's side of tools/remote-test.sh (DESIGN D24 12.2): wasabid on Zenix in FS-UAE, reached over SLIP through
zenix-slirp's forwards (TCP PORT and UDP PORT+1 to the Amiga's 1236), with wasabi-check.py's frames and checks.
    wasabi-remote.py PORT RUN WORKDIR [WASABI_CHECKOUT]
RUN  s   the screen (part s, D24 step 3): TEXT, its wait and timeout, KEY text and raw keys (Amiga+F2: console 2's
         login), SCREEN, WINDOWS, SUM before and after a change, GRAB against termtest's own CON_LINE picture at 3 and 4
         planes, a console never used
     c   the stock A1200 (parts c, d, r): the daemon started by hand while the network is down, then everything of
         section 4 but the screen, netd killed under it, the kernel log stream, REBOOT and its guard; after the reboot
         the previous boot's ending replayed, then the owner's own client (when WASABI_CHECKOUT names a checkout of
         Wasabi; skipped otherwise)
     l   the 1 MB machine (part l): a file copy both ways and the memory with a session
     b   init's daemon (part b): started at boot, /dev/kmsg, RESTART started again at once
Each run ends by writing /tmp/pc-done on Zenix (its script then goes on and ends the run). PASS/FAIL lines, the
memory lines ("mem:") for the report; exit status 0 only when every check passed. Standard library only."""
import hashlib, importlib.util, os, shutil, socket, struct, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("wasabi_check", os.path.join(HERE, "wasabi-check.py"))
wc = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(wc)
PORT, RUN, WORK = int(sys.argv[1]), sys.argv[2], os.path.abspath(sys.argv[3])
REAL = sys.argv[4] if len(sys.argv) > 4 else ""
wc.OFF = PORT - 1236
S, errtext, put = wc.S, wc.errtext, wc.put
T = 60                                          # SLIP moves about 11 KB/s: generous socket timeouts


def ok(cond, what):
    wc.check(cond, what)
    if cond:
        print("PASS " + what, flush=True)


def conn(**kw):
    kw.setdefault("timeout", T)
    return wc.Conn(**kw)


def wait_up(limit, why):
    t0 = time.time()
    while time.time() - t0 < limit:
        try:
            return conn(timeout=20)
        except Exception:
            time.sleep(2)
    raise RuntimeError("the daemon did not answer within %d s (%s)" % (limit, why))


def welcome(c):
    n, = struct.unpack_from(">H", c.welcome, 2)
    m, = struct.unpack_from(">H", c.welcome, 4 + n)
    return (c.welcome[4:4 + n].decode("latin-1"), c.welcome[6 + n:6 + n + m].decode("latin-1"),
            struct.unpack_from(">I", c.welcome, 6 + n + m)[0])


def sh(c, cmd, flags=0):
    o, e, rc, _ = wc.run(c, cmd, flags)
    return o.decode("latin-1"), e.decode("latin-1"), rc


def getfile(c, path, limit=30):
    """GET, waiting out "busy" (a transfer the daemon has not yet seen end)"""
    t0 = time.time()
    while True:
        got, e = c.data(wc.GET, S(path))
        if not (e and "busy" in e[1]) or time.time() - t0 > limit:
            return got, e
        time.sleep(1)


def done(c=None):
    """/tmp/pc-done on Zenix: its script goes on and ends the run"""
    try:
        if not c or put(c, "/tmp/pc-done", b"done\n") != "OK":     # a connection that broke: a new one
            c = conn(timeout=20)
            r = put(c, "/tmp/pc-done", b"done\n")
            if r != "OK":
                print("wasabi-remote: could not write /tmp/pc-done: %r" % (r,), flush=True)
        c.close()
    except Exception as e:
        print("wasabi-remote: could not write /tmp/pc-done: %r" % e, flush=True)


def mem(c, what):
    o, e, rc = sh(c, "free; ps")
    chip = [ln for ln in o.splitlines() if ln.startswith("chip RAM free")]
    wd = [" ".join(ln.split()) for ln in o.splitlines() if ln.endswith(" wasabid") or ln.endswith(" netd")]
    print("mem: %s: %s; %s" % (what, chip[0] if chip else "the command did not run (%r %r)" % (rc, e.strip()),
                               "; ".join(wd)), flush=True)
    return bool(chip)


# ---- run c: the stock A1200 ----
def part_c(c):
    banner, caps, refused = welcome(c)
    ok(banner.startswith("Zenix ") and caps == wc.CAPS and refused == 0,
       "WELCOME: banner %r, caps with the screen commands (the kernel has the console operations)" % banner)
    ok(c.simple(wc.PING)[0] == wc.PONG, "PING -> PONG")
    got, e = c.data(wc.GET, S("/tmp/wd1.log"))
    log = got.decode("latin-1")
    ok("the network is down: trying again every 10 s" in log and "the network is up: listening" in log,
       "started while netd was missing (-ENETDOWN): waited, then listened once ifup registered it (%r)" % log[-300:])
    got, e = c.data(wc.INFO)
    t = got.decode("latin-1")
    ok(e is None and t.startswith("Zenix ") and "Emu68: no" in t and "sl0 10.0.2.15" in t and "started by hand" in t
       and wc.KEY not in t, "INFO's lines (%r)" % t)
    got, e = c.data(wc.HEALTH)
    kv = dict(ln.split(" ", 1) for ln in got.decode("latin-1").splitlines() if " " in ln)
    ok(e is None and int(kv.get("mem.chip.free", 0)) > 100000 and kv.get("mem.fast.free") == "0" and
       kv.get("emu68") == "no" and "zenix.version" in kv and "uptime" in kv and int(kv.get("procs", 0)) >= 5 and
       "load.ready" in kv and "net.sl0.rx" in kv and int(kv.get("disk.root.free.kb", -1)) > 0, "HEALTH's keys (%r)" % kv)
    mem(c, "stock A1200, SLIP up, wasabid with one connection running a command")
    # files
    big = os.urandom(100 * 1024 + 17)
    t0 = time.time()
    ok(put(c, "/tmp/big.bin", big) == "OK", "PUT of 100 KB")
    t1 = time.time()
    got, e = c.data(wc.GET, S("/tmp/big.bin"))
    t2 = time.time()
    ok(e is None and got == big, "GET: the same 100 KB back")
    print("wasabi-remote: over SLIP in FS-UAE (emulator figures): PUT %.1f s, GET %.1f s for 100 KB" % (t1 - t0, t2 - t1))
    o, _, rc = sh(c, "sha256sum /tmp/big.bin")
    ok(rc == 0 and o.split()[0] == hashlib.sha256(big).hexdigest(), "sha256sum on Zenix agrees")
    ok(put(c, "/tmp/prog", b"ZEXE" + b"\0" * 60) == "OK", "PUT of a Zenix program")
    o, _, _ = sh(c, "ls -l /tmp/prog")
    ok(o.startswith("-rwxr-xr-x"), "a new Zenix program is 0755 (%r)" % o)
    ok(put(c, "/tmp/keep", b"old contents") == "OK", "PUT of a small file")
    # the cut (audit O11): the daemon must be inside the PUT before the reset, or the GET and LS below could run before
    # it has even read the PUT and see nothing of it, or a .wsb- file of a transfer it starts after them. So the PUT and
    # its first 8 KB go first, the reset only once LS shows its temporary file; then getfile waits until the daemon has
    # seen the reset (no longer "busy")
    k = conn()
    k.s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))   # the close is a reset: cut now
    k.send(wc.PUT, struct.pack(">II", 100 * 1024, 0xFFFFFFFF) + S("/tmp/keep"))
    k.send(wc.DATA, os.urandom(8192))
    t0, inside = time.time(), False
    while not inside and time.time() - t0 < T:
        got, e = c.data(wc.LS, S("/tmp"))
        inside = e is None and b".wsb-" in got
        if not inside:
            time.sleep(0.5)
    ok(inside, "a PUT being sent has its temporary .wsb- file beside the target")
    k.close()
    got, e = getfile(c, "/tmp/keep", limit=T)
    ok(got == b"old contents", "a PUT cut half-way leaves the old file (%r %r)" % (got[:20], e))
    got, e = c.data(wc.LS, S("/tmp"))
    ok(e is None and b".wsb-" not in got, "... and no temporary .wsb- file")
    r = put(c, "/sbin/wasabid", b"evil")
    ok(isinstance(r, tuple) and "running daemon" in r[1], "PUT of /sbin/wasabid refused (%r)" % (r,))
    _, e = getfile(c, "/dev/zero")
    ok(e is not None and "regular" in e[1], "GET of /dev/zero refused (%r)" % (e,))
    _, e = c.data(wc.GET, S("/dev/ttyS0"))
    ok(e is not None and c.simple(wc.PING)[0] == wc.PONG, "GET of /dev/ttyS0 refused, SLIP untouched (%r)" % (e,))
    _, e = c.data(wc.GET, S("/nope"))
    ok(e is not None and e[0] == 205, "GET of a missing file: code 205 (%r)" % (e,))
    got, e = c.data(wc.LS, S("/etc"))
    ls = {ln.split(" ", 6)[6]: ln.split(" ")[:2] for ln in got.decode("latin-1").splitlines()}
    o, _, _ = sh(c, "ls -l /etc")
    want = {ln.split()[-1]: ["d" if ln[0] == "d" else "f", ln.split()[4]] for ln in o.splitlines() if ln[:1] in "-d"}
    ok(e is None and {n: v for n, v in ls.items() if v[0] == "f"} == {n: v for n, v in want.items() if v[0] == "f"} and
       set(ls) == set(want), "LS /etc against ls -l: the names and sizes (%r / %r)" % (ls, want))
    ok(c.simple(wc.MKDIR, S("/tmp/newdir"))[0] == wc.OK, "MKDIR")
    t, p = c.simple(wc.MKDIR, S("/tmp/newdir"))
    ok(t == wc.ERR and errtext(p)[0] == 203, "MKDIR of an existing name: code 203")
    ok(c.simple(wc.DEL, S("/tmp/newdir"))[0] == wc.OK, "DEL of an empty directory")
    t, p = c.simple(wc.DEL, S("/tmp/none"))
    ok(t == wc.ERR and errtext(p)[0] == 205, "DEL of a missing file: code 205")
    # RUN
    o, e, rc, slot = wc.run(c, "echo out; echo err >&2; exit 3", 4)
    ok((o, e, rc) == (b"out\n", b"err\n", 3) and slot in (1, 2), "RUN: stdout, stderr apart, exit 3, the slot")
    o, e, rc, _ = wc.run(c, "echo out; echo err >&2", 1)
    ok(b"out" in o and b"err" in o and e == b"" and rc == 0, "RUN merged: both on STDOUT")
    o, e, rc = sh(c, "crash")
    ok(rc == 139, "RUN of a program that faults: 139 (%r)" % (rc,))
    t0 = time.time()
    o, e, rc = sh(c, "sleep 30 > /dev/null & echo started")
    ok(o == "started\n" and rc == 0 and time.time() - t0 < 8, "RUN 'sleep 30 &': EXIT when sh ends, no slot held")
    o, e, rc = sh(c, "seq 1 300")
    ok(rc == 0 and o.splitlines() == [str(i) for i in range(1, 301)], "RUN with 300 lines of output, every one")
    o, e, rc = sh(c, "echo $HOME $USER $TERM $PATH")
    ok(o == "/root root dumb /bin:/sbin\n", "RUN's environment (%r)" % o)
    # the slots
    a, b = conn(), conn()
    a.send(wc.RUN, struct.pack(">I", 4) + S("sleep 60"))
    b.send(wc.RUN, struct.pack(">I", 4) + S("sleep 60"))
    sa, sb = a.recv(), b.recv()
    _, _, rc = sh(c, "echo no")
    ok(rc[:1] == ("ERR",) and rc[2] == "all run slots are busy", "a third RUN: 'all run slots are busy' (%r)" % (rc,))
    got, e = c.data(wc.SLOTS)
    ok(e is None and len(got.decode().splitlines()) == 2, "SLOTS: the two (%r)" % got)
    got, e = c.data(wc.FREE, struct.pack(">II", struct.unpack(">I", sa[1])[0], 0))
    ok(e is None and got == b"stopped", "FREE: SIGINT, 'stopped' (%r %r)" % (got, e))
    t, p = a.recv()
    ok(t == wc.EXIT and struct.unpack(">I", p[:4])[0] == 130, "the freed command's client gets EXIT 130")
    got, e = c.data(wc.FREE, struct.pack(">II", struct.unpack(">I", sb[1])[0], 1))
    ok(e is None and got == b"asked", "FREE bit 0: 'asked'")
    ok(b.recv()[0] == wc.EXIT, "... and the command ends")
    a.close(); b.close()
    # FREE's "freed": the slot's own process (sh) held in the kernel by a server that never answers (msgpingd
    # -fsstall: sh's start of /tmp/hang/x waits for its open; G4), so SIGKILL cannot end it
    o, e, rc = sh(c, "msgpingd -port fs.hang -fsstall", 2)
    time.sleep(2)
    o, e, rc = sh(c, "mkdir -p /tmp/hang; svctest mount fs.hang /tmp/hang")
    ok("svctest mount /tmp/hang: 0" in o, "a filesystem server that never answers is mounted (%r %r)" % (o, e))
    a = conn()
    a.send(wc.RUN, struct.pack(">I", 4) + S("/tmp/hang/x"))
    n = struct.unpack(">I", a.recv()[1])[0]
    time.sleep(1)
    t0 = time.time()
    got, e = c.data(wc.FREE, struct.pack(">II", n, 0))
    ok(e is None and got == b"freed" and 2.5 < time.time() - t0 < 8,
       "FREE of a command held by a hung server: SIGINT, SIGKILL after 3 s, then 'freed' (%r %r %.1f s)" %
       (got, e, time.time() - t0))
    t, p = a.recv()
    ok(t == wc.ERR and "let go of" in errtext(p)[1], "... its client is told it was let go of (%r)" % (p,))
    a.close()
    got, e = c.data(wc.SLOTS)
    ok(e is None and b"stuck" in got, "SLOTS shows the stuck command (%r)" % got)
    o, e, rc = sh(c, "pkill -KILL -x msgpingd; sleep 2; umount /tmp/hang; echo um=$?")
    ok("um=0" in o, "the hung server killed and unmounted (%r %r)" % (o, e))
    time.sleep(1)
    got, e = c.data(wc.SLOTS)
    ok(e is None and b"stuck" not in got, "the stuck command ended when its server did, and was reaped (%r)" % got)
    # detached
    o, e, rc = sh(c, "sleep 1; echo bg-done", 2)
    ok(rc == 0 and e.startswith("[started in the background; its output goes to /tmp/wasabi-bg-"),
       "RUN detached: the note, EXIT 0 (%r)" % e)
    name = e.split("goes to ")[1].rstrip("]\n") if "goes to " in e else "/nope"
    time.sleep(4)
    got, _ = c.data(wc.GET, S(name))
    ok(got == b"bg-done\n", "the detached command's output file (%r)" % got)
    # PS and KILL
    got, e = c.data(wc.PS)
    names = [ln.split(" ", 6)[6].split("\t")[0] for ln in got.decode("latin-1").splitlines()]
    ok(e is None and {"boot", "init", "netd", "wasabid"} <= set(names), "PS: the processes (%s)" % names)
    kill = lambda tg, f=0: c.simple(wc.KILL, struct.pack(">I", f) + S(tg))
    t, p = kill("init")
    ok(t == wc.ERR and "never" in errtext(p)[1], "KILL init refused")
    t, p = kill("wasabid")
    ok(t == wc.ERR and "itself" in errtext(p)[1], "KILL of the daemon refused")
    t, p = kill("netd")
    ok(t == wc.ERR and "--force" in errtext(p)[1], "KILL netd without the flag refused")
    # a wrong key and the pause
    w = conn(hello=False)
    t, p = w.simple(wc.HELLO, struct.pack(">H", 1) + S("wrong-key-0123456789"))
    ok(t == wc.ERR and errtext(p) == (0, "bad key") and w.closed(), "a wrong key: ERR 'bad key', closed")
    t0 = time.time()
    w = conn()
    ok(0.9 <= time.time() - t0 < 6 and welcome(w)[2] == 1, "the next HELLO waits about 1 s; refused 1 in WELCOME")
    w.close()
    # the fifth connection, the 10 s rule, discovery
    cs = [conn() for _ in range(3)]
    c5 = conn(hello=False)
    t, p = c5.recv()
    ok(t == wc.ERR and errtext(p)[1] == "busy" and c5.closed(), "a fifth connection: ERR 'busy', closed")
    for x in cs:
        x.close()
    time.sleep(1)
    h = conn()
    h.s.sendall(struct.pack(">BI", wc.INFO, 20)[:3])
    t0 = time.time()
    ok(h.closed(20) and 9 < time.time() - t0 < 16, "half a frame: dropped after 10 s (%.1f s)" % (time.time() - t0))
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.settimeout(5)
    u.sendto(b"WASABI?1\n", ("127.0.0.1", PORT + 1))
    try:
        r = u.recv(200)
    except socket.timeout:
        r = b""
    u.close()
    ok(r.startswith(b"WASABI!1 zenix 1236 Zenix "), "discovery on UDP 1236 (%r)" % r)


def part_netd():
    """netd killed under the daemon: its sockets die (D19 rule 9), it listens again, which starts netd again"""
    c = conn()
    try:
        c.simple(wc.KILL, struct.pack(">I", 1) + S("netd"))         # the answer may die with the network
    except Exception:
        pass
    c.close()
    t0 = time.time()
    c = wait_up(120, "after netd was killed")
    ok(time.time() - t0 < 90, "netd killed: the daemon listens again and is reached (%.0f s)" % (time.time() - t0))
    got, e = c.data(wc.GET, S("/tmp/wd1.log"))
    log = got.decode("latin-1")
    ok("the network server went away" in log and log.count("the network is up: listening") >= 2,
       "... its log: the network server went away, listening again (%r)" % log[-300:])
    return c


def part_d1():
    s = conn()
    s.send(wc.DEBUG, struct.pack(">I", 0))
    time.sleep(3)                               # the subscription starts at the log's end: let it start first
    c = conn()
    sh(c, "echo remote-marker-1 > /dev/kmsg")
    c.close()
    seen, t0 = [], time.time()
    while time.time() - t0 < 20:
        t, p = s.recv()
        seen.append((t, p[10:]))
        if t == wc.LOG and p[10:] == b"remote-marker-1\n":
            break
    ok(seen and seen[-1][1] == b"remote-marker-1\n", "DEBUG: a line written to /dev/kmsg arrives (%r)" % seen[-3:])
    t0 = time.time()
    hb = False
    while time.time() - t0 < 12:
        t, p = s.recv()
        if t == wc.LOG and p[10:] == b"":
            hb = True
            break
    ok(hb, "a heartbeat (an empty LOG) within 12 s")
    lost = []

    def writer():
        k = conn()
        sh(k, "yes wrap-line-padding-padding-padding-padding | head -n 1500 > /dev/kmsg")
        k.close()
    import threading
    th = threading.Thread(target=writer)
    th.start()
    t0 = time.time()
    while time.time() - t0 < 90:
        t, p = s.recv()
        if t == wc.LOG and p[10:].startswith(b"[wasabi: ") and b"bytes of the kernel log were lost]" in p[10:]:
            lost.append(p[10:])
            break
    th.join()
    ok(lost, "56 KB written to the 8 KB log faster than SLIP carries it: the loss is said (%r)" % lost)
    return s


def part_reboot(s):
    c = conn()                                  # (an idle connection is dropped after 10 s: D24 3.2)
    sh(c, "cp /bin/sleep /tmp/zup; chmod 755 /tmp/zup; /tmp/zup 120", 2)
    time.sleep(3)
    o, _, _ = sh(c, "pgrep -x zup")
    ok(o.strip().isdigit(), "a process named zup runs (%r)" % o)
    t, p = c.simple(wc.REBOOT, struct.pack(">I", 2))
    ok(t == wc.ERR and "zup is running (pid" in errtext(p)[1], "REBOOT while a zup runs: the guard (%r)" % (p,))
    o, e, rc = sh(c, "pkill -x zup; sleep 1; ls /var/lib/random-seed")
    ok(rc == 0, "the random seed file is there before the reboot (%r %r)" % (o, e))
    t, p = c.simple(wc.REBOOT, struct.pack(">I", 2))
    ok(t == wc.OK, "REBOOT with the AmigaOS bit: OK")
    farewell = False
    try:
        t0 = time.time()
        while time.time() - t0 < 15:
            t, p = s.recv()
            if t is None:
                break
            if t == wc.LOG and b"rebooting - closing this stream" in p:
                farewell = True
                break
    except Exception:
        pass
    ok(farewell, "... the farewell on the log stream")
    t0 = time.time()
    try:
        x = conn(hello=False, timeout=10)
        t, p = x.recv()
        busy = t == wc.ERR and errtext(p)[1] == "busy"
    except Exception:
        busy = True                             # already gone
    ok(busy, "a connection while the reboot is under way: 'busy' (or the machine already gone)")
    time.sleep(20)                              # past the old daemon: the next answer is the new boot's


def part_c2():
    c = wait_up(400, "after the reboot")
    s = conn()
    s.send(wc.DEBUG, struct.pack(">I", 0))
    t, p = s.recv()
    ok(t == wc.LOG and p[10:].startswith(b"=== previous boot (1) ended here with reboot"),
       "after the reboot, a new subscriber first gets the previous boot's ending (%r)" % p[10:])
    s.close()
    o, _, _ = sh(c, "dmesg -p | grep -c 'resetting into AmigaOS'")
    ok(o.strip() == "1", "the previous boot ended through /bin/reboot amigaos (the boot note set: %r)" % o)
    return c


def part_r(port):
    if not REAL:
        print("wasabi-remote: the real client skipped (no Wasabi checkout given: WASABI)", flush=True)
        return
    home = os.path.join(WORK, "real-home")
    shutil.rmtree(home, ignore_errors=True)
    os.makedirs(home)
    env = dict(os.environ, HOME=home, XDG_CONFIG_HOME=home, XDG_CACHE_HOME=home, WASABI_KEY="")
    base = [sys.executable, os.path.join(REAL, "wasabi"), "--host", "127.0.0.1", "--port", str(port), "--key", wc.KEY]
    local = os.path.join(WORK, "real-put.bin")
    blob = os.urandom(20000)
    open(local, "wb").write(blob)
    back = os.path.join(WORK, "real-get.bin")
    for args, want in ((["ping"], "Zenix"), (["run", "echo from-the-real-client"], "from-the-real-client"),
                       (["put", local, "/tmp/real.bin"], ""), (["get", "/tmp/real.bin", back], ""),
                       (["ls", "/tmp"], "real.bin"), (["ps"], "wasabid"), (["info"], "Zenix"),
                       (["health"], ""), (["windows"], "tty1"), (["sum"], ""),
                       (["grab", "--raw", os.path.join(WORK, "real-grab.png")], "")):
        r = subprocess.run(base + args, capture_output=True, text=True, env=env, timeout=180)
        extra = True
        if args[0] == "get":
            extra = os.path.exists(back) and open(back, "rb").read() == blob
        if args[0] == "grab":
            g = os.path.join(WORK, "real-grab.png")
            extra = os.path.exists(g) and open(g, "rb").read(8) == b"\x89PNG\r\n\x1a\n"
        ok(r.returncode == 0 and want in r.stdout and extra, "the real client: %s (%d %r %r)" %
           (args[0], r.returncode, r.stdout[-200:], r.stderr[-300:]))


# ---- run s: the screen ----
def text(c, flags=0, con=0, tmo=0, pat=""):
    return c.data(wc.TEXT, struct.pack(">III", flags, con, tmo) + S(pat))


def rows_of(got):
    r, k = struct.unpack(">HH", got[:4])
    g = got[16:]
    return [g[i * k:(i + 1) * k].decode("latin-1").rstrip() for i in range(r)]


def zgrab(c, path):
    """termtest's own picture of the display (CON_LINE): ZGRB, w, h, depth, 16 palette words, a pen a pixel"""
    o, e, rc = sh(c, "termtest grab %s" % path)
    got, e2 = getfile(c, path, 120)
    if rc or e2 or got[:4] != b"ZGRB":
        return None
    w, h, d = struct.unpack(">HHH", got[4:10])
    pal = [struct.unpack(">H", got[10 + 2 * i:12 + 2 * i])[0] for i in range(16)]
    rgb = [bytes(((p >> s) & 15) * 17 for s in (8, 4, 0)) for p in pal]
    return w, h, d, b"".join(rgb[x & 15] for x in got[42:42 + w * h])


def part_s(c):
    banner, caps, refused = welcome(c)
    ok(caps == wc.CAPS, "WELCOME: caps with the screen commands")
    got, e = text(c)
    rows, cols, crow, ccol, ch = struct.unpack(">HHHHI", got[:12])
    ok(e is None and (rows, cols) == (32, 80) and got[12] == 1 and got[14] == 1 and len(got) == 16 + 32 * 80,
       "TEXT: console 1 shown, its header and a glyph a cell")
    got2, e = text(c, 1)                                  # WX_CELLS
    ok(e is None and len(got2) == 16 + 32 * 80 * 3 and got2[16::3][:80] == got[16:96],
       "TEXT with the cells: 3 bytes each, the same glyphs")
    ok(c.simple(wc.KEYT, struct.pack(">H", 1) + b"echo hello-s\n")[0] == wc.OK, "KEY text")
    got, e = text(c, 0, 0, 10, "echo hello-s")
    ok(e is None and any(r.endswith("echo hello-s") for r in rows_of(got)), "TEXT waiting: the typed line is on the screen")
    d = conn()
    o, er, rc, _ = wc.run(d, "sleep 3; echo s-marker-$((6*7)) > /dev/tty1", 2)
    d.close()
    t0 = time.time()
    got, e = text(c, 0, 1, 30, "^s-marker-42$")
    ok(e is None and "s-marker-42" in rows_of(got) and time.time() - t0 > 1.5,
       "TEXT waiting for a pattern that appears later (%.1f s)" % (time.time() - t0))
    t0 = time.time()
    got, e = text(c, 0, 1, 2, "never-there")
    ok(e is not None and "timeout" in e[1] and len(got) == 16 + 2560 and 1.5 < time.time() - t0 < 10,
       "TEXT's timeout: the screen, then ERR timeout (%r)" % (e,))
    for k in range(2):
        _, e = text(c, 0, 9)
        ok(e is not None and "never used" in e[1], "TEXT of a console never used: ERR (%d, nothing allocated)" % k)
    got, e = c.data(wc.WINDOWS)
    ok(e is None and got == b"S\ttty1\t0\t0\t640\t256\t3\ttty1\n", "WINDOWS: one S line (%r)" % got)
    s1, e1 = c.data(wc.SUM, struct.pack(">hhhh", 0, 0, 0, 0))
    s2, e2 = c.data(wc.SUM, struct.pack(">hhhh", 0, 0, 0, 0))
    ok(e1 is None and s1 == s2 and struct.unpack(">II", s1)[1] == 640 * 256, "SUM of a quiet screen twice: the same")
    for depth in (3, 4):
        if depth == 4:
            o, _, rc = sh(c, "termtest depth 16")
            ok("colours 16" in o, "the display to 16 colours (%r)" % o)
        z = zgrab(c, "/tmp/s.zg")
        got, e = c.data(wc.GRAB, S(""))
        z2 = zgrab(c, "/tmp/s.zg")
        same = z is not None and z == z2 and e is None and z[2] == depth and got[:12] == struct.pack(">III", 640, 256, 3)
        ok(same and got[12:] == z[3], "GRAB at %d planes: every pixel as termtest's CON_LINE picture of the same quiet "
           "moment (%s)" % (depth, e))
        if depth == 4:
            got, e = c.data(wc.WINDOWS)
            ok(e is None and got.endswith(b"\t4\ttty1\n"), "WINDOWS: the depth 4 now (%r)" % got)
    sh(c, "termtest depth 8")
    ok(c.simple(wc.KEYT, struct.pack(">H", 1) + b"# x\n")[0] == wc.OK, "KEY text again")
    s3, e3 = c.data(wc.SUM, struct.pack(">hhhh", 0, 0, 0, 0))
    ok(e3 is None and s3 != s1, "SUM after a change: different")
    # raw keys: Amiga+F2 shows console 2, whose login prompt TEXT waits for
    t, p = c.simple(wc.KEYT, struct.pack(">HHHHHHHHH", 0, 0x66, 0, 0x51, 0, 0xD1, 0, 0xE6, 0))
    ok(t == wc.OK, "KEY raw codes: Amiga+F2")
    got, e = text(c, 0, 2, 30, "login:")
    ok(e is None and got[12] == 1 and any("login:" in r for r in rows_of(got)), "TEXT of console 2: shown, its login prompt")
    got, e = c.data(wc.SCREENT, struct.pack(">I", 0) + S(""))
    ok(e is None and got.startswith(b"tty2 640 256 3 tty2\n") and b"tty1 640 256 3 tty1\n" in got,
       "SCREEN: the consoles, front first (%r)" % got)
    got, e = c.data(wc.SCREENT, struct.pack(">I", 0) + S("tty1"))
    got, e = text(c)
    ok(e is None and got[14] == 1, "SCREEN tty1: console 1 in front again")
    rec = lambda k, wait, a: struct.pack(">BBHHHhh", k, 0, wait, a, 0, 0, 0)
    t, p = c.simple(wc.INPUTS, rec(1, 2, ord("#")) + rec(0, 2, 0x60) + rec(0, 2, 0x20) + rec(0, 2, 0xA0) + rec(0, 0, 0xE0)
                    + rec(1, 0, ord("\n")))
    got, e = text(c, 0, 1, 10, "^#A$")
    ok(t == wc.OK and e is None, "INPUTS: a character, Shift held across raw events (A), Return (%r %r)" % (t, e))
    t0 = time.time()
    got, e = text(c, 0, 1, 14, "never-there")
    ok(e is not None and "timeout" in e[1] and 13 < time.time() - t0 < 30,
       "TEXT waiting 14 s (longer than the 10 s progress rule): ERR timeout (%r, %.1f s)" % (e, time.time() - t0))


# ---- run l: the 1 MB machine ----
def part_l(c):
    big = os.urandom(100 * 1024)
    ok(put(c, "/tmp/big.bin", big) == "OK", "1 MB machine: PUT of 100 KB")
    got, e = c.data(wc.GET, S("/tmp/big.bin"))
    ok(e is None and got == big, "1 MB machine: GET the same 100 KB back")
    mem(c, "1 MB chip RAM, SLIP up, wasabid with one connection running a command")


# ---- run b: init's daemon ----
def part_b(c):
    banner, caps, refused = welcome(c)
    ok(banner.startswith("Zenix "), "the daemon init started at boot answers, nobody logged in to start it")
    o, _, _ = sh(c, "ps; dmesg")
    procs = {ln.split()[-1]: ln.split() for ln in o.splitlines() if ln[:5].strip().isdigit()}
    initpid = procs.get("init", ["?"])[0]
    ok(procs.get("wasabid", ["", ""])[1] == initpid, "wasabid is init's child (%r, init %s)" % (procs.get("wasabid"), initpid))
    ok("init: wasabid: started /sbin/wasabid --init (pid" in o and "wasabid: started: port 1236, name zenix, under init" in o,
       "its start and its own lines are in the kernel log")
    got, e = c.data(wc.INFO)
    ok(e is None and "under init" in got.decode("latin-1"), "INFO says it runs under init")
    o, e, rc = sh(c, "echo kmsg-marker-b > /dev/kmsg; dmesg | grep -c kmsg-marker-b")
    ok(o.strip() == "1", "root's line written to /dev/kmsg is in dmesg (%r %r)" % (o, e))
    o, e, rc = sh(c, "echo 'echo nope > /dev/kmsg; echo st=$?' | su user")
    ok("st=1" in o + e, "a user may not open /dev/kmsg (%r %r)" % (o, e))
    o, e, rc = sh(c, "cat /dev/kmsg; echo st=$?")
    ok("st=1" in o, "/dev/kmsg is not opened for reading (%r %r)" % (o, e))
    t, p = c.simple(wc.RESTART)
    ok(t == wc.OK, "RESTART under init: OK")
    t0 = time.time()
    time.sleep(2)
    c = wait_up(60, "after RESTART")
    ok(time.time() - t0 < 30, "... started again at once (answers after %.0f s)" % (time.time() - t0))
    o, _, _ = sh(c, "dmesg | grep 'init: wasabid'")
    ok("ended, status 75 (start me again)" in o and o.count("init: wasabid: started") == 2,
       "init logged the status 75 and the new start (%r)" % o)
    for i in range(5):              # B2-5: a 75 after 5 s of running is not counted toward "given up"
        time.sleep(max(0, t0 + 6.5 - time.time()))
        t, p = c.simple(wc.RESTART)
        ok(t == wc.OK, "RESTART %d of five more, each after 5 s of running: OK" % (i + 1))
        t0 = time.time()
        time.sleep(2)
        c = wait_up(60, "after RESTART %d" % (i + 1))
    o, _, _ = sh(c, "dmesg | grep 'init: wasabid'")
    ok(o.count("init: wasabid: started") == 7 and "given up" not in o,
       "six RESTARTs within a minute after 5 s each: started again every time, never given up (%r)" % o)
    return c


def main():
    os.makedirs(WORK, exist_ok=True)
    c = None
    try:
        if RUN == "c":
            c = wait_up(500, "the first boot")
            part_c(c)
            c.close()
            c = part_netd()
            c.close()
            s = part_d1()
            part_reboot(s)
            c = part_c2()
            part_r(PORT)
        elif RUN == "s":
            c = wait_up(500, "the boot")
            part_s(c)
        elif RUN == "l":
            c = wait_up(500, "the 1 MB machine's boot")
            part_l(c)
        elif RUN == "b":
            c = wait_up(500, "the boot")
            c = part_b(c)
        else:
            print("wasabi-remote: no run called %s" % RUN)
            return 2
    except Exception as e:
        import traceback
        traceback.print_exc()
        wc.check(False, "the %s run raised %r" % (RUN, e))
        c = None
    done(c)
    print("wasabi-remote: %d checks, %d failed" % (wc.passes + wc.fails, wc.fails), flush=True)
    return 1 if wc.fails else 0


if __name__ == "__main__":
    sys.exit(main())
