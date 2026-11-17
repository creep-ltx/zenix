#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Small fake IMAP, POP3 and SMTP servers for mail's tests (DESIGN D59 section 8): tools/host/mail-test.sh on the PC
and tools/mail-test.sh in FS-UAE (through tools/mail-ns.py). Nothing real: example.org addresses, the tests' own
password, no network beyond the address given. One thread a client; every line both ways is logged to LOG as
"PROTO N > LINE" / "PROTO N < LINE" (a literal's bytes as "{N bytes}"); every message an SMTP client sends is kept
whole, byte for byte, in DIR/smtp-N.eml (N from 1).

The mailbox (INBOX, newest last): 1 plain ASCII; 2 multipart/mixed with an alternative (ISO-8859-1 quoted-printable
text, HTML) and a PDF whose name is RFC 2231 UTF-8, and a second attachment; 3 HTML only, UTF-8, an escape sequence
in its subject's encoded word and in its text; 4 Windows-1252; 5 a body over 600 KB; 6 a reply-to and Cc. Folders
INBOX, Sent, Archive, "Entw&APw-rfe" (modified UTF-7) and a \\Noselect one.

The IMAP login's user picks a hostile server: "overlong" (a 20,000-byte line before every answer, 5,000 bytes of
header fields), "badlit" (a literal of 99,999,999,999 bytes), "garbage" (LIST answered with NULs, escapes, unbalanced
parentheses and broken literals' look-alikes), "bye" (BYE in the middle of a FETCH, then the end). Any other user must
be USER with PASSWORD.

  mail-fake.py [--tls CERT KEY] PROTO PORT LOG DIR       (runs until killed)
As a module: serve(proto, host, port, log, dir, ssl_context=None, implicit=True) starts a listener thread."""
import base64
import os
import re
import socket
import sys
import threading

USER, PASSWORD = "amiga@example.org", "fake-pass-1"
LOCK = threading.Lock()
COUNT = {"imap": 0, "pop": 0, "smtp": 0}


def build_box():
    pdf = base64.encodebytes(b"%PDF-1.4 fake pdf\n" + bytes(range(256))).replace(b"\n", b"\r\n")
    big = b"".join(b"line %06d of a big message, padded to be long enough................\r\n" % i for i in range(9000))
    m = [
        b"From: Alice Example <alice@example.org>\r\nTo: amiga@example.org\r\nSubject: hello from alice\r\n"
        b"Date: Mon, 16 Nov 2026 10:00:00 +0000\r\nMessage-ID: <m1@example.org>\r\n\r\nHi Amiga,\r\n.a line with a dot\r\n"
        b"bye\r\n",
        b"From: =?UTF-8?Q?Bj=C3=B6rn?= <bjorn@example.org>\r\nTo: amiga@example.org\r\nSubject: =?ISO-8859-1?Q?r=E4ksm=F6rg=E5s?=\r\n"
        b"Date: Mon, 16 Nov 2026 11:00:00 +0100\r\nMessage-ID: <m2@example.org>\r\nMIME-Version: 1.0\r\n"
        b"Content-Type: multipart/mixed; boundary=\"OUT\"\r\n\r\nThis is MIME.\r\n--OUT\r\n"
        b"Content-Type: multipart/alternative; boundary=IN\r\n\r\n--IN\r\nContent-Type: text/plain; charset=iso-8859-1\r\n"
        b"Content-Transfer-Encoding: quoted-printable\r\n\r\nH=E4r =E4r texten med =C5=C4=D6, en l=E5ng rad som bryts=\r\n"
        b" mjukt.\r\n--IN\r\nContent-Type: text/html; charset=utf-8\r\n\r\n<p>HTML version</p>\r\n--IN--\r\n--OUT\r\n"
        b"Content-Type: application/pdf; name=\"x.pdf\"\r\nContent-Disposition: attachment;\r\n"
        b" filename*=UTF-8''r%C3%A4kning%2F..%2Fsecret.pdf\r\nContent-Transfer-Encoding: base64\r\n\r\n" + pdf +
        b"--OUT\r\nContent-Type: text/plain; charset=utf-8; name=notes.txt\r\nContent-Disposition: attachment; filename=notes.txt\r\n\r\n"
        b"just notes\r\n--OUT--\r\n",
        b"From: Mallory <mallory@example.org>\r\nTo: amiga@example.org\r\nSubject: =?UTF-8?B?" +
        base64.b64encode("ok \x1b[2J\x1b]0;owned\x07 html".encode()) + b"?=\r\nDate: Mon, 16 Nov 2026 12:00:00 +0000\r\n"
        b"Content-Type: text/html; charset=utf-8\r\nContent-Transfer-Encoding: quoted-printable\r\n\r\n"
        b"<html><head><style>p{color:red}</style></head><body><h1>News</h1><p>Caf=C3=A9 &amp; more=1B[31m red?</p>"
        b"<ul><li>one</li><li>two</li></ul><a href=3D\"http://example.org/x\">link</a><script>evil()</script></body></html>\r\n",
        b"From: Win <win@example.org>\r\nTo: amiga@example.org\r\nSubject: windows text\r\nDate: Mon, 16 Nov 2026 13:00:00 +0000\r\n"
        b"Content-Type: text/plain; charset=windows-1252\r\nContent-Transfer-Encoding: 8bit\r\n\r\n\x93quoted\x94 \x80 5 \x96 dash\r\n",
        b"From: Big <big@example.org>\r\nTo: amiga@example.org\r\nSubject: a big one\r\nDate: Mon, 16 Nov 2026 14:00:00 +0000\r\n\r\n" + big,
        b"From: Bob <bob@example.org>\r\nReply-To: Lists <list@example.org>\r\nTo: amiga@example.org, carol@example.org\r\n"
        b"Cc: dave@example.org\r\nSubject: plans\r\nDate: Mon, 16 Nov 2026 15:00:00 +0000\r\nMessage-ID: <m6@example.org>\r\n\r\n"
        b"Shall we meet?\r\n> earlier words\r\n",
    ]
    return [{"uid": 100 + i, "flags": set() if i not in (0, 4) else {"\\Seen"}, "data": d} for i, d in enumerate(m)]


BOXES = {"INBOX": None, "Sent": [], "Archive": [], "Entw&APw-rfe": []}


def header_fields(data, names):
    head = data.split(b"\r\n\r\n", 1)[0]
    out = []
    lines = head.split(b"\r\n")
    keep = False
    for l in lines:
        if l[:1] in (b" ", b"\t"):
            if keep:
                out.append(l)
            continue
        keep = l.split(b":", 1)[0].upper().decode("latin-1") in names
        if keep:
            out.append(l)
    return b"\r\n".join(out) + b"\r\n\r\n"


class Conn:
    def __init__(self, sock, proto, n, log, d):
        self.s, self.proto, self.n, self.log, self.dir = sock, proto, n, log, d
        self.buf = b""

    def note(self, way, text):
        with LOCK:
            self.log.write("%s %d %s %s\n" % (self.proto.upper(), self.n, way, text))

    def line(self):
        while b"\n" not in self.buf:
            d = self.s.recv(4096)
            if not d:
                return None
            self.buf += d
        l, self.buf = self.buf.split(b"\n", 1)
        l = l.rstrip(b"\r")
        self.note(">", l.decode("utf-8", "backslashreplace"))
        return l

    def read(self, n):
        while len(self.buf) < n:
            d = self.s.recv(65536)
            if not d:
                return None
            self.buf += d
        b, self.buf = self.buf[:n], self.buf[n:]
        self.note(">", "{%d bytes}" % n)
        return b

    def send(self, b, show=True):
        if isinstance(b, str):
            b = b.encode("utf-8")
        if show:
            self.note("<", b[:300].decode("latin-1"))
        try:
            self.s.sendall(b + b"\r\n")
        except OSError:
            pass

    def raw(self, b):
        self.note("<", "{%d bytes}" % len(b))
        try:
            self.s.sendall(b)
        except OSError:
            pass


def astrings(rest):
    """IMAP arguments: atoms, "quoted" strings; {N} literals are the caller's"""
    out, i = [], 0
    while i < len(rest):
        c = rest[i]
        if c == " ":
            i += 1
        elif c == '"':
            j, s = i + 1, ""
            while j < len(rest) and rest[j] != '"':
                if rest[j] == "\\":
                    j += 1
                s += rest[j]
                j += 1
            out.append(s)
            i = j + 1
        elif c == "(":
            j = rest.index(")", i)
            out.append(rest[i:j + 1])
            i = j + 1
        else:
            j = i
            while j < len(rest) and rest[j] != " ":
                j += 1
            out.append(rest[i:j])
            i = j
    return out


def imap(c, ctx, box_state):
    st = {"user": None, "box": None, "tls": False}
    c.send("* OK fake IMAP ready")

    def lit_line(l):
        """a command line with {N} literals read in: the text, the literals in order"""
        lits = []
        while True:
            m = re.search(rb"\{(\d+)\+?\}$", l)
            if not m:
                return l, lits
            c.send("+ go")
            data = c.read(int(m.group(1)))
            if data is None:
                return None, None
            lits.append(data)
            nxt = c.line()
            if nxt is None:
                return None, None
            l = l[:m.start()] + b"\x00LIT%d\x00" % (len(lits) - 1) + nxt

    while True:
        l = c.line()
        if l is None:
            return
        l, lits = lit_line(l)
        if l is None:
            return
        t = l.decode("utf-8", "replace")
        parts = t.split(" ", 2)
        if len(parts) < 2:
            c.send("* BAD what")
            continue
        tag, cmd, rest = parts[0], parts[1].upper(), parts[2] if len(parts) > 2 else ""
        uid = False
        if cmd == "UID":
            uid = True
            p2 = rest.split(" ", 1)
            cmd, rest = p2[0].upper(), p2[1] if len(p2) > 1 else ""
        args = astrings(rest)
        args = [lits[int(a[4:-1])].decode("utf-8", "replace") if a.startswith("\x00LIT") else a for a in args]
        user = st["user"] or ""
        if user == "overlong":
            c.send("* OK " + "x" * 20000)
        if cmd == "CAPABILITY":
            c.send("* CAPABILITY IMAP4rev1 STARTTLS AUTH=PLAIN")
            c.send(tag + " OK done")
        elif cmd == "STARTTLS":
            if not ctx or st["tls"]:
                c.send(tag + " NO no TLS here")
                continue
            c.send(tag + " OK begin TLS")
            try:
                c.s = ctx.wrap_socket(c.s, server_side=True)
            except Exception as e:
                c.note("!", "TLS %s" % e)
                return
            st["tls"] = True
            c.buf = b""
        elif cmd == "LOGIN":
            if len(args) == 2 and (args[0] in ("overlong", "badlit", "garbage", "bye") or (args[0] == USER and args[1] == PASSWORD)):
                st["user"] = args[0]
                c.send(tag + " OK logged in")
            else:
                c.send(tag + " NO [AUTHENTICATIONFAILED] wrong user or password")
        elif not st["user"]:
            c.send(tag + " BAD log in first")
        elif cmd == "LIST":
            if user == "garbage":
                c.raw(b"* LIST (\\HasNoChildren) \"/\" \"Evil\x1b[2J\x00Name\"\r\n* LIST ((( \r\n* LIST (\\Noselect) \"/\" Hidden\r\n"
                      b"* LIST () \"/\" {3}\r\nAb\x9b\r\n\x00\x01\x02 junk \xff\xfe\r\n* LIST () NIL INBOX\r\n")
            else:
                c.send("* LIST (\\Noselect \\HasChildren) \"/\" Parent")
                for name in BOXES:
                    c.send("* LIST (\\HasNoChildren) \"/\" \"%s\"" % name)
            c.send(tag + " OK listed")
        elif cmd == "SELECT":
            name = args[0] if args else ""
            if name not in box_state:
                c.send(tag + " NO no such folder")
                continue
            st["box"] = name
            msgs = box_state[name]
            c.send("* %d EXISTS" % len(msgs))
            c.send("* 0 RECENT")
            c.send("* OK [UIDVALIDITY 7] ok")
            c.send(tag + " OK [READ-WRITE] selected")
        elif cmd == "FETCH" or (cmd in ("STORE", "COPY") and uid) or cmd == "EXPUNGE" or cmd == "APPEND" or cmd == "NOOP":
            msgs = box_state.get(st["box"]) if st["box"] else None
            if cmd == "APPEND":
                name = args[0]
                if name not in box_state:
                    c.send(tag + " NO [TRYCREATE] no such folder")
                    continue
                box_state[name].append({"uid": 500 + len(box_state[name]), "flags": {"\\Seen"}, "data": lits[-1] if lits else b""})
                c.send(tag + " OK appended")
                continue
            if cmd == "NOOP":
                c.send(tag + " OK")
                continue
            if msgs is None:
                c.send(tag + " BAD select first")
                continue
            if cmd == "EXPUNGE":
                k = 0
                while k < len(msgs):
                    if "\\Deleted" in msgs[k]["flags"]:
                        c.send("* %d EXPUNGE" % (k + 1))
                        del msgs[k]
                    else:
                        k += 1
                c.send(tag + " OK expunged")
                continue
            seqset, rest2 = args[0], " ".join(args[1:])
            if uid:
                want = [i for i, m in enumerate(msgs) if str(m["uid"]) == seqset]
            else:
                a, _, b = seqset.partition(":")
                want = list(range(int(a) - 1, int(b or a)))
            if cmd == "STORE":
                op, fl = args[1].upper(), set(args[2].strip("()").split())
                for i in want:
                    if op.startswith("+"):
                        msgs[i]["flags"] |= fl
                    else:
                        msgs[i]["flags"] -= fl
                c.send(tag + " OK stored")
                continue
            if cmd == "COPY":
                dest = args[1]
                if dest not in box_state:
                    c.send(tag + " NO [TRYCREATE] no such folder")
                    continue
                for i in want:
                    box_state[dest].append(dict(msgs[i], flags=set(msgs[i]["flags"])))
                c.send(tag + " OK copied")
                continue
            for i in want:
                m = msgs[i]
                if user == "bye":
                    c.raw(b"* %d FETCH (UID %d BODY[] {500}\r\npartial" % (i + 1, m["uid"]))
                    c.send("")
                    c.send("* BYE going away")
                    return
                if user == "badlit":
                    c.raw(b"* %d FETCH (UID %d BODY[] {99999999999}\r\nabc" % (i + 1, m["uid"]))
                    return
                items = "UID %d" % m["uid"]
                body = None
                if "FLAGS" in rest2:
                    items += " FLAGS (%s)" % " ".join(sorted(m["flags"]))
                if "RFC822.SIZE" in rest2:
                    items += " RFC822.SIZE %d" % len(m["data"])
                if "HEADER.FIELDS" in rest2:
                    key = "BODY[HEADER.FIELDS (DATE FROM SUBJECT CONTENT-TYPE)]"
                    body = header_fields(m["data"], ("DATE", "FROM", "SUBJECT", "CONTENT-TYPE"))
                    if user == "overlong":
                        body = body[:-2] + b"X-Pad: " + b"y" * 5000 + b"\r\n\r\n"
                elif "BODY.PEEK[]" in rest2:
                    mm = re.search(r"BODY\.PEEK\[\]<(\d+)\.(\d+)>", rest2)
                    if mm:
                        a0, ln = int(mm.group(1)), int(mm.group(2))
                        body, key = m["data"][a0:a0 + ln], "BODY[]<%d>" % a0
                    else:
                        body, key = m["data"], "BODY[]"
                if body is not None:
                    c.raw(("* %d FETCH (%s %s {%d}\r\n" % (i + 1, items, key, len(body))).encode() + body + b")\r\n")
                else:
                    c.send("* %d FETCH (%s)" % (i + 1, items))
            c.send(tag + " OK fetched")
        elif cmd == "LOGOUT":
            c.send("* BYE see you")
            c.send(tag + " OK bye")
            return
        else:
            c.send(tag + " BAD unknown command")


def pop(c, ctx, box_state):
    msgs = box_state["INBOX"]
    st = {"user": None, "ok": False, "dele": set(), "tls": False}
    c.send("+OK fake POP3 ready")
    while True:
        l = c.line()
        if l is None:
            return
        w = l.decode("utf-8", "replace").split(" ")
        cmd = w[0].upper()
        if cmd == "STLS" and ctx and not st["tls"]:
            c.send("+OK begin TLS")
            c.s = ctx.wrap_socket(c.s, server_side=True)
            st["tls"] = True
            c.buf = b""
        elif cmd == "USER":
            st["user"] = " ".join(w[1:])
            c.send("+OK go on")
        elif cmd == "PASS":
            if st["user"] == USER and " ".join(w[1:]) == PASSWORD:
                st["ok"] = True
                c.send("+OK logged in")
            else:
                c.send("-ERR wrong user or password")
        elif not st["ok"]:
            c.send("-ERR log in first")
        elif cmd == "STAT":
            c.send("+OK %d %d" % (len(msgs), sum(len(m["data"]) for m in msgs)))
        elif cmd in ("TOP", "RETR"):
            k = int(w[1]) - 1
            if k < 0 or k >= len(msgs) or k in st["dele"]:
                c.send("-ERR no such message")
                continue
            d = msgs[k]["data"]
            if cmd == "TOP":
                d = d.split(b"\r\n\r\n", 1)[0] + b"\r\n\r\n"
            c.send("+OK here")
            out = b"".join((b"." + x if x.startswith(b".") else x) + b"\r\n" for x in d.split(b"\r\n")[:-1]) + b".\r\n"
            c.raw(out)
        elif cmd == "DELE":
            st["dele"].add(int(w[1]) - 1)
            c.send("+OK deleted")
        elif cmd == "RSET":
            st["dele"] = set()
            c.send("+OK reset")
        elif cmd == "QUIT":
            for k in sorted(st["dele"], reverse=True):
                del msgs[k]
            c.send("+OK bye")
            return
        else:
            c.send("-ERR unknown")


def smtp(c, ctx, implicit, d):
    st = {"tls": implicit, "auth": False, "from": None, "rcpt": []}
    c.send("220 fake.example.org ESMTP ready")
    while True:
        l = c.line()
        if l is None:
            return
        t = l.decode("utf-8", "replace")
        cmd = t[:4].upper()
        if cmd in ("EHLO", "HELO"):
            c.send("250-fake.example.org hello")
            if ctx and not st["tls"]:
                c.send("250-STARTTLS")
            c.send("250-SIZE 10000000")
            c.send("250 AUTH PLAIN LOGIN")
        elif t.upper() == "STARTTLS" and ctx and not st["tls"]:
            c.send("220 begin TLS")
            c.s = ctx.wrap_socket(c.s, server_side=True)
            st["tls"] = True
            c.buf = b""
        elif t.upper().startswith("AUTH PLAIN "):
            dec = base64.b64decode(t[11:]).split(b"\0")
            ok = len(dec) == 3 and dec[1].decode() == USER and dec[2].decode() == PASSWORD
            st["auth"] = ok
            c.send("235 ok" if ok else "535 wrong user or password")
        elif t.upper() == "AUTH LOGIN":
            c.send("334 VXNlcm5hbWU6")
            u = base64.b64decode(c.line() or b"")
            c.send("334 UGFzc3dvcmQ6")
            p = base64.b64decode(c.line() or b"")
            st["auth"] = u.decode() == USER and p.decode() == PASSWORD
            c.send("235 ok" if st["auth"] else "535 wrong")
        elif t.upper().startswith("MAIL FROM:"):
            if not st["auth"]:
                c.send("530 log in first")
                continue
            st["from"], st["rcpt"] = t[10:].strip("<>"), []
            c.send("250 ok")
        elif t.upper().startswith("RCPT TO:"):
            a = t[8:].strip("<>")
            if a.endswith("@refused.example.org"):
                c.send("550 no such user")
                continue
            st["rcpt"].append(a)
            c.send("250 ok")
        elif t.upper() == "DATA":
            c.send("354 go on")
            data = b""
            while True:
                x = c.line()
                if x is None:
                    return
                if x == b".":
                    break
                data += (x[1:] if x.startswith(b".") else x) + b"\r\n"
            with LOCK:
                COUNT["smtp_msgs"] = COUNT.get("smtp_msgs", 0) + 1
                k = COUNT["smtp_msgs"]
            with open(os.path.join(d, "smtp-%d.eml" % k), "wb") as f:
                f.write(data)
            with open(os.path.join(d, "smtp-%d.env" % k), "w") as f:
                f.write("%s\n%s\n" % (st["from"], " ".join(st["rcpt"])))
            c.send("250 queued as %d" % k)
        elif t.upper() == "QUIT":
            c.send("221 bye")
            return
        else:
            c.send("502 unknown")


def serve(proto, host, port, logpath, d, ctx=None, implicit=True, box_state=None):
    """proto imap, pop or smtp; implicit: TLS at once (else STARTTLS offered when ctx is given)"""
    ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind((host, port))
    ls.listen(8)
    log = open(logpath, "a", buffering=1, encoding="utf-8", errors="backslashreplace")
    if box_state is None:
        box_state = {k: ([] if v is not None else build_box()) for k, v in BOXES.items()}

    def client(s, n):
        if ctx and implicit:
            try:
                s = ctx.wrap_socket(s, server_side=True)
            except Exception as e:
                with LOCK:
                    log.write("%s %d ! TLS %s\n" % (proto.upper(), n, e))
                s.close()
                return
        c = Conn(s, proto, n, log, d)
        try:
            if proto == "imap":
                imap(c, None if implicit else ctx, box_state)
            elif proto == "pop":
                pop(c, None if implicit else ctx, box_state)
            else:
                smtp(c, None if implicit else ctx, bool(ctx and implicit), d)
        except (OSError, ValueError, IndexError) as e:
            c.note("!", "ended: %s" % e)
        try:
            c.s.close()
        except OSError:
            pass

    def run():
        while True:
            s, _ = ls.accept()
            with LOCK:
                COUNT[proto] += 1
                n = COUNT[proto]
            threading.Thread(target=client, args=(s, n), daemon=True).start()
    t = threading.Thread(target=run, daemon=True)
    t.start()
    return box_state


if __name__ == "__main__":
    a = sys.argv[1:]
    ctx = None
    if a and a[0] == "--tls":
        import ssl
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(a[1], a[2])
        a = a[3:]
    serve(a[0], "127.0.0.1", int(a[1]), a[2], a[3], ctx)
    threading.Event().wait()
