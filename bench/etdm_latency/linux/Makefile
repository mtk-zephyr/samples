# SPDX-License-Identifier: Apache-2.0
#
# Linux side of the eTDM latency bench.  Cross build, for example:
#   make CC=aarch64-linux-gnu-gcc SYSROOT=/path/to/target/sysroot
# A Yocto recipe sets CC, CFLAGS and LDFLAGS itself and runs plain `make`.

CC ?= cc
CFLAGS ?= -O2
# override: a CFLAGS given on the make command line must not drop these.
override CFLAGS += -Wall -Wextra -I../common
ifneq ($(SYSROOT),)
override CFLAGS += --sysroot=$(SYSROOT)
override LDFLAGS += --sysroot=$(SYSROOT)
endif
LDLIBS += -lasound

PROGS = etdm_loopback_test etdm_latency_bench

all: $(PROGS)

etdm_loopback_test: etdm_loopback_test.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

etdm_latency_bench: etdm_latency_bench.c ../common/bench_dsp.c ../common/bench_ring.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

install: $(PROGS)
	install -d $(DESTDIR)/usr/bin
	install -m 0755 $(PROGS) $(DESTDIR)/usr/bin/

clean:
	rm -f $(PROGS)

.PHONY: all install clean
