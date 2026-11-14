#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The terminal games in FS-UAE (roadmap block 7 item 7; ports/frotz, ports/bsdgames) on the minimum machine (an A1200
# with 4 MB of fast RAM), as root on the serial console (console 1's grid mirrored):
#  - frotz plays All Things Devours with rootfs/tests/games/devours.in: the transcript is the PC's line for line
#    (tools/games-host-test.sh makes it from the same pinned source);
#  - adventure's first room (its scrambled text read back with the C library's random), banner and number: the PC's
#    output byte for byte; fortune prints one of games/fortunes, lists its file and finds all 40 through the index; wump and arithmetic start and end;
#  - the curses games on console 1, read from its grid by termtest screen (CON_READ) and driven by its keys: robots
#    (its field and legend, q and y), hangman (the word's dashes, a guess shown, Ctrl-D), tetris (its key line, q, the
#    score and Return), worm (its status line, then its end and score), rain and worms (an O drawn on a cleared
#    screen, Ctrl-C);
#  - no game ran past its stack (the kernel log); the programs' sizes are printed.
cd "$(dirname "$0")/.." || exit 1
OUT=${ZENIX_OUT:-/tmp/zenix-games}; mkdir -p "$OUT"
BD=${ZENIX_BUILD:-build}
GAMES_EXPECTED="$OUT/expected" tools/games-host-test.sh >"$OUT/host.log" 2>&1 ||
    { echo "FAIL the PC's reference run (tools/games-host-test.sh)"; grep FAIL "$OUT/host.log"; exit 1; }
make -s BUILD=$BD all >"$OUT/build.log" 2>&1 || { echo "FAIL the build"; tail -5 "$OUT/build.log"; exit 1; }
ZENIX_BUILD=$BD tools/run-test.sh fsuae/a1200-020-fast.fs-uae 1500 tools/tests/games-test.txt 2>&1 | tr -d '\r' > "$OUT/run.log"
python3 - "$OUT/run.log" "$OUT/expected" <<'PY'
import re, sys
sys.path.insert(0, "tools")
from screenlog import Log, rows
L = Log(sys.argv[1])
E = sys.argv[2]
check, part, status = L.check, L.part, L.status
def exp(n): return open("%s/%s" % (E, n), encoding="latin-1").read().splitlines()
def same(name, got, want):
    while got and not got[-1].strip(): got = got[:-1]      # the script's own echo ends the part with a new line
    while want and not want[-1].strip(): want = want[:-1]
    got = [g.rstrip() for g in got]; want = [w.rstrip() for w in want]
    diff = [(i + 1, w, g) for i, (w, g) in enumerate(zip(want, got)) if w != g]
    check(len(got) == len(want) and not diff, "%s: %d lines the PC's, line for line" % (name, len(want)),
          (len(got), len(want), diff[:3]))
check(status("fst") == 0, "frotz ended with status 0", status("fst"))
same("frotz's transcript of devours.z5", part("fout", "fend").splitlines(), exp("devours.out"))
check(status("ast") == 0, "adventure ended with status 0", status("ast"))
adv = part("aout", "aend")
check("You are standing at the end of a road before a small brick building." in adv,
      "adventure's first room, its text read back on Zenix", adv[:300])
same("adventure's start and quit", adv.splitlines(), exp("adv.out"))
check(status("bst") == 0 and status("nst") == 0, "banner and number ended with status 0")
same("banner -w 40 ZENIX", part("bout", "nout").splitlines(), exp("banner.out"))
same("number 1999", part("nout", "nend").splitlines(), exp("number.out"))
fort = [f.strip() for f in open("games/fortunes", encoding="utf-8").read().split("\n%\n") if f.strip("%\n ")]
got = part("oout", "olst").strip()
check(status("ost") == 0 and got in fort, "fortune printed one of games/fortunes", got)
check("fortunes" in part("olst", "oall"), "fortune -f lists the fortunes file", part("olst", "oall"))
alltext = part("oall", "oend")
miss = [f for f in fort if f not in alltext]
check(not miss, "fortune -m . found all %d messages through the index" % len(fort), miss[:2])
w = part("wout", "rout")
check(status("wst") == 0 and "Instructions? (y-n)" in w and re.search(r"You are in room \d+ of the cave", w) is not None,
      "wump: the cave and its first room", w[-300:])
check(status("rst") == 0 and re.search(r"^\d \+ \d =", part("rout", "rend"), re.M) is not None,
      "arithmetic: a sum asked, the end of input ends it", part("rout", "rend")[-200:])
def grid(name): return rows(part(name, {"sr1": "sr2", "sr2": "sh1", "sh1": "sh2", "sh2": "st1", "st1": "st2",
                                         "st2": "sw1", "sw1": "sn1", "sn1": "ss1", "ss1": "send"}[name]))
r1 = grid("sr1"); t = "\n".join(r1.values())
check(status("ost2") == 0 and "Directions:" in t and "Score: 0" in t and "@" in t and "+:  robot" in t,
      "robots: its field, you (@) and the legend on the console", t[:400])
check("Really quit?" in "\n".join(grid("sr2").values()), "robots: q asks, y quits (status 0)", status("ost2"))
h1 = "\n".join(grid("sh1").values()); h2 = "\n".join(grid("sh2").values())
check(status("hst") == 0 and "Word: " in h1 and "-" in h1 and "Guessed: e" in h2, "hangman: the word, a guess, Ctrl-D",
      (h1[:300], h2[:300]))
t1 = "\n".join(grid("st1").values()); t2 = "\n".join(grid("st2").values())
check(status("tst") == 0 and "Score:" in t1 and " - quit " in t1 and "Your score:" in t2,
      "tetris: its well and keys, q, its score", (t1[:300], t2[:300]))
check(status("mst") == 0 and " Worm" in "\n".join(grid("sw1").values()) and
      re.search(r"Your final score was \d+", L.text) is not None, "worm: its status line, its end and score")
for nm, st, g in (("rain", "nst2", "sn1"), ("worms", "sst", "ss1")):
    t = "\n".join(grid(g).values())
    check(status(st) == 0 and "O" in t, "%s drew (an O on the console) and ended at Ctrl-C" % nm, (status(st), t[:200]))
m = part("mem", "mend")
check(not re.search(r"stack|guard", m.split("-rwx")[0], re.I), "no game ran past its stack (dmesg)", m[:300])
print("     (sizes on the disk: %s)" % ", ".join("%s %s" % (p.split("/")[-1], n) for n, p in
      re.findall(r"^-rwx\S+ +\d+ +\w+ +\w+ +(\d+) .* (/bin/\w+)$", m, re.M)))
check("robots_roll" in m and "tetris.scores" in m and "frotz" in m, "the games' data and score files are there", m[-400:])
L.waits()
L.done()
PY
