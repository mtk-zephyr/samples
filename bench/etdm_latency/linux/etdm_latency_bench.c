/*
 * Copyright (c) 2026 MediaTek Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * eTDM loop-latency bench, Linux side: the counterpart of the Zephyr
 * application in ../zephyr, using the same processing (../common/bench_dsp.c)
 * and the same lap measurement (../common/bench_ring.c).
 *
 *   DL11 (hw:0,6) -> eTDM_OUT1 (+OUT2) -> wires -> eTDM_IN1 (+IN2) -> CM0 -> UL9 (hw:0,14)
 *          ^                                                                  |
 *          +------------------ block processing (pass | FIR | IIR) <----------+
 *
 * Capture and playback are linked and run with period = block.  Playback
 * starts with enough silence queued to cover the DMA prefetch and the first
 * capture block.  Before the first processed block is queued, the playback
 * queue is rewound or padded to exactly `lead` frames ahead of the playback
 * DMA pointer, which is where the Zephyr side writes its blocks; from then on
 * both streams run off one clock and the queue stays there.  An xrun is counted as late, and the streams are
 * restarted.
 *
 * The processing time of a block runs from the moment a whole block is
 * available to the moment it is queued for playback: readi() (the copy out of
 * the DMA buffer), processing, and writei() (the copy in).  That is what the
 * Zephyr side times too.
 *
 * Without -S it runs one configuration; with -S it sweeps the variants,
 * blocks and leads of the Zephyr image at the channel count given by -c (the
 * mixer routes differ per channel count, so they are set between sweeps; see
 * ../tools/run_linux.sh).  Each run prints one BENCH line in the Zephyr format.
 *
 *   etdm_latency_bench [-c ch] [-v pass|fir|iir] [-b block] [-l lead] [-t secs]
 *                      [-w warmup_ms] [-a cpu] [-r rtprio] [-S] [-D dev] [-C dev]
 */

#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#include <alsa/asoundlib.h>

#include "bench_dsp.h"
#include "bench_ring.h"

#define RATE        48000
#define RING_FRAMES 2048 /* ALSA buffer per direction, as on Zephyr */
#define MAX_BLOCK   256
#define TIMEOUT     (RATE / 4)
/*
 * The DL11 playback DMA prefetches about this much as soon as it starts, so
 * ALSA's hardware pointer jumps ahead by it.  The start-up prefill covers it,
 * two blocks and the lead, with room to spare.
 */
#define PREFETCH_BYTES 4096
#define PREFILL_SPARE  64

static const unsigned int block_set[] = {16, 32, 64, 128};
static const unsigned int lead_set[] = {16, 32, 64, 128, 256};

struct run_cfg {
	unsigned int ch;
	enum bench_variant var;
	unsigned int block;
	unsigned int lead;
};

struct run_res {
	uint64_t blocks;
	uint32_t late;    /* xruns */
	uint32_t overrun; /* capture xruns among them */
	uint64_t proc_sum; /* ns */
	uint64_t proc_max;
	uint64_t wall;     /* ns */
};

static snd_pcm_uframes_t play_buffer; /* frames, as configured */
static const char *play_dev = "hw:0,6";
static const char *cap_dev = "hw:0,14";
static unsigned int run_ms = 3000, warmup_ms = 500;
static int cpu = -1;

