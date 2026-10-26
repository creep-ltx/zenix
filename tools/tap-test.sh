#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Host test of tools/serial-tap.py's `wait` (audit 2026-10-26 F1), no emulator: a fake Zenix shell on the tap's serial
# port redraws its prompt with each key, as user/sh.c's line editor does, and takes 2 s per command before it prints
# the next prompt. A `wait` for the prompt after a `send` that ends a line must wait for that next prompt, not match
# the prompt the redraw put in the echo; `waitecho` and a `send` without a line end still match the echo. A `wait`
# that times out, or is cut short when the run ends, is a failure; a `waitopt` may time out (F7).
#   tools/tap-test.sh [TAP]      TAP: the tap to test (default tools/serial-tap.py; an older copy shows the old rule)
cd "$(dirname "$0")/.." || exit 1
TAP=${1:-tools/serial-tap.py}
OUT=${ZENIX_OUT:-/tmp/zenix-tap-test}; mkdir -p "$OUT"
PORT=${ZENIX_SERIAL:-/tmp/zenix-tap-test-port}
pass=1
check() { if [ "$2" = ok ]; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }

cat >"$OUT/fake.py" <<'EOF'
# a fake shell on the serial port: echoes each key by redrawing "\rzenix:/# <line>\033[K", runs a line for 2 s
import os, select, sys, termios, time, tty
fd = os.open(sys.argv[1], os.O_RDWR | os.O_NOCTTY)
tty.setraw(fd, termios.TCSANOW)
ev = open(sys.argv[2], "w", buffering=1)
t0 = time.time()
def out(s): os.write(fd, s.encode())
out("clock: fake tick latch\r\nzenix:/# ")
line = ""
while True:
    c = os.read(fd, 1).decode("latin1")
    if c == "\x1d":
        os.read(fd, 1); continue                   # the tap's greeting
    if c in "\r\n":
        out("\r\n")
        ev.write("%.2f got %s\n" % (time.time() - t0, line))
        if line == "quit":
            break
        time.sleep(2)                              # the command runs
        if select.select([fd], [], [], 0)[0]:      # the next line was typed before its prompt
            ev.write("%.2f typed-ahead\n" % (time.time() - t0))
        out(line.replace("say ", "") + "\r\n")
        ev.write("%.2f prompt\n" % (time.time() - t0))
        out("zenix:/# ")
        line = ""
    else:
        line += c
        out("\rzenix:/# " + line + "\033[K")
EOF
cat >"$OUT/script.txt" <<'EOF'
wait zenix:/# 10
sleep 1
send say one\r
wait zenix:/# 10
send say two\r
wait (?m)^two 10
wait zenix:/# 10
send say thr
waitecho zenix:/# say thr 10
send ee\r
waitecho say three 10
wait zenix:/# 10
send quit\r
sleep 1
stop
EOF
rm -f "$OUT/events" "$OUT/serial.log" "$PORT.stop"
python3 "$TAP" "$PORT" "$OUT/serial.log" "$OUT/script.txt" >"$OUT/tap.out" 2>&1 &
TAPPID=$!
for _ in $(seq 50); do [ -e "$PORT" ] && break; sleep 0.1; done
python3 "$OUT/fake.py" "$PORT" "$OUT/events" &
FAKE=$!
for _ in $(seq 300); do [ -e "$PORT.stop" ] && break; sleep 0.1; done
kill $TAPPID $FAKE 2>/dev/null; wait $TAPPID $FAKE 2>/dev/null
rm -f "$PORT" "$PORT.stop"
# no line may be typed before the prompt it waits for was printed (the wait waited for it)
order=$(grep 'typed-ahead' "$OUT/events")
check "a wait for the prompt after 'send LINE\\r' waits for the next prompt, not the one in the echo" "$([ -z "$order" ] && [ "$(grep -c ' got ' "$OUT/events")" = 4 ] && echo ok || echo bad)"
check "... the lines arrived whole and in order" "$(awk '$2 == "got" { printf "%s|", $3 " " $4 }' "$OUT/events" | grep -qx 'say one|say two|say three|quit |' && echo ok || echo bad)"
check "waitecho and a send without a line end still match the echo, and no wait timed out" "$(! grep -q 'wait timed out' "$OUT/tap.out" && echo ok || { cat "$OUT/tap.out"; echo bad; })"
[ -n "$order" ] && echo "$order"

# A wait that times out is a failure (audit 2026-10-26 F7): the tap prints a FAIL line and adds it to ZENIX_TIMEOUTS;
# a `waitopt` that times out is allowed; the tap stopped while a wait still waits is the same failure as a timeout.
cat >"$OUT/script2.txt" <<'EOF'
wait zenix:/# 10
waitopt never-printed-a 1
wait never-printed-b 1
wait zenix:/# 1
wait never-printed-c 60
EOF
rm -f "$OUT/timeouts" "$OUT/serial.log"
ZENIX_TIMEOUTS="$OUT/timeouts" python3 "$TAP" "$PORT" "$OUT/serial.log" "$OUT/script2.txt" >"$OUT/tap2.out" 2>&1 &
TAPPID=$!
for _ in $(seq 50); do [ -e "$PORT" ] && break; sleep 0.1; done
python3 "$OUT/fake.py" "$PORT" "$OUT/events2" &
FAKE=$!
sleep 6
kill $TAPPID $FAKE 2>/dev/null; wait $TAPPID $FAKE 2>/dev/null
rm -f "$PORT" "$PORT.stop"
want='FAIL: serial-tap: wait timed out after 1 s: never-printed-b
FAIL: serial-tap: the run ended during a wait: never-printed-c'
check "a wait that timed out and one cut short by the end of the run are FAIL lines in ZENIX_TIMEOUTS" "$([ "$(cat "$OUT/timeouts" 2>/dev/null)" = "$want" ] && echo ok || { cat "$OUT/tap2.out"; echo bad; })"
check "... and on the tap's output; a waitopt that timed out is allowed, not a failure" "$(grep -q '^serial-tap: optional wait timed out (allowed): never-printed-a$' "$OUT/tap2.out" && [ "$(grep -c '^FAIL' "$OUT/tap2.out")" = 2 ] && echo ok || { cat "$OUT/tap2.out"; echo bad; })"
[ $pass = 1 ] && rm -rf "$OUT"
exit $((1 - pass))
