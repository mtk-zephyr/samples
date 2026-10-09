# Build rpmsg-test, the Linux side of the sample, for the Genio boards (aarch64).
#
#   make                               aarch64-linux-gnu-gcc
#   make CROSS_COMPILE=aarch64-poky-linux-    a Yocto SDK toolchain, once sourced
#   make CROSS_COMPILE=                the host compiler, to check it compiles
#
# The result is build/rpmsg-test.

CROSS_COMPILE ?= aarch64-linux-gnu-
CC := $(CROSS_COMPILE)gcc
CFLAGS ?= -O2 -Wall
CFLAGS += -I../include

build/rpmsg-test: src/main.c ../include/common.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ src/main.c

.PHONY: clean
clean:
	rm -rf build
