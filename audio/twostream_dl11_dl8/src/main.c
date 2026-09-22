/*
 * Copyright (c) 2026 MediaTek Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MT8188 two-stream teardown test: DL11 -> eTDM_OUT1 and DL8 -> eTDM_OUT2,
 * both 16 channel, 48 kHz, 32-bit, DSP-B.
 *
 * Both streams run at the same rate, so they share one audio PLL timing
 * domain.  What this asks is what happens to the survivor when one of them
 * stops.  A stop() that tears the domain down unconditionally takes the other
 * stream's bit clock with it and its DMA pointer stops moving; the
 * clock_control drivers underneath do not reference count, so the AFE driver
 * has to.
 *
 * No loopback sample covers this.  All three stop both streams at teardown,
 * which is the one order in which the fault cannot show.
 *
 * Playback only, and no wire is needed: the question is whether a memory
 * interface keeps moving frames, which needs the port clocked, not connected
 * to anything.
 *
 * Four phases, each measured rather than assumed:
 *
 *   1. both started      both advance
 *   2. DL8 stopped       DL11 still advances, DL8 does not
 *   3. DL8 restarted     both advance again
 *   4. DL11 stopped      DL8 still advances
 *
 * Phase 3 separates a missing increment from a missing decrement: a count that
 * never rises brings the domain up once and never again.  Phase 4 repeats the
 * test in the other order, because a reference count can be wrong in one
 * direction only.
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
#define FRAME_BYTES (CHANNELS * WORD_BYTES)

/*
 * The measurement window is shorter than the buffer so that a pointer that has
 * wrapped once is still told apart from one that has not moved.
 */
#define WINDOW_MS     50
#define EXPECT_FRAMES (SAMPLE_RATE * WINDOW_MS / 1000)

/*
 * The pointer is sampled from a thread, so the window is only as accurate as
 * the scheduler.  A fifth either way is generous and still an order of
 * magnitude away from the "stopped" band.
 */
#define MOVE_MIN (EXPECT_FRAMES * 4 / 5)
#define MOVE_MAX (EXPECT_FRAMES * 6 / 5)
#define STOP_MAX (EXPECT_FRAMES / 20)

/* Buffers live in the dedicated dma_region (see the mtk-afe snippet), outside
 * zephyr,sram, so AFE DMA cannot reach the Zephyr image, stack or heap.
 */
#define DMA_SECTION Z_GENERIC_SECTION(LINKER_DT_NODE_REGION_NAME(DT_NODELABEL(dma_region)))

static int32_t dl11_buf[BUF_SAMPLES] DMA_SECTION __aligned(64);
static int32_t dl8_buf[BUF_SAMPLES] DMA_SECTION __aligned(64);

/* Frames the interface moved over one window, or UINT32_MAX if its pointer
 * left the buffer.  An out-of-range pointer is worth naming rather than
 * folding into a frame count: it is what an AFE that is not powered reports,
 * and it would otherwise read as a wild rate.
 */
static uint32_t frames_moved(const struct device *dev, enum mt8188_memif_id memif_id, uint32_t base)
{
	uint32_t first = mt8188_afe_get_cur(dev, memif_id) - base;
	uint32_t second;
	uint32_t delta;

	if (first >= (uint32_t)BUF_BYTES) {
		return UINT32_MAX;
	}

	k_msleep(WINDOW_MS);

	second = mt8188_afe_get_cur(dev, memif_id) - base;
	if (second >= (uint32_t)BUF_BYTES) {
		return UINT32_MAX;
	}

	delta = (second >= first) ? (second - first) : (second + (uint32_t)BUF_BYTES - first);

	return delta / FRAME_BYTES;
}

static bool check(const char *label, uint32_t frames, bool want_moving)
{
	bool ok;

	if (frames == UINT32_MAX) {
		printk("    %-22s FAIL  DMA pointer outside its buffer\n", label);
		return false;
	}

	ok = want_moving ? (frames >= MOVE_MIN && frames <= MOVE_MAX) : (frames <= STOP_MAX);

	if (want_moving) {
		printk("    %-22s %s  %5u frames in %d ms, wanted ~%d\n", label,
		       ok ? "ok  " : "FAIL", frames, WINDOW_MS, EXPECT_FRAMES);
	} else {
		printk("    %-22s %s  %5u frames in %d ms, wanted 0\n", label, ok ? "ok  " : "FAIL",
		       frames, WINDOW_MS);
	}

	return ok;
}

static int setup(const struct device *dev, enum mt8188_memif_id memif_id, enum mt8188_route_src src,
		 enum mt8188_route_dst dst, int32_t *buf, uint32_t base)
{
	struct mt8188_afe_cfg cfg = {
		.rate = SAMPLE_RATE,
		.channels = CHANNELS,
		.word_size = 32,
		.fmt = MT8188_ETDM_FMT_DSPB,
		.data_mode = MT8188_ETDM_DATA_ONE_PIN,
		.mclk_freq = 256 * SAMPLE_RATE,
		.mclk_dir = MT8188_MCLK_DIR_OUT,
		.period_frames = SAMPLE_RATE / 100,
	};
	int ret;

