/*
 * Copyright (c) 2026 MediaTek Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * eTDM loopback check for Linux on the Genio EVKs: the Linux counterpart of
 * the Zephyr samples/audio/loopback_* samples, used to bring the Linux path
 * up before benchmarking it.
 *
 * Plays a running pattern on one PCM and checks every frame captured on
 * another, through the loopback wires on the board:
 *
 *   up to 16 ch: DL11 (hw:0,6) -> eTDM_OUT1 -> wires -> eTDM_IN1 -> CM0 -> UL9 (hw:0,14)
 *   32 ch:       DL11 (hw:0,6) -> eTDM_OUT1+OUT2 -> wires -> eTDM_IN1+IN2 -> CM0 -> UL9
 *
 * UL9 at every channel count: Linux's UL8 takes only two channels from
 * eTDM_IN1.
 *
 * The sample word is the one the Zephyr samples use: channel tag in the top
 * byte, running frame counter below.  The first tagged frame fixes the slot
 * rotation and the frame lag; every later frame must match exactly.
 *
 *   etdm_loopback_test [-c channels] [-D playback] [-C capture] [-p period] [-n periods] [-t secs]
 *
 * Defaults: 16 ch, playback hw:0,6, capture hw:0,14, period 480, 4 periods, 10 s.
 */

#include <errno.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <alsa/asoundlib.h>

#define RATE           48000
#define CH_TAG_SHIFT   24
#define CNT_MASK       0x00ffffffU
#define PATTERN(g, ch) (((uint32_t)(ch) << CH_TAG_SHIFT) | ((g) & CNT_MASK))

struct opts {
	unsigned int channels;
	const char *play_dev;
	const char *cap_dev;
	snd_pcm_uframes_t period;
	unsigned int periods;
	unsigned int secs;
};

static int setup(snd_pcm_t *pcm, const struct opts *o, const char *what)
{
	snd_pcm_hw_params_t *hw;
	snd_pcm_sw_params_t *sw;
	snd_pcm_uframes_t boundary;
	snd_pcm_uframes_t period = o->period;
	snd_pcm_uframes_t buffer = o->period * o->periods;
	unsigned int rate = RATE;
	int err;

	snd_pcm_hw_params_alloca(&hw);
	if ((err = snd_pcm_hw_params_any(pcm, hw)) < 0 ||
	    (err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
	    (err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S32_LE)) < 0 ||
	    (err = snd_pcm_hw_params_set_channels(pcm, hw, o->channels)) < 0 ||
	    (err = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, NULL)) < 0 ||
	    (err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, NULL)) < 0 ||
	    (err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer)) < 0 ||
	    (err = snd_pcm_hw_params(pcm, hw)) < 0) {
		fprintf(stderr, "%s: hw params: %s\n", what, snd_strerror(err));
		return err;
	}
	if (rate != RATE || period != o->period) {
		fprintf(stderr, "%s: got rate %u, period %lu (asked %u, %lu)\n", what, rate,
			(unsigned long)period, RATE, (unsigned long)o->period);
		return -EINVAL;
	}
	/*
	 * Start only on snd_pcm_start().  The default threshold of one frame
	 * starts DL11 on the first prefill write, and its ~4 KB prefetch then
	 * consumes more than a small prefill at once; the buffer size starts
	 * both linked streams on the last prefill write instead.
	 */
	snd_pcm_sw_params_alloca(&sw);
	if ((err = snd_pcm_sw_params_current(pcm, sw)) < 0 ||
	    (err = snd_pcm_sw_params_get_boundary(sw, &boundary)) < 0 ||
	    (err = snd_pcm_sw_params_set_start_threshold(pcm, sw, boundary)) < 0 ||
	    (err = snd_pcm_sw_params(pcm, sw)) < 0) {
		fprintf(stderr, "%s: sw params: %s\n", what, snd_strerror(err));
		return err;
	}
	printf("  %-8s %s: %u ch, %u Hz, S32_LE, period %lu, buffer %lu frames\n", what,
	       snd_pcm_name(pcm), o->channels, rate, (unsigned long)period,
	       (unsigned long)buffer);
	return 0;
}

static void fill(uint32_t *buf, uint32_t *g, snd_pcm_uframes_t frames, unsigned int ch)
{
	for (snd_pcm_uframes_t f = 0; f < frames; f++, (*g)++) {
		for (unsigned int c = 0; c < ch; c++) {
			*buf++ = PATTERN(*g, c);
		}
	}
}

