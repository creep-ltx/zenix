#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Keyboard test: boot Zenix in FS-UAE inside a private invisible Wayland session (the cage rig, as shot.py), press
keys on the emulated Amiga keyboard with wtype, make the shell write what arrived into files, then read the files
back from the disk image and compare. Checks the Swedish keymap's four layers (plain, Shift, Alt, Shift+Alt), Caps
Lock, software key repeat (a held key), typing while heavy output runs (nothing lost), and the virtual consoles
(Amiga+F2 shows console 2 with its own login, Amiga+F1 goes back).

usage: kbd-test.py config.fs-uae [serial-link]      (run by tools/kbd-test.sh)
FS-UAE maps host keys to Amiga raw codes by position (a US host layout): host [ ; ' are Amiga 0x1A 0x29 0x2A.
Traps (knowledge base, zenix-m0-m3-findings.md): the first key sent is lost, modifiers need real key events."""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fsuae_conf  # noqa: E402
Cage = fsuae_conf.load_cage()

conf = sys.argv[1]
link = sys.argv[2] if len(sys.argv) > 2 else "/tmp/zenix-serial"
out = os.path.dirname(os.path.abspath(conf))         # kbd-test.sh's $OUT: the config, the log and the screenshots together
here = os.path.dirname(os.path.abspath(__file__))
log = os.path.join(out, "serial.log")
os.makedirs(out, exist_ok=True)
script = os.path.join(out, "script.txt")
open(script, "w").write("sleep 1\n")
slot = fsuae_conf.emulator_slot()     # before the tap: its script and the timings start with the emulator
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

        def text(s):                                 # plain ASCII through wtype's own key lookup
            w("-s", "60", s)

        def key(k, *mods):                           # a key with real modifier events around it
            cmd = []
            for m in mods: cmd += ["-P", m]
            if mods: cmd += ["-s", "150"]
            cmd += ["-k", k]
            if mods: cmd += ["-s", "150"]
            for m in mods: cmd += ["-p", m]
            w(*cmd)

        def redir(name):                             # " > /tmp/NAME" in Swedish key positions: > is Shift+<, / is Shift+7
            text(" "); key("less", "Shift_L"); text(" "); key("7", "Shift_L"); text("tmp"); key("7", "Shift_L"); text(name)

        at(22)                                       # the shell prompt is up (a 68020 boots in about 15 s)
        w("-k", "x")                                 # warm-up: the first key is lost
        time.sleep(0.5)
        # 1. layers: å ö ä (plain), £ (Shift+3), [ (Alt+å key), { (Shift+Alt+å key), ² (Alt+2), \ (Alt+<, typed twice:
        # the shell takes \\ as one backslash). The host's right Alt: the left one never reaches FS-UAE in the cage.
        text("echo ")
        key("bracketleft"); key("semicolon"); key("apostrophe")
        key("3", "Shift_L")
        key("bracketleft", "Alt_R"); key("bracketleft", "Alt_R", "Shift_R")
        key("2", "Alt_R")
        key("less", "Alt_R"); key("less", "Alt_R")   # host 'less' = the extra key left of Z (raw 0x30)
        redir("k1.txt"); w("-k", "Return")
        time.sleep(1)
        # 2. Caps Lock: letters upper case, digits unchanged, Shift gives lower case
        text("echo ")
        w("-k", "Caps_Lock"); time.sleep(0.3)
        text("ab1"); key("c", "Shift_L")
        w("-k", "Caps_Lock"); time.sleep(0.3)
        text("d"); redir("k2.txt"); w("-k", "Return")
        time.sleep(1)
        # 3. repeat: hold z for 1.2 s: 1 + about (1200 - 500) / 30 = roughly 24 z's; anything from 10 to 40 is right
        text("echo ")
        w("-P", "z", "-s", "1200", "-p", "z")
        redir("k3.txt"); w("-k", "Return")
        time.sleep(2)
        # 4. typing while the screen is busy (audit B4): keys typed during heavy output wait in the input ring and the
        # tty queue, then run; nothing is lost (the kernel log would say so, k5.txt)
        text("seq 1 3000"); w("-k", "Return")
        time.sleep(0.5)
        text("echo typed during output"); redir("k4.txt"); w("-k", "Return")
        time.sleep(40)
        # 4b. scrollback at the shell's prompt (its line editor is in raw mode): Amiga+Up shows older lines,
        # Amiga+Down comes back. Screenshots compared by kbd-test.sh.
        cage.screenshot(os.path.join(out, "sb0.png"))
        key("Up", "Super_L"); time.sleep(1)
        cage.screenshot(os.path.join(out, "sb1.png"))
        key("Down", "Super_L"); time.sleep(1)
        cage.screenshot(os.path.join(out, "sb2.png"))
        # 5. virtual consoles: Amiga+F2 shows console 2 and its own login prompt; log in as root there (autologin is
        # console 1's only); two logins are then running. Amiga+F1 goes back: console 1's shell has no jobs, console
        # 2's has the sleep started there.
        key("F2", "Super_L")
        time.sleep(4)
        text("root"); w("-k", "Return")
        time.sleep(4)
        text("sleep 300 "); key("6", "Shift_L"); w("-k", "Return")    # & is Shift+6 on a Swedish keyboard
        time.sleep(1)
        text("pgrep login"); redir("k6.txt"); w("-k", "Return")       # (no | or -: other keys there)
        time.sleep(2)
        key("F1", "Super_L")
        time.sleep(2)
        text("jobs"); redir("k7.txt"); w("-k", "Return")
        time.sleep(2)
        text("dmesg"); redir("k5.txt"); w("-k", "Return")
        time.sleep(2)
        text("sync"); w("-k", "Return")
        time.sleep(3)
        cage.screenshot(os.path.join(out, "screen.png"))
finally:
    tap.terminate()
