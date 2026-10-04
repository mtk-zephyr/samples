# Capture: UL3

Captures 16 channels at 48 kHz from eTDM_IN2 into memory, printing one sample per
channel per second. UL3 is wired straight to eTDM_IN2, which runs as a standalone master using its own MCK, BCK and WS pins (107, 108, 109).

## Run it

```bash
west build -b mt8370_genio_510_evk/mt8188/a55 -S mtk-afe samples/audio/record_ul3
adb push build/zephyr/zephyr.bin /root/zephyr.bin
adb shell 'echo on > /sys/devices/platform/soc/10b10000.afe/power/control'
adb shell 'sh /root/genio-inmate.sh /root/zephyr.bin genio-510-evk-zephyr-afe.cell'
```

See [the group README](../README.md) for why each of those steps is needed.

## Expected

```
MT8188 UL3 capture started: 48kHz 16ch 32-bit from eTDM_IN2
UL3 ch0: 0x00000000  UL3 ch1: 0x00000000  ...
```

## You will almost certainly record silence

Nothing on a bare EVK drives the input port, and with one core there is no way
to play and capture at once from a single sample. **All-zero samples are the
correct result here**, not a failure — this sample shows that the capture path
runs and moves frames, nothing more.

To verify capture actually carries data, run a loopback instead: those play and
capture at the same time and compare what comes back against what went out.