int main(int argc, char **argv)
{
	struct opts o = {
		.channels = 16,
		.play_dev = "hw:0,6",
		.cap_dev = "hw:0,14",
		.period = 480,
		.periods = 4,
		.secs = 10,
	};
	snd_pcm_t *play, *cap;
	uint32_t *pbuf, *cbuf;
	uint32_t play_g = 0, want = 0;
	uint64_t played = 0, verified = 0, cap_frames = 0;
	unsigned int rot = 0;
	int64_t lag = 0;
	int aligned = 0;
	time_t t0, last;
	int opt, err;

	while ((opt = getopt(argc, argv, "c:D:C:p:n:t:h")) != -1) {
		switch (opt) {
		case 'c':
			o.channels = strtoul(optarg, NULL, 0);
			break;
		case 'D':
			o.play_dev = optarg;
			break;
		case 'C':
			o.cap_dev = optarg;
			break;
		case 'p':
			o.period = strtoul(optarg, NULL, 0);
			break;
		case 'n':
			o.periods = strtoul(optarg, NULL, 0);
			break;
		case 't':
			o.secs = strtoul(optarg, NULL, 0);
			break;
		default:
			fprintf(stderr,
				"usage: %s [-c 2|16|32] [-D playback] [-C capture] [-p period] "
				"[-n periods] [-t secs]\n",
				argv[0]);
			return opt == 'h' ? 0 : 2;
		}
	}
	if (o.channels < 1 || o.channels > 32) {
		fprintf(stderr, "channels must be 1..32\n");
		return 2;
	}
	printf("eTDM loopback: %u ch, %u Hz, %u s\n", o.channels, RATE, o.secs);
	if ((err = snd_pcm_open(&play, o.play_dev, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
		fprintf(stderr, "open %s: %s\n", o.play_dev, snd_strerror(err));
		return 1;
	}
	if ((err = snd_pcm_open(&cap, o.cap_dev, SND_PCM_STREAM_CAPTURE, 0)) < 0) {
		fprintf(stderr, "open %s: %s\n", o.cap_dev, snd_strerror(err));
		return 1;
	}
	if (setup(play, &o, "playback") < 0 || setup(cap, &o, "capture") < 0) {
		return 1;
	}

	pbuf = calloc(o.period * o.channels, sizeof(*pbuf));
	cbuf = calloc(o.period * o.channels, sizeof(*cbuf));
	if (!pbuf || !cbuf) {
		return 1;
	}

	/*
	 * Start both together.  The capture port takes its clocks from the
	 * wires, so it only produces frames once playback drives them.
	 */
	if ((err = snd_pcm_link(cap, play)) < 0) {
		fprintf(stderr, "link: %s (starting separately)\n", snd_strerror(err));
	}
	for (unsigned int p = 0; p < o.periods; p++) {
		fill(pbuf, &play_g, o.period, o.channels);
		if ((err = snd_pcm_writei(play, pbuf, o.period)) < 0) {
			fprintf(stderr, "prefill: %s\n", snd_strerror(err));
			return 1;
		}
		played += o.period;
	}
	if ((err = snd_pcm_start(cap)) < 0) {
		fprintf(stderr, "start: %s\n", snd_strerror(err));
		return 1;
	}
	if (snd_pcm_state(play) != SND_PCM_STATE_RUNNING && (err = snd_pcm_start(play)) < 0) {
		fprintf(stderr, "start playback: %s\n", snd_strerror(err));
		return 1;
	}

	t0 = last = time(NULL);
	while (time(NULL) - t0 < (time_t)o.secs) {
		snd_pcm_sframes_t n = snd_pcm_readi(cap, cbuf, o.period);

		if (n < 0) {
			fprintf(stderr, "\nERROR: capture %s after %llu frames\n", snd_strerror(n),
				(unsigned long long)cap_frames);
			return 1;
		}
		for (snd_pcm_sframes_t f = 0; f < n; f++, cap_frames++) {
			const uint32_t *fr = &cbuf[f * o.channels];

			if (!aligned) {
				/* Look for the first frame carrying our tags. */
				uint32_t tag0 = fr[0] >> CH_TAG_SHIFT;

				if (fr[0] == 0 || tag0 >= o.channels) {
					continue;
				}
				rot = tag0;
				want = fr[0] & CNT_MASK;
				lag = (int64_t)cap_frames - want;
				aligned = 1;
				printf("  aligned at capture frame %llu: slot rotation %u, frame lag %lld\n",
				       (unsigned long long)cap_frames, rot, (long long)lag);
			}
			for (unsigned int s = 0; s < o.channels; s++) {
				uint32_t exp = PATTERN(want, (s + rot) % o.channels);

				if (fr[s] != exp) {
					fprintf(stderr,
						"\nERROR: capture frame %llu slot %u: expected 0x%08x, "
						"got 0x%08x\n  verified %llu frames\n",
						(unsigned long long)cap_frames, s, exp, fr[s],
						(unsigned long long)verified);
					return 1;
				}
			}
			want = (want + 1) & CNT_MASK;
			verified++;
		}

		fill(pbuf, &play_g, o.period, o.channels);
		n = snd_pcm_writei(play, pbuf, o.period);
		if (n < 0) {
			fprintf(stderr, "\nERROR: playback %s after %llu frames\n", snd_strerror(n),
				(unsigned long long)played);
			return 1;
		}
		played += n;

		if (time(NULL) != last) {
			last = time(NULL);
			printf("[%3lds] played %llu fr  verified %llu fr  rot %u lag %lld\n",
			       (long)(last - t0), (unsigned long long)played,
			       (unsigned long long)verified, rot, (long long)lag);
			fflush(stdout);
		}
	}

	snd_pcm_drop(play);
	snd_pcm_drop(cap);
	if (!verified) {
		printf("RESULT: FAIL, nothing tagged was captured (wires, pin mux, routes?)\n");
		return 1;
	}
	printf("RESULT: PASS, %llu frames verified\n", (unsigned long long)verified);
	return 0;
}
