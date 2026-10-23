#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Make an FS-UAE config from fsuae/*.fs-uae runnable on this machine.

The configs in the repository name no path of the machine they were written on. Three things are filled in:
  $ZENIX_KICKSTART_DIR   the directory with the Kickstart ROM files (kicka1200.rom, kicka3000.rom, kicka4000.rom,
                         kick204-a500plus.rom). From the environment variable of that name; default FS-UAE's own
                         Kickstarts directory, ~/Documents/FS-UAE/Kickstarts.
  ../build/              the build directory: ZENIX_BUILD if set, else build/ of this source tree.
  /tmp/zenix-serial      the serial port: ZENIX_SERIAL if set (each test run its own port).

usage: tools/fsuae_conf.py SRC.fs-uae DST.fs-uae     (tools/run-test.sh, tools/kbd-test.sh and make run use it)
From Python: import fsuae_conf; text = fsuae_conf.resolve(open(src).read())
A missing Kickstart file is an error that names the variable to set.

The screenshot and keyboard tests (shot.py, kbd-test.py, joystick-test.py) also need the headless cage rig (a private
Wayland session for FS-UAE): ZENIX_CAGE_RIG names the directory that holds maker/cage_rig.py; load_cage() imports it."""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_KICKSTART_DIR = os.path.join("~", "Documents", "FS-UAE", "Kickstarts")


class ConfError(Exception):
    pass


def kickstart_dir():
    return os.path.expanduser(os.environ.get("ZENIX_KICKSTART_DIR") or DEFAULT_KICKSTART_DIR)


def resolve(text, serial=None, build=None):
    kdir = kickstart_dir()
    text = text.replace("$ZENIX_KICKSTART_DIR", kdir)
    serial = serial or os.environ.get("ZENIX_SERIAL")
    if serial:
        text = text.replace("/tmp/zenix-serial", serial)
    build = (build or os.environ.get("ZENIX_BUILD") or os.path.join(ROOT, "build")).rstrip("/") + "/"
    text = re.sub(r"(\.\./)+build/", lambda m: build, text)
    for rom in re.findall(r"^kickstart_file *= *(.+?) *$", text, re.M):
        if not os.path.isfile(rom):
            raise ConfError(f"Kickstart ROM {rom} not found: set ZENIX_KICKSTART_DIR to the directory that holds it")
    return text


def load_cage():
    rig = os.environ.get("ZENIX_CAGE_RIG")
    if not rig:
        sys.exit("set ZENIX_CAGE_RIG to the directory that holds maker/cage_rig.py (the headless cage rig)")
    sys.path.insert(0, os.path.expanduser(rig))
    from maker.cage_rig import Cage
    return Cage


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    try:
        text = resolve(open(sys.argv[1]).read())
    except ConfError as e:
        sys.exit(f"fsuae_conf: {e}")
    with open(sys.argv[2], "w") as f:
        f.write(text)


if __name__ == "__main__":
    main()
