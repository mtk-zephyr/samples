# Playback: DL11, 32 channels

Plays 32 channels out of a co-clocked port pair — channels 0-15 direct to
eTDM_OUT1, channels 16-31 through the DL8/DL11 mux to eTDM_OUT2, with OUT2
slaved to OUT1 so both share BCK and LRCK.

Each channel carries a distinct DC level, so the channel mapping can be
identified on a scope or logic analyser. No wires needed; put a probe on the
eTDM_OUT1 pins (4, 5, 6, 11).

This is the first sample to run when you want to see a real signal leave the
die. If you want the path *verified* rather than observed, run a loopback.

## Run it

```bash
west build -b mt8370_genio_510_evk/mt8188/a55 -S mtk-afe samples/audio/playback_dl11
adb push build/zephyr/zephyr.bin /root/zephyr.bin
adb shell 'echo on > /sys/devices/platform/soc/10b10000.afe/power/control'
adb shell 'sh /root/genio-inmate.sh /root/zephyr.bin genio-700-evk-zephyr-afe.cell'
```

See [the group README](../README.md) for why each of those steps is needed.

## Expected

```
MT8188 DL11 playback started: 48kHz 32ch 32-bit DSP-B
  ch0-15  -> eTDM_OUT1 (O072-O087)
  ch16-31 -> eTDM_OUT2 (O048-O063), via DL8_DL11 mux
playing... DMA offset 0x12c00 / 0x96000
```

The DMA offset must advance between lines and wrap at the buffer size. An
offset that stays put, or one outside the buffer, means the AFE is not running —
check the power domain first.
