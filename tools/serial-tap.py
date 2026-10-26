#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Create a PTY, symlink its slave end at argv[1] for FS-UAE's serial_port,
and append everything received on the master end to the log file argv[2].

Optional argv[3]: a keystroke script driven while logging. Lines:
    sleep <seconds>
    send <text>        (\\r \\n \\t escapes recognized)
    wait <regex> [timeout]
                       wait until the serial output received since the last
                       `send` (or since the start) matches the Python regular
                       expression (carriage returns, ESC[K and the colour
                       codes ESC[...m removed first). After a `send` that
                       ends a line (its text holds \r or \n), only the output
                       after the echo of that line counts: from the first
                       line feed that arrives after the tap typed the last
                       \r or \n of the text. The shell's line editor redraws
                       the prompt with each key, so the echo holds the prompt
                       and the typed text, and a wait for either would match
                       at once (audit 2026-10-26 F1). After a `send` without
                       a line end (a key, a half-typed line) the echo counts.
                       The last word is the timeout in seconds if it is a
                       number (default 60). A timeout is a failure (audit
                       2026-10-26 F7): the script goes on, the tap prints
                       "FAIL: serial-tap: wait timed out after N s: <regex>"
                       on its output and, with ZENIX_TIMEOUTS=FILE set (make
                       check sets it for every job), adds that line to FILE;
                       tools/run-test.sh then exits 3 and make check fails the
                       test. The run ending (the tap stopped) while a wait is
                       still waiting is the same failure ("the run ended
                       during a wait"). With ZENIX_TAP_TRACE=FILE set, each
                       wait that matched writes how long it took to FILE.
    waitopt <regex> [timeout]
                       as `wait`, but the text may never come: a test where
                       it not coming is a correct result (a machine that
                       refuses to start, a login that runs out of memory).
                       A timeout says "serial-tap: optional wait timed out
                       (allowed): <regex>" and is no failure; the test itself
                       checks what happened instead.
    waitecho <regex> [timeout]
                       as `wait`, but the echo of the line counts too (the
                       old rule): for a test that checks what was echoed.
    stop               end the emulator now, as a power cut would (tools/run-test.sh
                       kills FS-UAE when the tap leaves <serial port>.stop); also
                       to end a run as soon as its script is done
    shot <name>        (under tools/shot.py) take a screenshot now: shot.py saves
                       it as <prefix>-<name>.png; the script goes on when it
                       is saved (at most 30 s). Without shot.py: nothing.

After every boot the tap sends one harmless byte pair, 0x1D '.' (the kernel's
test hook with a letter that is no command), half a second after the kernel's
"clock: ... tick latch" line (at a line start), when its receive interrupt is
on. The kernel's serial copy
of the console waits for the port only once it has received something since
boot (else it drops what does not fit, to keep the screen fast); this makes
the log complete even before the script types anything.

The tap holds its own slave fd open so the master never enters the EIO
"no slave" state between emulator sessions."""
import os
import pty
import re
import select
import signal
import sys
import termios
import threading
import time
import tty

link, log = sys.argv[1], sys.argv[2]
script = sys.argv[3] if len(sys.argv) > 3 else None
master, slave = pty.openpty()
tty.setraw(slave, termios.TCSANOW)      # no echo/canonical processing
os.open(os.ttyname(slave), os.O_RDONLY | os.O_NOCTTY)  # keepalive slave fd
try:
    os.unlink(link)
except FileNotFoundError:
    pass
os.symlink(os.ttyname(slave), link)
print(f"serial tap: {link} -> {os.ttyname(slave)}, logging to {log}", flush=True)


received = bytearray()          # everything logged, for `wait`
mark = 0                        # where the output since the last `send` starts
eol_at = None                   # the last `send` ended a line: where the output was when its last \r/\n was typed
cond = threading.Condition()


SGR = re.compile(rb"\x1b\[[0-9;:]*[mK]")   # colour codes and erase-to-end-of-line: not part of the text waited for


def wait_start(echo):
    """Where the output a `wait` searches starts (cond held): None while the echo of the typed line has not ended."""
    if echo or eol_at is None:
        return mark
    lf = received.find(b"\n", eol_at)
    return None if lf < 0 else lf + 1


TRACE = os.environ.get("ZENIX_TAP_TRACE")      # a file: each wait's time and where its search started go there


TIMEOUTS = os.environ.get("ZENIX_TIMEOUTS")   # a file: every unexpected timeout is added there (make check reads it)
pending = None                  # the wait the script is in: (regex, optional), for a run that ends during it


def fail_wait(text):
    line = f"FAIL: serial-tap: {text}"
    print(line, flush=True)
    if TIMEOUTS:
        with open(TIMEOUTS, "a") as f:
            f.write(line + "\n")


def wait_for(pattern, timeout, echo=False, optional=False):
    global pending
    rx = re.compile(pattern.encode("latin1"))
    t0 = time.time()
    end = t0 + timeout
    pending = (pattern, optional)
    try:
        return wait_loop(rx, pattern, t0, end, timeout, echo, optional)
    finally:
        pending = None


def wait_loop(rx, pattern, t0, end, timeout, echo, optional):
    with cond:
        while True:
            start = wait_start(echo)
            text = b"" if start is None else SGR.sub(b"", bytes(received[start:]).replace(b"\r", b""))
            if start is not None and rx.search(text):
                if TRACE:
                    with open(TRACE, "a") as f:
                        f.write(f"wait {pattern!r}: matched after {time.time() - t0:.1f} s, searching from byte {start}\n")
                return True
            left = end - time.time()
            if left <= 0:
                if optional:
                    print(f"serial-tap: optional wait timed out (allowed): {pattern}", flush=True)
                else:
                    fail_wait(f"wait timed out after {timeout:g} s: {pattern}")
                return False
            cond.wait(min(left, 0.5))


TICK_LINE = re.compile(rb"(?:^|\n)clock: [^\n]*tick latch")
send_lock = threading.Lock()    # a `send` line and the greeting never interleave (0x1D takes the next byte)


def greet():
    """0x1D '.' after each boot's "tick latch" line: the kernel then knows someone listens (kernel/serial.c)."""
    seen = 0
    while True:
        with cond:
            while True:
                # at a line start: the previous boot's lines, replayed early in the next boot (before its receive
                # interrupt is on), hold the same text after "> "
                m = TICK_LINE.search(bytes(received), seen)
                if m:
                    seen = m.end()
                    break
                cond.wait(1)
        time.sleep(0.5)
        with send_lock:
            os.write(master, b"\x1d.")


SHOT_REQ = os.environ.get("ZENIX_SHOT_REQ")     # set by shot.py: screenshot requests go there, one name a line


def shot(name):
    if not SHOT_REQ:
        return
    with open(SHOT_REQ, "a") as f:
        f.write(name + "\n")
    end = time.time() + 30
    while not os.path.exists(f"{SHOT_REQ}.{name}") and time.time() < end:
        time.sleep(0.2)


def run_script(path):
    global mark, eol_at
    for line in open(path):
        line = line.rstrip("\n")
        if line.strip() == "stop":
            open(link + ".stop", "w").close()
            return
        if line.startswith("shot "):
            shot(line[5:].strip())
        elif line.startswith("sleep "):
            time.sleep(float(line[6:]))
        elif line.startswith(("wait ", "waitecho ", "waitopt ")):
            cmd, arg = line.split(" ", 1)
            words = arg.rsplit(" ", 1)
            if len(words) == 2 and re.fullmatch(r"[0-9]+(\.[0-9]*)?", words[1]):
                arg, secs = words[0], float(words[1])
            else:
                secs = 60.0
            wait_for(arg, secs, cmd == "waitecho", cmd == "waitopt")
        elif line.startswith("send "):
            text = line[5:].encode().decode("unicode_escape").encode("latin1")
            last_eol = max(text.rfind(b"\r"), text.rfind(b"\n"))
            with cond:
                mark = len(received)
                eol_at = None
            with send_lock:
                for i, ch in enumerate(text):       # trickle like a human types
                    if i == last_eol:
                        with cond:
                            eol_at = len(received)   # its echo's line feed comes after this
                    os.write(master, bytes([ch]))
                    time.sleep(0.02)
    if SHOT_REQ:
        with open(SHOT_REQ, "a") as f:
            f.write("END\n")                # the script is done: shot.py may end the run


def on_term(signum, frame):
    """Stopped (the run is over): a wait still waiting never saw its text, which is the same failure as a timeout."""
    p = pending
    if p is not None:
        if p[1]:
            print(f"serial-tap: the run ended during an optional wait (allowed): {p[0]}", flush=True)
        else:
            fail_wait(f"the run ended during a wait: {p[0]}")
    os._exit(0)


signal.signal(signal.SIGTERM, on_term)
threading.Thread(target=greet, daemon=True).start()
if script:
    threading.Thread(target=run_script, args=(script,), daemon=True).start()

with open(log, "wb", 0) as f:
    while True:
        r, _, _ = select.select([master], [], [], 1)
        if master in r:
            try:
                data = os.read(master, 4096)
            except OSError:
                continue
            if not data:
                continue
            f.write(data)
            with cond:
                received.extend(data)
                cond.notify_all()
