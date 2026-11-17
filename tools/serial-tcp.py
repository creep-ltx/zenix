#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Join a serial port that is a PTY (tools/serial-tap.py's link) to an emulator's serial port on TCP.

Amiberry opens no PTY as its serial port (it looks the name up among the machine's serial devices); it listens on TCP
instead (serial_port = TCP://127.0.0.1:PORT/wait: the emulation waits for the connection). tools/run-test.sh starts this
between the tap and Amiberry (ZENIX_EMU=amiberry), so the tests' scripts run unchanged.

usage: tools/serial-tcp.py LINK PORT        (connects to 127.0.0.1:PORT, retrying for 30 s; ends when either side does)"""
import os
import select
import socket
import sys
import termios
import time
import tty


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    fd = os.open(sys.argv[1], os.O_RDWR | os.O_NOCTTY)
    tty.setraw(fd, termios.TCSANOW)
    end = time.time() + 30
    while True:
        try:
            s = socket.create_connection(("127.0.0.1", int(sys.argv[2])), timeout=2)
            break
        except OSError:
            if time.time() > end:
                sys.exit(f"serial-tcp: nothing listens on port {sys.argv[2]}")
            time.sleep(0.2)
    s.setblocking(True)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    while True:
        r, _, _ = select.select([fd, s], [], [])
        if s in r:
            d = s.recv(4096)
            if not d:
                return
            os.write(fd, d)
        if fd in r:
            try:
                d = os.read(fd, 4096)
            except OSError:
                return
            if not d:
                return
            s.sendall(d)


if __name__ == "__main__":
    main()
