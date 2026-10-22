# ZXFS host build: library, tools and tests. Run `make test`.
CC ?= gcc
CFLAGS ?= -std=gnu11 -O2 -g -Wall -Wextra
SAN ?=
LIB = zxfs.c host/zdev.c

all: build/zxfs-tool build/ztest

build:
	mkdir -p build

build/ztest: tests/ztest.c $(LIB) zxfs.h host/zdev.h | build
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/ztest.c $(LIB)

build/zxfs-tool: tools/zxfs-tool.c $(LIB) zxfs.h host/zdev.h | build
	$(CC) $(CFLAGS) $(SAN) -o $@ tools/zxfs-tool.c $(LIB)

test: build/ztest build/zxfs-tool
	build/ztest
	bash tests/tool-test.sh build/zxfs-tool

# a 32 MB example image holding the Zenix docs, to look at with build/zxfs-tool
image: build/zxfs-tool
	rm -f build/example.zxfs
	build/zxfs-tool mkfs build/example.zxfs 32 -l zxfs-example
	build/zxfs-tool put build/example.zxfs -r ../docs /docs
	build/zxfs-tool ls build/example.zxfs -l /docs

# needs fuse3 (pkg-config fuse3) and its headers
build/zxfs-fuse: tools/zxfs-fuse.c $(LIB) zxfs.h host/zdev.h | build
	$(CC) $(CFLAGS) $(SAN) -o $@ tools/zxfs-fuse.c $(LIB) $$(pkg-config --cflags --libs fuse3)

build/zbench: tests/zbench.c $(LIB) zxfs.h host/zdev.h | build
	$(CC) $(CFLAGS) -o $@ tests/zbench.c $(LIB)

fuse-test: build/zxfs-tool build/zxfs-fuse
	bash tests/fuse-test.sh

# library-only speed on a RAM device (host figures, not an Amiga)
bench: build/zbench
	build/zbench

# the same tests under AddressSanitizer + UBSan
asan:
	$(MAKE) clean
	$(MAKE) SAN="-fsanitize=address,undefined -fno-omit-frame-pointer" test

clean:
	rm -rf build
.PHONY: all test fuse-test bench image asan clean
