/*
 * Copyright (c) 2026 MediaTek Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MT8188 full-duplex TDM loopback sample, second port pair.
 *
 *   playback: DL11 → eTDM_OUT1 (direct range I022..I037 → O072..O087)
 *   capture:  UL8  ← eTDM_IN1  (hardware-wired, no AFE_CONN bits, no CM0)
 *
 * 16 channels, 48 kHz, 32-bit, DSP-B, one-pin. Both memifs use a single eTDM
 * port here, so 16 channels is the ceiling. eTDM_OUT1 is a standalone clock
 * master and eTDM_IN1 is a plain external slave that takes BCK/LRCK on its own
 * pins, so the loop is closed with wires on the EVK:
 *
 *   pin 5  I2SO1_BCK → pin 126 TDMIN_BCK
 *   pin 6  I2SO1_WS  → pin 127 TDMIN_LRCK
 *   pin 11 I2SO1_D0  → pin 128 TDMIN_DI
 *   pin 4  I2SO1_MCK → pin 125 TDMIN_MCK   (optional — see below)
 *
 * MCK is not required: an eTDM input in slave mode latches on the received BCK
 * and clocks its AFIFO from the a1sys domain, and the capture config passes
 * mclk_freq = 0 so the port never drives its own MCK pin.
 *
 * A running pattern is played out and every recorded frame is checked against
 * it from a timer interrupt. Status prints once a second; the first failure
 * stops both streams and prints what went wrong.
 *
 * This pairing exists to isolate the capture interconnect: UL8 is wired
 * straight to eTDM_IN1, where UL9 reaches it through AFE_CONN and CM0. Compare
 * with loopback_dl11_ul9 (same playback path, UL9 capture) and
 * loopback_dl8_ul3 (the eTDM2 pair).
 */

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/devicetree_regions.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/printk.h>

#include <zephyr/drivers/audio/mt8188_afe.h>

#define SAMPLE_RATE 48000
#ifndef CHANNELS
#define CHANNELS 16
#endif
#define WORD_BYTES 4 /* 32-bit */

/* A single eTDM port carries at most 16 channels, and DL11/UL8 each use one
 * port. The counts below are the ones where the TDM slot count the port emits
 * (get_etdm_ch_fixup() rounds up to a power of two) matches the channel count
 * the memif moves.
 */
BUILD_ASSERT(CHANNELS == 2 || CHANNELS == 4 || CHANNELS == 8 || CHANNELS == 16,
	     "CHANNELS must be 2, 4, 8 or 16: DL11/UL8 use a single eTDM port");

#define PERIOD_FRAMES (SAMPLE_RATE / 100) /* 480 frames = 10 ms */
#define NUM_PERIODS   10                  /* 100 ms ring per direction */
#define BUF_FRAMES    (PERIOD_FRAMES * NUM_PERIODS)
#define BUF_SAMPLES   (BUF_FRAMES * CHANNELS)
#define BUF_BYTES     (BUF_SAMPLES * WORD_BYTES)
#define PERIOD_BYTES  (PERIOD_FRAMES * CHANNELS * WORD_BYTES)

/* Tick twice per period so a period boundary is never missed. */
#define TICK_MS        5
/* Ticks without any DMA pointer movement that count as a stall (30 ms = 3
 * periods) — long enough not to trip on tick jitter.
 */
#define STALL_TICKS    6
/* Periods to let the paths settle before checking anything. */
#define SETTLE_TICKS   4
/* Startup trace: both DMA pointers sampled at roughly one-frame intervals from
 * the moment the streams are running, before the timer takes over. The steady
 * offset between the two is FIFO depth and says nothing about alignment; what
 * the frame lag reports is the frames the capture side misses, or writes
 * before data arrives, while that offset is still forming. Rows are printed
 * only when a pointer starts moving or the offset moves by TRACE_GAP_STEP, to
 * keep the dump short.
 */
#define TRACE_N        48
#define TRACE_STEP_US  20
#define TRACE_GAP_STEP 4
#define FRAME_BYTES    (CHANNELS * WORD_BYTES)

/* Sample word: channel tag in the top byte, running global frame index below.
 * The index is global rather than buffer-relative, so the pattern never
 * repeats over the ring and stalled or stale data cannot pass the check.
 */
#define CH_TAG_SHIFT   24
#define CNT_MASK       0x00ffffffU
#define PATTERN(g, ch) (((uint32_t)(ch) << CH_TAG_SHIFT) | ((g) & CNT_MASK))

