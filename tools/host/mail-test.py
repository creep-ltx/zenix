#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""mail's sessions on the PC (DESIGN D59 section 8; run by tools/host/mail-test.sh): tools/host/mail-client (the core
over sockets) against tools/host/mail-fake.py on 127.0.0.1, each server at a port of its own:
  imap     TLS (a test CA made at this run, tools/host/tls-test.py's make_pki): a wrong password refused, the login,
           the folders (modified UTF-7, \\Noselect left out), the index (newest first, flags, dates, names, encoded
           subjects with an escape made clean), each message read: quoted-printable ISO-8859-1, the alternative's
           plain part, RFC 2231 names, HTML made text, Windows-1252, a big one cut at 512 KB; an attachment saved
           byte for byte; flags, copy, expunge, append; STARTTLS; a certificate for another name and an unknown root
           refused; no password over a plain connection unless asked for
  hostile  a 20,000-byte line before every answer and 5,000 bytes of header fields; a literal of 99,999,999,999
           bytes; LIST answered with garbage; BYE in the middle of a FETCH
  pop      the index (TOP), a message read (dot-unstuffed), DELE, RSET, QUIT; STLS
  smtp     STARTTLS and AUTH PLAIN, implicit TLS, a refused recipient, a wrong password, no STARTTLS offered: not sent;
           the message received byte for byte as built (dots doubled and undone), Bcc only in the envelope
  mail-test.py OUT CLIENT"""
import importlib.util
import os
import re
import socket
import ssl
import subprocess
import sys

OUT, CLIENT = sys.argv[1], sys.argv[2]
HERE = os.path.dirname(os.path.abspath(__file__))
bad = 0


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    saved = sys.argv
    sys.argv = [saved[0]]
    spec.loader.exec_module(m)
    sys.argv = saved
    return m


fake = load("fake", os.path.join(HERE, "mail-fake.py"))
tt = load("tlstest", os.path.join(HERE, "tls-test.py"))
pki = os.path.join(OUT, "pki")
os.makedirs(pki, exist_ok=True)
sites, anchors, _ = tt.make_pki(pki)


def check(ok, what, detail=""):
    global bad
    print(("PASS " if ok else "FAIL ") + what + ("" if ok else ": " + repr(detail)[-900:]))
    bad += not ok


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def ctx_for(host):
    c = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    c.minimum_version = ssl.TLSVersion.TLSv1_2
    c.maximum_version = ssl.TLSVersion.TLSv1_2
    c.load_cert_chain(*sites[host][0])
    return c


LOG = os.path.join(OUT, "servers.log")
SMTPDIR = os.path.join(OUT, "smtp")
os.makedirs(SMTPDIR, exist_ok=True)
for f in os.listdir(SMTPDIR):
    os.unlink(os.path.join(SMTPDIR, f))
P = {}


def start(key, proto, ctx=None, implicit=True):
    P[key] = free_port()
    return fake.serve(proto, "127.0.0.1", P[key], LOG, SMTPDIR, ctx, implicit)


def run(name, script, name_=None):
    sp = os.path.join(OUT, name + ".script")
    open(sp, "w").write(script)
    args = [CLIENT, "--anchors", anchors]
    if name_:
        args += ["--name", name_]
    r = subprocess.run(args + [sp], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=180)
    out = r.stdout.decode("utf-8", "surrogateescape")
    open(os.path.join(OUT, name + ".out"), "w", errors="surrogateescape").write(out)
    return r.returncode, out


def clean(s):
    return not re.search(r"[\x00-\x09\x0b-\x1f\x7f-\x9f]", s)


U, PW = fake.USER, fake.PASSWORD
boxes = start("imaps", "imap", ctx_for("rsa.test"))
start("imap-starttls", "imap", ctx_for("rsa.test"), False)
start("imap-plain", "imap")
start("imap-other", "imap", ctx_for("unknown.test"))
start("pop", "pop", ctx_for("rsa.test"), False)
start("smtp", "smtp", ctx_for("rsa.test"), False)
start("smtps", "smtp", ctx_for("rsa.test"), True)
start("smtp-plain", "smtp")

# ---- IMAP over TLS ----
att = os.path.join(OUT, "saved.pdf")
app = os.path.join(OUT, "append.eml")
open(app, "wb").write(b"From: amiga@example.org\r\nTo: x@example.org\r\nSubject: kept\r\n\r\nsent text\r\n")
rc, out = run("imap", f"""open imap {P['imaps']} on
login {U} wrong-pass
open imap {P['imaps']} on
login {U} {PW}
folders
select INBOX 300
fetch 5
fetch 4
fetch 3
fetch 2
fetch 1
fetch 0
fetch 4
save 4 0 {att}
flag 0 seen 1
flag 1 deleted 1
copy 2 Archive
copy 2 Nowhere
expunge
select INBOX 3
append Sent {app}
select Sent 10
select Archive 10
logout
""")
check(rc == 0 and "!! TLS ok" in out, "IMAP: TLS 1.2 with the test CA", out[:400])
check("!! login -2 [AUTHENTICATIONFAILED] wrong user or password" in out and "!! login 0" in out,
      "a wrong password refused with the server's words; the right one logs in", out[:800])
check("!! box INBOX = INBOX" in out and "!! box Entw&APw-rfe = Entwürfe" in out and "Parent" not in out and
      "!! folders 0 4" in out, "the folders: INBOX first, modified UTF-7 shown as UTF-8, \\Noselect left out", out)
items = re.findall(r"^!! item (\d+) (\d+) (\d+) (\d+) (.*) \| (.*)$", out, re.M)
check(len(items) >= 6 and items[0][1] == "105" and items[5][1] == "100", "the index: newest first, with UIDs", items[:6])
it = {int(u): (int(f), int(d), fr, sj) for _, u, f, d, fr, sj in items[:6]}
for k in range(100, 106):
    it.setdefault(k, (0, 0, "", ""))
check(it[101][2] == "Björn" and it[101][3] == "räksmörgås" and it[101][0] & 8 and not it[101][0] & 1,
      "the index: the sender's name, an ISO-8859-1 subject, unread, multipart/mixed marked", it.get(101))
check(it[100][0] & 1 and it[100][1] == 1794823200, "the index: read, the date in seconds (UTC)", it.get(100))
check(clean(it[102][3]) and it[102][3].startswith("ok [2J") and "owned" in it[102][3],
      "an escape sequence in an encoded subject made clean", it.get(102))
sec = out.split("!! fetch ")[1:] + [""] * 7
alice, bjorn, mallory, win, big, bob = sec[:6]
check(bob.startswith("0 cut 0") and "Shall we meet?" in bob and "!! replyto Lists <list@example.org>" in bob
      and "!! cc dave@example.org" in bob, "a message read: Reply-To and Cc", bob[:500])
check(big.startswith("0 cut 1 n 524288") and "line 000000 of a big message" in big, "a message over 512 KB: its first 512 KB, cut said", big[:300])
check("!! text “quoted” € 5 – dash" in win, "Windows-1252", win[:400])
check("!! html 1" in mallory and "!! text News" in mallory and "!! text Café & more[31m red?" in mallory and
      "!! text * one" in mallory and "link <http://example.org/x>" in mallory and "evil" not in mallory and
      "color" not in mallory and clean(mallory.replace("\n", "")),
      "HTML only: headings, a list, a link, style and script dropped, the escape dropped", mallory[:700])
check("!! subject räksmörgås" in bjorn and "!! from Björn <bjorn@example.org>" in bjorn and
      "!! text Här är texten med ÅÄÖ, en lång rad som bryts mjukt." in bjorn and "HTML version" not in bjorn,
      "multipart: the alternative's plain part, quoted-printable ISO-8859-1 with a soft break; headers decoded", bjorn[:700])
check("!! att 0 räkning_.._secret.pdf | application/pdf |" in bjorn and "!! att 1 notes.txt | text/plain |" in bjorn,
      "attachments listed: an RFC 2231 UTF-8 name made safe (no '/'), a text attachment kept as one", bjorn[-400:])
check(alice.startswith("0 cut 0") and "!! text .a line with a dot" in alice, "plain ASCII", alice[:300])
pdf = open(att, "rb").read() if os.path.exists(att) else b""
check(pdf == b"%PDF-1.4 fake pdf\n" + bytes(range(256)), "an attachment saved byte for byte (base64)", len(pdf))
check("!! flag 0" in out and "!! copy 0" in out and "!! copy -2 [TRYCREATE] no such folder" in out and "!! expunge 0" in out,
      "flags, copy, a copy to no folder refused, expunge", out[-1500:])
check(re.search(r"!! select 0 5 3", out) and "!! append 0" in out and re.search(r"!! select 0 1 1 \n!! item 0 500 1 0 amiga@example.org \| kept", out)
      and re.search(r"!! select 0 1 1 \n!! item 0 \d+ \d+ \d+ Win", out),
      "after expunge 5 left, the limit's 3 shown; the appended copy in Sent; the copied message in Archive", out[-1200:])
L = open(LOG, encoding="utf-8", errors="replace").read()
check(re.search(r'LOGIN "amiga@example.org" "fake-pass-1"', L) and re.search(r"UID STORE 105 \+FLAGS\.SILENT \(\\Seen\)", L)
      and re.search(r"UID STORE 104 \+FLAGS\.SILENT \(\\Deleted\)", L) and "UID COPY 103 \"Archive\"" in L and re.search(r"APPEND \"Sent\" \(\\Seen\) \{\d+\}", L),
      "the commands on the wire", L[-2000:])

# ---- IMAP: STARTTLS, wrong names, no password without TLS ----
rc, out = run("imap-starttls", f"open imap {P['imap-starttls']} starttls\nlogin {U} {PW}\nselect INBOX 2\nlogout\n")
check("!! TLS ok" in out and "!! login 0" in out and "!! select 0 6 2" in out, "IMAP STARTTLS: TLS, then the login", out)
rc, out = run("imap-name", f"open imap {P['imaps']} on\n", "other.test")
check(rc == 1 and "!! open TLS:" in out, "TLS: a certificate for another name refused", out)
rc, out = run("imap-unknown", f"open imap {P['imap-other']} on\n", "unknown.test")
check(rc == 1 and "!! open TLS:" in out, "TLS: an unknown root refused", out)
rc, out = run("imap-plain", f"open imap {P['imap-plain']} off\nlogin {U} {PW}\nopen imap {P['imap-plain']} off\nlogin {U} {PW} plain\nselect INBOX 1\n")
check("!! login -2 the password is not sent without TLS" in out and "!! login 0" in out and "!! select 0 6 1" in out,
      "plain IMAP: no password without TLS unless asked (plain_auth)", out)
rc, out = run("imap-literal", f"open imap {P['imap-plain']} off\nlogin {U} pass\"with\\quote plain\nopen imap {P['imap-plain']} off\nlogin {U} pässwörd plain\n")
L = open(LOG, encoding="utf-8", errors="replace").read()
check("!! login -2" in out and r'"pass\"with\\quote"' in L and re.search(r'LOGIN "amiga@example.org" \{10\}', L),
      "a password with quotes escaped; one outside ASCII sent as a literal", L[-800:])

# ---- hostile servers ----
rc, out = run("hostile-overlong", f"open imap {P['imap-plain']} off\nlogin overlong x plain\nfolders\nselect INBOX 300\nfetch 0\n")
check("!! login 0" in out and "!! folders 0 4" in out and "!! select 0 6 6" in out and "Shall we meet?" in out and
      re.search(r"!! item 5 100 1 \d+ Alice Example \| hello from alice", out), "20,000-byte lines and 5,000 bytes of headers: cut, the session goes on", out[-1500:])
rc, out = run("hostile-badlit", f"open imap {P['imap-plain']} off\nlogin badlit x plain\nselect INBOX 300\nfolders\n")
check("!! select -3" in out and "literal of" in out and "!! folders -1" in out, "a 99,999,999,999-byte literal: refused, the connection ended", out)
rc, out = run("hostile-garbage", f"open imap {P['imap-plain']} off\nlogin garbage x plain\nfolders\n")
boxes_g = re.findall(r"^!! box (.*)$", out, re.M)
check("!! folders 0" in out and all(clean(b) for b in boxes_g) and "Hidden" not in out and any("Evil" in b for b in boxes_g),
      "garbage in LIST: no crash, names clean, \\Noselect still left out", boxes_g)
rc, out = run("hostile-bye", f"open imap {P['imap-plain']} off\nlogin bye x plain\nselect INBOX 300\nfetch 0\n")
check("!! select -" in out and "!! fetch -" in out and rc == 0, "BYE in the middle of a FETCH: said, no crash", out)

# ---- POP3 ----
rc, out = run("pop", f"""open pop {P['pop']} starttls
login {U} wrong
open pop {P['pop']} starttls
login {U} {PW}
folders
select INBOX 6
fetch 5
flag 3 deleted 1
flag 2 deleted 1
flag 3 deleted 0
expunge
open pop {P['pop']} starttls
login {U} {PW}
select INBOX 300
""")
check("!! TLS ok" in out and "!! login -2 wrong user or password" in out and "!! login 0" in out and "!! box INBOX = INBOX" in out,
      "POP3: STLS, a wrong password refused, the login", out[:800])
check(re.search(r"!! select 0 6 6 \n!! item 0 6 1 \d+ Bob \| plans", out) and "!! text .a line with a dot" in out,
      "POP3: the index (TOP), a message read with a dot line unstuffed", out)
check(re.search(r"!! select 0 5 5", out), "POP3: DELE, RSET with the others deleted again, QUIT: one message gone", out[-800:])
L = open(LOG, encoding="utf-8", errors="replace").read()
check(re.search(r"POP 2 > DELE 3\n.*\nPOP 2 > DELE 4\n.*\nPOP 2 > RSET\n.*\nPOP 2 > DELE 4\n.*\nPOP 2 > QUIT", L),
      "POP3: the deletions on the wire (DELE, RSET, DELE again for the others, QUIT)", L[-1500:])

# ---- SMTP ----
draft = os.path.join(OUT, "draft.txt")
open(draft, "w").write("From: Amiga <amiga@example.org>\nTo: Bob <bob@example.org>\nCc: carol@example.org\n"
                       "Bcc: secret@example.org\nSubject: Grüße\n\nHej!\n.dot line\n..two dots\nÅÄÖ\n")
built = os.path.join(OUT, "built.eml")
rc, out = run("smtp", f"""build {draft} {built}
send {P['smtp']} starttls {U} {PW} {draft}
send {P['smtps']} on {U} {PW} {draft}
send {P['smtp']} starttls {U} wrong {draft}
send {P['smtp-plain']} starttls {U} {PW} {draft}
""")
check("!! build 0 3 recipients" in out and out.count("!! send 0") == 2, "SMTP: sent over STARTTLS and over TLS", out)
check("!! send -2 the login: the server said 535" in out, "SMTP: a wrong password refused", out)
check("!! send -2 the mail server offers no STARTTLS: not sent" in out, "SMTP: no STARTTLS offered: not sent", out)
b = open(built, "rb").read()
r1 = open(os.path.join(SMTPDIR, "smtp-1.eml"), "rb").read() if os.path.exists(os.path.join(SMTPDIR, "smtp-1.eml")) else b""
env = open(os.path.join(SMTPDIR, "smtp-1.env")).read() if os.path.exists(os.path.join(SMTPDIR, "smtp-1.env")) else ""
mask = lambda x: re.sub(rb"(Date|Message-ID): [^\r]*", rb"\1: X", x)
check(mask(r1) == mask(b) and b"\r\n.dot line\r\n..two dots\r\n" in r1, "the message received byte for byte as built (dots doubled, undone)", (r1[:300], b[:300]))
check(env == "amiga@example.org\nbob@example.org carol@example.org secret@example.org\n" and b"secret" not in r1,
      "Bcc in the envelope only", env)
check(b"Subject: =?UTF-8?B?R3LDvMOfZQ==?=\r\n" in r1 and b"Content-Transfer-Encoding: quoted-printable" in r1 and
      b"=C3=85=C3=84=C3=96" in r1, "the subject as an encoded word, the body quoted-printable", r1[:600])
draft2 = os.path.join(OUT, "draft2.txt")
open(draft2, "w").write("From: amiga@example.org\nTo: x@refused.example.org\nSubject: x\n\ny\n")
rc, out = run("smtp-refused", f"send {P['smtp']} starttls {U} {PW} {draft2}\n")
check("!! send -2 x@refused.example.org: the server said 550 no such user" in out, "a refused recipient said", out)
print("mail-test: %s" % ("all passed" if not bad else "%d failed" % bad))
sys.exit(1 if bad else 0)
