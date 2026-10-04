# Loopback: DL8 to UL3

The eTDM2 pair. DL8 reaches eTDM_OUT2 through the shared DL8/DL11 mux, and UL3
is wired straight to eTDM_IN2. 16 channels at 48 kHz — both memifs use a single
port each, so that is the ceiling here.

Run this alongside `loopback_dl11_ul8` to tell a fault in one port pair from a
fault in the driver.

## Wiring

| From | To |
|---|---|
| pin 114 `I2SO2_MCK` | pin 107 `I2SIN_MCK` |
| pin 115 `I2SO2_BCK` | pin 108 `I2SIN_BCK` |
| pin 116 `I2SO2_WS` | pin 109 `I2SIN_WS` |
| pin 117 `I2SO2_D0` | pin 110 `I2SIN_D0` |
## Run it

```bash
west build -b mt8370_genio_510_evk/mt8188/a55 -S mtk-afe samples/audio/loopback_dl8_ul3
adb push build/zephyr/zephyr.bin /root/zephyr.bin
adb shell 'echo on > /sys/devices/platform/soc/10b10000.afe/power/control'
adb shell 'sh /root/genio-inmate.sh /root/zephyr.bin genio-510-evk-zephyr-afe.cell'
```

See [the group README](../README.md) for why each of those steps is needed.

## Expected

```
[   1s] played 54720 fr  verified 49920 fr  ticks 209  rot 0 lag 30  isr max 1578 us
```

Same shape as the other loopbacks. `lag` settles at a different constant here —
around 30 rather than -1 — because the route through the mux has a different
pipeline depth. A constant value is what matters, not which one.
