# ZenixFS host build: library, tools and tests. Run `make test`.
CC ?= gcc
CFLAGS ?= -std=gnu11 -O2 -g -Wall -Wextra
SAN ?=
LIB = zenixfs.c host/zdev.c

all: build/zenixfs-tool build/ztest

build:
	mkdir -p build

build/ztest: tests/ztest.c $(LIB) zenixfs.h host/zdev.h | build
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/ztest.c $(LIB)

build/zenixfs-tool: tools/zenixfs-tool.c $(LIB) zenixfs.h host/zdev.h | build
	$(CC) $(CFLAGS) $(SAN) -o $@ tools/zenixfs-tool.c $(LIB)

test: build/ztest build/zenixfs-tool
	build/ztest
	bash tests/tool-test.sh build/zenixfs-tool

# a 32 MB example image holding the Zenix docs, to look at with build/zenixfs-tool
image: build/zenixfs-tool
	rm -f build/example.zenixfs
	build/zenixfs-tool mkfs build/example.zenixfs 32 -l zenixfs-example
	build/zenixfs-tool put build/example.zenixfs -r ../docs /docs
	build/zenixfs-tool ls build/example.zenixfs -l /docs

# needs fuse3 (pkg-config fuse3) and its headers
build/zenixfs-fuse: tools/zenixfs-fuse.c $(LIB) zenixfs.h host/zdev.h | build
	$(CC) $(CFLAGS) $(SAN) -o $@ tools/zenixfs-fuse.c $(LIB) $$(pkg-config --cflags --libs fuse3)

build/zbench: tests/zbench.c $(LIB) zenixfs.h host/zdev.h | build
	$(CC) $(CFLAGS) -o $@ tests/zbench.c $(LIB)

fuse-test: build/zenixfs-tool build/zenixfs-fuse
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
