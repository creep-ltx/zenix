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

test: build/ztest
	build/ztest

# the same tests under AddressSanitizer + UBSan
asan:
	$(MAKE) clean
	$(MAKE) SAN="-fsanitize=address,undefined -fno-omit-frame-pointer" test

clean:
	rm -rf build
.PHONY: all test asan clean
