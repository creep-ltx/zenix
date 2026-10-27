#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""telnet's core against the BBS simulator (DESIGN D20 8.1), on the PC: tools/host/telnet-client (net/telnet/ and
user/xfer/) calls tools/bbs-sim.py in each mode; every screen the filter hands on is compared with bbs-sim's EXPECT,
the board's record is checked (the negotiation, the keys, the answers), and Zmodem runs both ways with the real
lsz/lrz through telnet, with BINARY and without it (-e), with a resume after ZCRC and a refused splice.
    telnet-sim-test.py CLIENT LSZ LRZ WORKDIR"""
import hashlib, importlib.util, json, os, shutil, subprocess, sys, time

client, lsz, lrz, work = sys.argv[1:5]
here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("bbs_sim", os.path.join(here, "..", "bbs-sim.py"))
sim = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sim)
fails = 0


def check(ok, what):
    global fails
    print(("PASS" if ok else "FAIL") + " sim: " + what)
    fails += 0 if ok else 1


def sha(p):
    return hashlib.sha256(open(p, "rb").read()).hexdigest()


shutil.rmtree(work, ignore_errors=True)
files = os.path.join(work, "files")
os.makedirs(files)
# test files: CR NUL, 0xFF, CAN and a Zmodem header inside the data, every byte value, and random bytes
crnul = (b"line\r\x00\r\n\xff\xff\x18\x18**\x18B00\r\x00\x00" * 3000)[:50000]
open(os.path.join(files, "crnul.bin"), "wb").write(crnul)
open(os.path.join(files, "bytes.bin"), "wb").write(bytes(range(256)) * 8)
seed, rnd = 12345, bytearray()
for i in range(300000):
    seed = (seed * 1103515245 + 12345) & 0xFFFFFFFF
    rnd.append(seed >> 16 & 255)
open(os.path.join(files, "random.bin"), "wb").write(bytes(rnd))


def call(name, mode, steps, refuse_binary=False, uploads=(), prepare=None):
    """one call: the board started, the client run with its script; returns (record, screen, events, xfer, outdir)"""
    out = os.path.join(work, name)
    os.makedirs(os.path.join(out, "down"))
    if prepare:
        prepare(os.path.join(out, "down"))
    ready, rec = os.path.join(out, "ready"), os.path.join(out, "record.jsonl")
    argv = [sys.executable, os.path.join(here, "..", "bbs-sim.py"), "--port", "0", "--ready", ready, "--record", rec,
            "--files", files, "--upload", os.path.join(out, "up"), "--lsz", lsz, "--lrz", lrz, "--once"]
    if refuse_binary:
        argv.append("--refuse-binary")
    board = subprocess.Popen(argv)
    try:
        for _ in range(100):
            if os.path.exists(ready) and open(ready).read().strip():
                break
            time.sleep(0.05)
        port = open(ready).read().strip()
        script = os.path.join(out, "script")
        open(script, "w").write("\n".join(steps) + "\n")
        r = subprocess.run([client, "127.0.0.1", port, mode, script, out] + list(uploads), timeout=300)
        check(r.returncode == 0, "%s: the client ran its script (exit %d)" % (name, r.returncode))
        board.wait(timeout=30)
    finally:
        if board.poll() is None:
            board.kill()
            board.wait()
    record = [json.loads(l) for l in open(rec)]
    screen = open(os.path.join(out, "screen.bin"), "rb").read()
    events = open(os.path.join(out, "events.txt")).read().split("\n")
    xfer = open(os.path.join(out, "xfer.txt")).read()
    return record, screen, [e for e in events if e], xfer, out


def screens_of(screen):
    """the screen's pieces by the board's markers: {name: bytes}"""
    got, pos = {}, 0
    while True:
        i = screen.find(b"\r\n<<END ", pos)
        if i < 0:
            return got
        j = screen.index(b">>\r\n", i)
        got.setdefault(screen[i + 8:j].decode(), []).append(screen[pos:i])
        pos = j + 4


def negotiation(name, record, ttype, binary):
    kinds = [r["ev"] for r in record]
    check("loop" not in kinds, "%s: no option command more than twice (no loop)" % name)
    check("client_iac_first" not in kinds, "%s: nothing negotiated before the board's first IAC (port %d)" %
          (name, record[0]["port"]))
    check([r["name"] for r in record if r["ev"] == "ttype"] == [ttype, ttype], "%s: TTYPE %s, twice the same" % (name, ttype))
    naws = [r for r in record if r["ev"] == "naws"]
    check(len(naws) == 1 and naws[0]["cols"] == 80 and naws[0]["rows"] == 32, "%s: NAWS 80 x 32, once" % name)
    last = [r for r in record if r["ev"] == "agreed"][-1]
    us, him = last["us"], last["him"]
    check(bool(us.get("1")) and bool(us.get("3")) and him.get("24") and him.get("31"),
          "%s: the board echoes (ECHO), SGA, TTYPE and NAWS agreed" % name)
    check(bool(us.get("0")) == binary and bool(him.get("0")) == binary, "%s: BINARY %s both ways" %
          (name, "on" if binary else "refused"))
    opts = {(r["verb"], r["opt"]) for r in record if r["ev"] == "opt"}
    check(not any(v == "WILL" and o == 1 for v, o in opts), "%s: the client never offers to echo" % name)


def keys_sent(record):
    return bytes.fromhex("".join(r["hex"] for r in record if r["ev"] == "keys"))


def screens_check(name, mode, screen, events, names):
    got = screens_of(screen)
    evs = list(events)
    for n in names:
        want, wev = sim.expect(n, mode)
        check(got.get(n, [None])[0] == want, "%s: %s on the screen as D20's filter makes it" % (name, n))
        for e in wev:
            line = "%s %s" % e
            check(line in evs, "%s: %s: event %s" % (name, n, line))
            if line in evs:
                evs.remove(line)


# ---- an Amiga board, BINARY on ----
amiga_screens = ["latin1", "colours", "ice", "clear2j", "wrap", "lf", "cursor", "dsr", "music", "ff", "sub", "amiga",
                 "iac", "amigaart"]
steps = ["wait <<END hello>>"]
for n in amiga_screens:
    steps += ["line " + n, "wait <<END %s>>" % n]
steps += ["key \\xe5\\xe4\\xf6\\x7f\\e[3~\\e[A", "line", "line font", "wait <<END font>>",
          "key \\xe5\\xa4", "line",
          "line sz crnul.bin random.bin bytes.bin", "wait <<END sz>>",
          "line rz", "wait <<END rz>>", "line bye", "eof"]
ups = [os.path.join(files, n) for n in ("crnul.bin", "random.bin", "bytes.bin")]
record, screen, events, xfer, out = call("amiga", "amiga", steps, uploads=ups)
negotiation("amiga", record, "ANSI", True)
screens_check("amiga", "amiga", screen, events, amiga_screens + ["font"])
check([r["answer"] for r in record if r["ev"] == "dsr"] == ["\x1b[12;34R"], "amiga: ESC[6n reached the console, its answer the board")
check([r["answer"] for r in record if r["ev"] == "wbr"] == ["\x9b1;1;32;80 r"], "amiga: CSI 0 SP q answered 0x9B 1;1;32;80 SP r")
keys = keys_sent(record)
check(b"\xe5\xe4\xf6\x08\x7f\x1b[A\r" in keys, "amiga: keys: Latin-1 as given, Backspace BS, Del DEL, arrows as given")
check(b"\x86\r" in keys and "bell 1" in events, "amiga: after the board's font switch to PC: a-ring as CP437, the currency sign rings")
for n in ("crnul.bin", "random.bin", "bytes.bin"):
    p = os.path.join(out, "down", n)
    check(os.path.exists(p) and sha(p) == sha(os.path.join(files, n)), "amiga: Zmodem from lsz: %s the same" % n)
x = [r for r in record if r["ev"] == "xfer"]
check(len(x) == 2 and x[0]["rc"] == 0 and x[1]["rc"] == 0, "amiga: lsz and lrz on the board ended well")
check(len(x) == 2 and all(x[1]["files"].get(n) == sha(os.path.join(files, n)) for n in ("crnul.bin", "random.bin", "bytes.bin")),
      "amiga: Zmodem to lrz: the three files the same")
check("recv 3 escctl 0" in xfer and "send 3 escctl 0" in xfer, "amiga: BINARY on: no -e either way")

# ---- a PC board that refuses BINARY: -e, a resume after ZCRC, a refused splice ----
pc_screens = ["grid", "colours", "ice", "clear2j", "wrap", "lf", "cursor", "dsr", "music", "ff", "sub", "amiga", "iac",
              "art"]


def prepare(down):
    open(os.path.join(down, "random.bin.part"), "wb").write(bytes(rnd[:100000]))       # the same file: resumed
    open(os.path.join(down, "crnul.bin.part"), "wb").write(b"another file" * 1000)     # another: not spliced


steps = ["wait <<END hello>>"]
for n in pc_screens:
    steps += ["line " + n, "wait <<END %s>>" % n]
steps += ["key \\xe5\\xe4\\xf6\\xc5\\xc4\\xd6\\x7f\\e[3~", "line",
          "line sz crnul.bin random.bin", "wait <<END sz>>",
          "line sz -e bytes.bin", "wait <<END sz>>",
          "line rz", "wait <<END rz>>", "line bye", "eof"]
record, screen, events, xfer, out = call("pc", "pc", steps, refuse_binary=True, uploads=ups, prepare=prepare)
negotiation("pc", record, "ANSI", False)
screens_check("pc", "pc", screen, events, pc_screens)
check(not [r for r in record if r["ev"] == "wbr"], "pc: the Amiga's CSI 0 SP q is not answered in the PC mode")
check(b"\x86\x84\x94\x8f\x8e\x99\x08\x7f\r" in keys_sent(record), "pc: keys: Swedish letters as CP437, BS, DEL")
d = os.path.join(out, "down")
check(sha(os.path.join(d, "random.bin")) == sha(os.path.join(files, "random.bin")) and
      not os.path.exists(os.path.join(d, "random.bin.part")) and "say 4 random.bin random.bin.part" in xfer,
      "pc: random.bin resumed from its part after ZCRC, whole")
check(sha(os.path.join(d, "crnul.bin.1")) == sha(os.path.join(files, "crnul.bin")) and
      open(os.path.join(d, "crnul.bin.part"), "rb").read() == b"another file" * 1000 and
      not os.path.exists(os.path.join(d, "crnul.bin")) and "say 5 crnul.bin crnul.bin.part" in xfer,
      "pc: another file's crnul.bin.part not spliced: kept, the file came as crnul.bin.1 (CR NUL intact with -e)")
check(sha(os.path.join(d, "bytes.bin")) == sha(os.path.join(files, "bytes.bin")), "pc: sz -e bytes.bin the same")
x = [r for r in record if r["ev"] == "xfer"]
check(len(x) == 3 and all(r["rc"] == 0 for r in x), "pc: lsz (twice) and lrz on the board ended well")
check(len(x) == 3 and all(x[2]["files"].get(n) == sha(os.path.join(files, n)) for n in ("crnul.bin", "random.bin", "bytes.bin")),
      "pc: Zmodem to lrz without BINARY (-e): the three files the same")
check("recv 2 escctl 1" in xfer and "send 3 escctl 1" in xfer, "pc: BINARY refused: -e both ways")

# ---- a Unix host ----
steps = ["wait <<END hello>>", "line unix", "wait <<END unix>>", "line colours", "wait <<END colours>>",
         "key \\xe5\\x7f\\e[3~", "line", "line ff", "wait <<END ff>>", "line bye", "eof"]
record, screen, events, xfer, out = call("unix", "unix", steps)
negotiation("unix", record, "XTERM-16COLOR", True)
screens_check("unix", "unix", screen, events, ["unix", "colours"])
check(screens_of(screen).get("ff", [None])[0] == b"a\x0cb", "unix: FF as it is (no BBS rewrite)")
check(b"\xc3\xa5\x7f\x1b[3~\r" in keys_sent(record), "unix: keys: a-ring as UTF-8, Backspace DEL, Del as given")

shutil.rmtree(work, ignore_errors=True)
print("sim: %s" % ("all passed" if not fails else "%d FAILED" % fails))
sys.exit(1 if fails else 0)
