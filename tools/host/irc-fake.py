#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""A small fake IRC server for irc's tests (DESIGN D58 section 8): tools/host/irc-test.sh on the PC and
tools/irc-test.sh in FS-UAE (through tools/irc-ns.py). Nothing real: no network beyond the address given, no
passwords but the tests' own. One thread a client; every line received is logged ("N > LINE", N the connection's
number) and so is every line sent ("N < LINE"), to LOG.

What it does, so that a test can drive it from the client's side alone:
  registration  NICK/USER (a NICK "taken" gets 433), PASS, CAP REQ :sasl -> ACK, AUTHENTICATE PLAIN -> "+", the
                PLAIN blob checked against SASL_USER/SASL_PASS (903 or 904); then 001, 005 (PREFIX=(qov)~@+), a MOTD
                with an ISO-8859-1 line, a UTF-8 line, mIRC colours, an escape sequence trying to clear the screen, a
                line of 1,200 bytes, garbage (a lone ":", a command with no parameters, NUL and C1 bytes), and a PING
                whose PONG is logged
  JOIN #c       the JOIN echoed, a topic (332), names (353: ~owner @op +voice bob, 366), then bob says hello to the
                client's nick (a highlight) and the connection's number
  PRIVMSG #c    "!drop": the server closes the connection (the client's reconnect); "!flood": three lines from
                others and a CTCP VERSION from bob; "!kick": the client kicked; "!mode": +o and -v on members;
                "!quit": bob quits; "!nick": bob becomes robert; any other text: logged only
  PRIVMSG bob   bob answers in private ("hi back")
  WHOIS, NAMES, TOPIC, NICK, PART, MODE, NOTICE, QUIT as a server would answer them

  irc-fake.py [--tls CERT KEY] [--sasl USER PASS] [--host H] PORT LOG      (runs until killed)