static struct bench_dsp dsp;
static struct bench_ring ring;
static int32_t in_blk[MAX_BLOCK * BENCH_MAX_CHANNELS];
static int32_t out_blk[MAX_BLOCK * BENCH_MAX_CHANNELS];
static int32_t zeros[RING_FRAMES * BENCH_MAX_CHANNELS];

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static int setup(snd_pcm_t *pcm, const struct run_cfg *c)
{
	snd_pcm_hw_params_t *hw;
	snd_pcm_sw_params_t *sw;
	snd_pcm_uframes_t period = c->block, buffer = RING_FRAMES, boundary;
	unsigned int rate = RATE;
	int err;

	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_sw_params_alloca(&sw);
	if ((err = snd_pcm_hw_params_any(pcm, hw)) < 0 ||
	    (err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
	    (err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S32_LE)) < 0 ||
	    (err = snd_pcm_hw_params_set_channels(pcm, hw, c->ch)) < 0 ||
	    (err = snd_pcm_hw_params_set_rate(pcm, hw, rate, 0)) < 0 ||
	    (err = snd_pcm_hw_params_set_period_size(pcm, hw, period, 0)) < 0 ||
	    (err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer)) < 0 ||
	    (err = snd_pcm_hw_params(pcm, hw)) < 0) {
		fprintf(stderr, "%s: hw params (%u ch, block %u): %s\n", snd_pcm_name(pcm), c->ch,
			c->block, snd_strerror(err));
		return err;
	}
	if (snd_pcm_stream(pcm) == SND_PCM_STREAM_PLAYBACK) {
		play_buffer = buffer;
	}
	if ((err = snd_pcm_sw_params_current(pcm, sw)) < 0 ||
	    (err = snd_pcm_sw_params_get_boundary(sw, &boundary)) < 0 ||
	    /* Started by snd_pcm_start() only, never by filling. */
	    (err = snd_pcm_sw_params_set_start_threshold(pcm, sw, boundary)) < 0 ||
	    (err = snd_pcm_sw_params_set_avail_min(pcm, sw, c->block)) < 0 ||
	    (err = snd_pcm_sw_params(pcm, sw)) < 0) {
		fprintf(stderr, "%s: sw params: %s\n", snd_pcm_name(pcm), snd_strerror(err));
		return err;
	}
	return 0;
}

/*
 * Make the playback queue exactly `lead` frames, measured from the playback DMA
 * pointer, before the first block after a start is queued.
 */
static int align_lead(snd_pcm_t *play, const struct run_cfg *c)
{
	snd_pcm_sframes_t avail = snd_pcm_avail(play);
	snd_pcm_sframes_t queued, n = 0;

	if (avail < 0) {
		return avail;
	}
	queued = (snd_pcm_sframes_t)play_buffer - avail;
	if (queued > (snd_pcm_sframes_t)c->lead) {
		n = snd_pcm_rewind(play, queued - c->lead);
	} else if (queued < (snd_pcm_sframes_t)c->lead) {
		n = snd_pcm_writei(play, zeros, c->lead - queued);
	}
	return n < 0 ? (int)n : 0;
}

/* Queue enough silence for the start-up and start both linked streams. */
static int start(snd_pcm_t *play, snd_pcm_t *cap, const struct run_cfg *c)
{
	snd_pcm_uframes_t prefill = PREFETCH_BYTES / (c->ch * sizeof(int32_t)) + 2 * c->block +
				    c->lead + PREFILL_SPARE;
	int err;

	if ((err = snd_pcm_prepare(play)) < 0 || (err = snd_pcm_prepare(cap)) < 0) {
		return err;
	}
	if ((err = snd_pcm_writei(play, zeros, prefill)) < 0) {
		return err;
	}
	return snd_pcm_start(cap); /* linked: starts playback too */
}

