#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The random devices' first boot (DESIGN D19 9.3), part f of tools/random-test.sh: boot a fresh image in FS-UAE inside
the headless cage rig (as kbd-test.py), run the serial script given, and when the Amiga prints "type-now" press keys on
the emulated keyboard with wtype (spaces, then Return): only key presses and releases (1 bit each) can seed the pool
of a fresh image, so /dev/random must wait until then.

usage: random-test.py config.fs-uae serial-link script.txt log seconds"""
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fsuae_conf  # noqa: E402
Cage = fsuae_conf.load_cage()

conf, link, script, log, secs = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], int(sys.argv[5])
here = os.path.dirname(os.path.abspath(__file__))
slot = fsuae_conf.emulator_slot()
tap = subprocess.Popen([sys.executable, os.path.join(here, "serial-tap.py"), link, log, script],
                       stdout=open(log + ".tap", "w"), stderr=subprocess.STDOUT)
time.sleep(1)
t0 = time.time()
stop = link + ".stop"
try:
    with Cage(["fs-uae", conf]) as cage:
        env = cage._env()

        def seen(rx):
            try:
                return re.search(rx, open(log, "rb").read().decode("latin-1").replace("\r", ""), re.M)
            except OSError:
                return None
        while time.time() - t0 < secs and not seen(r"^type-now"):
            time.sleep(0.5)
        subprocess.run(["wtype", "-k", "x"], env=env, check=False)      # warm-up: the first key is lost
        time.sleep(0.5)
        subprocess.run(["wtype", "-k", "BackSpace"], env=env, check=False)
        for _ in range(4):                                               # 4 x 60 spaces: 480 events, 480 bits
            subprocess.run(["wtype", "-d", "40", " " * 60], env=env, check=False)
            time.sleep(0.3)
        subprocess.run(["wtype", "-k", "Return"], env=env, check=False)
        while time.time() - t0 < secs and not os.path.exists(stop) and tap.poll() is None:
            time.sleep(0.5)
finally:
    tap.terminate()
    tap.wait()
    slot.close()
    if os.path.exists(stop):
        os.unlink(stop)