As a module: serve(host, port, log, ssl_context=None, sasl=None) starts a listener thread."""
import base64
import socket
import sys
import threading

LOCK = threading.Lock()
COUNT = [0]


def serve(host, port, logpath, ctx=None, sasl=None):
    ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind((host, port))
    ls.listen(8)
    log = open(logpath, "a", buffering=1, encoding="utf-8", errors="backslashreplace")

    def run():
        while True:
            c, _ = ls.accept()
            with LOCK:
                COUNT[0] += 1
                n = COUNT[0]
            threading.Thread(target=client, args=(c, n, log, ctx, sasl), daemon=True).start()
    t = threading.Thread(target=run, daemon=True)
    t.start()
    return t


def client(c, n, log, ctx, sasl):
    if ctx:
        try:
            c = ctx.wrap_socket(c, server_side=True)
        except Exception as e:
            with LOCK:
                log.write("%d ! TLS %s\n" % (n, e))
            c.close()
            return
    S = "fake.test"
    st = {"nick": None, "user": False, "reg": False, "sasl": None}

    def send(b):
        if isinstance(b, str):
            b = b.encode("utf-8")
        with LOCK:
            log.write("%d < %s\n" % (n, b[:200].decode("latin-1")))
        try:
            c.sendall(b + b"\r\n")
        except OSError:
            pass

    def num(code, rest):
        send(":%s %s %s %s" % (S, code, st["nick"] or "*", rest))

    def register():
        st["reg"] = True
        nk = st["nick"]
        num("001", ":Welcome to the fake network %s" % nk)
        num("005", "PREFIX=(qov)~@+ CHANTYPES=# CHANMODES=beI,k,l,imnpst :are supported")
        num("375", ":- fake.test message of the day")
        send(b":fake.test 372 " + nk.encode() + b" :- Latin-1: r\xe4ksm\xf6rg\xe5s")
        send(":fake.test 372 %s :- UTF-8: räksmörgås ✓" % nk)
        send(":fake.test 372 %s :- \x0304,01red\x03 \x02bold\x02 and \x1b[2Jclear" % nk)
        send(":fake.test 372 %s :- %s" % (nk, "L" * 1200 + "END"))
        send(b":")
        send(b"PRIVMSG")
        send(b":fake.test 372 " + nk.encode() + b" :- nul\x00here c1\xc2\x9b2J done")
        num("376", ":End of MOTD")
        send("PING :fake-token-%d" % n)

    buf = b""
    while True:
        try:
            d = c.recv(4096)
        except OSError:
            break
        if not d:
            break
        buf += d
        while b"\n" in buf:
            raw, buf = buf.split(b"\n", 1)
            raw = raw.rstrip(b"\r")
            line = raw.decode("utf-8", "backslashreplace")
            with LOCK:
                log.write("%d > %s\n" % (n, line))
            if not line:
                continue
            parts = line.split(" :", 1)
            words = parts[0].split()
            text = parts[1] if len(parts) > 1 else ""
            cmd = words[0].upper()
            arg = words[1:]
            me = "%s!user@zenix.test" % st["nick"]
            if cmd == "CAP" and arg[:1] == ["REQ"]:
                send(":%s CAP * %s :%s" % (S, "ACK" if sasl and "sasl" in text else "NAK", text))
            elif cmd == "CAP" and arg[:1] == ["END"]:
                if st["nick"] and st["user"] and not st["reg"]:
                    register()
            elif cmd == "AUTHENTICATE":
                if arg == ["PLAIN"]:
                    send("AUTHENTICATE +")
                else:
                    try:
                        z = base64.b64decode(arg[0]).split(b"\0")
                    except Exception:
                        z = []
                    ok = sasl and len(z) == 3 and z[1].decode() == sasl[0] and z[2].decode() == sasl[1]
                    if ok:
                        num("900", "%s %s :You are now logged in" % (me, sasl[0]))
                        num("903", ":SASL authentication successful")
                    else:
                        num("904", ":SASL authentication failed")
            elif cmd == "NICK":
                new = (arg[0] if arg else text)
                if new == "taken":
                    send(":%s 433 * %s :Nickname is already in use" % (S, new))
                    continue
                if st["reg"]:
                    send(":%s NICK :%s" % (me, new))
                st["nick"] = new
                if st["user"] and not st["reg"] and st["sasl"] is None and not sasl:
                    register()
            elif cmd == "USER":
                st["user"] = True
                if st["nick"] and not st["reg"] and not sasl:
                    register()
            elif cmd == "PONG":
                pass
            elif cmd == "PING":
                send(":%s PONG %s :%s" % (S, S, text or (arg[0] if arg else "")))
            elif cmd == "JOIN":
                for ch in arg[0].split(","):
                    send(":%s JOIN %s" % (me, ch))
                    num("332", "%s :the topic of %s" % (ch, ch))
                    num("353", "= %s :%s ~owner @op +voice bob" % (ch, st["nick"]))
                    num("366", "%s :End of /NAMES list." % ch)
                    send(":bob!b@bob.test PRIVMSG %s :hello %s, welcome (connection %d)" % (ch, st["nick"], n))
            elif cmd == "PART":
                send(":%s PART %s :%s" % (me, arg[0], text))
            elif cmd == "NAMES":
                num("353", "= %s :%s ~owner @op +voice bob" % (arg[0], st["nick"]))
                num("366", "%s :End of /NAMES list." % arg[0])
            elif cmd == "TOPIC":
                if text:
                    send(":%s TOPIC %s :%s" % (me, arg[0], text))
            elif cmd == "WHOIS":
                num("311", "%s b bob.test * :Bob Fake" % arg[0])
                num("312", "%s fake.test :the fake server" % arg[0])
                num("318", "%s :End of /WHOIS list." % arg[0])
            elif cmd == "QUIT":
                send("ERROR :Closing link (%s)" % text)
                c.close()
                return
            elif cmd == "PRIVMSG" and arg and arg[0].startswith("#"):
                ch = arg[0]
                if text == "!drop":
                    c.close()
                    return
                if text == "!flood":
                    for who in ("alice", "carol", "dave"):
                        send(":%s!x@y PRIVMSG %s :line from %s" % (who, ch, who))
                    send(":bob!b@bob.test PRIVMSG %s :\x01VERSION\x01" % st["nick"])
                if text == "!kick":
                    send(":op!o@op.test KICK %s %s :out you go" % (ch, st["nick"]))
                if text == "!mode":
                    send(":op!o@op.test MODE %s +o-v+l bob voice 10" % ch)
                if text == "!quit":
                    send(":bob!b@bob.test QUIT :gone fishing")
                if text == "!nick":
                    send(":bob!b@bob.test NICK robert")
                if text == "!action":
                    send(":voice!v@v.test PRIVMSG %s :\x01ACTION waves\x01" % ch)
            elif cmd == "PRIVMSG" and arg and arg[0] == "bob":
                send(":bob!b@bob.test PRIVMSG %s :hi back" % st["nick"])
            elif cmd == "NOTICE":
                pass
    try:
        c.close()
    except OSError:
        pass


if __name__ == "__main__":
    a = sys.argv[1:]
    ctx = sasl = None
    host = "127.0.0.1"
    while a and a[0].startswith("--"):
        if a[0] == "--tls":
            import ssl
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx.load_cert_chain(a[1], a[2])
            a = a[3:]
        elif a[0] == "--sasl":
            sasl = (a[1], a[2])
            a = a[3:]
        elif a[0] == "--host":
            host = a[1]
            a = a[2:]
        else:
            sys.exit("irc-fake.py: unknown option " + a[0])
    serve(host, int(a[0]), a[1], ctx, sasl).join()
