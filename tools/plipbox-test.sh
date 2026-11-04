#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Host test of the plipbox driver's line protocol (net/drv/plipbox-core.c) against the model of the box on the
# parallel port (tools/plipbox-model.h), DESIGN D39: tools/plipbox-test.sh   (TRACE=1 shows the driver's log)
set -e
cd "$(dirname "$0")/.."
mkdir -p build
cc -Wall -Wextra -O1 -g -fsanitize=address,undefined -o build/plipbox-test tools/plipbox-test.c
build/plipbox-test
