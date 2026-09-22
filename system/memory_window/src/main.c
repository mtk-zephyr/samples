/*
 * Copyright (c) 2025 MediaTek
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Inmate memory-window probe.
 *
 * A plain boot cannot tell an 8 MB Jailhouse grant from a 2 MB one: hello_world
 * never touches high addresses, so an over-declared window boots cleanly and
 * faults later under memory pressure.
 *
 * Note that simply writing to a high address in the declared window does NOT
 * work: arch/arm64/core/mmu.c maps _image_ram_start.._image_ram_end, i.e. the
 * image extent, not the whole SRAM region, so anything past the image is
 * unmapped and faults for reasons unrelated to the grant.
 *
 * Instead this places a large array in .bss so the linker extends the image
 * across the window. Boot-time .bss zeroing then touches every byte of it: if
 * the cell grants less than the board declares, the fault happens at boot,
 * which is the point.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

/* Spans well past the 2 MB boundary, leaving headroom for stack and heap. */
#define PROBE_MB 6U

static volatile uint32_t probe[(PROBE_MB << 20) / sizeof(uint32_t)];

#define MAGIC 0x5A11ED00u

int main(void)
{
	const uintptr_t base = (uintptr_t)CONFIG_SRAM_BASE_ADDRESS;
	const size_t size = (size_t)CONFIG_SRAM_SIZE * 1024U;
	const size_t n = ARRAY_SIZE(probe);
	unsigned int bad = 0;

	printk("\nMEMTEST board=%s\n", CONFIG_BOARD_TARGET);
	printk("MEMTEST window base=0x%lx size=0x%zx (%zu MB)\n",
	       (unsigned long)base, size, size >> 20);
	printk("MEMTEST probe array %u MB at 0x%lx..0x%lx\n", PROBE_MB,
	       (unsigned long)&probe[0], (unsigned long)&probe[n - 1]);
	printk("MEMTEST .bss zeroing already touched every byte above\n");

	/* Distinct pattern per slot so aliasing shows up as a mismatch. */
	const size_t step = n / 16U;

	for (size_t i = 0, k = 0; i < n; i += step, k++) {
		probe[i] = MAGIC | (uint32_t)k;
	}
	probe[n - 1] = MAGIC | 0xFFu;

	for (size_t i = 0, k = 0; i < n; i += step, k++) {
		uint32_t want = MAGIC | (uint32_t)k;

		if (probe[i] != want) {
			printk("MEMTEST BAD  0x%lx want 0x%08x got 0x%08x\n",
			       (unsigned long)&probe[i], want, probe[i]);
			bad++;
		}
	}
	if (probe[n - 1] != (MAGIC | 0xFFu)) {
		printk("MEMTEST BAD  top-of-array readback\n");
		bad++;
	}

	printk("MEMTEST DONE mismatches=%u -> %s\n", bad, bad ? "FAIL" : "PASS");

	while (1) {
		k_sleep(K_SECONDS(1));
	}
	return 0;
}
