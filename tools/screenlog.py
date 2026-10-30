# SPDX-License-Identifier: GPL-2.0-only
"""Reading a serial log of the terminal programs' tests (tools/sumi-test.sh, maki-test.sh, markdown-test.sh): the
grids `termtest screen` printed (CON_READ: "row NN|...|" in UTF-8 since DESIGN D28, "attr NN|...|", the cursor)
between the script's ==NAME markers, the PASS/FAIL lines, and the memory line from free and ps while the program was
open. The log is read as UTF-8 (a byte that is not is kept as U+DC80-DCFF, Python's surrogateescape)."""
import re
import sys


class Log:
    def __init__(self, path):
        self.text = open(path, encoding="utf-8", errors="surrogateescape").read()
        self.bad = 0

    def check(self, ok, what, detail=""):
        print(("PASS " if ok else "FAIL ") + what + ("" if ok else ": " + repr(detail)[:600]))
        self.bad += not ok

    def part(self, name, nxt):
        m = re.search(r"^==%s\n(.*?)^==%s$" % (name, nxt), self.text, re.M | re.S)   # (the last marker may end
        # the log with no new line: the run stops as soon as the tap has seen it)
        return m.group(1) if m else ""

    def status(self, name):
        """the exit status the script echoed as NAME=N (None if never)"""
        m = re.search(r"(?:(?<!\w)|(?<=\x1b\[\?1049l))%s=(\d+)$" % name, self.text, re.M)   # (also right after
        # the text-UI library's last escape)
        return m and int(m.group(1))

    def memory(self, f, prog, label):
        """free before and with the program open, and its ps line: checked and printed (emulator figures)"""
        ps = re.search(r"^ +\d+ +\d+ +\d+ +-?\d+ +\w+ +(\d+) +(\d+) %s$" % prog, f, re.M)
        fr = re.findall(r"^chip RAM free: (\d+) KB$", f, re.M)
        self.check(ps is not None and len(fr) == 2, "free and ps while %s is open" % label, f[-800:])
        if ps and len(fr) == 2:
            print("     (memory, stock A1200, 80 x 32, emulator figures: %s owns %s KB, its shared code %s KB; chip "
                  "RAM free %s KB before, %s KB with %s, the test's own shell and ps)"
                  % (prog, ps.group(1), ps.group(2), fr[0], fr[1], label))

    def waits(self):
        self.check("serial-tap: wait timed out" not in self.text and "the run ended during a wait" not in self.text,
                   "every wait was met", re.findall(r"FAIL: serial-tap.*", self.text))

    def done(self):
        sys.exit(1 if self.bad else 0)


def rows(t):
    return {int(m.group(1)): m.group(2) for m in re.finditer(r"^row (\d\d)\|(.*)\|$", t, re.M)}


def attrs(t):
    return {int(m.group(1)): m.group(2) for m in re.finditer(r"^attr (\d\d)\|(.*)\|$", t, re.M)}


def cursor(t):
    m = re.search(r"^cursor (\d+) (\d+)( shown)?$", t, re.M)
    return m and (int(m.group(1)), int(m.group(2)), bool(m.group(3)))