	/* Per-channel DC level: channel n gets n << 24. */
	for (int f = 0; f < BUF_FRAMES; f++) {
		for (int ch = 0; ch < CHANNELS; ch++) {
			buf[f * CHANNELS + ch] = (int32_t)((uint32_t)ch << 24);
		}
	}

	ret = mt8188_afe_configure(dev, memif_id, &cfg);
	if (ret != 0) {
		printk("configure failed: %d\n", ret);
		return ret;
	}

	ret = mt8188_afe_route(dev, src, dst);
	if (ret != 0) {
		printk("route failed: %d\n", ret);
		return ret;
	}

	ret = mt8188_afe_set_buf(dev, memif_id, base, BUF_BYTES);
	if (ret != 0) {
		printk("set_buf failed: %d\n", ret);
		return ret;
	}

	return 0;
}

int main(void)
{
	const struct device *dev;
	uint32_t dl11_pa, dl8_pa;
	bool pass = true;
	int ret;

	dev = DEVICE_DT_GET(DT_NODELABEL(afe));
	if (!device_is_ready(dev)) {
		printk("AFE device not ready\n");
		return -ENODEV;
	}

	/* The driver maps dma_region one to one, so the linker address already
	 * is the address the AFE needs.
	 */
	dl11_pa = (uint32_t)(uintptr_t)dl11_buf;
	dl8_pa = (uint32_t)(uintptr_t)dl8_buf;

	printk("MT8188 two-stream teardown: DL11 and DL8, %d ch, %d Hz, 32-bit, DSP-B\n", CHANNELS,
	       SAMPLE_RATE);
	printk("  DL11 buffer %p -> eTDM_OUT1, DL8 buffer %p -> eTDM_OUT2\n", dl11_buf, dl8_buf);
	printk("  both at one rate, so both draw on the same audio PLL timing domain\n");

	ret = setup(dev, MT8188_DL11, MT8188_ROUTE_SRC_DL11, MT8188_ROUTE_DST_ETDM_OUT1, dl11_buf,
		    dl11_pa);
	if (ret != 0) {
		return ret;
	}

	ret = setup(dev, MT8188_DL8, MT8188_ROUTE_SRC_DL8, MT8188_ROUTE_DST_ETDM_OUT2, dl8_buf,
		    dl8_pa);
	if (ret != 0) {
		return ret;
	}

	ret = mt8188_afe_start(dev, MT8188_DL11);
	if (ret != 0) {
		printk("start(DL11) failed: %d\n", ret);
		return ret;
	}

	ret = mt8188_afe_start(dev, MT8188_DL8);
	if (ret != 0) {
		printk("start(DL8) failed: %d\n", ret);
		mt8188_afe_stop(dev, MT8188_DL11);
		return ret;
	}

	printk("\n  1. both started\n");
	pass &= check("DL11", frames_moved(dev, MT8188_DL11, dl11_pa), true);
	pass &= check("DL8", frames_moved(dev, MT8188_DL8, dl8_pa), true);

	ret = mt8188_afe_stop(dev, MT8188_DL8);
	if (ret != 0) {
		printk("stop(DL8) failed: %d\n", ret);
		pass = false;
	}

	printk("\n  2. DL8 stopped -- DL11 must be untouched\n");
	pass &= check("DL11 survivor", frames_moved(dev, MT8188_DL11, dl11_pa), true);
	pass &= check("DL8 stopped", frames_moved(dev, MT8188_DL8, dl8_pa), false);

	ret = mt8188_afe_start(dev, MT8188_DL8);
	if (ret != 0) {
		printk("restart(DL8) failed: %d\n", ret);
		pass = false;
	}

	printk("\n  3. DL8 restarted -- the domain must come back for it\n");
	pass &= check("DL11", frames_moved(dev, MT8188_DL11, dl11_pa), true);
	pass &= check("DL8", frames_moved(dev, MT8188_DL8, dl8_pa), true);

	ret = mt8188_afe_stop(dev, MT8188_DL11);
	if (ret != 0) {
		printk("stop(DL11) failed: %d\n", ret);
		pass = false;
	}

	printk("\n  4. DL11 stopped -- the other order\n");
	pass &= check("DL8 survivor", frames_moved(dev, MT8188_DL8, dl8_pa), true);
	pass &= check("DL11 stopped", frames_moved(dev, MT8188_DL11, dl11_pa), false);

	mt8188_afe_stop(dev, MT8188_DL8);

	printk("\n%s\n", pass ? "PASS -- the timing domain is reference counted"
			      : "FAIL -- see the phases above");

	return pass ? 0 : -EIO;
}
