#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""irc's sessions on the PC (DESIGN D58 section 8; run by tools/host/irc-test.sh): tools/host/irc-client (the core
over sockets, no screen) against tools/host/irc-fake.py on 127.0.0.1, at ports of its own:
  plain   a nick taken (433: another), registration, the MOTD's ISO-8859-1 and UTF-8, mIRC codes and an escape
          sequence dropped, a line over 512 bytes cut and said, garbage lines survived, PING answered; the autojoin;
          a highlight; messages both ways, /me, a query, /whois, /names, /topic, modes, a flood with a CTCP VERSION
          answered once, a member's quit, /nick, nick completion; the server dropping the connection: connected
          again and the channel joined again; /part, /quit with its message
  sasl    SASL PLAIN accepted (903), and a wrong password refused (904) with the login going on
  tls     the same fake server behind TLS with a test CA made at this run (tools/host/tls-test.py's make_pki):
          registration over TLS; an unknown root and a wrong name refused
  irc-test.py OUT CLIENT"""
import importlib.util
import os
import re
import socket
import ssl
import subprocess
import sys
import time

OUT, CLIENT = sys.argv[1], sys.argv[2]
HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("fake", os.path.join(HERE, "irc-fake.py"))
fake = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fake)
bad = 0


def check(ok, what, detail=""):
    global bad
    print(("PASS " if ok else "FAIL ") + what + ("" if ok else ": " + repr(detail)[-600:]))
    bad += not ok


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def run(name, script, conf, port, extra=()):
    sp = os.path.join(OUT, name + ".script")
    cp = os.path.join(OUT, name + ".conf")
    open(sp, "w").write(script)
    open(cp, "w").write(conf)
    r = subprocess.run([CLIENT] + list(extra) + ["--config", cp, str(port), sp], stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, timeout=120)
    out = r.stdout.decode("utf-8", "surrogateescape")
    open(os.path.join(OUT, name + ".out"), "w", errors="surrogateescape").write(out)
    return r.returncode, out


def log(name):
    time.sleep(0.3)
    return open(os.path.join(OUT, name + ".log"), encoding="utf-8", errors="replace").read()


# ---- plain ----
pp = free_port()
fake.serve("127.0.0.1", pp, os.path.join(OUT, "plain.log"))
script = """expect End of MOTD
expect hello taken_, welcome
type hi everyone
type /me waves back
complete b 0
complete o 0
complete o 1
complete o 2
type /msg bob hello bob
expect hi back
window 3
type /whois bob
expect Bob Fake
window 2
type /names
expect #zenix: 5 nicks
type /topic new topic here
expect changed the topic to: new topic here
type !mode
expect mode #zenix [+o-v+l bob voice 10]
dump
type !flood
expect line from dave
expect CTCP VERSION from bob
type !action
expect * voice waves
type !quit
expect bob has quit (gone fishing)
type /nick zen2
expect you are now known as zen2
type /nosuchcommand x
mark
type !drop
lost
expect has joined #zenix
dump
type /part #zenix bye channel
type /quit bye now
"""
rc, out = run("plain", script, "nick taken\nautojoin #zenix\nhighlight amiga\nquit see you\n", pp)
L = log("plain")
check(rc == 0 and "!! quit" in out, "plain: the script ran to its end", out[-800:])
check("1 > NICK taken\n" in L and "1 > NICK taken_\n" in L and "[status] -!- you are now known" not in out.split("End of MOTD")[0],
      "433 while registering: the nick taken_ tried", L[:400])
check("räksmörgås" in out.split("Latin-1:")[1][:20] if "Latin-1:" in out else False,
      "the MOTD's ISO-8859-1 line shown as UTF-8")
check("UTF-8: räksmörgås ✓" in out, "the MOTD's UTF-8 line kept")
check("- red bold and [2Jclear" in out, "mIRC's colours and the escape sequence dropped (no ESC byte)")
check("\x1b" not in out and "\x9b" not in out and "\x00" not in out, "no ESC, CSI or NUL anywhere in the windows' lines")
check("the server sent a line over 512 bytes" in out and "LEND" not in out, "a line of 1,200 bytes: cut at 512 and said", out[:3000])
check("nul here c12J done" in out, "NUL as a space, the C1 CSI dropped")
check("[status] <>" not in out and "!! " not in out.split("End of MOTD")[0], "garbage lines (a lone colon, PRIVMSG alone) survived, not shown")
check("1 > PONG :fake-token-1" in L, "the server's PING answered")
check("1 > JOIN #zenix" in L, "autojoin after 001")
check("[#zenix] <bob> hello taken_, welcome" in out, "a channel message in its window")
check("1 > PRIVMSG #zenix :hi everyone" in L and "[#zenix] <taken_> hi everyone" in out, "text typed in the channel sent and shown")
check("1 > PRIVMSG #zenix :\\x01ACTION waves back\\x01" in L or "1 > PRIVMSG #zenix :\x01ACTION waves back\x01" in L,
      "/me sent as a CTCP ACTION", L[-2000:])
check("!! complete b 0: bob" in out and "!! complete o 0: owner" in out and "!! complete o 1: op" in out and
      "!! complete o 2: (none)" in out, "nick completion: the members, our own nick left out", out)
check("1 > PRIVMSG bob :hello bob" in L and "[bob] <bob> hi back" in out, "/msg sent; the answer opened a query window")
check("[bob] -!- bob b bob.test * Bob Fake" in out, "/whois: the answer in the current window", out[-3000:])
check("@op" in out and "!! nicks 5" in out, "names: 5 members with their prefixes", out)
check("1 > TOPIC #zenix :new topic here" in L, "/topic sent")
check("mode #zenix [+o-v+l bob voice 10] by op" in out, "a channel mode shown")
check("!! nicks 5:  taken_ ~owner @op  voice @bob" in out,
      "+o and -v changed the members' prefixes (+l's argument skipped)", out[out.find("!! nicks"):][:200])
check(L.count("NOTICE bob :\x01VERSION Zenix irc") == 1 or L.count("NOTICE bob :\\x01VERSION Zenix irc") == 1,
      "a CTCP VERSION answered once", L[-1500:])
check("* voice waves" in out, "an ACTION from another shown")
check("bob has quit (gone fishing)" in out, "a member's QUIT shown in the channel")
check("1 > NICK zen2" in L and "you are now known as zen2" in out, "/nick")
check("1 > NOSUCHCOMMAND x" in L, "an unknown /command goes to the server as it is")
check("2 > NICK zen2" in L, "the dropped connection: connected again, with the nick in use", L[-1500:])
check("2 > JOIN #zenix" in L, "after the reconnect the channel joined again")
check("disconnected: the server closed the connection" in out, "the loss said in the windows")
check("2 > PART #zenix :bye channel" in L, "/part with a message")
check("2 > QUIT :bye now" in L, "/quit with a message")

# ---- SASL ----
sp_ = free_port()
fake.serve("127.0.0.1", sp_, os.path.join(OUT, "sasl.log"), sasl=("acc", "s3cret pass"))
rc, out = run("sasl", "expect End of MOTD\ntype /quit\n", "nick zz\nsasl acc s3cret pass\n", sp_)
L = log("sasl")
check(rc == 0 and "You are now logged in" in out and re.search(r"\d > CAP END", L) and re.search(r"\d > AUTHENTICATE PLAIN", L),
      "SASL PLAIN accepted (900, 903), then CAP END", out[-600:])
rc, out = run("sasl2", "expect End of MOTD\ntype /quit\n", "nick zz\nsasl acc wrong\n", sp_)
check(rc == 0 and "SASL: the login failed (904)" in out, "a wrong SASL password said (904), the login goes on",
      out[-600:])
rc, out = run("sasl3", "expect End of MOTD\ntype /quit\n", "nick zz\nsasl acc wrong\n", pp)
check(rc == 0 and "the server has no SASL" in out, "a server without SASL (NAK): said, the login goes on", out[-600:])

# ---- TLS ----
spec = importlib.util.spec_from_file_location("tlstest", os.path.join(HERE, "tls-test.py"))
tt = importlib.util.module_from_spec(spec)
saved = sys.argv
sys.argv = [saved[0]]
spec.loader.exec_module(tt)
sys.argv = saved
pki = os.path.join(OUT, "pki")
os.makedirs(pki, exist_ok=True)
sites, anchors, _ = tt.make_pki(pki)


def ctx_for(host):
    c = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    c.minimum_version = ssl.TLSVersion.TLSv1_2
    c.maximum_version = ssl.TLSVersion.TLSv1_2
    c.load_cert_chain(*sites[host][0])
    return c


tp = free_port()
fake.serve("127.0.0.1", tp, os.path.join(OUT, "tls.log"), ctx_for("rsa.test"))
rc, out = run("tls", "expect End of MOTD\ntype /join #tls\nexpect hello zt, welcome\ntype hello over tls\ntype /quit\n",
              "nick zt\n", tp, ["--tls", "--anchors", anchors, "--name", "rsa.test"])
L = log("tls")
check(rc == 0 and "!! TLS ok" in out and re.search(r"\d > PRIVMSG #tls :hello over tls", L) and re.search(r"(\d) > PONG :fake-token-\1", L),
      "TLS: registration, a channel, messages both ways over TLS 1.2", out[-800:])
rc, out = run("tls-name", "end\n", "nick zt\n", tp, ["--tls", "--anchors", anchors, "--name", "other.test"])
check(rc == 1 and "!! TLS:" in out and "!! TLS ok" not in out, "TLS: a certificate for another name refused", out)
up = free_port()
fake.serve("127.0.0.1", up, os.path.join(OUT, "tls2.log"), ctx_for("unknown.test"))
rc, out = run("tls-unknown", "end\n", "nick zt\n", up, ["--tls", "--anchors", anchors, "--name", "unknown.test"])
check(rc == 1 and "!! TLS:" in out and "!! TLS ok" not in out, "TLS: an unknown root refused", out)
print("irc-test: %s" % ("all passed" if not bad else "%d failed" % bad))
sys.exit(1 if bad else 0)
