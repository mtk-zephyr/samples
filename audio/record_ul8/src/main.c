/*
 * Copyright (c) 2026 MediaTek Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MT8188 capture sample: UL8 ← eTDM_IN1, 16 channel, 48 kHz, 32-bit.
 * Prints one sample per channel per second.
 */

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/devicetree_regions.h>
#include <zephyr/sys/printk.h>

#include <zephyr/drivers/audio/mt8188_afe.h>

#define SAMPLE_RATE 48000
#define CHANNELS    16
#define WORD_BYTES  4 /* 32-bit */
#define BUF_MS      100

#define BUF_FRAMES  (SAMPLE_RATE * BUF_MS / 1000)
#define BUF_SAMPLES (BUF_FRAMES * CHANNELS)
#define BUF_BYTES   (BUF_SAMPLES * WORD_BYTES)

/* DMA buffer lives in the dedicated dma_region (see board DTS), outside
 * zephyr,sram, so AFE DMA cannot reach the Zephyr image, stack or heap.
 */
static int32_t
	dma_buf[BUF_SAMPLES] Z_GENERIC_SECTION(LINKER_DT_NODE_REGION_NAME(DT_NODELABEL(dma_region)))
		__aligned(64);

int main(void)
{
	const struct device *dev;
	struct mt8188_afe_cfg cfg = {
		.rate = SAMPLE_RATE,
		.channels = CHANNELS,
		.word_size = 32,
		.fmt = MT8188_ETDM_FMT_DSPB,
		.data_mode = MT8188_ETDM_DATA_ONE_PIN,
		.mclk_freq = 256 * SAMPLE_RATE, /* 12.288 MHz, codec MCLK */
		.mclk_dir = MT8188_MCLK_DIR_OUT,
		.period_frames = SAMPLE_RATE / 100, /* 10 ms period */
	};
	int ret;

	dev = DEVICE_DT_GET(DT_NODELABEL(afe));
	if (!device_is_ready(dev)) {
		printk("AFE device not ready\n");
		return -ENODEV;
	}

	ret = mt8188_afe_configure(dev, MT8188_UL8, &cfg);
	if (ret != 0) {
		printk("configure failed: %d\n", ret);
		return ret;
	}

	ret = mt8188_afe_route(dev, MT8188_ROUTE_SRC_ETDM_IN1, MT8188_ROUTE_DST_UL8);
	if (ret != 0) {
		printk("route failed: %d\n", ret);
		return ret;
	}

	/* Single CPU-address -> hardware-address conversion point: a non-flat
	 * mapping would only need changing here.
	 *
	 * The driver maps dma_region one to one, so the linker address already
	 * is the address the AFE needs. k_mem_phys_addr() is deliberately not
	 * used: its range assert only covers the kernel VM window, which this
	 * region sits outside.
	 */
	const uint32_t dma_buf_pa = (uint32_t)(uintptr_t)dma_buf;

	ret = mt8188_afe_set_buf(dev, MT8188_UL8, dma_buf_pa, BUF_BYTES);
	if (ret != 0) {
		printk("set_buf failed: %d\n", ret);
		return ret;
	}

	ret = mt8188_afe_start(dev, MT8188_UL8);
	if (ret != 0) {
		printk("start failed: %d\n", ret);
		return ret;
	}

	printk("MT8188 UL8 capture started: 48kHz 16ch 32-bit from eTDM_IN1\n");

	while (1) {
		k_msleep(1000);

		/* get_cur returns a physical address */
		uint32_t ptr = mt8188_afe_get_cur(dev, MT8188_UL8);
		uint32_t offset = (ptr > dma_buf_pa) ? (ptr - dma_buf_pa) : 0;
		uint32_t frame = (offset / (CHANNELS * sizeof(dma_buf[0])));

		/* Print one sample per channel from that frame */
		uint32_t sample_idx = (frame > 0 ? frame - 1 : 0) * CHANNELS;

		for (int ch = 0; ch < CHANNELS; ch++) {
			printk("UL8 ch%d: 0x%08x  ", ch, (uint32_t)dma_buf[sample_idx + ch]);
		}
		printk("\n");
	}

	return 0;
}
