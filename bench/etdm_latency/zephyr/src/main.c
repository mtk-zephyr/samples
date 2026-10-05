/*
 * Copyright (c) 2026 MediaTek Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * eTDM loop-latency bench, Zephyr side.
 *
 *   DL11 -> eTDM_OUT1 (+OUT2 at 32 ch) -> wires -> eTDM_IN1 (+IN2) -> CM0 -> UL9
 *          ^                                                              |
 *          +---------------- block processing (pass | FIR | IIR) <--------+
 *
 * UL9 at every channel count, as on Linux, so both sides use one path.
 *
 * Captured frames are taken in blocks of `block` frames as soon as the capture
 * DMA pointer has passed them (busy polling: the driver has no period
 * interrupt), processed, and written to the playback ring `lead` frames ahead
 * of the playback DMA pointer.  Slot 0 carries the marker of bench_ring.c; the
 * input frames between two of its arrivals are one lap of the loop.  A block
 * that would land behind the playback pointer is counted as late and the
 * output is moved forward by `lead` again.
 *
 * One boot sweeps channels x variant x block x lead and prints one BENCH line
 * per run.  The same image runs in the A55, the A78 and, built for the smp
 * variant, the two-core AFE cells; on two cores the second processes half of
 * the channels of every block.
 */

#include <string.h>

#include <zephyr/cache.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/devicetree_regions.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/sys/printk.h>

#include <zephyr/drivers/audio/mt8188_afe.h>

#include "bench_dsp.h"
#include "bench_ring.h"

#define RATE        48000
#define RING_FRAMES 2048 /* per direction, 42.7 ms */
#define MAX_BLOCK   256
#define GUARD       8 /* frames a write must stay ahead of the playback pointer */
#define TIMEOUT     (RATE / 4) /* reinject a marker after 250 ms without one */

#define NR_CPUS     CONFIG_MP_MAX_NUM_CPUS

static const unsigned int channel_set[] = {2, 16, 32};
#ifdef CONFIG_BENCH_QUICK
static const unsigned int block_set[] = {32, 64};
static const unsigned int lead_set[] = {32, 64, 128};
#else
static const unsigned int block_set[] = {16, 32, 64, 128};
static const unsigned int lead_set[] = {16, 32, 64, 128, 256};
#endif

/*
 * AFE buffers, outside zephyr,sram (see the mtk-afe snippet).  The driver maps
 * the region non-cacheable, where reads are very slow.  With
 * CONFIG_BENCH_CACHED_BUFFERS, main() maps it again in place, cacheable (the
 * kernel maps devices one to one, so the address stays the same), and the
 * bench keeps the AFE, which does not snoop the caches, in step with explicit
 * maintenance: captured blocks are invalidated before they are read, played
 * blocks cleaned after they are written.
 */
static int32_t cap_ring[RING_FRAMES * BENCH_MAX_CHANNELS]
	Z_GENERIC_SECTION(LINKER_DT_NODE_REGION_NAME(DT_NODELABEL(dma_region))) __aligned(64);
static int32_t play_ring[RING_FRAMES * BENCH_MAX_CHANNELS]
	Z_GENERIC_SECTION(LINKER_DT_NODE_REGION_NAME(DT_NODELABEL(dma_region))) __aligned(64);

#define DMA_BASE DT_REG_ADDR(DT_NODELABEL(dma_region))
#define DMA_SIZE DT_REG_SIZE(DT_NODELABEL(dma_region))

/* Working blocks, cacheable. */
static int32_t in_blk[MAX_BLOCK * BENCH_MAX_CHANNELS] __aligned(64);
static int32_t out_blk[MAX_BLOCK * BENCH_MAX_CHANNELS] __aligned(64);

static struct bench_dsp dsp;
static struct bench_ring ring;
static const struct device *afe;

struct dma_pos {
	enum mt8188_memif_id id;
	uint32_t base;
	uint32_t bytes;
	uint32_t frame_bytes;
	uint32_t last_off;
	uint64_t wraps;
};

