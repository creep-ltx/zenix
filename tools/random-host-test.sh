#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The entropy pool on the host (DESIGN D19 9.3): kernel/random.c's BLAKE2s and ChaCha20 against the RFCs' vectors, the
# crediting rules and the seed file's rule (tools/random-host-test.c); then mkroot.sh and mkzup.py refusing a random
# seed in an image or a package.
cd "$(dirname "$0")/.." || exit 1
T=$(mktemp -d /tmp/zenix-random.XXXXXX)
trap 'rm -rf "$T"' EXIT
pass=1
cc -Wall -Wextra -O1 -g -fsanitize=address,undefined -o "$T/random-host-test" tools/random-host-test.c || { echo "FAIL the host build"; exit 1; }
"$T/random-host-test" || pass=0
# mkroot.sh refuses an image with a seed: a copy of the tree's tools and rootfs with one added
mkdir -p "$T/tree" && cp -r tools rootfs "$T/tree/" && mkdir -p "$T/tree/rootfs/var/lib" && echo x > "$T/tree/rootfs/var/lib/random-seed"
mkdir -p "$T/b" && : > "$T/b/hello.zx"
if NOMAN=1 NOTESTS=1 "$T/tree/tools/mkroot.sh" "$T/r.img" "$T/b" "hello" 1M 64 >"$T/mk.log" 2>&1; then
    echo "FAIL mkroot.sh accepted an image with a random seed"; pass=0
elif grep -q 'refusing to put a random seed' "$T/mk.log" && [ ! -e "$T/r.img" ]; then
    echo "PASS mkroot.sh refuses an image with a random seed"
else
    echo "FAIL mkroot.sh failed for another reason: $(cat "$T/mk.log")"; pass=0
fi
if python3 -c 'import sys; sys.path.insert(0, "tools"); import mkzup; mkzup.check_path("/var/lib/random-seed")' >"$T/zup.log" 2>&1; then
    echo "FAIL mkzup.py accepted a random seed in a package"; pass=0
elif grep -q 'refusing to put a random seed' "$T/zup.log"; then
    echo "PASS mkzup.py refuses a random seed in a package"
else
    echo "FAIL mkzup.py: $(cat "$T/zup.log")"; pass=0
fi
exit $((1 - pass))
