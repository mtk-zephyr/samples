# Capture: UL8

Captures 16 channels at 48 kHz from eTDM_IN1 into memory, printing one sample per
channel per second. UL8 is wired straight to eTDM_IN1, with no routing matrix and no merge unit in the way.

## Run it

```bash
west build -b mt8370_genio_510_evk/mt8188/a55 -S mtk-afe samples/audio/record_ul8
adb push build/zephyr/zephyr.bin /root/zephyr.bin
adb shell 'echo on > /sys/devices/platform/soc/10b10000.afe/power/control'
adb shell 'sh /root/genio-inmate.sh /root/zephyr.bin genio-700-evk-zephyr-afe.cell'
```

See [the group README](../README.md) for why each of those steps is needed.

## Expected

```
MT8188 UL8 capture started: 48kHz 16ch 32-bit from eTDM_IN1
UL8 ch0: 0x00000000  UL8 ch1: 0x00000000  ...
```

## You will almost certainly record silence

Nothing on a bare EVK drives the input port, and with one core there is no way
to play and capture at once from a single sample. **All-zero samples are the
correct result here**, not a failure — this sample shows that the capture path
runs and moves frames, nothing more.

To verify capture actually carries data, run a loopback instead: those play and
capture at the same time and compare what comes back against what went out.
