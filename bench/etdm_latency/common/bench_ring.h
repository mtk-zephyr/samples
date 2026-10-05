/*
 * Copyright (c) 2026 MediaTek Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Loop-latency measurement of the eTDM latency bench, shared by the Zephyr and
 * the Linux application.
 *
 * The bench closes a ring: OUT -> loopback wire -> IN -> processing -> OUT.
 * One slot carries a marker word.  Whenever the marker arrives on the input it
 * is written straight back to the output at the same position in the block,
 * with its sequence number advanced, so it keeps going round.  The number of
 * input frames between two consecutive arrivals is the latency of one lap:
 * capture buffering, processing, playback buffering and the wire.
 *
 * The other slots carry the processed audio; the marker slot is never
 * filtered, so a filter adds its processing time to the lap but not its
 * group delay.
 */

#ifndef BENCH_RING_H_
#define BENCH_RING_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BENCH_MARK_MAGIC 0x5aa50000U
#define BENCH_MARK_MASK  0xffff0000U

struct bench_ring {
	unsigned int channels;
	unsigned int out_ch;   /* slot the marker is played on */
	int in_ch;             /* slot it comes back on, -1 until seen */
	uint32_t timeout;      /* input frames without a marker before reinjecting */
	uint64_t in_frames;    /* input frames consumed */
	uint64_t last_arrival; /* input frame of the last marker or injection */
	int have_last;         /* last_seq/last_arrival describe a real arrival */
	uint16_t last_seq;
	uint16_t next_inject;
	int started;           /* a marker has been injected since init */

	/* Statistics since bench_ring_reset_stats(). */
	uint32_t laps;
	uint32_t lost; /* arrivals whose sequence number skipped */
	uint32_t injects;
	uint32_t lap_min;
	uint32_t lap_max;
	uint64_t lap_sum;
};

void bench_ring_init(struct bench_ring *r, unsigned int channels, unsigned int out_ch,
		     uint32_t timeout);
void bench_ring_reset_stats(struct bench_ring *r);

/**
 * Account one block of @p frames input frames and write the marker slot of the
 * corresponding output block.  @p in and @p out are interleaved with
 * r->channels samples per frame; every other slot of @p out is left alone.
 */
void bench_ring_block(struct bench_ring *r, const int32_t *in, int32_t *out, unsigned int frames);

#ifdef __cplusplus
}
#endif

#endif /* BENCH_RING_H_ */
