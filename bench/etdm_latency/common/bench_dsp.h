/*
 * Copyright (c) 2026 MediaTek Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Processing stage of the eTDM latency bench, shared verbatim by the Zephyr
 * and the Linux application so both run exactly the same code.
 *
 * Samples are signed Q31 in interleaved frames.  All arithmetic is integer, so
 * a given input produces the same output on every target and compiler; the
 * self-test checksum is how that is checked across them.
 */

#ifndef BENCH_DSP_H_
#define BENCH_DSP_H_

#include <stdint.h>

#include "bench_coeffs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BENCH_MAX_CHANNELS 32

enum bench_variant {
	BENCH_PASS, /* copy input to output */
	BENCH_FIR,  /* BENCH_FIR_TAPS-tap FIR per channel */
	BENCH_IIR,  /* BENCH_IIR_SECTIONS biquads per channel */
	BENCH_VARIANT_NR,
};

struct bench_dsp {
	enum bench_variant variant;
	unsigned int channels;
	/* Channels copied unprocessed whatever the variant, e.g. the marker. */
	uint32_t pass_mask;
	/*
	 * Per-channel state, so disjoint channel ranges of one block can be
	 * processed concurrently.  The FIR history is stored twice over so a
	 * tap walk never wraps.
	 */
	unsigned int fir_pos[BENCH_MAX_CHANNELS];
	int32_t fir_hist[BENCH_MAX_CHANNELS][2 * BENCH_FIR_TAPS];
	int32_t iir_state[BENCH_MAX_CHANNELS][BENCH_IIR_SECTIONS][4];
};

/** Clear all state and select a variant.  channels <= BENCH_MAX_CHANNELS. */
void bench_dsp_init(struct bench_dsp *d, enum bench_variant variant, unsigned int channels,
		    uint32_t pass_mask);

/**
 * Process @p frames frames of channels [ch_first, ch_end) from @p in to @p out.
 * Both are interleaved with d->channels samples per frame and may not overlap.
 */
void bench_dsp_process(struct bench_dsp *d, const int32_t *in, int32_t *out, unsigned int frames,
		       unsigned int ch_first, unsigned int ch_end);

/**
 * Run a fixed pseudo-random input through a fresh instance of @p variant and
 * return a checksum of the output.  Equal on every target if the code is.
 */
uint32_t bench_dsp_selftest(enum bench_variant variant, unsigned int channels);

const char *bench_variant_name(enum bench_variant variant);

#ifdef __cplusplus
}
#endif

#endif /* BENCH_DSP_H_ */
