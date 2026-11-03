#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Host test of the PC Card NE2000 driver's slot, CIS and chip code (net/drv/ne2000-core.c) against the model of the
# A1200's slot and the cards (tools/ne2000-model.h), DESIGN D36: tools/ne2000-test.sh   (TRACE=1 shows the driver's log)
set -e
cd "$(dirname "$0")/.."
mkdir -p build
cc -Wall -Wextra -O1 -g -fsanitize=address,undefined -o build/ne2000-test tools/ne2000-test.c
build/ne2000-test
