#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Host test of DESIGN D16's core (kernel/event.c, kernel/signal.c) against a model of the scheduler: tools/signal-host-test.sh
set -e
cd "$(dirname "$0")/.."
mkdir -p build
cc -m32 -Wall -Wextra -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -Wno-builtin-declaration-mismatch -O1 -g -fsanitize=undefined -fno-sanitize=alignment -o build/signal-host-test tools/signal-host-test.c
build/signal-host-test
