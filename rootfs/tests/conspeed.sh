# SPDX-License-Identifier: GPL-2.0-only
# The console's speed (tools/conspeed-test.sh, DESIGN D28 21): fixed outputs timed with time, on the screen alone
# (the serial copy off: conmirror off) and to /dev/null; each figure written to /dev/ttyS0 as cs-NAME-WHERE-<seconds>s
# for the serial log, so what is typed on the serial line stays one short line (long typed lines lose bytes there).
seq 1 600 | sed 's/$/ the quick brown fox jumps over the lazy dog 0123456789 abcdefghij/' > /tmp/long
awk '{ s = ""; for (i = 1; i <= length($0); i += 10) s = s sprintf("\033[3%dm%s", int(i / 10) % 7 + 1, substr($0, i, 10)); print s }' /tmp/long > /tmp/col
r() { echo "cs-$1-$(sed -n 's/^real.0m//p' /tmp/t)" > /dev/ttyS0; }
conmirror off
time seq 1 2000 2> /tmp/t; r seq-scr
time seq 1 2000 > /dev/null 2> /tmp/t; r seq-nul
time cat /tmp/long 2> /tmp/t; r long-scr
time cat /tmp/long > /dev/null 2> /tmp/t; r long-nul
time cat /tmp/col 2> /tmp/t; printf '\033[0m'; r colour-scr
time cat /tmp/col > /dev/null 2> /tmp/t; r colour-nul
time ls -laR / 2> /tmp/t; r lsR-scr
time ls -laR / > /dev/null 2> /tmp/t; r lsR-nul
echo "cs-lsR-lines-$(ls -laR / | wc -l)l" > /dev/ttyS0
conmirror on
echo cs-done > /dev/ttyS0
