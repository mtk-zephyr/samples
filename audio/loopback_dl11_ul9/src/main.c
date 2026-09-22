/*
 * Copyright (c) 2026 MediaTek Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MT8188 full-duplex TDM loopback sample.
 *
 *   playback: DL11 → eTDM_OUT1 (ch0-15) + eTDM_OUT2 (ch16-31, via the mux)
 *   capture:  UL9  ← CM0 (bypass) ← AFE_CONN ← eTDM_IN1 + eTDM_IN2
 *
 * 32 channels, 48 kHz, 32-bit, DSP-B, one-pin. A single eTDM port carries at
 * most 16 channels, so the stream is split across a cowork'd port pair in each
 * direction: eTDM_OUT1 is the cowork master and eTDM_OUT2 is slaved to it,
 * and on the capture side the driver slaves eTDM_IN2 to eTDM_IN1 internally.
 *
 * Playback is the clock master; eTDM_IN1 is a plain external slave that takes
 * MCK/BCK/LRCK on its own pins, so the loop is closed with wires on the EVK:
 *
 *   pin 4   I2SO1_MCK → pin 125 TDMIN_MCK
 *   pin 5   I2SO1_BCK → pin 126 TDMIN_BCK
 *   pin 6   I2SO1_WS  → pin 127 TDMIN_LRCK
 *   pin 11  I2SO1_D0  → pin 128 TDMIN_DI    (ch0-15)
 *   pin 117 I2SO2_D0  → pin 110 I2SIN_D0    (ch16-31)
 *
 * Only the one clock harness above is needed, and at 32 channels it is also
 * the only one possible: eTDM_OUT2 is a cowork slave and so does not drive its
 * own BCK/WS, and eTDM_IN2 takes its clocks from eTDM_IN1 rather than its own
 * pins. Below 17 channels only the OUT1/IN1 wires are used.
 *
 * A running pattern is played out and every recorded frame is checked against
 * it from a timer interrupt. Status prints once a second; the first failure
 * stops both streams and prints what went wrong.
 */

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/devicetree_regions.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/printk.h>

#include <zephyr/drivers/audio/mt8188_afe.h>

#define SAMPLE_RATE 48000
#ifndef CHANNELS
#define CHANNELS 32
#endif
#define WORD_BYTES 4 /* 32-bit */

/* The AFE_CONN tables and the fixed 16+16 split across a cowork'd port pair
 * wire whole 16-channel ranges, so only these counts keep the memif channel
 * count in step with the TDM slot count the eTDM ports emit.
 */
BUILD_ASSERT(CHANNELS == 2 || CHANNELS == 4 || CHANNELS == 8 || CHANNELS == 16 || CHANNELS == 32,
	     "CHANNELS must be 2, 4, 8, 16 or 32");

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
static volatile uint32_t lag; /* played counter − received frame index */
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

/* Latched slot → played-channel map, taken from the first frame checked.
 * Deliberately not assumed to be a rotation, or the identity: the two halves
 * of a 32-channel stream travel over separate wires (ch0-15 through
 * eTDM_OUT1/IN1, ch16-31 through eTDM_OUT2/IN2), so each half can arrive
 * rotated independently, or the halves can land swapped. Whatever the first
 * frame shows is validated as a permutation of the played channels and then
 * enforced for the rest of the run.
 */
