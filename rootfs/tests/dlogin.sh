# dlogin-test (tools/dlogin-test.sh, DESIGN D38 revision 3): run by init as a daemon (/etc/daemons) after a boot with
# /etc/console "login graphical", so dlogin owns console 1 as it would on a real machine. It types into the shown
# console through CON_INPUT (gcontest keys, on console 2's descriptor: raw keys go to whatever console is shown) and
# says each step on the serial port, where the test's script waits for it and takes a screenshot.
k() { gcontest keys "$@" < /dev/tty2 > /dev/null; }
say() { echo "dlt-$1" > /dev/ttyS0; sleep 4; }
sleep 20
say greeter
k 24 a4 16 96 12 92 21 a1 14 94 44 c4
sleep 2
say typed
k 11 91 13 93 18 98 36 b6 24 a4 44 c4
sleep 6
say wrong
k 21 a1 12 92 33 b3 13 93 12 92 14 94 44 c4
sleep 15
say desk
k 66 44 c4 e6
sleep 10
k 17 97 22 a2 40 c0 60 30 b0 e0 40 c0 60 07 87 e0 14 94 37 b7 19 99 60 07 87 e0 17 97 22 a2 01 81 44 c4
sleep 4
k 66 40 c0 e6
sleep 1
k 4c cc 44 c4
sleep 12
say back
k 13 93 18 98 18 98 14 94 44 c4
sleep 1
k 44 c4
sleep 6
say rootref
k 45 c5
sleep 1
k 24 a4 16 96 12 92 21 a1 14 94 44 c4
sleep 1
k 21 a1 12 92 33 b3 13 93 12 92 14 94 42 c2 4e ce 44 c4
sleep 12
say textsession
k 17 97 22 a2 40 c0 60 30 b0 e0 40 c0 60 07 87 e0 14 94 37 b7 19 99 60 07 87 e0 17 97 22 a2 02 82 44 c4
sleep 3
k 12 92 32 b2 17 97 14 94 44 c4
sleep 15
say back2
echo "dlt-ids $(cat /tmp/id1) / $(cat /tmp/id2)" > /dev/ttyS0
dmesg | grep 'dlogin:' > /dev/ttyS0
echo dlt-end > /dev/ttyS0
exit 0