static int run(const struct run_cfg *c, struct run_res *r)
{
	snd_pcm_t *play = NULL, *cap = NULL;
	uint64_t t_start, t_measure, t_end;
	int measuring = 0, aligned = 0, err;

	memset(r, 0, sizeof(*r));
	bench_dsp_init(&dsp, c->var, c->ch, 1U << 0);
	bench_ring_init(&ring, c->ch, 0, TIMEOUT);

	if ((err = snd_pcm_open(&play, play_dev, SND_PCM_STREAM_PLAYBACK, 0)) < 0 ||
	    (err = snd_pcm_open(&cap, cap_dev, SND_PCM_STREAM_CAPTURE, 0)) < 0) {
		fprintf(stderr, "open: %s\n", snd_strerror(err));
		goto out;
	}
	if ((err = setup(play, c)) < 0 || (err = setup(cap, c)) < 0) {
		goto out;
	}
	if ((err = snd_pcm_link(cap, play)) < 0) {
		fprintf(stderr, "link: %s\n", snd_strerror(err));
		goto out;
	}
	if ((err = start(play, cap, c)) < 0) {
		fprintf(stderr, "start: %s\n", snd_strerror(err));
		goto out;
	}

	t_start = now_ns();
	t_measure = t_start + (uint64_t)warmup_ms * 1000000ULL;
	t_end = t_measure + (uint64_t)run_ms * 1000000ULL;

	while (1) {
		uint64_t now = now_ns(), t0, dt;
		snd_pcm_sframes_t n;

		if (!measuring && now >= t_measure) {
			measuring = 1;
			bench_ring_reset_stats(&ring);
			memset(r, 0, sizeof(*r));
		}
		if (now >= t_end) {
			r->wall = now - t_measure;
			break;
		}

		/* Wait for a whole block, so the timing below excludes the wait. */
		n = snd_pcm_wait(cap, 1000);
		if (n >= 0) {
			n = snd_pcm_avail_update(cap);
		}
		if (n >= 0 && n < (snd_pcm_sframes_t)c->block) {
			continue;
		}
		t0 = now_ns();
		if (n >= 0) {
			n = snd_pcm_readi(cap, in_blk, c->block);
		}
		if (n == -EPIPE) {
			r->late++;
			r->overrun++;
			snd_pcm_drop(cap);
			if ((err = start(play, cap, c)) < 0) {
				goto out;
			}
			aligned = 0;
			continue;
		}
		if (n < 0) {
			fprintf(stderr, "capture: %s\n", snd_strerror(n));
			err = n;
			goto out;
		}

		bench_dsp_process(&dsp, in_blk, out_blk, n, 0, c->ch);
		bench_ring_block(&ring, in_blk, out_blk, n);

		if (!aligned) {
			if ((err = align_lead(play, c)) < 0 && err != -EPIPE) {
				fprintf(stderr, "align: %s\n", snd_strerror(err));
				goto out;
			}
			aligned = 1;
		}
		n = snd_pcm_writei(play, out_blk, n);
		dt = now_ns() - t0;
		if (n == -EPIPE) {
			r->late++;
			snd_pcm_drop(cap);
			if ((err = start(play, cap, c)) < 0) {
				goto out;
			}
			aligned = 0;
			continue;
		}
		if (n < 0) {
			fprintf(stderr, "playback: %s\n", snd_strerror(n));
			err = n;
			goto out;
		}

		r->blocks++;
		r->proc_sum += dt;
		if (dt > r->proc_max) {
			r->proc_max = dt;
		}
	}
	err = 0;
out:
	if (cap) {
		snd_pcm_drop(cap);
		snd_pcm_unlink(cap);
		snd_pcm_close(cap);
	}
	if (play) {
		snd_pcm_drop(play);
		snd_pcm_close(play);
	}
	return err;
}

static const char *cpu_name(void)
{
	char path[96];
	unsigned long long midr = 0;
	int c = cpu >= 0 ? cpu : sched_getcpu();
	FILE *f;

	snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/regs/identification/midr_el1",
		 c);
	f = fopen(path, "r");
	if (f) {
		if (fscanf(f, "%llx", &midr) != 1) {
			midr = 0;
		}
		fclose(f);
	}
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
	const uint32_t avg_lap = ring.laps ? (uint32_t)(ring.lap_sum * 10 / ring.laps) : 0;
	const uint32_t load_pm = r->wall ? (uint32_t)(r->proc_sum * 1000 / r->wall) : 0;

	printf("BENCH os=linux cpu=%s cores=1 ch=%u var=%s block=%u lead=%u ms=%llu "
	       "laps=%u lap_min=%u lap_avg=%u.%u lap_max=%u lost=%u injects=%u late=%u "
	       "overrun=%u proc_avg_ns=%llu proc_max_ns=%llu load=%u.%u%%\n",
	       cpu_name(), c->ch, bench_variant_name(c->var), c->block, c->lead,
	       (unsigned long long)(r->wall / 1000000), ring.laps, ring.laps ? ring.lap_min : 0,
	       avg_lap / 10, avg_lap % 10, ring.lap_max, ring.lost, ring.injects, r->late,
	       r->overrun, (unsigned long long)(r->blocks ? r->proc_sum / r->blocks : 0),
	       (unsigned long long)r->proc_max, load_pm / 10, load_pm % 10);
	fflush(stdout);
}

