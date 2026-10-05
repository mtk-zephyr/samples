/*
 * Copyright (c) 2026 MediaTek Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "bench_dsp.h"

static inline int32_t sat32(int64_t v)
{
	if (v > INT32_MAX) {
		return INT32_MAX;
	}
	if (v < INT32_MIN) {
		return INT32_MIN;
	}
	return (int32_t)v;
}

/* Round a product sum down by @p shift bits and saturate. */
static inline int32_t round_shift(int64_t acc, unsigned int shift)
{
	return sat32((acc + ((int64_t)1 << (shift - 1))) >> shift);
}

void bench_dsp_init(struct bench_dsp *d, enum bench_variant variant, unsigned int channels,
		    uint32_t pass_mask)
{
	memset(d, 0, sizeof(*d));
	d->variant = variant;
	d->channels = channels;
	d->pass_mask = pass_mask;
}

static int32_t fir_step(struct bench_dsp *d, unsigned int ch, int32_t x)
{
	unsigned int pos = d->fir_pos[ch];
	int32_t *h;
	int64_t acc = 0;

	pos = (pos == 0) ? BENCH_FIR_TAPS - 1 : pos - 1;
	d->fir_pos[ch] = pos;
	h = &d->fir_hist[ch][pos];
	h[0] = x;
	h[BENCH_FIR_TAPS] = x;

	/* h[k] is x[n - k]. */
	for (unsigned int k = 0; k < BENCH_FIR_TAPS; k++) {
		acc += (int64_t)bench_fir_coeffs[k] * h[k];
	}

	return round_shift(acc, 31);
}

static int32_t iir_step(struct bench_dsp *d, unsigned int ch, int32_t x)
{
	x = round_shift((int64_t)x * BENCH_IIR_GAIN_Q31, 31);

	for (unsigned int s = 0; s < BENCH_IIR_SECTIONS; s++) {
		const int32_t *c = bench_iir_coeffs[s];
		int32_t *st = d->iir_state[ch][s]; /* x1 x2 y1 y2 */
		int64_t acc;
		int32_t y;

		acc = (int64_t)c[0] * x + (int64_t)c[1] * st[0] + (int64_t)c[2] * st[1] -
		      (int64_t)c[3] * st[2] - (int64_t)c[4] * st[3];
		y = round_shift(acc, 30);

		st[1] = st[0];
		st[0] = x;
		st[3] = st[2];
		st[2] = y;
		x = y;
	}

	return x;
}

void bench_dsp_process(struct bench_dsp *d, const int32_t *in, int32_t *out, unsigned int frames,
		       unsigned int ch_first, unsigned int ch_end)
{
	const unsigned int stride = d->channels;

	for (unsigned int ch = ch_first; ch < ch_end; ch++) {
		const int32_t *src = in + ch;
		int32_t *dst = out + ch;
		enum bench_variant v = (d->pass_mask & (1U << ch)) ? BENCH_PASS : d->variant;

		switch (v) {
		case BENCH_FIR:
			for (unsigned int f = 0; f < frames; f++) {
				dst[f * stride] = fir_step(d, ch, src[f * stride]);
			}
			break;
		case BENCH_IIR:
			for (unsigned int f = 0; f < frames; f++) {
				dst[f * stride] = iir_step(d, ch, src[f * stride]);
			}
			break;
		default:
			for (unsigned int f = 0; f < frames; f++) {
				dst[f * stride] = src[f * stride];
			}
			break;
		}
	}
}

uint32_t bench_dsp_selftest(enum bench_variant variant, unsigned int channels)
{
	static struct bench_dsp d;
	enum { BLOCK = 64, BLOCKS = 32 };
	static int32_t in[BLOCK * BENCH_MAX_CHANNELS];
	static int32_t out[BLOCK * BENCH_MAX_CHANNELS];
	uint32_t lcg = 0x12345678U;
	uint32_t sum = 0x811c9dc5U; /* FNV-1a */

	bench_dsp_init(&d, variant, channels, 0);

	for (unsigned int b = 0; b < BLOCKS; b++) {
		for (unsigned int i = 0; i < BLOCK * channels; i++) {
			lcg = lcg * 1664525U + 1013904223U;
			in[i] = (int32_t)lcg;
		}
		bench_dsp_process(&d, in, out, BLOCK, 0, channels);
		for (unsigned int i = 0; i < BLOCK * channels; i++) {
			uint32_t v = (uint32_t)out[i];

			for (unsigned int byte = 0; byte < 4; byte++) {
				sum = (sum ^ ((v >> (8 * byte)) & 0xffU)) * 16777619U;
			}
		}
	}

	return sum;
}

const char *bench_variant_name(enum bench_variant variant)
{
	switch (variant) {
	case BENCH_PASS:
		return "pass";
	case BENCH_FIR:
		return "fir";
	case BENCH_IIR:
		return "iir";
	default:
		return "?";
	}
}
