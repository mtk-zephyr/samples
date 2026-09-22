/*
 * Copyright (c) 2025 MediaTek
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * UART RX / interrupt-load test.
 *
 * Echoes every received byte back, counting bytes and ISR entries. Sending
 * 0x04 (Ctrl-D) is not echoed; it asks for a STATS line instead, so a bulk
 * transfer can be verified byte-for-byte without the counters interleaving
 * into the echoed stream.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>

#define STATS_REQ 0x04

static const struct device *const uart_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

static volatile uint32_t rx_bytes;
static volatile uint32_t isr_calls;
static volatile bool stats_req;

static void uart_cb(const struct device *dev, void *user_data)
{
	uint8_t c;

	ARG_UNUSED(user_data);

	isr_calls++;

	/*
	 * uart_irq_update() returns void in this tree, so it cannot be folded
	 * into the loop condition the way most in-tree samples write it.
	 */
	uart_irq_update(dev);

	while (uart_irq_rx_ready(dev)) {
		if (uart_fifo_read(dev, &c, 1) != 1) {
			break;
		}
		rx_bytes++;
		if (c == STATS_REQ) {
			stats_req = true;
		} else {
			uart_poll_out(dev, c);
		}
	}
}

int main(void)
{
	if (!device_is_ready(uart_dev)) {
		printk("RXTEST uart not ready\n");
		return 0;
	}

	uart_irq_callback_user_data_set(uart_dev, uart_cb, NULL);
	uart_irq_rx_enable(uart_dev);

	printk("RXTEST ready %s\n", CONFIG_BOARD_TARGET);

	while (1) {
		if (stats_req) {
			stats_req = false;
			printk("\nSTATS rx=%u isr=%u\n", rx_bytes, isr_calls);
		}
		k_sleep(K_MSEC(20));
	}

	return 0;
}
