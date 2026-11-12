#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Host test of usbd's xHCI back end and USB core (usb/xhci-core.c, usb/core.c) against the model of the controller and
# of a USB stick backed by an image file, with misbehaving sticks (tools/xhci-model.h): DESIGN D46, docs/d46-usb.md 6.1.
#   tools/usb-host-test.sh [-v]
cd "$(dirname "$0")/.." || exit 1
T=$(mktemp -d "${TMPDIR:-/tmp}/zusb.XXXXXX")
trap 'rm -rf "$T"' EXIT
cc -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-function -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -o "$T/usb-host-test" tools/usb-host-test.c || { echo "FAIL build usb-host-test"; exit 1; }
head -c $((8 * 1024 * 1024)) /dev/urandom > "$T/stick.img"       # 8 MB of bytes no two sectors share
"$T/usb-host-test" "$@" "$T/stick.img"
