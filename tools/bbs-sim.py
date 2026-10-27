#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""A BBS for telnet's tests (DESIGN D20 8.1): a telnet server that negotiates as a board does and records what the
client asked and answered, then sends test screens on command, and runs Zmodem with the real lsz/lrz.
    bbs-sim.py [--host ADDR] [--port N] [--record FILE] [--ready FILE] [--files DIR] [--upload DIR]
               [--lsz PATH] [--lrz PATH] [--refuse-binary] [--iac-delay MS] [--once]
    bbs-sim.py --self-test [--lsz PATH --lrz PATH]
It listens on ADDR:N (127.0.0.1, 2323; port 0 picks one and writes it to the READY file, which is written when it
listens) and serves one caller at a time until killed (--once: one call). Nothing leaves the PC; no root.

Negotiation: on port 23 at once, on any other port after --iac-delay ms (default 300), so a client that negotiates
before the board's first IAC is caught: WILL ECHO, WILL SGA, DO TTYPE, DO NAWS, WILL BINARY, DO BINARY (--refuse-binary:
neither; the client's BINARY asks are refused), TTYPE SEND twice. The record (--record, JSON lines, appended and
flushed as it happens) has: "opt" (every option command received: verb, opt), "client_iac_first", "ttype" (each
name), "naws" (cols, rows, the raw bytes: a 255 must come doubled), "agreed" (per option, ours and theirs, after each
change), "keys" (the data bytes received, hex, as they came, CR NUL folded unless the client's BINARY is on), "line"
(each command line), "dsr" (the answer to ESC[6n), "wbr" (the answer to the Amiga's window bounds request
CSI 0 SP q), "xfer" (a transfer's command, lrzsz's exit status, sha256 of the files received), "loop" (an option
command received more than twice: a negotiation loop), "end".

Commands, typed as a line ending in CR (or LF) - each screen ends with CR LF "<<END name>>" CR LF:
  menu      the list               grid      every CP437 byte 1-255 but BEL BS HT LF FF CR SUB ESC (those act)
  latin1    Latin-1 0x20-0x7E and 0xA0-0xFF                colours   the 16 colours, SGR 1 alone and with a colour
  ice       iCE: CSI ? 33 h, blinking backgrounds, CSI ? 33 l   clear2j  ESC[2J without ESC[H, then text
  wrap      a full 80-column row, then a cursor move       lf        LF without CR
  cursor    cursor codes (CUP, CUU/CUD/CUF/CUB, save/restore, EL)
  dsr       ESC[6n: the client's answer is recorded        music     ANSI music strings (BANSI, CSI = 2 M, SO,
                                                                     broken by text, unterminated)
  ff        a form feed between text                       sub       a SUB between text
  font      SyncTERM's font switch: CSI SP D, 0;42, 1;0 (swallowed), 0;0
  amiga     an Amiga screen: Latin-1, 0x9B CSI, CSI n u, cursor off/on, CSI 0 SP q (the answer is recorded)
  iac       text with 0xFF in it (sent doubled)            unix      UTF-8 text and a box line
  art       the hand-made CP437 screen (tools/testdata/bbs-sim/welcome.ans)
  amigaart  the hand-made Latin-1 screen (tools/testdata/bbs-sim/amiga.asc)
  ttype     TTYPE SEND once more                           bye       the board hangs up
  sz [-e] [-r] NAME...   the board sends files from --files with lsz (-e: escape controls; -r: ask for a resume)
  rz [-e]                the board receives into --upload with lrz
The screens are made here or drawn for the test (tools/testdata/bbs-sim/): never real art packs (they are
copyrighted; local art may be used by hand, never committed). EXPECT below says what a D20 filter makes of each."""
import argparse, hashlib, json, os, re, select, socket, subprocess, sys, tempfile, threading, time

IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240
BINARY, ECHO, SGA, TTYPE, NAWS = 0, 1, 3, 24, 31
NAMES = {WILL: "WILL", WONT: "WONT", DO: "DO", DONT: "DONT"}
HERE = os.path.dirname(os.path.abspath(__file__))
E = b"\x1b["
ANS_CPR = re.compile(rb"\x1b\[[0-9]+;[0-9]+R")
ANS_WBR = re.compile(rb"\x9b1;1;[0-9]+;[0-9]+ r")


def screens():
    s = {}
    g = b""
    acts = {7, 8, 9, 10, 12, 13, 26, 27}
    for row in range(16):
        g += b"%X: " % row
        for col in range(16):
            b = row * 16 + col
            g += b" " if b == 0 or b in acts else bytes([b])
        g += b"\r\n"
    s["grid"] = g
    lat = bytes(range(0x20, 0x7F)) + b"\r\n" + bytes(range(0xA0, 0x100)) + b"\r\n"
    s["latin1"] = lat
    c = b""
    for i in range(8):
        c += E + b"0;3%dm dark %d " % (i, i) + E + b"1;3%dm bright %d " % (i, i) + E + b"0m"
        c += E + b"1m" + E + b"3%dm 1 then %d " % (i, i) + E + b"0;4%dm bg %d " % (i, i) + E + b"0m\r\n"
    s["colours"] = c
    s["ice"] = E + b"?33h" + E + b"5;44m blink bg " + E + b"6;41m fast " + E + b"25m steady " + E + b"0m" + E + b"?33l"
    s["clear2j"] = b"before" + E + b"2J" + b"after 2J"
    s["wrap"] = b"=" * 80 + E + b"2CX"
    s["lf"] = b"a\nb\nc"
    s["cursor"] = E + b"5;10Hpos" + E + b"A" + E + b"B" + E + b"C" + E + b"D" + E + b"s" + E + b"u" + E + b"K"
    s["dsr"] = E + b"6n"
    s["music"] = (b"A" + E + b"NT120L8CDEFG\x0eB" + E + b"|MFO3C\x0e" + b"C" + E + b"NCD!D" + E + b"=2M" + E +
                  b"MCDE\x0eE" + E + b"=1M" + E + b"MF" + E + b"|ABC")
    s["ff"] = b"a\x0cb"
    s["sub"] = b"a\x1ab"
    s["font"] = b"1" + E + b" D2" + E + b"0;42 D3" + E + b"1;0 D4" + E + b"0;0 D5"
    s["amiga"] = ("Åäö é ß".encode("latin-1") + b"\x9b1;33mY\x9b0m" + b"\x9b80u" + b"\x9b0 p" + b"\x9b p" +
                  b"\x9b0 q" + b"Z")
    s["iac"] = b"x\xffy"
    s["unix"] = "å─x".encode("utf-8") + E + b"1mB" + E + b"0m"
    s["art"] = open(os.path.join(HERE, "testdata", "bbs-sim", "welcome.ans"), "rb").read()
    s["amigaart"] = open(os.path.join(HERE, "testdata", "bbs-sim", "amiga.asc"), "rb").read()
    return s


SCREENS = screens()
# what a D20 filter (net/telnet/filter.c) must hand the console for each screen, per mode ("pc", "amiga"; "unix" for
# the unix screen); a screen not listed here passes unchanged. Events: the filter's TF_EV_* in order.
BBS = {                                 # the same in both BBS modes
    "ice": (E + b"5;44m blink bg " + E + b"6;41m fast " + E + b"25m steady " + E + b"0m", [("ice", 1), ("ice", 0)]),
    "music": (b"ABC!DE" + E + b"MF", []),       # after CSI = 1 M (BANSI) CSI M is delete line again
    "ff": (b"a" + E + b"2J" + E + b"Hb", []),
    "sub": (b"ab", []),
}
EXPECT = {(n, m): v for n, v in BBS.items() for m in ("pc", "amiga")}
EXPECT.update({
    ("font", "amiga"): (b"12345", [("mode", "pc"), ("mode", "amiga"), ("mode", "pc")]),
    ("font", "pc"): (b"12345", [("mode", "amiga"), ("mode", "pc")]),
    ("amiga", "amiga"): ("Åäö é ß".encode("latin-1") + E + b"1;33mY" + E + b"0m" + E + b"?25l" + E + b"?25hZ", []),
    ("amigaart", "amiga"): (SCREENS["amigaart"].replace(b"\x9b", E), []),
    ("unix", "unix"): (b"\xe5?x" + E + b"1mB" + E + b"0m", []),
})


def expect(name, mode):
    if (name, mode) in EXPECT:
        return EXPECT[(name, mode)]
    return SCREENS[name], []


class Call:
    def __init__(self, c, a, rec):
        self.c, self.a, self.rec = c, a, rec
        self.us, self.him = {}, {}          # our options and theirs: True on, False off
        self.asked_us, self.asked_him = set(), set()
        self.counts = {}
        self.ttypes = 0
        self.started = False
        self.inbuf = b""
        self.st = 0                         # decoder: 0 data, 1 IAC, 2 verb, 3 SB, 4 SB IAC
        self.verb = 0
        self.sb = b""
        self.cr = False
        self.line = b""

    def log(self, **kv):
        if self.rec:
            self.rec.write(json.dumps(kv) + "\n")
            self.rec.flush()

    def raw(self, b):
        self.c.sendall(b)

    def send(self, b):
        """data to the client: IAC doubled, CR NUL unless our BINARY is on"""
        b = b.replace(b"\xff", b"\xff\xff")
        if not self.us.get(BINARY):
            b = b.replace(b"\r", b"\r\x00").replace(b"\r\x00\n", b"\r\n")
        self.raw(b)

    def cmd(self, verb, opt):
        self.raw(bytes([IAC, verb, opt]))

    def agreed(self):
        self.log(ev="agreed", us={str(k): v for k, v in self.us.items()}, him={str(k): v for k, v in self.him.items()})

    def start(self):
        if self.started:
            return
        self.started = True
        for verb, opt in ((WILL, ECHO), (WILL, SGA), (DO, TTYPE), (DO, NAWS)):
            self.cmd(verb, opt)
            (self.asked_us if verb == WILL else self.asked_him).add(opt)
        if not self.a.refuse_binary:
            self.cmd(WILL, BINARY)
            self.cmd(DO, BINARY)
            self.asked_us.add(BINARY)
            self.asked_him.add(BINARY)

    def option(self, verb, opt):
        key = "%s %d" % (NAMES[verb], opt)
        self.counts[key] = self.counts.get(key, 0) + 1
        self.log(ev="opt", verb=NAMES[verb], opt=opt)
        if self.counts[key] > 2:
            self.log(ev="loop", cmd=key, count=self.counts[key])
        binary_ok = not self.a.refuse_binary
        if verb == WILL:
            ok = opt in (TTYPE, NAWS) or (opt == BINARY and binary_ok)
            if opt in self.asked_him:
                self.asked_him.discard(opt)
                self.him[opt] = ok
                if not ok:
                    self.cmd(DONT, opt)
            elif not self.him.get(opt):
                self.him[opt] = ok
                self.cmd(DO if ok else DONT, opt)
            if ok and opt == TTYPE:
                self.ttype_send()
        elif verb == WONT:
            self.asked_him.discard(opt)
            if self.him.get(opt):
                self.cmd(DONT, opt)
            self.him[opt] = False
        elif verb == DO:
            ok = opt in (ECHO, SGA) or (opt == BINARY and binary_ok)
            if opt in self.asked_us:
                self.asked_us.discard(opt)
                self.us[opt] = ok
                if not ok:
                    self.cmd(WONT, opt)
            elif not self.us.get(opt):
                self.us[opt] = ok
                self.cmd(WILL if ok else WONT, opt)
        else:
            self.asked_us.discard(opt)
            if self.us.get(opt):
                self.cmd(WONT, opt)
            self.us[opt] = False
        self.agreed()

    def ttype_send(self):
        self.raw(bytes([IAC, SB, TTYPE, 1, IAC, SE]))

    def subneg(self, sb, raw):
        if sb[:2] == bytes([TTYPE, 0]):
            self.log(ev="ttype", name=sb[2:].decode("latin-1"))
            self.ttypes += 1
            if self.ttypes == 1:
                self.ttype_send()               # a second SEND must get the same name
        elif sb[:1] == bytes([NAWS]) and len(sb) == 5:
            self.log(ev="naws", cols=sb[1] << 8 | sb[2], rows=sb[3] << 8 | sb[4], raw=raw.hex())
        else:
            self.log(ev="sb", data=sb.hex())

    def decode(self, d):
        """the client's bytes: options handled, data returned"""
        out = bytearray()
        for b in d:
            if self.st == 0:
                if b == IAC:
                    if not self.started and self.a.port != 23:
                        self.log(ev="client_iac_first")
                    self.st = 1
                    continue
                if self.cr and b == 0 and not self.him.get(BINARY):
                    self.cr = False
                    continue
                self.cr = b == 13
                out.append(b)
            elif self.st == 1:
                self.st = 0
                if b == IAC:
                    out.append(IAC)
                elif b in NAMES:
                    self.verb, self.st = b, 2
                elif b == SB:
                    self.sb, self.sbraw, self.st = b"", b"", 3
            elif self.st == 2:
                self.st = 0
                self.option(self.verb, b)
            elif self.st == 3:
                self.sbraw += bytes([b])
                if b == IAC:
                    self.st = 4
                else:
                    self.sb += bytes([b])
            elif self.st == 4:
                self.sbraw += bytes([b])
                if b == IAC:
                    self.sb += b"\xff"
                    self.st = 3
                elif b == SE:
                    self.st = 0
                    self.subneg(self.sb, self.sbraw)
                else:
                    self.st = 0
        return bytes(out)

    def screen(self, name):
        self.send(SCREENS[name] + b"\r\n<<END %s>>\r\n" % name.encode())

    def transfer(self, words):
        prog = self.a.lsz if words[0] == "sz" else self.a.lrz
        if not prog:
            self.send(b"no %s here\r\n" % words[0].encode())
            return
        flags = [w for w in words[1:] if w.startswith("-")]
        names = [w for w in words[1:] if not w.startswith("-")]
        if words[0] == "sz":
            argv = [prog, "-b", "-q"] + flags + [os.path.join(self.a.files, n) for n in names]
            cwd = None
        else:
            os.makedirs(self.a.upload, exist_ok=True)
            argv = [prog, "-b", "-q"] + flags
            cwd = self.a.upload
        before = set(os.listdir(self.a.upload)) if words[0] == "rz" else set()
        a, b = socket.socketpair()
        p = subprocess.Popen(argv, stdin=b, stdout=b, stderr=subprocess.DEVNULL, cwd=cwd)
        b.close()
        self.c.setblocking(False)
        try:
            while True:
                r, _, _ = select.select([self.c, a], [], [], 0.2)
                if self.c in r:
                    try:
                        d = self.c.recv(65536)
                    except BlockingIOError:
                        d = None
                    if d == b"":
                        break
                    if d:
                        d = self.decode(d)
                        if d:
                            a.sendall(d)
                if a in r:
                    d = a.recv(65536)
                    if not d:
                        break                   # lrzsz ended and all it wrote is sent
                    self.c.setblocking(True)
                    self.send(d)
                    self.c.setblocking(False)
        finally:
            self.c.setblocking(True)
            a.close()
        rc = p.wait()
        files = {}
        if words[0] == "rz":
            for n in sorted(set(os.listdir(self.a.upload)) - before):
                files[n] = hashlib.sha256(open(os.path.join(self.a.upload, n), "rb").read()).hexdigest()
        self.log(ev="xfer", cmd=" ".join(words), rc=rc, files=files)
        # what the client sends after the session (a sender's "OO") is not a command
        self.c.settimeout(0.3)
        try:
            while True:
                d = self.c.recv(65536)
                if not d:
                    break
                self.decode(d)
        except socket.timeout:
            pass
        self.c.settimeout(None)
        self.send(b"\r\n<<END %s>>\r\n" % words[0].encode())

    def command(self, line):
        words = line.decode("latin-1").split()
        self.log(ev="line", text=line.decode("latin-1"))
        if not words:
            return True
        w = words[0].lower()
        if w == "menu":
            self.send(b"commands: " + " ".join(sorted(SCREENS) + ["ttype", "sz", "rz", "bye"]).encode() +
                      b"\r\n<<END menu>>\r\n")
        elif w in SCREENS:
            self.screen(w)
        elif w == "ttype":
            self.ttype_send()
            self.send(b"<<END ttype>>\r\n")
        elif w in ("sz", "rz"):
            self.transfer(words)
        elif w == "bye":
            self.send(b"Goodbye.\r\n")
            return False
        else:
            self.send(b"unknown: " + line + b"\r\n<<END unknown>>\r\n")
        return True

    def run(self):
        self.log(ev="call", port=self.a.port)
        if self.a.port == 23:
            self.start()
        else:
            self.c.settimeout(self.a.iac_delay / 1000)
            early = b""
            try:
                early = self.c.recv(65536)
            except socket.timeout:
                pass
            self.c.settimeout(None)
            self.start()
            self.handle(early)
        self.send(b"Zenix test board (bbs-sim). Type menu.\r\n<<END hello>>\r\n")
        while True:
            try:
                d = self.c.recv(65536)
            except OSError:
                break
            if not d:
                break
            if not self.handle(d):
                break
        self.log(ev="end")

    def handle(self, d):
        d = self.decode(d)
        if not d:
            return True
        self.log(ev="keys", hex=d.hex())
        # the terminal's own answers (to ESC[6n and to the Amiga's CSI 0 SP q) are recorded, not typed
        for kind, pat in (("dsr", ANS_CPR), ("wbr", ANS_WBR)):
            for m in pat.finditer(d):
                self.log(ev=kind, answer=m.group(0).decode("latin-1"))
            d = pat.sub(b"", d)
        for b in d:
            if b in (13, 10):
                line, self.line = self.line, b""
                if (line or b == 13) and not self.command(line):
                    return False
            elif b != 0:
                self.line += bytes([b])
        return True


def serve(a, ready=None):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((a.host, a.port))
    s.listen(4)
    a.port = s.getsockname()[1]
    rec = open(a.record, "a") if a.record else None
    if a.ready:
        with open(a.ready, "w") as f:
            f.write("%d\n" % a.port)
    if ready:
        ready.set()
    while True:
        c, _ = s.accept()
        with c:
            try:
                Call(c, a, rec).run()
            except (BrokenPipeError, ConnectionResetError):
                pass
        if a.once:
            break
    s.close()


# ---- the self-test: a small telnet client in Python against the board, lrz/lsz on the client's side too ----

class Client:
    def __init__(self, port, binary=True):
        self.s = socket.create_connection(("127.0.0.1", port))
        self.binary = binary
        self.buf = b""
        self.st = 0

    def decode(self, d):
        out = bytearray()
        for b in d:
            if self.st == 0:
                if b == IAC:
                    self.st = 1
                else:
                    out.append(b)
            elif self.st == 1:
                if b == IAC:
                    out.append(IAC)
                    self.st = 0
                elif b in NAMES:
                    self.verb, self.st = b, 2
                elif b == SB:
                    self.st, self.sb = 3, b""
                else:
                    self.st = 0
            elif self.st == 2:
                self.st = 0
                ok = {WILL: (ECHO, SGA) + ((BINARY,) if self.binary else ()),
                      DO: (TTYPE, NAWS) + ((BINARY,) if self.binary else ())}
                if self.verb in (WILL, DO):
                    yes = b in ok[self.verb]
                    ans = (DO if yes else DONT) if self.verb == WILL else (WILL if yes else WONT)
                    self.s.sendall(bytes([IAC, ans, b]))
                    if self.verb == DO and b == NAWS:
                        self.s.sendall(bytes([IAC, SB, NAWS, 0, 80, 0, 255, 255, IAC, SE]))
            elif self.st == 3:
                if b == IAC:
                    self.st = 4
                else:
                    self.sb += bytes([b])
            elif self.st == 4:
                self.st = 0
                if b == SE and self.sb == bytes([TTYPE, 1]):
                    self.s.sendall(bytes([IAC, SB, TTYPE, 0]) + b"SELFTEST" + bytes([IAC, SE]))
        if not self.binary:
            out = bytes(out).replace(b"\r\x00", b"\r")
        return bytes(out)

    def until(self, marker, timeout=10):
        end = time.time() + timeout
        while marker not in self.buf:
            self.s.settimeout(max(0.01, end - time.time()))
            try:
                d = self.s.recv(65536)
            except socket.timeout:
                raise SystemExit("self-test: no %r (got %r)" % (marker, self.buf[-200:]))
            if not d:
                raise SystemExit("self-test: closed before %r" % marker)
            self.buf += self.decode(d)
        i = self.buf.index(marker)
        got, self.buf = self.buf[:i], self.buf[i + len(marker):]
        return got

    def type(self, line):
        self.s.sendall(line.encode() + b"\r\x00")

    def relay(self, argv, cwd):
        """lrz/lsz on our side of the call, through the telnet coding, until it ends"""
        a, b = socket.socketpair()
        p = subprocess.Popen(argv, stdin=b, stdout=b, stderr=subprocess.DEVNULL, cwd=cwd)
        b.close()
        pend = self.buf
        self.buf = b""
        if pend:
            a.sendall(pend)
        def out(d):
            d = d.replace(b"\xff", b"\xff\xff")
            if not self.binary:
                d = d.replace(b"\r", b"\r\x00")
            self.s.sendall(d)
        while p.poll() is None:
            r, _, _ = select.select([self.s, a], [], [], 0.2)
            if self.s in r:
                d = self.s.recv(65536)
                if not d:
                    break
                d = self.decode(d)
                try:
                    a.sendall(d)
                except OSError:
                    self.buf += d               # it ended meanwhile: the board's again
            if a in r:
                d = a.recv(65536)
                if d:
                    out(d)
        p.wait()
        a.setblocking(False)
        try:
            while True:                         # what it wrote last
                d = a.recv(65536)
                if not d:
                    break
                out(d)
        except OSError:
            pass
        a.close()
        return p.returncode


def self_test(a):
    fails = 0

    def check(ok, what):
        nonlocal fails
        print(("PASS" if ok else "FAIL") + " bbs-sim self-test: " + what)
        fails += 0 if ok else 1

    work = tempfile.mkdtemp(prefix="bbs-sim-")
    try:
        a.files, a.upload = os.path.join(work, "files"), os.path.join(work, "up")
        os.makedirs(a.files)
        data = bytes((i * 7 + (i >> 8)) & 255 for i in range(70000))
        open(os.path.join(a.files, "test.bin"), "wb").write(data)
        for binary in (True, False):
            a.record = os.path.join(work, "rec%d.jsonl" % binary)
            a.port, a.once, a.refuse_binary = 0, True, not binary
            ev = threading.Event()
            t = threading.Thread(target=serve, args=(a, ev), daemon=True)
            t.start()
            ev.wait(5)
            c = Client(a.port, binary)
            c.until(b"<<END hello>>\r\n")
            for name in ("grid", "art", "music", "amiga", "iac"):
                c.type(name)
                got = c.until(b"\r\n<<END %s>>\r\n" % name.encode())
                check(got == SCREENS[name], "%s arrives as made (BINARY %s)" % (name, "on" if binary else "off"))
            c.type("dsr")
            c.until(E + b"6n")
            c.s.sendall(E + b"12;34R")
            c.until(b"<<END dsr>>\r\n")
            if a.lsz and a.lrz:
                down = os.path.join(work, "down%d" % binary)
                os.makedirs(down)
                c.type("sz test.bin" + ("" if binary else " -e"))
                c.until(b"**\x18B00")
                c.buf = b"**\x18B00" + c.buf
                rc = c.relay([a.lrz, "-b", "-q"] + ([] if binary else ["-e"]), down)
                c.until(b"<<END sz>>\r\n")
                same = open(os.path.join(down, "test.bin"), "rb").read() == data
                check(rc == 0 and same, "sz: lsz on the board to lrz here, the file the same (BINARY %s)" %
                      ("on" if binary else "off"))
                c.type("rz")
                c.until(b"**\x18B01")
                rc = c.relay([a.lsz, "-b", "-q"] + ([] if binary else ["-e"]) + [os.path.join(a.files, "test.bin")], None)
                c.until(b"<<END rz>>\r\n")
                same = open(os.path.join(a.upload, "test.bin"), "rb").read() == data
                check(rc == 0 and same, "rz: lsz here to lrz on the board (BINARY %s)" % ("on" if binary else "off"))
                os.unlink(os.path.join(a.upload, "test.bin"))
            c.type("bye")
            c.until(b"Goodbye.")
            c.s.close()
            t.join(5)
            rec = [json.loads(l) for l in open(a.record)]
            kinds = [r["ev"] for r in rec]
            check([r["name"] for r in rec if r["ev"] == "ttype"] == ["SELFTEST", "SELFTEST"], "TTYPE asked twice, recorded")
            n = [r for r in rec if r["ev"] == "naws"]
            check(len(n) == 1 and n[0]["cols"] == 80 and n[0]["rows"] == 255 and "ffff" in n[0]["raw"],
                  "NAWS recorded with its doubled 255")
            check([r["answer"] for r in rec if r["ev"] == "dsr"] == ["\x1b[12;34R"], "the ESC[6n answer recorded")
            check("loop" not in kinds and "client_iac_first" not in kinds and "end" in kinds,
                  "no loop, no early IAC, the end recorded")
            last = [r for r in rec if r["ev"] == "agreed"][-1]
            check(bool(last["us"].get("0")) == binary and bool(last["him"].get("0")) == binary, "BINARY agreed: %s" % binary)
            if a.lsz and a.lrz:
                x = [r for r in rec if r["ev"] == "xfer"]
                check(len(x) == 2 and x[1]["files"].get("test.bin") == hashlib.sha256(data).hexdigest(),
                      "the transfers recorded with the received file's sha256")
    finally:
        subprocess.run(["rm", "-rf", work])
    if not (a.lsz and a.lrz):
        print("bbs-sim self-test: no lsz/lrz given: the transfers were not tested")
    print("bbs-sim self-test: %s" % ("all passed" if not fails else "%d FAILED" % fails))
    return 1 if fails else 0


def main():
    p = argparse.ArgumentParser(description="a BBS for telnet's tests (DESIGN D20 8.1)")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=2323)
    p.add_argument("--record")
    p.add_argument("--ready")
    p.add_argument("--files", default=".")
    p.add_argument("--upload", default=".")
    p.add_argument("--lsz")
    p.add_argument("--lrz")
    p.add_argument("--refuse-binary", action="store_true")
    p.add_argument("--iac-delay", type=int, default=300)
    p.add_argument("--once", action="store_true")
    p.add_argument("--self-test", action="store_true")
    a = p.parse_args()
    if a.self_test:
        sys.exit(self_test(a))
    serve(a)


if __name__ == "__main__":
    main()
