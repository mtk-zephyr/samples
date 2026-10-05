# eTDM loop-latency bench

Measures how long audio takes to go once round an eTDM loopback on a Genio EVK,
with and without processing in the loop, on Linux and on Zephyr (one A55, one
A78, or two A55 cores), so the four can be compared on the same board, wires
and code.

```
   DL11 -> eTDM_OUT1 (+OUT2 at 32 ch) -> wires -> eTDM_IN1 (+IN2) -> CM0 -> UL9
    ^                                                                       |
    +-------------------- block processing: pass | FIR | IIR <--------------+
```

Both sides use DL11 and UL9 at every channel count: on Linux, UL8 takes only
two channels from eTDM_IN1.

## What is measured

Slot 0 carries a marker word. Whenever it arrives on the input, it is written
straight back to the output at the same position in the block, with its
sequence number advanced, so it keeps going round. The input frames between
two arrivals are one **lap**: capture buffering, processing, playback
buffering and the wire. At 48 kHz one frame is 20.8 µs.

The other slots go through the processing stage:

| Variant | Processing per channel |
|---|---|
| `pass` | copy |
| `fir` | 64-tap low-pass FIR, Q31 |
| `iir` | 8th-order Butterworth low-pass, 4 biquads, Q2.30 |

The marker slot is never filtered, so a filter adds its processing time to the
lap but not its group delay (31.5 frames for the FIR). The processing is
integer only and shared verbatim by both sides
([`common/bench_dsp.c`](common/bench_dsp.c)), and the `DSP selftest` checksums
each side prints at start must match between targets.

Two knobs set the latency:

- **block**: frames taken from the capture buffer and processed at once.
- **lead**: how far ahead of the playback DMA pointer a processed block is
  written. Zephyr places each block there directly. Linux brings its playback
  queue to exactly `lead` frames, rewinding or padding it once after each start,
  and then keeps it there, because both streams run off one clock.

Too small a lead makes blocks late. A run is **stable** if it had no late
block, no overrun, no lost lap and no marker reinjection. The interesting
number per configuration is the lap at the smallest stable lead.

Each run also reports the processing time per block and the CPU load it amounts
to. The time runs from a whole block being available to its output being
queued, so the copies out of and into the DMA buffers are included on both
sides (`readi()` and `writei()` on Linux).

## The playback prefetch

The DL11 playback DMA reads about 4 KB ahead of what is on the wire: right after
a start, its pointer runs about 4 KB in front of the capture pointer and stays
there. That is a fixed byte count, so its share of a lap shrinks with the frame
size: about 504 frames (10.5 ms) at 2 channels, 63 frames at 16, and 31 at 32.
The Zephyr side reports it per run as `hw_gap`, in frames.

No documented register sets this prefetch size. Where latency matters at a low
channel count, carry the channels in a wider TDM frame: 2 used channels in 16
slots go round in the 16-channel time, about 2.5 ms instead of 11.7 ms at
block 32.

## Wiring and board setup

The wires of the AFE loopback samples: see
[`../../audio/loopback_dl11_ul9/README.md`](../../audio/loopback_dl11_ul9/README.md).
Five wires cover every channel count.

## Zephyr

| Target | Cell | Image |
|---|---|---|
| one A55 | `genio-510-evk-zephyr-afe.cell` | `mt8370_genio_510_evk/mt8188/a55` |
| one A78 | `genio-510-evk-zephyr-afe-a78.cell` | the same image |
| two A55 | `genio-510-evk-zephyr-afe-smp.cell` | `mt8370_genio_510_evk/mt8188/a55/smp` |

```bash
west build -p -b mt8370_genio_510_evk/mt8188/a55 -S mtk-afe -d build/bench samples/bench/etdm_latency/zephyr
python3 samples/bench/etdm_latency/tools/run_zephyr.py build/bench/zephyr/zephyr.bin \
        genio-510-evk-zephyr-afe.cell bench-zephyr-a55.log
```

One boot sweeps 2, 16 and 32 channels × the three variants × blocks 16 to 128 ×
leads 16 to 256 frames, 3 s per run, about 11 minutes, and prints one `BENCH`
line per run. `CONFIG_BENCH_QUICK=y` sweeps a smaller set;
`CONFIG_BENCH_RUN_MS` sets the time per run. The driver has no period
interrupt, so the bench polls the DMA pointers; on two cores, the second core
processes half the channels of every block.

The driver maps the AFE buffers non-cacheable, and reading them that way is
slow: inside a Jailhouse cell, about 7 MB/s, against 480 MB/s cached, which
the bench prints at start. By default the bench therefore remaps the buffer
region cacheable and does explicit cache maintenance (the AFE does not snoop
the caches): it invalidates a captured block before reading it, and cleans a
played block after writing it. `CONFIG_BENCH_CACHED_BUFFERS=n` keeps the
driver's mapping. The `mem` field of each `BENCH` line says which was used.

## Linux

Native, with Jailhouse not enabled. The board's Linux needs the eTDM pin mux,
the DL11/UL9 routes, and the two programs from [`linux/`](linux), which an
`etdm-bench` Yocto recipe builds:

- `etdm_loopback_test` checks the loop first (`-c 2|16|32`). It must pass.
- `etdm_latency_bench` runs one configuration, or with `-S` the Zephyr sweep
  of variants, blocks and leads at the channel count of `-c`. `-a` pins it to a
  CPU and `-r` runs it SCHED_FIFO. It needs root.

The mixer routes differ per channel count, so a whole sweep goes through
[`tools/run_linux.sh`](tools/run_linux.sh), on the board. It sets the routes
for 2, 16 and 32 channels in turn, and holds the cpufreq governor at
`performance` while it runs:

```bash
sh run_linux.sh 3 bench-linux-a55.log    # pinned to an A55
sh run_linux.sh 5 bench-linux-a78.log    # pinned to an A78
```

Zephyr is run with the same governor setting. Under Linux's default
`schedutil`, the A55 cluster slows down while Linux is idle, and that includes
the core a Zephyr cell runs on.

Two things about the Linux side:

- The streams start only on `snd_pcm_start()`: the start threshold is the
  boundary. With the default of one frame, DL11 starts on the first prefill
  write, and its prefetch consumes a small prefill at once.
- ALSA's buffers for the AFE are non-cacheable, from a `dma_alloc_coherent`
  pool. Making them cacheable would need a kernel change, so the Linux results
  are for the stock mapping.

## Comparing

```bash
python3 samples/bench/etdm_latency/tools/summarize.py bench-*.log --csv all.csv
python3 samples/bench/etdm_latency/tools/summarize.py --overall bench-*.log
```

The first prints each target's lowest stable latency per channel count,
variant and block; `--overall` takes the lowest over all blocks.
