# Loopback: DL11 to UL8

Plays a running pattern out of eTDM_OUT1 and checks every frame that comes back
on eTDM_IN1, 16 channels at 48 kHz. UL8 is wired straight to eTDM_IN1 with no
routing matrix and no merge unit in the way, which is what makes this the one to
run first: it isolates the capture interconnect.

## Wiring

| From | To |
|---|---|
| pin 5 `I2SO1_BCK` | pin 126 `TDMIN_BCK` |
| pin 6 `I2SO1_WS` | pin 127 `TDMIN_LRCK` |
| pin 11 `I2SO1_D0` | pin 128 `TDMIN_DI` |

MCK (pin 4 to 125) is optional: an eTDM input in slave mode latches on the
received BCK and the capture config passes `mclk_freq = 0`, so the port never
drives its own MCK pin.
## Run it

```bash
west build -b mt8370_genio_510_evk/mt8188/a55 -S mtk-afe samples/audio/loopback_dl11_ul8
adb push build/zephyr/zephyr.bin /root/zephyr.bin
adb shell 'echo on > /sys/devices/platform/soc/10b10000.afe/power/control'
adb shell 'sh /root/genio-inmate.sh /root/zephyr.bin genio-700-evk-zephyr-afe.cell'
```

See [the group README](../README.md) for why each of those steps is needed.

## Expected

```
running — data handled every 5 ms from the timer interrupt
[   1s] played 54720 fr  verified 49920 fr  ticks 209  rot 0 lag -1  isr max 1577 us
[   2s] played 103200 fr  verified 98400 fr  ticks 411  rot 0 lag -1  isr max 1579 us
```

`verified` must keep climbing with `played`, `rot` must be 0, and `lag` must
hold a constant value. The first mismatch stops both streams and prints the
frame, the slot, and what was expected against what arrived.

It runs until you destroy the cell — it never stops a stream.
