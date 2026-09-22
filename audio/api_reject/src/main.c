/*
 * Copyright (c) 2026 MediaTek Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MT8188 AFE argument rejection test.
 *
 * Walks the guards on configure() and set_buf() and checks each one returns
 * what it promises.  These are worth a test of their own because of what sits
 * behind them: several eTDM register fields are written as a count less one,
 * so a zero channel count underflows into an all-ones field and the block
 * accepts it, leaving a stream running at a width nothing asked for.  The
 * buffer registers steer a bus master with no address translation in front of
 * it, so a zero size puts the end below the base and an unaligned address puts
 * the wrap point mid-sample.  Neither reports an error from the hardware.
 *
 * Every case is paired with a valid call of the same function, so a guard that
 * has started rejecting everything is told apart from one that works.  Without
 * that, a bug that rejects every configuration would read as a clean pass.
 *
 * Nothing is ever started, so no wiring is needed and the AFE is left as it
 * was found.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/devicetree_regions.h>
#include <zephyr/sys/printk.h>

#include <zephyr/drivers/audio/mt8188_afe.h>

#define SAMPLE_RATE 48000
#define CHANNELS    16
#define WORD_BYTES  4 /* 32-bit */
#define BUF_FRAMES  480
#define BUF_SAMPLES (BUF_FRAMES * CHANNELS)
#define BUF_BYTES   (BUF_SAMPLES * WORD_BYTES)

/* The driver's own alignment is private to it, so this is the value the
 * interface documents rather than a symbol shared with it.  A test that
 * imported the constant would follow the code it is meant to hold still.
 */
#define BUF_ALIGN 16U

#define DMA_SECTION Z_GENERIC_SECTION(LINKER_DT_NODE_REGION_NAME(DT_NODELABEL(dma_region)))

static int32_t dma_buf[BUF_SAMPLES] DMA_SECTION __aligned(64);

static unsigned int passed;
static unsigned int failed;

static const char *errname(int err)
{
	switch (err) {
	case 0:
		return "0";
	case -EINVAL:
		return "-EINVAL";
	case -ENOTSUP:
		return "-ENOTSUP";
	default:
		return "other";
	}
}

static void check(const char *what, int got, int want)
{
	bool ok = (got == want);

	if (ok) {
		passed++;
	} else {
		failed++;
	}

	printk("    %-34s %s  got %-8s wanted %s\n", what, ok ? "ok  " : "FAIL", errname(got),
	       errname(want));
}

int main(void)
{
	const struct device *dev;
	uint32_t pa;
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
	struct mt8188_afe_cfg bad;

	dev = DEVICE_DT_GET(DT_NODELABEL(afe));
	if (!device_is_ready(dev)) {
		printk("AFE device not ready\n");
		return -ENODEV;
	}

	pa = (uint32_t)(uintptr_t)dma_buf;

	printk("MT8188 AFE argument rejection test\n");
	printk("  buffer %p, %u bytes\n", dma_buf, (uint32_t)BUF_BYTES);

	printk("\n  configure()\n");
	check("valid, as a control", mt8188_afe_configure(dev, MT8188_DL11, &cfg), 0);
	check("memory interface out of range",
	      mt8188_afe_configure(dev, (enum mt8188_memif_id)MT8188_MEMIF_NR, &cfg), -EINVAL);
	check("memory interface not driven", mt8188_afe_configure(dev, MT8188_DL2, &cfg), -ENOTSUP);
	check("null configuration", mt8188_afe_configure(dev, MT8188_DL11, NULL), -EINVAL);

	bad = cfg;
	bad.channels = 0U;
	check("zero channels", mt8188_afe_configure(dev, MT8188_DL11, &bad), -EINVAL);

	bad = cfg;
	bad.channels = MT8188_AFE_MAX_CHANNELS + 1U;
	check("more channels than a port pair", mt8188_afe_configure(dev, MT8188_DL11, &bad),
	      -EINVAL);

	bad = cfg;
	bad.word_size = 24U;
	check("word size the block cannot carry", mt8188_afe_configure(dev, MT8188_DL11, &bad),
	      -EINVAL);

	bad = cfg;
	bad.rate = SAMPLE_RATE - 1U;
	check("rate with no timing value", mt8188_afe_configure(dev, MT8188_DL11, &bad), -EINVAL);

	printk("\n  set_buf()\n");
	check("valid, as a control", mt8188_afe_set_buf(dev, MT8188_DL11, pa, BUF_BYTES), 0);
	check("memory interface out of range",
	      mt8188_afe_set_buf(dev, (enum mt8188_memif_id)MT8188_MEMIF_NR, pa, BUF_BYTES),
	      -EINVAL);
	check("memory interface not driven", mt8188_afe_set_buf(dev, MT8188_DL2, pa, BUF_BYTES),
	      -ENOTSUP);
	check("zero size", mt8188_afe_set_buf(dev, MT8188_DL11, pa, 0U), -EINVAL);
	check("size not a whole number of slots",
	      mt8188_afe_set_buf(dev, MT8188_DL11, pa, BUF_ALIGN + 1U), -EINVAL);
	check("base address unaligned", mt8188_afe_set_buf(dev, MT8188_DL11, pa + 1U, BUF_BYTES),
	      -EINVAL);
	check("base plus size past 32 bits",
	      mt8188_afe_set_buf(dev, MT8188_DL11, UINT32_MAX - BUF_ALIGN + 1U, BUF_ALIGN * 2U),
	      -EINVAL);

	printk("\n%s -- %u of %u checks\n", failed == 0U ? "PASS" : "FAIL", passed,
	       passed + failed);

	return (failed == 0U) ? 0 : -EIO;
}
