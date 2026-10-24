#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Host test of the serial copy of the console (kernel/serial.c) against a model of Paula's port: tools/serial-test.sh
set -e
cd "$(dirname "$0")/.."
mkdir -p build
cc -Wall -Wextra -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -O1 -g -fsanitize=address,undefined -o build/serial-test tools/serial-test.c
build/serial-test
