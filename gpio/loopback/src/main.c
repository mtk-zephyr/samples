/*
 * Copyright (c) 2026 MediaTek Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * MT8188 GPIO and EINT driver tests, bank 1 (gpio32_63).
 *
 * Needs a jumper between GPIO 38 (bank pin 6) and GPIO 40 (bank pin 8). Pin 6
 * drives, pin 8 observes, so every edge is produced by software: no bounce, and
 * the event count is exact. A physical button cannot give either.
 *
 * The board devicetree reserves every pin in the bank except 6 and 8, so the
 * two are also the only ones a write may reach.
 */

#include <stdarg.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/atomic.h>

#define DRIVE_PIN  6	/* GPIO 38 */
#define SENSE_PIN  8	/* GPIO 40 */

/* Long enough for the level to settle through the jumper and the EINT block. */
#define SETTLE	   K_MSEC(20)

static const struct device *const gpio = DEVICE_DT_GET(DT_NODELABEL(gpio32_63));

static struct gpio_callback cb_data;
static atomic_t events;

static int passed;
static int failed;

static void edge_handler(const struct device *port, struct gpio_callback *cb, gpio_port_pins_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	atomic_inc(&events);
}

static void check(const char *tag, bool ok, const char *fmt, ...)
{
	va_list ap;

	if (ok) {
		passed++;
	} else {
		failed++;
	}

	printk("%s %s ", ok ? "PASS" : "FAIL", tag);
	va_start(ap, fmt);
	vprintk(fmt, ap);
	va_end(ap);
	printk("\n");
}

/* Drive the output, wait for it to settle, and report how many events landed. */
static int pulse(int level)
{
	atomic_clear(&events);
	gpio_pin_set(gpio, DRIVE_PIN, level);
	k_sleep(SETTLE);
	return (int)atomic_get(&events);
}

int main(void)
{
	int ret;
	int val;

	printk("GPIOTEST board=%s\n", CONFIG_BOARD_TARGET);

	if (!device_is_ready(gpio)) {
		printk("FAIL B1 gpio device not ready\n");
		printk("GPIOTEST DONE passed=0 failed=1 -> FAIL\n");
		return 0;
	}
	printk("GPIOTEST device ready, drive=pin%d sense=pin%d\n", DRIVE_PIN, SENSE_PIN);

	/* --- B6: a reserved pin must be refused ------------------------- */
	ret = gpio_pin_configure(gpio, 0, GPIO_OUTPUT_INACTIVE);
	check("B6-reserved-pin", ret == -EINVAL, "configure(pin0) = %d, want -EINVAL", ret);

	/* --- B2: output drives, input reads it back through the jumper --- */
	ret = gpio_pin_configure(gpio, DRIVE_PIN, GPIO_OUTPUT_INACTIVE);
	check("B2-configure-output", ret == 0, "configure(pin%d, OUTPUT) = %d", DRIVE_PIN, ret);

	ret = gpio_pin_configure(gpio, SENSE_PIN, GPIO_INPUT);
	check("B2-configure-input", ret == 0, "configure(pin%d, INPUT) = %d", SENSE_PIN, ret);

	gpio_pin_set(gpio, DRIVE_PIN, 1);
	k_sleep(SETTLE);
	val = gpio_pin_get(gpio, SENSE_PIN);
	check("B2-drive-high", val == 1, "drove 1, sensed %d", val);

	gpio_pin_set(gpio, DRIVE_PIN, 0);
	k_sleep(SETTLE);
	val = gpio_pin_get(gpio, SENSE_PIN);
	check("B2-drive-low", val == 0, "drove 0, sensed %d", val);

	/* --- B3: toggle must move the pad, not just the input register --- */
	/*
	 * gpio_pin_toggle() on a driver that reads DIN instead of DOUT looks
	 * correct until the pad is held externally; here the jumper makes the
	 * sensed value the proof.
	 */
	int seen[4];

	for (int i = 0; i < 4; i++) {
		gpio_pin_toggle(gpio, DRIVE_PIN);
		k_sleep(SETTLE);
		seen[i] = gpio_pin_get(gpio, SENSE_PIN);
	}
	check("B3-toggle", seen[0] == 1 && seen[1] == 0 && seen[2] == 1 && seen[3] == 0,
	      "toggled 4x, sensed %d%d%d%d, want 1010", seen[0], seen[1], seen[2], seen[3]);

	/* --- B7: level-triggered interrupts are deliberately refused ----- */
	ret = gpio_pin_interrupt_configure(gpio, SENSE_PIN, GPIO_INT_LEVEL_HIGH);
	check("B7-level-refused", ret == -ENOTSUP, "LEVEL_HIGH = %d, want -ENOTSUP", ret);

	/* --- interrupt plumbing ----------------------------------------- */
	gpio_init_callback(&cb_data, edge_handler, BIT(SENSE_PIN));
	ret = gpio_add_callback(gpio, &cb_data);
	check("B5-add-callback", ret == 0, "add_callback = %d", ret);

	/* --- B5: one edge type fires on that edge only ------------------- */
	gpio_pin_set(gpio, DRIVE_PIN, 0);
	k_sleep(SETTLE);

	ret = gpio_pin_interrupt_configure(gpio, SENSE_PIN, GPIO_INT_EDGE_RISING);
	check("B5-rising-configure", ret == 0, "EDGE_RISING = %d", ret);

	val = pulse(1);
	check("B5-rising-on-rise", val == 1, "rising edge produced %d event(s), want 1", val);

	val = pulse(0);
	check("B5-rising-ignores-fall", val == 0, "falling edge produced %d event(s), want 0", val);

	ret = gpio_pin_interrupt_configure(gpio, SENSE_PIN, GPIO_INT_EDGE_FALLING);
	check("B5-falling-configure", ret == 0, "EDGE_FALLING = %d", ret);

	val = pulse(1);
	check("B5-falling-ignores-rise", val == 0, "rising edge produced %d event(s), want 0", val);

	val = pulse(0);
	check("B5-falling-on-fall", val == 1, "falling edge produced %d event(s), want 1", val);

	/* --- B4: both edges, the emulated case --------------------------- */
	/*
	 * The controller detects one condition per line, so both-edges is
	 * emulated by flipping the polarity inside the handler. An inverted
	 * flip still fires on one edge, which looks like working code until
	 * the events are counted. Count them.
	 */
	ret = gpio_pin_interrupt_configure(gpio, SENSE_PIN, GPIO_INT_EDGE_BOTH);
	check("B4-both-configure", ret == 0, "EDGE_BOTH = %d", ret);

	int rises = 0;
	int falls = 0;

	for (int i = 0; i < 4; i++) {
		rises += pulse(1);
		falls += pulse(0);
	}
	check("B4-both-rising", rises == 4, "4 rising edges produced %d event(s)", rises);
	check("B4-both-falling", falls == 4, "4 falling edges produced %d event(s)", falls);

	/* --- disable must actually stop delivery -------------------------- */
	ret = gpio_pin_interrupt_configure(gpio, SENSE_PIN, GPIO_INT_DISABLE);
	check("B4-disable", ret == 0, "INT_DISABLE = %d", ret);

	val = pulse(1) + pulse(0);
	check("B4-disabled-silent", val == 0, "after disable, %d event(s) still arrived", val);

	gpio_remove_callback(gpio, &cb_data);

	printk("GPIOTEST DONE passed=%d failed=%d -> %s\n", passed, failed,
	       failed == 0 ? "PASS" : "FAIL");
	return 0;
}