/* DMA buffers live in the dedicated dma_region (see board DTS), outside
 * zephyr,sram, so AFE DMA cannot reach the Zephyr image, stack or heap. The
 * region is mapped non-cacheable, so no cache maintenance is needed here.
 */
static uint32_t play_buf[BUF_SAMPLES] Z_GENERIC_SECTION(
	LINKER_DT_NODE_REGION_NAME(DT_NODELABEL(dma_region))) __aligned(64);
static uint32_t
	cap_buf[BUF_SAMPLES] Z_GENERIC_SECTION(LINKER_DT_NODE_REGION_NAME(DT_NODELABEL(dma_region)))
		__aligned(64);

static const struct device *afe_dev;
static uint32_t play_buf_pa;
static uint32_t cap_buf_pa;

static struct k_timer tick;
static K_SEM_DEFINE(err_sem, 0, 1);

/* Owned by the timer interrupt; read by main() for the status line. */
static volatile uint32_t play_period; /* next ring period to refill */
static volatile uint64_t play_frames; /* frames written to play_buf */
static volatile uint32_t cap_period;  /* next ring period to verify */
static volatile uint64_t cap_frames;  /* frames consumed from cap_buf */
static volatile uint32_t ticks;
static volatile uint32_t max_isr_us;
static volatile bool aligned;
static volatile uint32_t ch_rot; /* channel tag found in slot 0 */
static volatile uint32_t lag;    /* played counter − received frame index */
static volatile bool failed;

/* Startup pointer trace, filled by main() before the timer is started. */
static struct {
	uint32_t us;
	uint32_t play_off;
	uint32_t cap_off;
} trace[TRACE_N];

/* lag is a modular (2^24) offset because the pattern counter wraps there; the
 * verify arithmetic needs it that way, but a negative latency shows up as a
 * value just below 2^24. Sign-extend it for display only.
 */
static int32_t lag_signed(void)
{
	uint32_t l = lag;

	return (l & (CNT_MASK / 2 + 1)) ? (int32_t)l - (int32_t)(CNT_MASK + 1) : (int32_t)l;
}

static struct {
	const char *what;
	bool is_mismatch;
	uint32_t frame;
	uint32_t ch;
	uint32_t want;
	uint32_t got;
} err;

static void fail(const char *what, uint32_t val)
{
	if (failed) {
		return;
	}
	err.what = what;
	err.is_mismatch = false;
	err.got = val;
	failed = true;
	k_sem_give(&err_sem);
}

static void fail_mismatch(uint32_t frame, uint32_t ch, uint32_t want, uint32_t got)
{
	if (failed) {
		return;
	}
	err.what = "recorded sample does not match what was played";
	err.is_mismatch = true;
	err.frame = frame;
	err.ch = ch;
	err.want = want;
	err.got = got;
	failed = true;
	k_sem_give(&err_sem);
}

/* Write one ring period of the pattern, continuing the global frame index. */
static void fill_period(uint32_t p)
{
	uint32_t *dst = &play_buf[p * PERIOD_FRAMES * CHANNELS];
	uint32_t g = (uint32_t)play_frames;

	for (int f = 0; f < PERIOD_FRAMES; f++, g++) {
		for (int ch = 0; ch < CHANNELS; ch++) {
			*dst++ = PATTERN(g, ch);
		}
	}

	/* Make the period visible to the AFE before it reads it. */
	barrier_dmem_fence_full();

	play_frames += PERIOD_FRAMES;
	play_period = (p + 1) % NUM_PERIODS;
}

/* Check one ring period against the pattern. Returns false on the first
 * mismatch, leaving cap_period/cap_frames pointing at the failing period.
 */
static bool verify_period(uint32_t p)
{
	const uint32_t *src = &cap_buf[p * PERIOD_FRAMES * CHANNELS];
	uint32_t g = (uint32_t)cap_frames;

	barrier_dmem_fence_full();

	for (int f = 0; f < PERIOD_FRAMES; f++, g++) {
		uint32_t want_cnt;

		/* The path has a constant channel rotation and a constant
		 * frame delay. Latch both from the first frame checked, then
		 * hold them to it: a rotation or delay that changes later is
		 * itself a failure.
		 */
		if (!aligned) {
			uint32_t tag = src[0] >> CH_TAG_SHIFT;

			if (tag >= CHANNELS) {
				fail("channel tag out of range in first recorded frame", src[0]);
				return false;
			}
			ch_rot = tag;
			lag = ((src[0] & CNT_MASK) - g) & CNT_MASK;
			aligned = true;
		}

		want_cnt = (g + lag) & CNT_MASK;

		for (int ch = 0; ch < CHANNELS; ch++) {
			uint32_t want = PATTERN(want_cnt, (ch_rot + ch) % CHANNELS);

			if (src[ch] != want) {
				fail_mismatch(g, ch, want, src[ch]);
				return false;
			}
		}
		src += CHANNELS;
	}

	cap_frames += PERIOD_FRAMES;
	cap_period = (p + 1) % NUM_PERIODS;
	return true;
}