static int parse_variant(const char *s)
{
	for (int v = 0; v < BENCH_VARIANT_NR; v++) {
		if (!strcmp(s, bench_variant_name(v))) {
			return v;
		}
	}
	return -1;
}

int main(int argc, char **argv)
{
	struct run_cfg c = {.ch = 16, .var = BENCH_PASS, .block = 32, .lead = 64};
	struct run_res r;
	int sweep = 0, rtprio = 0, opt, v;

	while ((opt = getopt(argc, argv, "c:v:b:l:t:w:a:r:SD:C:h")) != -1) {
		switch (opt) {
		case 'c':
			c.ch = strtoul(optarg, NULL, 0);
			break;
		case 'v':
			v = parse_variant(optarg);
			if (v < 0) {
				fprintf(stderr, "variant: pass, fir or iir\n");
				return 2;
			}
			c.var = v;
			break;
		case 'b':
			c.block = strtoul(optarg, NULL, 0);
			break;
		case 'l':
			c.lead = strtoul(optarg, NULL, 0);
			break;
		case 't':
			run_ms = strtoul(optarg, NULL, 0) * 1000;
			break;
		case 'w':
			warmup_ms = strtoul(optarg, NULL, 0);
			break;
		case 'a':
			cpu = strtol(optarg, NULL, 0);
			break;
		case 'r':
			rtprio = strtol(optarg, NULL, 0);
			break;
		case 'S':
			sweep = 1;
			break;
		case 'D':
			play_dev = optarg;
			break;
		case 'C':
			cap_dev = optarg;
			break;
		default:
			fprintf(stderr,
				"usage: %s [-c ch] [-v pass|fir|iir] [-b block] [-l lead] [-t secs] "
				"[-w warmup_ms] [-a cpu] [-r rtprio] [-S] [-D dev] [-C dev]\n",
				argv[0]);
			return opt == 'h' ? 0 : 2;
		}
	}
	if (c.ch < 1 || c.ch > BENCH_MAX_CHANNELS || c.block < 1 || c.block > MAX_BLOCK ||
	    c.lead < 1 || c.lead > RING_FRAMES / 2) {
		/* Half the buffer: at 2 ch the prefetch takes another quarter. */
		fprintf(stderr, "out of range: ch 1..%d, block 1..%d, lead 1..%d\n",
			BENCH_MAX_CHANNELS, MAX_BLOCK, RING_FRAMES / 2);
		return 2;
	}

	if (cpu >= 0) {
		cpu_set_t set;

		CPU_ZERO(&set);
		CPU_SET(cpu, &set);
		if (sched_setaffinity(0, sizeof(set), &set) < 0) {
			perror("sched_setaffinity");
			return 1;
		}
	}
	if (rtprio > 0) {
		struct sched_param sp = {.sched_priority = rtprio};

		if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0) {
			perror("sched_setscheduler");
			return 1;
		}
	}
	if (mlockall(MCL_CURRENT | MCL_FUTURE) < 0) {
		perror("mlockall");
	}

	printf("eTDM loop-latency bench: linux, %s (cpu %d), %u Hz, ring %u frames, rtprio %d\n",
	       cpu_name(), cpu >= 0 ? cpu : sched_getcpu(), RATE, RING_FRAMES, rtprio);
	for (v = 0; v < BENCH_VARIANT_NR; v++) {
		printf("DSP selftest %s: 2ch %08x 16ch %08x 32ch %08x\n", bench_variant_name(v),
		       bench_dsp_selftest(v, 2), bench_dsp_selftest(v, 16),
		       bench_dsp_selftest(v, 32));
	}

	if (!sweep) {
		if (run(&c, &r) < 0) {
			return 1;
		}
		report(&c, &r);
		return 0;
	}

	for (v = 0; v < BENCH_VARIANT_NR; v++) {
		c.var = v;
		for (unsigned int j = 0; j < sizeof(block_set) / sizeof(block_set[0]); j++) {
			c.block = block_set[j];
			for (unsigned int k = 0; k < sizeof(lead_set) / sizeof(lead_set[0]); k++) {
				c.lead = lead_set[k];
				if (run(&c, &r) == 0) {
					report(&c, &r);
				}
			}
		}
	}
	printf("BENCH done\n");
	return 0;
}
