#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Host test of the plipbox driver's line protocol (net/drv/plipbox-line.c, the kernel's DRV_PARXFER, and
# net/drv/plipbox-core.c above it) against the model of the box on the
# parallel port (tools/plipbox-model.h), DESIGN D39, and a download through it (tools/plipbox-flow.c):
# tools/plipbox-test.sh   (TRACE=1 shows the driver's log)
set -e
cd "$(dirname "$0")/.."
mkdir -p build
cc -Wall -Wextra -O1 -g -fsanitize=address,undefined -o build/plipbox-test tools/plipbox-test.c
build/plipbox-test
# a download through the box on the model's clock: where a TCP transfer's time goes (D39 section 7)
cc -Wall -Wextra -Wno-unused-function -O1 -g -fsanitize=address,undefined -o build/plipbox-flow tools/plipbox-flow.c
build/plipbox-flow
