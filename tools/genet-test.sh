#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Host test of the CM4 Ethernet driver's chip code (net/drv/genet-core.c) against the GENET model
# (tools/genet-model.h), DESIGN D19 step 9: tools/genet-test.sh   (TRACE=1 shows the driver's log)
set -e
cd "$(dirname "$0")/.."
mkdir -p build
cc -Wall -Wextra -O1 -g -fsanitize=address,undefined -o build/genet-test tools/genet-test.c
build/genet-test
