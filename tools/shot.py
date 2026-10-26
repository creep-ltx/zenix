#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Boot Zenix in FS-UAE inside a private invisible Wayland session (cage on
the headless backend, nothing appears on the desktop), type into the serial
port with a script (tools/serial-tap.py format) and save screenshots.

usage: shot.py config.fs-uae script.txt out_prefix t1 [t2 ...]
 -> out_prefix-<t>.png for each time t (seconds after start)
ZENIX_SERIAL=/tmp/name: a private serial port (the config is always copied and filled in by fsuae_conf.py, as run-test.sh does).
RESET_AT=s[,s...]: press Ctrl + Left Amiga + Right Amiga (a warm reset) at those times (tools/diag-test.sh).
SHOT_LIMIT=s: end the run (and its cage and FS-UAE) at most s seconds after the emulator started (default: the last
screenshot or reset + 30 s). The time spent waiting for the machine's emulator lock does not count, so a caller needs no
timeout of its own; a TERM, HUP or INT (a caller's timeout) also ends the cage and FS-UAE before shot.py exits.
Needs the headless cage rig: ZENIX_CAGE_RIG names its directory (tools/fsuae_conf.py)."""
import os
import signal
import subprocess
import sys
import time


def _end(sig, frame):   # a signal or the time limit: leave through the with-block, which ends cage and FS-UAE
    raise SystemExit(128 + sig)


for _s in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT, signal.SIGALRM):
    signal.signal(_s, _end)

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fsuae_conf  # noqa: E402
Cage = fsuae_conf.load_cage()

conf, script, prefix = sys.argv[1:4]
times = sorted(float(t) for t in sys.argv[4:])
here = os.path.dirname(os.path.abspath(__file__))
log = prefix + ".serial.log"
serial = os.environ.get("ZENIX_SERIAL", "/tmp/zenix-serial")
try:                    # the Kickstart directory, the build directory and the serial port filled in (fsuae_conf.py)
    text = fsuae_conf.resolve(open(conf).read(), serial=serial, states=os.path.abspath(prefix) + ".states")
except fsuae_conf.ConfError as e:
    sys.exit(f"shot.py: {e}")
conf = prefix + ".fs-uae"
open(conf, "w").write(text)
resets = sorted(float(t) for t in os.environ.get("RESET_AT", "").split(",") if t)
slot = fsuae_conf.emulator_slot()     # before the tap: its script and the timings start with the emulator
tap = subprocess.Popen([sys.executable, os.path.join(here, "serial-tap.py"),
                        serial, log, script],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1)
t0 = time.time()
try:
    with Cage(["fs-uae", conf]) as cage:
        last = max(times + resets) if times or resets else 0
        signal.alarm(max(1, int(float(os.environ.get("SHOT_LIMIT", last + 30)))))
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
        events = [(t, "shot") for t in times] + [(t, "reset") for t in resets]
        for t, what in sorted(events):
            time.sleep(max(0, t0 + t - time.time()))
            if what == "shot":
                cage.screenshot("%s-%d.png" % (prefix, int(t)))
            else:                                  # the keyboard reset: all three held, then released
                subprocess.run(["wtype", "-P", "Control_L", "-s", "150", "-P", "Super_L", "-s", "150",
                                "-P", "Super_R", "-s", "700", "-p", "Super_R", "-p", "Super_L", "-p", "Control_L"],
                               env=cage._env(), check=False)
finally:
    signal.alarm(0)
    tap.terminate()