struct run_cfg {
	unsigned int ch;
	enum bench_variant var;
	unsigned int block;
	unsigned int lead;
};

struct run_res {
	uint64_t blocks;
	uint32_t late;
	uint32_t overrun;
	uint64_t proc_sum; /* cycles */
	uint32_t proc_max;
	uint64_t wall;     /* cycles of the measured interval */
	int64_t gap_sum;   /* playback minus capture DMA position, frames */
};

/* Frames the DMA has moved since the stream started, unwrapped. */
static uint64_t dma_frames(struct dma_pos *p)
{
	uint32_t off = mt8188_afe_get_cur(afe, p->id) - p->base;

	if (off < p->bytes) {
		if (off < p->last_off) {
			p->wraps++;
		}
		p->last_off = off;
	}
	return p->wraps * RING_FRAMES + p->last_off / p->frame_bytes;
}

static void copy_in(int32_t *dst, int32_t *src, size_t bytes)
{
	if (IS_ENABLED(CONFIG_BENCH_CACHED_BUFFERS)) {
		sys_cache_data_invd_range(src, bytes);
	}
	memcpy(dst, src, bytes);
}

static void copy_out(int32_t *dst, const int32_t *src, size_t bytes)
{
	memcpy(dst, src, bytes);
	if (IS_ENABLED(CONFIG_BENCH_CACHED_BUFFERS)) {
		sys_cache_data_flush_range(dst, bytes);
	}
}

static void ring_read(int32_t *dst, uint64_t frame, unsigned int frames, unsigned int ch)
{
	unsigned int at = frame % RING_FRAMES;
	unsigned int first = MIN(frames, RING_FRAMES - at);

	copy_in(dst, &cap_ring[at * ch], first * ch * sizeof(int32_t));
	copy_in(dst + first * ch, cap_ring, (frames - first) * ch * sizeof(int32_t));
}

static void ring_write(const int32_t *src, uint64_t frame, unsigned int frames, unsigned int ch)
{
	unsigned int at = frame % RING_FRAMES;
	unsigned int first = MIN(frames, RING_FRAMES - at);

	copy_out(&play_ring[at * ch], src, first * ch * sizeof(int32_t));
	copy_out(play_ring, src + first * ch, (frames - first) * ch * sizeof(int32_t));
}

static void ring_zero(uint64_t frame, unsigned int frames, unsigned int ch)
{
	while (frames--) {
		int32_t *f = &play_ring[(frame++ % RING_FRAMES) * ch];

		memset(f, 0, ch * sizeof(int32_t));
		if (IS_ENABLED(CONFIG_BENCH_CACHED_BUFFERS)) {
			sys_cache_data_flush_range(f, ch * sizeof(int32_t));
		}
	}
}

#if NR_CPUS > 1
/* Second core: processes the upper half of the channels of each block. */
static atomic_t job_seq;
static atomic_t done_seq;
static unsigned int job_frames, job_first, job_end;

static void worker(void *a, void *b, void *c)
{
	atomic_val_t seen = 0;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	while (1) {
		atomic_val_t seq = atomic_get(&job_seq);

		if (seq == seen) {
			continue;
		}
		seen = seq;
		bench_dsp_process(&dsp, in_blk, out_blk, job_frames, job_first, job_end);
		atomic_set(&done_seq, seq);
	}
}

/*
 * Started from main(): a cooperative thread that spins, started with the
 * other static threads, would hold CPU 0 before the second core is brought
 * up, and it never would be.
 */
K_THREAD_DEFINE(worker_tid, 2048, worker, NULL, NULL, NULL, K_PRIO_COOP(1), 0, SYS_FOREVER_MS);
#endif

static void process(unsigned int frames, unsigned int ch)
{
#if NR_CPUS > 1
	unsigned int half = ch / 2;

	job_frames = frames;
	job_first = half;
	job_end = ch;
	atomic_inc(&job_seq);
	bench_dsp_process(&dsp, in_blk, out_blk, frames, 0, half);
	while (atomic_get(&done_seq) != atomic_get(&job_seq)) {
	}
#else
	bench_dsp_process(&dsp, in_blk, out_blk, frames, 0, ch);
#endif
	bench_ring_block(&ring, in_blk, out_blk, frames);
}

