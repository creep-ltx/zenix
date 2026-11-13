#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# DESIGN D53: a script's #! line, the kernel's parser (kernel/shebang.h) on the PC (tools/shebang-host-test.c).
cd "$(dirname "$0")/.." || exit 1
T=$(mktemp -d "${TMPDIR:-/tmp}/zenix-shebang-host.XXXXXX")
trap 'rm -rf "$T"' EXIT
cc -Wall -Wextra -O1 -g -fsanitize=address,undefined -o "$T/shebang-host-test" tools/shebang-host-test.c || { echo "FAIL the host build"; exit 1; }
"$T/shebang-host-test"
