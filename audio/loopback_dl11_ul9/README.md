# Loopback: DL11 to UL9, 32 channels

The widest path the AFE offers. A single eTDM port carries at most 16 channels,
so the stream is split across a co-clocked pair in each direction: eTDM_OUT1 is
the cowork master with OUT2 slaved to it, and on the capture side UL9 reaches
memory through the AFE_CONN matrix and the CM0 merge unit.

This is the sample that exercises the merge unit and the cowork linkage. The
slot-to-channel map it prints is the evidence they are wired correctly.

## Wiring

| From | To | |
|---|---|---|
| pin 4 `I2SO1_MCK` | pin 125 `TDMIN_MCK` | |
| pin 5 `I2SO1_BCK` | pin 126 `TDMIN_BCK` | |
| pin 6 `I2SO1_WS` | pin 127 `TDMIN_LRCK` | |
| pin 11 `I2SO1_D0` | pin 128 `TDMIN_DI` | channels 0-15 |
| pin 117 `I2SO2_D0` | pin 110 `I2SIN_D0` | channels 16-31 |

One clock harness only, and at 32 channels it is also the only one possible:
eTDM_OUT2 is a cowork slave and does not drive its own BCK/WS, and eTDM_IN2
takes its clocks from eTDM_IN1 rather than its own pins.
## Run it

```bash
west build -b mt8370_genio_510_evk/mt8188/a55 -S mtk-afe samples/audio/loopback_dl11_ul9
adb push build/zephyr/zephyr.bin /root/zephyr.bin
adb shell 'echo on > /sys/devices/platform/soc/10b10000.afe/power/control'
adb shell 'sh /root/genio-inmate.sh /root/zephyr.bin genio-510-evk-zephyr-afe.cell'
```

See [the group README](../README.md) for why each of those steps is needed.

## Expected

```
  slot->channel map (frame lag -11):
     0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15
    16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31
[   1s] played 55680 fr  verified 50880 fr  ticks 213  lag -11  isr max 3088 us
```

The map must be the identity, 0 through 31 in order. Any rotation or
interleaving means the merge unit or the cowork linkage is misprogrammed —
which still produces a stream, just not the one you asked for.

## A warning about the capture side

UL9's DMA is paced by CM0 on the AFE's internal clock, not by anything arriving
on the wire. **It advances at a healthy 48 kHz even with the input pins
completely dead**, recording silence. A sample that only checked capture was
moving would call that a pass. Comparing the frames is what catches it.