/* Current ring period of a memif's DMA pointer, or -1 if out of range. */
static int cur_period(enum mt8188_memif_id memif, uint32_t base_pa, uint32_t *off_out)
{
	uint32_t off = mt8188_afe_get_cur(afe_dev, memif) - base_pa;

	*off_out = off;
	if (off >= BUF_BYTES) {
		return -1;
	}
	return (int)(off / PERIOD_BYTES);
}

/* All data handling happens here, in the timer interrupt. */
static void tick_handler(struct k_timer *t)
{
	static uint32_t prev_play_off, prev_cap_off;
	static uint32_t play_move_tick, cap_move_tick;
	uint32_t t0 = k_cycle_get_32();
	uint32_t play_off, cap_off, isr_us;
	int play_cur, cap_cur, advanced;

	ARG_UNUSED(t);

	if (failed) {
		return;
	}
	ticks++;

	/* The pointer registers only hold a buffer address once the memif has
	 * fetched for the first time, so an out-of-range read is tolerated
	 * while the paths are still settling.
	 */
	play_cur = cur_period(MT8188_DL11, play_buf_pa, &play_off);
	cap_cur = cur_period(MT8188_UL8, cap_buf_pa, &cap_off);
	if (play_cur < 0 || cap_cur < 0) {
		if (ticks > SETTLE_TICKS) {
			fail(play_cur < 0 ? "DL11 DMA pointer outside the playback buffer"
					  : "UL8 DMA pointer outside the capture buffer",
			     play_cur < 0 ? play_off : cap_off);
		}
		return;
	}

	if (play_off != prev_play_off) {
		play_move_tick = ticks;
	}
	if (cap_off != prev_cap_off) {
		cap_move_tick = ticks;
	}
	prev_play_off = play_off;
	prev_cap_off = cap_off;

	if (ticks > SETTLE_TICKS) {
		if (ticks - play_move_tick > STALL_TICKS) {
			fail("DL11 DMA pointer stopped moving", play_off);
			return;
		}
		if (ticks - cap_move_tick > STALL_TICKS) {
			fail("UL8 DMA pointer stopped moving (no BCK/LRCK?)", cap_off);
			return;
		}
	}

	/* Refill every period the playback DMA has finished with, stopping at
	 * the one it is reading now. The oldest period is rewritten a full
	 * ring ahead of being read again, which is what keeps the global frame
	 * counter continuous across the wrap.
	 */
	advanced = (play_cur - (int)play_period + NUM_PERIODS) % NUM_PERIODS;
	if (advanced > NUM_PERIODS - 2) {
		fail("playback underrun: DMA overtook the refill point", (uint32_t)advanced);
		return;
	}
	while (play_period != (uint32_t)play_cur) {
		fill_period(play_period);
	}

	advanced = (cap_cur - (int)cap_period + NUM_PERIODS) % NUM_PERIODS;
	if (advanced > NUM_PERIODS - 2) {
		fail("capture overrun: DMA overwrote unverified periods", (uint32_t)advanced);
		return;
	}
	if (ticks > SETTLE_TICKS) {
		while (cap_period != (uint32_t)cap_cur) {
			if (!verify_period(cap_period)) {
				return;
			}
		}
	} else {
		/* Still settling: consume without checking. */
		while (cap_period != (uint32_t)cap_cur) {
			cap_frames += PERIOD_FRAMES;
			cap_period = (cap_period + 1) % NUM_PERIODS;
		}
	}

	isr_us = k_cyc_to_us_floor32(k_cycle_get_32() - t0);
	if (isr_us > max_isr_us) {
		max_isr_us = isr_us;
	}
}

/* Startup pointer trace. A pointer register only holds a buffer address once
 * its memif has fetched, so an out-of-range value is reported as idle — which
 * is what shows which engine moved first.
 */