static int afe_setup(unsigned int ch)
{
	const uint32_t bytes = RING_FRAMES * ch * sizeof(int32_t);
	struct mt8188_afe_cfg play = {
		.rate = RATE,
		.channels = ch,
		.word_size = 32,
		.fmt = MT8188_ETDM_FMT_DSPB,
		.data_mode = MT8188_ETDM_DATA_ONE_PIN,
		.mclk_freq = 256 * RATE,
		.mclk_dir = MT8188_MCLK_DIR_OUT,
		.period_frames = RING_FRAMES / 4,
	};
	struct mt8188_afe_cfg cap = {
		.rate = RATE,
		.channels = ch,
		.word_size = 32,
		.fmt = MT8188_ETDM_FMT_DSPB,
		.data_mode = MT8188_ETDM_DATA_ONE_PIN,
		.slave_mode = true,
		.mclk_freq = 0,
		.mclk_dir = MT8188_MCLK_DIR_IN,
		.period_frames = RING_FRAMES / 4,
	};
	const bool pair = ch > MT8188_ETDM_MAX_CHANNELS;
	int ret;

	ret = mt8188_afe_configure(afe, MT8188_DL11, &play);
	ret = ret ?: mt8188_afe_route(afe, MT8188_ROUTE_SRC_DL11,
				      pair ? MT8188_ROUTE_DST_ETDM_OUT1_OUT2
					   : MT8188_ROUTE_DST_ETDM_OUT1);
	ret = ret ?: mt8188_afe_set_buf(afe, MT8188_DL11, (uint32_t)(uintptr_t)play_ring, bytes);
	ret = ret ?: mt8188_afe_configure(afe, MT8188_UL9, &cap);
	ret = ret ?: mt8188_afe_route(afe,
				      pair ? MT8188_ROUTE_SRC_ETDM_IN1_IN2
					   : MT8188_ROUTE_SRC_ETDM_IN1,
				      MT8188_ROUTE_DST_UL9);
	ret = ret ?: mt8188_afe_set_buf(afe, MT8188_UL9, (uint32_t)(uintptr_t)cap_ring, bytes);
	return ret;
}

