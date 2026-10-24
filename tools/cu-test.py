#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""cu test (DESIGN D12): boot Zenix in FS-UAE inside the private headless cage rig (as kbd-test.py), type on the
emulated Amiga keyboard with wtype, and play "the other computer" on the serial port (serial-tap.py with a script).
The keyboard starts `cu > /tmp/cu.out` (its port-to-screen half writes there, so the file shows what reached the
screen), the "PC" sends a line, the keyboard types a line (it must arrive on the serial port), then ~. ends cu and
`echo back` checks that the console's copy on the serial port is back. tools/cu-test.sh reads the results.

usage: cu-test.py config.fs-uae [serial-link]
Swedish key positions (FS-UAE maps host keys by position): > is Shift+<, / is Shift+7, ~ is Shift+` ."""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fsuae_conf  # noqa: E402
Cage = fsuae_conf.load_cage()

conf = sys.argv[1]
link = sys.argv[2] if len(sys.argv) > 2 else "/tmp/zenix-serial"
here = os.path.dirname(os.path.abspath(__file__))
out = os.environ.get("ZENIX_OUT", "/tmp/zenix-cu")
os.makedirs(out, exist_ok=True)
log = f"{out}/serial.log"
script = f"{out}/script.txt"
# the PC side: wait until the console's copy shows the cu command typed (cu starts then), then send a line to it
open(script, "w").write("wait cu\\.out 150\nsleep 4\nsend hello-from-pc\\r\\n\nsleep 40\n")
tap = subprocess.Popen([sys.executable, os.path.join(here, "serial-tap.py"), link, log, script],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1)
t0 = time.time()


def at(t):
    time.sleep(max(0, t0 + t - time.time()))


try:
    with Cage(["fs-uae", conf]) as cage:
        env = cage._env()

        def w(*args):
            subprocess.run(["wtype", *args], env=env, check=False)
            time.sleep(0.15)

        def text(s):
            w("-s", "60", s)

        def key(k, *mods):
            cmd = []
            for m in mods: cmd += ["-P", m]
            if mods: cmd += ["-s", "150"]
            cmd += ["-k", k]
            if mods: cmd += ["-s", "150"]
            for m in mods: cmd += ["-p", m]
            w(*cmd)

        at(24)                                       # the prompt is up
        w("-k", "x"); time.sleep(0.5)                # warm-up: the first key is lost
        w("-k", "BackSpace"); time.sleep(0.3)
        text("cu "); key("less", "Shift_L"); text(" "); key("7", "Shift_L"); text("tmp"); key("7", "Shift_L"); text("cu.out")
        w("-k", "Return")
        at(44)                                       # the PC's line has arrived (sent 4 s after the cu command)
        text("abc"); w("-k", "Return")               # to the other computer
        time.sleep(1)
        key("grave", "Shift_L"); text(".")           # ~. at the start of a line: cu ends
        time.sleep(3)
        text("echo back"); w("-k", "Return")         # the console's copy on the serial port is back
        time.sleep(2)
        text("sync"); w("-k", "Return")
        time.sleep(3)
finally:
    tap.terminate()
