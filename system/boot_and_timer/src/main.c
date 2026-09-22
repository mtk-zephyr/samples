/*
 * Copyright (c) 2012-2014 Wind River Systems, Inc.
 * Copyright (c) 2026 MediaTek Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Boot, board identity and timer accuracy.
 *
 * Prints the board target it was built for, the counter frequency the kernel
 * believes in, and three five-second sleeps bracketed by markers.
 *
 * The markers are deliberately not turned into a verdict here.  k_uptime_get()
 * derives from the very timer under test, so the firmware cannot detect a
 * misconfigured one -- it would be marking its own homework.  host/check_timer.py
 * stamps each line on arrival and measures the intervals against the host's
 * clock, which is independent.
 *
 * This is the check that catches a misdeclared interrupt trigger type: Zephyr's
 * GIC devicetree flags encode only level-versus-edge, and Linux's
 * IRQ_TYPE_LEVEL_HIGH shares a value with IRQ_TYPE_EDGE.  Get it wrong and the
 * architected timer's PPIs are left edge-triggered -- which builds, boots, and
 * keeps time badly.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define SLEEP_SECONDS 5
#define ITERATIONS    3

int main(void)
{
	printk("\nBOOT board=%s\n", CONFIG_BOARD_TARGET);
	printk("BOOT cntfrq=%u\n", sys_clock_hw_cycles_per_sec());

	for (int i = 0; i < ITERATIONS; i++) {
		printk("SLEEP_START %d\n", i);
		k_sleep(K_SECONDS(SLEEP_SECONDS));
		printk("SLEEP_END   %d\n", i);
	}

	printk("BOOT DONE\n");

	while (1) {
		k_sleep(K_SECONDS(1));
	}
	return 0;
}