static void print_trace(void)
{
	int32_t prev_gap = 0;
	bool prev_pidle = true;
	bool prev_cidle = true;
	bool first = true;

	printk("  startup, both DMA pointers sampled every ~%u us:\n", TRACE_STEP_US);

	for (uint32_t i = 0; i < TRACE_N; i++) {
		bool pidle = trace[i].play_off >= BUF_BYTES;
		bool cidle = trace[i].cap_off >= BUF_BYTES;
		uint32_t pf = pidle ? 0 : trace[i].play_off / FRAME_BYTES;
		uint32_t cf = cidle ? 0 : trace[i].cap_off / FRAME_BYTES;
		int32_t gap = (int32_t)pf - (int32_t)cf;
		bool live = !pidle && !cidle;
		bool show;

		show = first || pidle != prev_pidle || cidle != prev_cidle ||
		       (live &&
			(gap - prev_gap >= TRACE_GAP_STEP || prev_gap - gap >= TRACE_GAP_STEP)) ||
		       i == TRACE_N - 1;
		if (!show) {
			continue;
		}
		first = false;
		prev_pidle = pidle;
		prev_cidle = cidle;
		if (live) {
			prev_gap = gap;
		}

		printk("    t=%5u us  DL11 ", trace[i].us);
		if (pidle) {
			printk("idle   ");
		} else {
			printk("%4u fr", pf);
		}
		printk("  UL8 ");
		if (cidle) {
			printk("idle\n");
		} else {
			printk("%4u fr  ahead by %3d fr\n", cf, gap);
		}
	}
}

static void print_error(void)
{
	printk("\nERROR: %s\n", err.what);
	if (err.is_mismatch) {
		printk("  frame %u  slot %u: expected 0x%08x, got 0x%08x\n", err.frame, err.ch,
		       err.want, err.got);
		printk("  (channel rotation %u, frame lag %d)\n", ch_rot, lag_signed());
	} else {
		printk("  value 0x%08x (%u)\n", err.got, err.got);
	}
	printk("  played %llu frames, verified %llu frames\n", play_frames, cap_frames);
}

