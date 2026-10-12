/*
 * Copyright (c) 2026 MediaTek Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Arms the watchdog for 5 s, kicks it ten times at 1 s, then stops kicking and
 * reports that it is still alive every 250 ms until the watchdog takes effect.
 * Every line carries the time since the arm and the counter value, so the log
 * can be set against the time of the reset or of the cell's restart.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define TIMEOUT_MS 5000U
#define KICKS      10
#define KICK_MS    1000
#define ALIVE_MS   250

static const struct device *const wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));

static int64_t armed_ms;

static void report(const char *what)
{
	printk("WDTEXPIRY %s: %lld ms since the arm, cycle %llu\n", what,
	       (long long)(k_uptime_get() - armed_ms), (unsigned long long)k_cycle_get_64());
}

int main(void)
{
	const struct wdt_timeout_cfg cfg = {
		.window.max = TIMEOUT_MS,
		.flags = WDT_FLAG_RESET_SOC,
	};
	int64_t last_kick_ms;
	char what[16];
	int channel;
	int ret;

	if (!device_is_ready(wdt)) {
		printk("WDTEXPIRY FAIL watchdog not ready\n");
		return 0;
	}

	channel = wdt_install_timeout(wdt, &cfg);
	if (channel < 0) {
		printk("WDTEXPIRY FAIL wdt_install_timeout() = %d\n", channel);
		return 0;
	}

	ret = wdt_setup(wdt, 0);
	if (ret != 0) {
		printk("WDTEXPIRY FAIL wdt_setup() = %d\n", ret);
		return 0;
	}

	armed_ms = k_uptime_get();
	printk("WDTEXPIRY armed for %u ms, counter at %u Hz\n", TIMEOUT_MS,
	       sys_clock_hw_cycles_per_sec());
	report("arm");

	for (int i = 1; i <= KICKS; i++) {
		k_sleep(K_TIMEOUT_ABS_MS(armed_ms + (int64_t)i * KICK_MS));
		ret = wdt_feed(wdt, channel);
		if (ret != 0) {
			printk("WDTEXPIRY FAIL wdt_feed() = %d\n", ret);
			return 0;
		}
		snprintk(what, sizeof(what), "kick %d", i);
		report(what);
	}

	last_kick_ms = k_uptime_get();
	printk("WDTEXPIRY stopped kicking; the watchdog is due at %lld ms\n",
	       (long long)(last_kick_ms - armed_ms + TIMEOUT_MS));

	for (int64_t t = ALIVE_MS;; t += ALIVE_MS) {
		k_sleep(K_TIMEOUT_ABS_MS(last_kick_ms + t));
		report("alive");
	}

	return 0;
}
