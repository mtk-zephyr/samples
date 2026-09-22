# Playback: DL8, 16 channels

Plays 16 channels out of eTDM_OUT2, reaching the interconnect through the
DL8/DL11 mux. The port runs as a standalone clock master here, generating its
own BCK and WS on the I2SO2 pins (114 MCK, 115 BCK, 116 WS, 117 D0).

Each channel carries a distinct DC level so the mapping can be identified on a
scope. No wires needed.

## Concurrency

DL8 and DL11 **can** run together at 16 channels each: DL11 uses only its direct
range to eTDM_OUT1 and leaves this mux to DL8. They **cannot** at 32 channels,
where DL11 needs the mux for its upper 16 channels as well as both ports.
`twostream_dl11_dl8` is built on exactly that legal pairing.

## Run it

```bash
west build -b mt8370_genio_510_evk/mt8188/a55 -S mtk-afe samples/audio/playback_dl8
adb push build/zephyr/zephyr.bin /root/zephyr.bin
adb shell 'echo on > /sys/devices/platform/soc/10b10000.afe/power/control'
adb shell 'sh /root/genio-inmate.sh /root/zephyr.bin genio-700-evk-zephyr-afe.cell'
```

See [the group README](../README.md) for why each of those steps is needed.

## Expected

```
MT8188 DL8 playback started: 48kHz 16ch 32-bit DSP-B
  ch0-15 -> eTDM_OUT2 (O048-O063), via DL8_DL11 mux
playing... DMA offset 0x09600 / 0x4b000
```