static uint8_t slot_ch[CHANNELS];

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

		/* The path has a constant slot→channel map and a constant frame
		 * delay. Latch both from the first frame checked, then hold the
		 * run to them: a map or delay that changes later is itself a
		 * failure.
		 */
		if (!aligned) {
			bool seen[CHANNELS] = {false};

			for (int ch = 0; ch < CHANNELS; ch++) {
				uint32_t tag = src[ch] >> CH_TAG_SHIFT;

				if (tag >= CHANNELS || seen[tag]) {
					fail("first recorded frame is not a permutation "
					     "of the played channels",
					     src[ch]);
					return false;
				}
				seen[tag] = true;
				slot_ch[ch] = (uint8_t)tag;
			}
			lag = ((src[0] & CNT_MASK) - g) & CNT_MASK;
			/* Publish the map before the flag main() reads it by. */
			barrier_dmem_fence_full();
			aligned = true;
		}

		want_cnt = (g + lag) & CNT_MASK;

		for (int ch = 0; ch < CHANNELS; ch++) {
			uint32_t want = PATTERN(want_cnt, slot_ch[ch]);

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
	cap_cur = cur_period(MT8188_UL9, cap_buf_pa, &cap_off);
	if (play_cur < 0 || cap_cur < 0) {
		if (ticks > SETTLE_TICKS) {
			fail(play_cur < 0 ? "DL11 DMA pointer outside the playback buffer"
					  : "UL9 DMA pointer outside the capture buffer",
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
			fail("UL9 DMA pointer stopped moving (no BCK/LRCK?)", cap_off);
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

/* slot → channel map, as discovered on the first checked frame. */
static void print_map(void)
{
	printk("  slot->channel map (frame lag %d):", lag_signed());
	for (int ch = 0; ch < CHANNELS; ch++) {
		printk("%s%2u", (ch % 16) ? " " : "\n   ", slot_ch[ch]);
	}
	printk("\n");
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
		printk("  UL9 ");
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
		printk("  played channel %u was expected in this slot\n", slot_ch[err.ch]);
		print_map();
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
		/* eTDM_IN1 takes BCK/LRCK from its pins and must not drive
		 * MCK back at the master over the loopback wire.
		 */
		.slave_mode = true,
		.mclk_freq = 0,
		.mclk_dir = MT8188_MCLK_DIR_IN,
		.period_frames = PERIOD_FRAMES,
	};
	const enum mt8188_route_dst play_dst = (CHANNELS > MT8188_ETDM_MAX_CHANNELS)
						       ? MT8188_ROUTE_DST_ETDM_OUT1_OUT2
						       : MT8188_ROUTE_DST_ETDM_OUT1;
	const enum mt8188_route_src cap_src = (CHANNELS > MT8188_ETDM_MAX_CHANNELS)
						      ? MT8188_ROUTE_SRC_ETDM_IN1_IN2
						      : MT8188_ROUTE_SRC_ETDM_IN1;
	uint32_t secs = 0;
	bool map_shown = false;
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

	printk("MT8188 DL11→UL9 loopback: %d ch, %d Hz, 32-bit, DSP-B\n", CHANNELS, SAMPLE_RATE);
	printk("  play buffer %p (%u bytes), capture buffer %p\n", play_buf, (uint32_t)BUF_BYTES,
	       cap_buf);
	printk("  BCK %u Hz — wire I2SO1 MCK/BCK/WS/D0 to TDMIN MCK/BCK/LRCK/DI\n",
	       SAMPLE_RATE *
		       (CHANNELS > MT8188_ETDM_MAX_CHANNELS ? MT8188_ETDM_MAX_CHANNELS : CHANNELS) *
		       32);

	/* Playback first: eTDM_OUT1 is the clock master, and configuring it
	 * before the capture side means eTDM_IN1 is written last and keeps its
	 * own slave-mode framing.
	 */
	ret = mt8188_afe_configure(afe_dev, MT8188_DL11, &cfg_play);
	if (ret != 0) {
		printk("configure(DL11) failed: %d\n", ret);
		return ret;
	}
	ret = mt8188_afe_route(afe_dev, MT8188_ROUTE_SRC_DL11, play_dst);
	if (ret != 0) {
		printk("route(DL11) failed: %d\n", ret);
		return ret;
	}
	ret = mt8188_afe_set_buf(afe_dev, MT8188_DL11, play_buf_pa, BUF_BYTES);
	if (ret != 0) {
		printk("set_buf(DL11) failed: %d\n", ret);
		return ret;
	}

	ret = mt8188_afe_configure(afe_dev, MT8188_UL9, &cfg_cap);
	if (ret != 0) {
		printk("configure(UL9) failed: %d\n", ret);
		return ret;
	}
	ret = mt8188_afe_route(afe_dev, cap_src, MT8188_ROUTE_DST_UL9);
	if (ret != 0) {
		printk("route(UL9) failed: %d\n", ret);
		return ret;
	}
	ret = mt8188_afe_set_buf(afe_dev, MT8188_UL9, cap_buf_pa, BUF_BYTES);
	if (ret != 0) {
		printk("set_buf(UL9) failed: %d\n", ret);
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
	ret = mt8188_afe_start(afe_dev, MT8188_UL9);
	if (ret != 0) {
		printk("start(UL9) failed: %d\n", ret);
		return ret;
	}
	ret = mt8188_afe_start(afe_dev, MT8188_DL11);
	if (ret != 0) {
		printk("start(DL11) failed: %d\n", ret);
		mt8188_afe_stop(afe_dev, MT8188_UL9);
		return ret;
	}

	/* Record how the two engines came up relative to each other, before the
	 * timer starts touching the same registers.
	 */
	trace_t0 = k_cycle_get_32();
	for (uint32_t i = 0; i < TRACE_N; i++) {
		trace[i].us = k_cyc_to_us_floor32(k_cycle_get_32() - trace_t0);
		trace[i].play_off = mt8188_afe_get_cur(afe_dev, MT8188_DL11) - play_buf_pa;
		trace[i].cap_off = mt8188_afe_get_cur(afe_dev, MT8188_UL9) - cap_buf_pa;
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
			mt8188_afe_stop(afe_dev, MT8188_UL9);
			mt8188_afe_stop(afe_dev, MT8188_DL11);
			printk("stopped\n");
			return -EIO;
		}

		if (aligned && !map_shown) {
			map_shown = true;
			print_map();
		}

		secs++;
		printk("[%4us] played %llu fr  verified %llu fr  ticks %u  "
		       "lag %d  isr max %u us\n",
		       secs, play_frames, cap_frames, ticks, aligned ? lag_signed() : 0,
		       max_isr_us);
	}

	return 0;
}
