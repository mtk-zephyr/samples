/*
 * Copyright (c) 2026 MediaTek Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MT8188 playback sample: DL11 → eTDM_OUT1 + eTDM_OUT2, 32 channel,
 * 48 kHz, 32-bit, DSP-B.
 *
 * A single eTDM port carries at most 16 channels, so the 32-channel stream
 * is split across two co-clocked ports:
 *   ch0-15  → I022..I037 (direct)              → O072..O087 = eTDM_OUT1
 *   ch16-31 → I046..I061 (via DL8_DL11 mux)    → O048..O063 = eTDM_OUT2
 * eTDM_OUT1 is the cowork master; OUT2 is slaved to it so both share
 * BCLK/LRCK. The driver sets the mux and the cowork linkage internally.
 *
 * Each channel is filled with a distinct DC level so the channel mapping
 * can be identified on a scope / logic analyser.
 */

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/devicetree_regions.h>
#include <zephyr/sys/printk.h>

#include <zephyr/drivers/audio/mt8188_afe.h>

#define SAMPLE_RATE 48000
#define CHANNELS    32
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

	/* Per-channel DC level: channel n gets n << 24, so each slot is
	 * distinguishable when probing the TDM frame.
	 */
	for (int f = 0; f < BUF_FRAMES; f++) {
		for (int ch = 0; ch < CHANNELS; ch++) {
			dma_buf[f * CHANNELS + ch] = (int32_t)((uint32_t)ch << 24);
		}
	}

	ret = mt8188_afe_configure(dev, MT8188_DL11, &cfg);
	if (ret != 0) {
		printk("configure failed: %d\n", ret);
		return ret;
	}

	/* Route DL11 → eTDM_OUT1 (ch0-15) + eTDM_OUT2 (ch16-31) */
	ret = mt8188_afe_route(dev, MT8188_ROUTE_SRC_DL11, MT8188_ROUTE_DST_ETDM_OUT1_OUT2);
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

	ret = mt8188_afe_set_buf(dev, MT8188_DL11, dma_buf_pa, BUF_BYTES);
	if (ret != 0) {
		printk("set_buf failed: %d\n", ret);
		return ret;
	}

	ret = mt8188_afe_start(dev, MT8188_DL11);
	if (ret != 0) {
		printk("start failed: %d\n", ret);
		return ret;
	}

	printk("MT8188 DL11 playback started: 48kHz 32ch 32-bit DSP-B\n");
	printk("  ch0-15  -> eTDM_OUT1 (O072-O087)\n");
	printk("  ch16-31 -> eTDM_OUT2 (O048-O063), via DL8_DL11 mux\n");

	while (1) {
		k_msleep(1000);

		/* get_cur returns a physical address */
		uint32_t ptr = mt8188_afe_get_cur(dev, MT8188_DL11);
		uint32_t offset = (ptr > dma_buf_pa) ? (ptr - dma_buf_pa) : 0;

		printk("playing... DMA offset 0x%05x / 0x%05x\n", offset, (uint32_t)BUF_BYTES);
	}

	return 0;
}
