#!/usr/bin/env python3
"""Boot Zenix in FS-UAE inside a private invisible Wayland session (cage on
the headless backend, nothing appears on the desktop), type into the serial
port with a script (tools/serial-tap.py format) and save screenshots.

usage: shot.py config.fs-uae script.txt out_prefix t1 [t2 ...]
 -> out_prefix-<t>.png for each time t (seconds after start)
Needs the cage rig from the Creator project (maker/cage_rig.py)."""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.expanduser("~/Projects/DemoScene/Creator"))
from maker.cage_rig import Cage  # noqa: E402

conf, script, prefix = sys.argv[1:4]
times = sorted(float(t) for t in sys.argv[4:])
here = os.path.dirname(os.path.abspath(__file__))
log = prefix + ".serial.log"
tap = subprocess.Popen([sys.executable, os.path.join(here, "serial-tap.py"),
                        "/tmp/zenix-serial", log, script],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1)
t0 = time.time()
try:
    with Cage(["fs-uae", conf]) as cage:
        time.sleep(6)
        for _ in range(int(os.environ.get("ZOOMOUT", "2"))):   # FS-UAE: Shift+F11 pads the viewport
            subprocess.run(["wtype", "-M", "shift", "-k", "F11", "-m", "shift"],
                           env=cage._env(), check=False)
            time.sleep(0.5)
        kt = os.environ.get("WTYPE")         # text typed on the emulated Amiga keyboard
        if kt:
            time.sleep(max(0, t0 + float(os.environ.get("WTYPE_AT", "16")) - time.time()))
            subprocess.run(["wtype", "-k", "x"], env=cage._env(), check=False)   # warm-up
            subprocess.run(["wtype", "-s", "80", kt, "-k", "Return"], env=cage._env(), check=False)
        wk = os.environ.get("WKEYS")         # e.g. "shift+Up:5": modifier+key pressed N times
        if wk:
            time.sleep(max(0, t0 + float(os.environ.get("WKEYS_AT", "30")) - time.time()))
            subprocess.run(["wtype", "-k", "x"], env=cage._env(), check=False)
            for item in wk.split(","):
                keyspec, _, count = item.partition(":")
                mods, _, key = keyspec.rpartition("+")
                cmd = ["wtype"]
                names = {"shift": "Shift_L", "ctrl": "Control_L", "alt": "Alt_L", "logo": "Super_L"}
                if mods:                           # real key events: FS-UAE ignores xkb modifier state
                    for m in mods.split("+"): cmd += ["-P", names[m]]
                    cmd += ["-s", "150"]           # time for the Amiga keyboard handshake between events
                cmd += ["-k", key]
                if mods:
                    cmd += ["-s", "150"]
                    for m in mods.split("+"): cmd += ["-p", names[m]]
                for _ in range(int(count or 1)):
                    subprocess.run(cmd, env=cage._env(), check=False)
                    time.sleep(0.15)
        for t in times:
            time.sleep(max(0, t0 + t - time.time()))
            cage.screenshot("%s-%d.png" % (prefix, int(t)))
finally:
    tap.terminate()
