#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Make an FS-UAE config from fsuae/*.fs-uae runnable on this machine.

The configs in the repository name no path of the machine they were written on. Three things are filled in:
  $ZENIX_KICKSTART_DIR   the directory with the Kickstart ROM files (kicka1200.rom, kicka3000.rom, kicka4000.rom,
                         kick204-a500plus.rom). From the environment variable of that name; default FS-UAE's own
                         Kickstarts directory (Documents/FS-UAE/Kickstarts in the home directory).
  ../build/              the build directory: ZENIX_BUILD if set, else build/ of this source tree.
  /tmp/zenix-serial      the serial port: ZENIX_SERIAL if set (each test run its own port).
With ZENIX_FAST=1 the machine also runs in FS-UAE's warp mode (make_fast; tools/check.sh's fast mode).

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


def fast_mode():
    """ZENIX_FAST=1: the fast mode of the tests (tools/check.sh sets it; docs/README.md "Fast mode")."""
    return os.environ.get("ZENIX_FAST", "") not in ("", "0")


def make_fast(text):
    """The same machine, run as fast as the PC can: FS-UAE's warp mode (the emulated time runs ahead of the clock on
    the wall, 6-13 times as measured; the chipset and the CPU stay cycle-exact, so the kernel sees exactly the machine
    it sees at stock speed), and the fast floppy drive where the config does not choose a speed itself.
    Measured 2026-11-04 (docs/README.md "Fast mode"): a faster CPU instead (accuracy 0 or -1 with uae_cpu_speed =
    max, with or without the JIT) let FS-UAE's IDE emulation hang for good (BSY after a READ SECTORS of 2) in 5 of 8
    boots, and a cycle-exact 68020 ignores uae_cpu_frequency."""
    if not re.search(r"^warp_mode *=", text, re.M):
        text = text.rstrip("\n") + "\nwarp_mode = 1\n"
    if not re.search(r"^floppy_drive_speed *=", text, re.M):
        text = text.rstrip("\n") + "\nfloppy_drive_speed = 0\n"
    return text


def resolve(text, serial=None, build=None, states=None, fast=None):
    if fast is None:
        fast = fast_mode()
    if fast:
        text = make_fast(text)
    kdir = kickstart_dir()
    text = text.replace("$ZENIX_KICKSTART_DIR", kdir)
    serial = serial or os.environ.get("ZENIX_SERIAL")
    if serial:
        text = text.replace("/tmp/zenix-serial", serial)
    build = (build or os.environ.get("ZENIX_BUILD") or os.path.join(ROOT, "build")).rstrip("/") + "/"
    text = re.sub(r"(\.\./)+build/", lambda m: build, text)
    if states and not re.search(r"^save_states_dir *=", text, re.M):
        # FS-UAE keeps a floppy's writes (its save-state overlay) under the config's name, in one folder for all
        # runs: two runs of the same config name would read each other's. Each run gets a folder of its own.
        text = text.rstrip("\n") + f"\nsave_states_dir = {states}\n"
    for rom in re.findall(r"^kickstart_file *= *(.+?) *$", text, re.M):
        if not os.path.isfile(rom):
            raise ConfError(f"Kickstart ROM {rom} not found: set ZENIX_KICKSTART_DIR to the directory that holds it")
    return text


# Up to ZENIX_EMU_SLOTS (default 6) FS-UAEs at once, each with its own serial port and images (tools/run-test.sh says
# more). A run takes the first free slot - lock file /tmp/zenix-emulator.lock (slot 0, the one other projects on this
# machine take with flock) or /tmp/zenix-emulator.lock.N - and holds it until its run ends. No run waits for anything
# but a free slot (and a load of 16 or less), so no two trees can wait for each other.
EMULATOR_LOCK = os.environ.get("ZENIX_EMU_LOCK", "/tmp/zenix-emulator.lock")
_held = None                        # the slot this process holds (emulator_slot), so a Cage inside it takes no second one


class _Slot:
    def __init__(self, f):
        self.f = f

    def close(self):
        global _held
        if self.f:
            self.f.close()
            self.f = None
            if _held is self:
                _held = None


def emulator_slot():
    """take a free emulator slot (waiting while none is free or the load is above 16); close() the result to let go.
    A process that already holds one gets a handle that lets go of nothing: take the slot before starting the serial
    tap, so the tap's script and the timings start with the emulator, not with the wait."""
    import fcntl
    import time
    global _held
    if _held:
        return _Slot(None)
    n = max(1, int(os.environ.get("ZENIX_EMU_SLOTS") or 6))
    while True:
        for i in range(n):
            f = open(EMULATOR_LOCK if i == 0 else f"{EMULATOR_LOCK}.{i}", "a")
            try:
                fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError:
                f.close()
                continue
            while float(open("/proc/loadavg").read().split()[0]) > 16:
                time.sleep(30)
            _held = _Slot(f)
            return _held
        time.sleep(2)


def load_cage():
    rig = os.environ.get("ZENIX_CAGE_RIG")
    if not rig:
        sys.exit("set ZENIX_CAGE_RIG to the directory that holds maker/cage_rig.py (the headless cage rig)")
    sys.path.insert(0, os.path.expanduser(rig))
    from maker.cage_rig import Cage as RigCage

    class Cage(RigCage):            # the rig's session, holding an emulator slot from its start to its end
        def __init__(self, *args, **kw):
            self._slot = emulator_slot()
            try:
                super().__init__(*args, **kw)
            except BaseException:          # a signal while cage starts: end what was started, then let go
                try:
                    if getattr(self, "proc", None):
                        self.stop()
                finally:
                    self._slot.close()
                raise

        def __exit__(self, *exc):
            try:
                return super().__exit__(*exc)
            finally:
                self._slot.close()
    return Cage


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    try:
        dst = os.path.abspath(sys.argv[2])
        text = resolve(open(sys.argv[1]).read(), states=os.path.splitext(dst)[0] + ".states")
    except ConfError as e:
        sys.exit(f"fsuae_conf: {e}")
    with open(sys.argv[2], "w") as f:
        f.write(text)


if __name__ == "__main__":
    main()
