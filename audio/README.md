# Audio Front End (eTDM)

The MT8188 Audio Front End moves samples between memory and the die's eTDM
serial ports. These ten samples exercise the paths the driver supports:
playback, capture, full-duplex loopbacks that check captured frames against
what was played, and two that test driver behaviour rather than audio.

The interface is `<zephyr/drivers/audio/mt8188_afe.h>`. It is deliberately not
the Zephyr DAI interface — the routing matrix, the channel-merge units and the
co-clocked port pairs have no expression in DAI.

## Before anything will work

Audio has more prerequisites than the other samples here, and **every one of
them fails silently**: the driver returns success at each step and no sound
moves. Work through all four.

**1. Build with the snippet.**

```bash
west build -b mt8390_genio_700_evk/mt8188/a55 -S mtk-afe samples/audio/loopback_dl11_ul8
```

`mtk-afe` enables the AFE node, selects the eTDM pins and reserves the 8 MB
buffer region at `0x61000000`. Without it the AFE is off and the build fails on
the missing `dma_region`.

**2. Use a cell that grants the AFE.** The ordinary `genio-*-evk-zephyr` cell
grants none of the five register blocks the driver touches, and the inmate is
stopped on its first access. You need the `-afe` cell:

```bash
adb shell 'sh /root/genio-inmate.sh /root/zephyr.bin genio-700-evk-zephyr-afe.cell'
```

That cell is 700-authored and works unchanged on the 510: same die family, AFE
at the same addresses, identical inmate window and console.

**3. Ask Linux to hold the audio power domain up.**

```bash
adb shell 'echo on > /sys/devices/platform/soc/10b10000.afe/power/control'
```

The AFE sits in the MTCMOS `audio` domain, which is **off on a stock boot**, and
no SPM region is granted to the inmate — so neither the cell nor the driver can
raise it. Without this every register write is dropped while every driver call
returns 0. The symptom is a DMA pointer outside its buffer.

Unbinding Linux's `mt8188-audio` driver does *not* work instead: the domain
tracks an active stream, not a loaded driver.

**4. Have Linux mux the eTDM pins.** The pin controller is granted by no cell,
so pin muxing goes through the hypervisor's mediator, which discards an
ungranted write and reports success. The snippet's `pinctrl-0` therefore
applies nothing under the cell, and the pins must be muxed by the board's Linux
devicetree instead. Check before blaming the driver:

```bash
adb shell 'cat /sys/kernel/debug/pinctrl/10005000.pinctrl-pinctrl_paris/pins' \
  | grep -E '^pin (4|5|6|11|125|126|127|128) '
```

The first digit after the colon is the mux mode. Those eight pins need **mode
3**; pins 107-110 and 114-117 need **mode 1**.

## Wiring

| Port | Signals | Pins |
|---|---|---|
| eTDM_IN1 | MCK, BCK, LRCK, DI | 125, 126, 127, 128 |
| eTDM_IN2 | MCK, BCK, WS, D0 | 107, 108, 109, 110 |
| eTDM_OUT1 | MCK, BCK, WS, D0 | 4, 5, 6, 11 |
| eTDM_OUT2 | MCK, BCK, WS, D0 | 114, 115, 116, 117 |

Each loopback's README lists the wires it needs. Playback-only and
capture-only samples need none, though capture with nothing driving the port
records silence.

## The samples

| Sample | Path | Wires |
|---|---|---|
| [`loopback_dl11_ul8`](loopback_dl11_ul8) | DL11 → eTDM_OUT1 → eTDM_IN1 → UL8, 16 ch | 3 |
| [`loopback_dl8_ul3`](loopback_dl8_ul3) | DL8 → eTDM_OUT2 → eTDM_IN2 → UL3, 16 ch | 4 |
| [`loopback_dl11_ul9`](loopback_dl11_ul9) | 32 ch across a co-clocked port pair, via CM0 | 5 |
| [`playback_dl11`](playback_dl11) | DL11 → eTDM_OUT1 + OUT2, 32 ch | — |
| [`playback_dl8`](playback_dl8) | DL8 → eTDM_OUT2 through the shared mux, 16 ch | — |
| [`record_ul3`](record_ul3) | UL3 ← eTDM_IN2, 16 ch | — |
| [`record_ul8`](record_ul8) | UL8 ← eTDM_IN1, 16 ch | — |
| [`record_ul9`](record_ul9) | UL9 ← CM0 ← eTDM_IN1 + IN2, 32 ch | — |
| [`twostream_dl11_dl8`](twostream_dl11_dl8) | Stopping one stream must not kill the other | — |
| [`api_reject`](api_reject) | `configure()` and `set_buf()` argument guards | — |

**The loopbacks are the ones worth running.** They are the only samples that
put a playback and a capture stream on the die at once and compare the captured
frames against the generated ones, which is what distinguishes a working path
from one that merely runs.

**The capture-only samples need an external TDM source.** With one core there is
no way to drive their input from the board itself, so on a bare EVK they record
silence — which is a correct result, not a failure.

## Which tree

Nothing to do: the AFE driver is on `mtk-genio-dev`, which the workspace
`west.yml` already tracks, and on `mtk-v4.4.2` as well if you are building from
the stable branch.