int main(void)
{
	struct mt8188_afe_cfg cfg_play = {
		.rate = SAMPLE_RATE,
		.channels = CHANNELS,
		.word_size = 32,
		.fmt = MT8188_ETDM_FMT_DSPB,
		.data_mode = MT8188_ETDM_DATA_ONE_PIN,
		.mclk_freq = 256 * SAMPLE_RATE, /* 12.288 MHz, master */
		.mclk_dir = MT8188_MCLK_DIR_OUT,
		.period_frames = PERIOD_FRAMES,
	};
	struct mt8188_afe_cfg cfg_cap = {
		.rate = SAMPLE_RATE,
		.channels = CHANNELS,
		.word_size = 32,
		.fmt = MT8188_ETDM_FMT_DSPB,
		.data_mode = MT8188_ETDM_DATA_ONE_PIN,
		/* eTDM_IN1 takes BCK/LRCK from its pins and must not drive MCK
		 * back at the master over the loopback wire.
		 */
		.slave_mode = true,
		.mclk_freq = 0,
		.mclk_dir = MT8188_MCLK_DIR_IN,
		.period_frames = PERIOD_FRAMES,
	};
	uint32_t secs = 0;
	uint32_t trace_t0;
	int ret;

	afe_dev = DEVICE_DT_GET(DT_NODELABEL(afe));
	if (!device_is_ready(afe_dev)) {
		printk("AFE device not ready\n");
		return -ENODEV;
	}

	/* Single CPU-address -> hardware-address conversion point: a non-flat
	 * mapping would only need changing here.
	 *
	 * The driver maps dma_region one to one, so the linker address already
	 * is the address the AFE needs. k_mem_phys_addr() is deliberately not
	 * used: its range assert only covers the kernel VM window, which this
	 * region sits outside.
	 */
	play_buf_pa = (uint32_t)(uintptr_t)play_buf;
	cap_buf_pa = (uint32_t)(uintptr_t)cap_buf;

	printk("MT8188 DL11→UL8 loopback: %d ch, %d Hz, 32-bit, DSP-B\n", CHANNELS, SAMPLE_RATE);
	printk("  play buffer %p (%u bytes), capture buffer %p\n", play_buf, (uint32_t)BUF_BYTES,
	       cap_buf);
	printk("  BCK %u Hz — wire I2SO1 BCK/WS/D0 to TDMIN BCK/LRCK/DI\n",
	       SAMPLE_RATE * CHANNELS * 32);

	/* Playback first: eTDM_OUT1 is the clock master, and configuring it
	 * before the capture side means eTDM_IN1 is written last and keeps its
	 * own slave-mode framing. At <=16 channels DL11 reaches eTDM_OUT1 on its
	 * direct I022..I037 range, so the DL8_DL11 mux is left alone.
	 */
	ret = mt8188_afe_configure(afe_dev, MT8188_DL11, &cfg_play);
	if (ret != 0) {
		printk("configure(DL11) failed: %d\n", ret);
		return ret;
	}
	ret = mt8188_afe_route(afe_dev, MT8188_ROUTE_SRC_DL11, MT8188_ROUTE_DST_ETDM_OUT1);
	if (ret != 0) {
		printk("route(DL11) failed: %d\n", ret);
		return ret;
	}
	ret = mt8188_afe_set_buf(afe_dev, MT8188_DL11, play_buf_pa, BUF_BYTES);
	if (ret != 0) {
		printk("set_buf(DL11) failed: %d\n", ret);
		return ret;
	}

	ret = mt8188_afe_configure(afe_dev, MT8188_UL8, &cfg_cap);
	if (ret != 0) {
		printk("configure(UL8) failed: %d\n", ret);
		return ret;
	}
	/* UL8 ← eTDM_IN1 is hardware-wired; route() has no CONN bits to set
	 * but is called for symmetry and to validate the pairing.
	 */
	ret = mt8188_afe_route(afe_dev, MT8188_ROUTE_SRC_ETDM_IN1, MT8188_ROUTE_DST_UL8);
	if (ret != 0) {
		printk("route(UL8) failed: %d\n", ret);
		return ret;
	}
	ret = mt8188_afe_set_buf(afe_dev, MT8188_UL8, cap_buf_pa, BUF_BYTES);
	if (ret != 0) {
		printk("set_buf(UL8) failed: %d\n", ret);
		return ret;
	}

	/* Prefill the whole playback ring; the timer takes over from here.
	 * fill_period() leaves play_period back at 0 after the last period.
	 */
	for (uint32_t p = 0; p < NUM_PERIODS; p++) {
		fill_period(p);
	}

	/* Capture first — eTDM_IN1 idles until clocks appear on its pins —
	 * then playback, which starts BCK/LRCK and so starts the capture too.
	 */
	ret = mt8188_afe_start(afe_dev, MT8188_UL8);
	if (ret != 0) {
		printk("start(UL8) failed: %d\n", ret);
		return ret;
	}
	ret = mt8188_afe_start(afe_dev, MT8188_DL11);
	if (ret != 0) {
		printk("start(DL11) failed: %d\n", ret);
		mt8188_afe_stop(afe_dev, MT8188_UL8);
		return ret;
	}

	/* Record how the two engines came up relative to each other, before the
	 * timer starts touching the same registers.
	 */
	trace_t0 = k_cycle_get_32();
	for (uint32_t i = 0; i < TRACE_N; i++) {
		trace[i].us = k_cyc_to_us_floor32(k_cycle_get_32() - trace_t0);
		trace[i].play_off = mt8188_afe_get_cur(afe_dev, MT8188_DL11) - play_buf_pa;
		trace[i].cap_off = mt8188_afe_get_cur(afe_dev, MT8188_UL8) - cap_buf_pa;
		k_busy_wait(TRACE_STEP_US);
	}

	k_timer_init(&tick, tick_handler, NULL);
	k_timer_start(&tick, K_MSEC(TICK_MS), K_MSEC(TICK_MS));

	/* Printed only once the timer is running: printk() blocks on the console
	 * for long enough that the playback DMA would outrun the refill point
	 * and report a spurious underrun.
	 */
	print_trace();

	printk("running — data handled every %d ms from the timer interrupt\n", TICK_MS);

	while (1) {
		if (k_sem_take(&err_sem, K_MSEC(1000)) == 0) {
			k_timer_stop(&tick);
			print_error();
			/* Slave first, while its clocks are still running. */
			mt8188_afe_stop(afe_dev, MT8188_UL8);
			mt8188_afe_stop(afe_dev, MT8188_DL11);
			printk("stopped\n");
			return -EIO;
		}

		secs++;
		printk("[%4us] played %llu fr  verified %llu fr  ticks %u  "
		       "rot %u lag %d  isr max %u us\n",
		       secs, play_frames, cap_frames, ticks, aligned ? ch_rot : 0,
		       aligned ? lag_signed() : 0, max_isr_us);
	}

	return 0;
}
