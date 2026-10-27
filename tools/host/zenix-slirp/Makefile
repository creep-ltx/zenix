# SPDX-License-Identifier: GPL-2.0-only
# zenix-slirp, the PC's end of Zenix's SLIP line (docs/d19-network.md 5.3). Needs libslirp and GLib (pkg-config slirp).
#   make -C tools/host/zenix-slirp          builds ../../../build/zenix-slirp
OUT ?= ../../../build
CFLAGS ?= -O2 -g -Wall -Wextra
$(OUT)/zenix-slirp: zenix-slirp.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $$(pkg-config --cflags slirp) -o $@ $< $$(pkg-config --libs slirp)
