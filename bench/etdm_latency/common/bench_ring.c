/*
 * Copyright (c) 2026 MediaTek Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "bench_ring.h"

static inline int is_marker(int32_t v)
{
	return ((uint32_t)v & BENCH_MARK_MASK) == BENCH_MARK_MAGIC;
}

void bench_ring_reset_stats(struct bench_ring *r)
{
	r->laps = 0;
	r->lost = 0;
	r->injects = 0;
	r->lap_min = UINT32_MAX;
	r->lap_max = 0;
	r->lap_sum = 0;
}

void bench_ring_init(struct bench_ring *r, unsigned int channels, unsigned int out_ch,
		     uint32_t timeout)
{
	memset(r, 0, sizeof(*r));
	r->channels = channels;
	r->out_ch = out_ch;
	r->in_ch = -1;
	r->timeout = timeout;
	r->next_inject = 0x1000;
	bench_ring_reset_stats(r);
}

void bench_ring_block(struct bench_ring *r, const int32_t *in, int32_t *out, unsigned int frames)
{
	const unsigned int stride = r->channels;
	const uint64_t block_start = r->in_frames;

	for (unsigned int f = 0; f < frames; f++) {
		const int32_t *fr = &in[f * stride];
		int32_t mark = 0;

		if (r->in_ch < 0) {
			/* Not seen yet: the capture may rotate slots, so look in all. */
			for (unsigned int s = 0; s < stride; s++) {
				if (is_marker(fr[s])) {
					r->in_ch = (int)s;
					break;
				}
			}
		}

		if (r->in_ch >= 0 && is_marker(fr[r->in_ch])) {
			uint16_t seq = (uint16_t)fr[r->in_ch];
			uint64_t now = r->in_frames + f;

			if (r->have_last && seq == (uint16_t)(r->last_seq + 1)) {
				uint32_t lap = (uint32_t)(now - r->last_arrival);

				r->laps++;
				r->lap_sum += lap;
				if (lap < r->lap_min) {
					r->lap_min = lap;
				}
				if (lap > r->lap_max) {
					r->lap_max = lap;
				}
			} else if (r->have_last) {
				r->lost++;
			}
			r->have_last = 1;
			r->last_seq = seq;
			r->last_arrival = now;
			mark = (int32_t)(BENCH_MARK_MAGIC | (uint16_t)(seq + 1));
		}

		out[f * stride + r->out_ch] = mark;
	}
	r->in_frames += frames;

	/*
	 * First block, or nothing has come round for too long (a glitch ate
	 * the marker): put a fresh one on the first frame of this output block.
	 * The lap it starts is not counted, since it did not begin with an
	 * arrival.
	 */
	if (!r->started || r->in_frames - r->last_arrival > r->timeout) {
		out[r->out_ch] = (int32_t)(BENCH_MARK_MAGIC | r->next_inject);
		r->next_inject += 0x1000;
		r->injects++;
		r->started = 1;
		r->have_last = 0;
		r->last_arrival = block_start;
	}
}
