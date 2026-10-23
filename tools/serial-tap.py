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
                       expression (carriage returns and ESC[K removed first;
                       the echo of the typed line counts too, so pick a
                       pattern the echo cannot match). The last word is the
                       timeout in seconds if it is a number (default 60). On
                       a timeout the script goes on, and the tap prints
                       "serial-tap: wait timed out: <regex>" on its output.

The tap holds its own slave fd open so the master never enters the EIO
"no slave" state between emulator sessions."""
import os
import pty
import re
import select
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
cond = threading.Condition()


def wait_for(pattern, timeout):
    rx = re.compile(pattern.encode("latin1"))
    end = time.time() + timeout
    with cond:
        while True:
            text = bytes(received[mark:]).replace(b"\r", b"").replace(b"\x1b[K", b"")
            if rx.search(text):
                return True
            left = end - time.time()
            if left <= 0:
                print(f"serial-tap: wait timed out: {pattern}", flush=True)
                return False
            cond.wait(min(left, 0.5))


def run_script(path):
    global mark
    for line in open(path):
        line = line.rstrip("\n")
        if line.startswith("sleep "):
            time.sleep(float(line[6:]))
        elif line.startswith("wait "):
            words = line[5:].rsplit(" ", 1)
            if len(words) == 2 and re.fullmatch(r"[0-9]+(\.[0-9]*)?", words[1]):
                wait_for(words[0], float(words[1]))
            else:
                wait_for(line[5:], 60.0)
        elif line.startswith("send "):
            text = line[5:].encode().decode("unicode_escape")
            with cond:
                mark = len(received)
            for ch in text.encode("latin1"):    # trickle like a human types
                os.write(master, bytes([ch]))
                time.sleep(0.02)


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
