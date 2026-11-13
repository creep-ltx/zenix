# SPDX-License-Identifier: GPL-2.0-only
# DESIGN D53: a recipe starts a #! script (make runs it through sh -c)
all:
	@/tests/shebang/args.sh rec1 rec2
