# Build rpmsg-test, the Linux side of the sample, for the Genio boards (aarch64).
#
#   make                               aarch64-linux-gnu-gcc, or the CC of a
#                                      sourced Yocto SDK environment
#   make CROSS_COMPILE=<prefix>        another toolchain prefix
#   make CROSS_COMPILE=                the host compiler, to check it compiles
#
# The result is build/rpmsg-test.

CROSS_COMPILE ?= aarch64-linux-gnu-
# A Yocto SDK's environment script sets CC with the --sysroot its compiler
# needs: keep that CC unless CROSS_COMPILE is given on the command line.
ifeq ($(origin CROSS_COMPILE),command line)
CC = $(CROSS_COMPILE)gcc
else ifeq ($(origin CC),default)
CC = $(CROSS_COMPILE)gcc
endif
CFLAGS ?= -O2 -Wall
# override: a CFLAGS given on the make command line must not drop this.
override CFLAGS += -I../include

build/rpmsg-test: src/main.c ../include/common.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ src/main.c

.PHONY: clean
clean:
	rm -rf build