static int run(const struct run_cfg *c, struct run_res *r)
{
	const uint32_t frame_bytes = c->ch * sizeof(int32_t);
	const uint64_t hz = sys_clock_hw_cycles_per_sec();
	struct dma_pos cap = {MT8188_UL9, (uint32_t)(uintptr_t)cap_ring,
			      RING_FRAMES * frame_bytes, frame_bytes};
	struct dma_pos play = {MT8188_DL11, (uint32_t)(uintptr_t)play_ring,
			       RING_FRAMES * frame_bytes, frame_bytes};
	uint64_t t_start, t_measure, t_end;
	uint64_t in_next = 0, out_next = 0;
	int64_t delta = 0;
	bool synced = false, measuring = false;
	int ret;

	memset(r, 0, sizeof(*r));
	memset(cap_ring, 0, sizeof(cap_ring));
	memset(play_ring, 0, sizeof(play_ring));
	if (IS_ENABLED(CONFIG_BENCH_CACHED_BUFFERS)) {
		/* No dirty line may later land on top of what the AFE wrote. */
		sys_cache_data_flush_and_invd_range(cap_ring, sizeof(cap_ring));
		sys_cache_data_flush_range(play_ring, sizeof(play_ring));
	}
	bench_dsp_init(&dsp, c->var, c->ch, BIT(0));
	bench_ring_init(&ring, c->ch, 0, TIMEOUT);

	ret = afe_setup(c->ch);
	if (ret) {
		printk("AFE setup for %u ch failed: %d\n", c->ch, ret);
		return ret;
	}
	/* Capture first: eTDM_IN1 idles until playback drives the clocks. */
	ret = mt8188_afe_start(afe, MT8188_UL9);
	ret = ret ?: mt8188_afe_start(afe, MT8188_DL11);
	if (ret) {
		printk("AFE start failed: %d\n", ret);
		mt8188_afe_stop(afe, MT8188_UL9);
		return ret;
	}

	t_start = k_cycle_get_64();
	t_measure = t_start + hz * CONFIG_BENCH_WARMUP_MS / 1000;
	t_end = t_measure + hz * CONFIG_BENCH_RUN_MS / 1000;

	while (1) {
		uint64_t now = k_cycle_get_64();
		uint64_t avail;

		if (!measuring && now >= t_measure) {
			measuring = true;
			bench_ring_reset_stats(&ring);
			memset(r, 0, sizeof(*r));
		}
		if (now >= t_end) {
			r->wall = now - t_measure;
			break;
		}

		uint64_t cap_now = dma_frames(&cap);

		avail = cap_now - in_next;
		if (avail > RING_FRAMES - c->block) {
			/* Capture lapped us: drop what was overwritten. */
			r->overrun++;
			in_next += avail - c->block;
			avail = c->block;
		}
		if (avail < c->block) {
			continue;
		}

		uint64_t t0 = k_cycle_get_64();
		uint64_t p, out;

		ring_read(in_blk, in_next, c->block, c->ch);
		process(c->block, c->ch);

		p = dma_frames(&play);
		r->gap_sum += (int64_t)p - (int64_t)cap_now;
		out = in_next + delta;
		if (!synced || out < p + GUARD) {
			if (synced) {
				r->late++;
			}
			delta = (int64_t)(p + c->lead) - (int64_t)in_next;
			out = in_next + delta;
			/* Silence what was skipped, so no stale marker replays. */
			if (synced && out > out_next) {
				ring_zero(out_next, MIN(out - out_next, RING_FRAMES), c->ch);
			}
			synced = true;
		}
		ring_write(out_blk, out, c->block, c->ch);
		out_next = out + c->block;
		in_next += c->block;

		uint32_t dt = (uint32_t)(k_cycle_get_64() - t0);

		r->blocks++;
		r->proc_sum += dt;
		if (dt > r->proc_max) {
			r->proc_max = dt;
		}
	}

	/* Slave first, while its clocks still run. */
	mt8188_afe_stop(afe, MT8188_UL9);
	mt8188_afe_stop(afe, MT8188_DL11);
	return 0;
}

/*
 * Copy rates between the AFE buffers and cacheable RAM, printed once at
 * start: the AFE buffers are non-cacheable, and the copy in and out of them
 * is part of every block's processing time.
 */
static void copy_rates(void)
{
	static int32_t scratch[ARRAY_SIZE(in_blk)] __aligned(64);
	const size_t bytes = sizeof(scratch);
	const uint64_t hz = sys_clock_hw_cycles_per_sec();
	struct {
		const char *what;
		void *dst;
		const void *src;
	} t[] = {
		{"RAM -> RAM", scratch, in_blk},
		{"AFE buffer -> RAM", scratch, cap_ring},
		{"RAM -> AFE buffer", play_ring, scratch},
	};

	ARRAY_FOR_EACH(t, i) {
		uint64_t c0 = k_cycle_get_64();

		if (t[i].dst == scratch) {
			copy_in(t[i].dst, (int32_t *)t[i].src, bytes);
		} else {
			copy_out(t[i].dst, t[i].src, bytes);
		}
		uint64_t ns = (k_cycle_get_64() - c0) * 1000000000ULL / hz;

		printk("copy %-18s %u KiB in %llu us: %llu MB/s\n", t[i].what,
		       (unsigned int)(bytes / 1024), ns / 1000, ns ? bytes * 1000ULL / ns : 0);
	}
}

static const char *cpu_name(void)
{
	uint64_t midr;

	__asm__ volatile("mrs %0, midr_el1" : "=r"(midr));
	switch ((midr >> 4) & 0xfff) {
	case 0xd05:
		return "a55";
	case 0xd41:
		return "a78";
	default:
		return "unknown";
	}
}

