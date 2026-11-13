#!/bin/make -f
# SPDX-License-Identifier: GPL-2.0-only
# DESIGN D53: a makefile started by its #! line; V comes from the command line
V = unset
all:
	@echo "make ran V=$(V)"
