#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Joystick test: FS-UAE maps the keyboard to the joystick in port 2 (joystick_port_1 = keyboard); the keys are pressed
from outside through wtype in a private invisible Wayland session (the cage rig of the Creator project), while
`input -w 16` runs in Zenix. Passes if the up, left, down, right and fire states were all reported.
usage: tools/joystick-test.py   (private serial port, waits for other FS-UAE runs)"""
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fsuae_conf  # noqa: E402
Cage = fsuae_conf.load_cage()

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.dirname(here)
serial, log = "/tmp/zenix-joy-serial", "/tmp/zenix-joy.log"
conf = "/tmp/zenix-joy.fs-uae"
text = fsuae_conf.resolve(open(os.path.join(root, "fsuae/a1200-020-fast.fs-uae")).read(), serial=serial)
text = text.replace("joystick_port_1 = none", "joystick_port_1 = keyboard")
open(conf, "w").write(text)
subprocess.run(["make", "-s", "all"], cwd=root, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
while subprocess.run(["pgrep", "-x", "fs-uae"], stdout=subprocess.DEVNULL).returncode == 0:
    time.sleep(1)
tap = subprocess.Popen([sys.executable, os.path.join(here, "serial-tap.py"), serial, log,
                        os.path.join(here, "tests/joystick-test.txt")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1)
t0 = time.time()
try:
    with Cage(["fs-uae", conf]) as cage:
        time.sleep(max(0, 26.5 - (time.time() - t0)))
        env = cage._env()
        subprocess.run(["wtype", "-k", "x"], env=env, check=False)          # the first key is lost: warm-up
        time.sleep(0.5)
        for k in ("Up", "Left", "Down", "Right", "Control_R"):
            subprocess.run(["wtype", "-P", k, "-s", "1200", "-p", k], env=env, check=False)
            time.sleep(0.4)
        time.sleep(8)
finally:
    tap.terminate()
out = open(log, errors="replace").read()
want = {"up": r"joystick U", "left": r"joystick -+L", "down": r"joystick -D", "right": r"joystick ---R", "fire": r"joystick ----F"}
ok = True
for name, pat in want.items():
    got = re.search(pat, out) is not None
    print(("PASS" if got else "FAIL"), "joystick", name)
    ok &= got
sys.exit(0 if ok else 1)
