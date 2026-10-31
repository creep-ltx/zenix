#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""ZINE's pictures (DESIGN D29 section 15): boot Zenix in FS-UAE inside the cage rig (as tools/kbd-test.py), run
`zine /tests/zine/ztake` from the serial line, and take a screenshot while the program has the display and one after
Zenix is back. tools/zine-emu-test.sh checks the colours.

usage: zine-shot.py config.fs-uae [serial-link]       (screenshots into the config's directory: during.png, after.png)"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fsuae_conf  # noqa: E402
Cage = fsuae_conf.load_cage()

conf = sys.argv[1]
link = sys.argv[2] if len(sys.argv) > 2 else "/tmp/zenix-serial"
out = os.path.dirname(os.path.abspath(conf))
here = os.path.dirname(os.path.abspath(__file__))
log = os.path.join(out, "serial.log")
script = os.path.join(out, "script.txt")
open(script, "w").write("wait zenix:/# 120\nsleep 2\nsend zine /tests/zine/ztake; echo ztake-rc=$?\\r\n"
                        "wait ztake-rc= 60\nsleep 3\nsend echo shot-done\\r\nwait shot-done 20\n")
slot = fsuae_conf.emulator_slot()
tap = subprocess.Popen([sys.executable, os.path.join(here, "serial-tap.py"), link, log, script],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1)


def seen(s, limit):
    end = time.time() + limit
    while time.time() < end:
        try:
            if s in open(log, "rb").read():
                return True
        except OSError:
            pass
        time.sleep(0.1)
    return False


try:
    with Cage(["fs-uae", conf]) as cage:
        if seen(b"handing the machine to an Amiga program", 180):
            time.sleep(1.5)
            cage.screenshot(os.path.join(out, "during.png"))
        if seen(b"shot-done", 60):
            cage.screenshot(os.path.join(out, "after.png"))
finally:
    tap.terminate()