static void report(const struct run_cfg *c, const struct run_res *r)
{
	const uint64_t hz = sys_clock_hw_cycles_per_sec();
	const uint32_t secs_ms = (uint32_t)(r->wall * 1000 / hz);
	const uint32_t avg_lap = ring.laps ? (uint32_t)(ring.lap_sum * 10 / ring.laps) : 0;
	const uint32_t proc_avg_ns = r->blocks ? (uint32_t)(r->proc_sum * 1000000000ULL / hz / r->blocks) : 0;
	const uint32_t proc_max_ns = (uint32_t)((uint64_t)r->proc_max * 1000000000ULL / hz);
	const uint32_t load_pm = r->wall ? (uint32_t)(r->proc_sum * 1000 / r->wall) : 0;
	const int32_t hw_gap = r->blocks ? (int32_t)(r->gap_sum / (int64_t)r->blocks) : 0;

	printk("BENCH os=zephyr cpu=%s cores=%u mem=%s ch=%u var=%s block=%u lead=%u ms=%u "
	       "laps=%u lap_min=%u lap_avg=%u.%u lap_max=%u lost=%u injects=%u late=%u "
	       "overrun=%u proc_avg_ns=%u proc_max_ns=%u load=%u.%u%% hw_gap=%d\n",
	       cpu_name(), NR_CPUS, IS_ENABLED(CONFIG_BENCH_CACHED_BUFFERS) ? "wb" : "nc", c->ch,
	       bench_variant_name(c->var), c->block, c->lead, secs_ms,
	       ring.laps, ring.laps ? ring.lap_min : 0, avg_lap / 10, avg_lap % 10, ring.lap_max,
	       ring.lost, ring.injects, r->late, r->overrun, proc_avg_ns, proc_max_ns,
	       load_pm / 10, load_pm % 10, hw_gap);
}

int main(void)
{
	struct run_cfg c;
	struct run_res r;

	afe = DEVICE_DT_GET(DT_NODELABEL(afe));
	if (!device_is_ready(afe)) {
		printk("AFE device not ready\n");
		return -ENODEV;
	}

	if (IS_ENABLED(CONFIG_BENCH_CACHED_BUFFERS)) {
		mm_reg_t va;

		device_unmap((mm_reg_t)DMA_BASE, DMA_SIZE);
		device_map(&va, DMA_BASE, DMA_SIZE, K_MEM_CACHE_WB);
		if (va != DMA_BASE) {
			printk("cacheable buffer map landed at 0x%lx, not 0x%lx\n", (unsigned long)va,
			       (unsigned long)DMA_BASE);
			return -EFAULT;
		}
	}

	printk("\neTDM loop-latency bench: %s, %u core(s), %u Hz, ring %u frames, AFE buffers %s\n",
	       cpu_name(), NR_CPUS, RATE, RING_FRAMES,
	       IS_ENABLED(CONFIG_BENCH_CACHED_BUFFERS) ? "cached" : "non-cacheable");
	for (int v = 0; v < BENCH_VARIANT_NR; v++) {
		printk("DSP selftest %s: 2ch %08x 16ch %08x 32ch %08x\n", bench_variant_name(v),
		       bench_dsp_selftest(v, 2), bench_dsp_selftest(v, 16),
		       bench_dsp_selftest(v, 32));
	}

	copy_rates();
#if NR_CPUS > 1
	k_thread_start(worker_tid);
#endif

	ARRAY_FOR_EACH(channel_set, i) {
		c.ch = channel_set[i];
		for (c.var = 0; c.var < BENCH_VARIANT_NR; c.var++) {
			ARRAY_FOR_EACH(block_set, j) {
				c.block = block_set[j];
				ARRAY_FOR_EACH(lead_set, k) {
					c.lead = lead_set[k];
					if (run(&c, &r) == 0) {
						report(&c, &r);
					}
				}
			}
		}
	}
	printk("BENCH done\n");
	return 0;
}
