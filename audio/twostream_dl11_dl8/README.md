# Two-stream teardown

Starts DL11 on eTDM_OUT1 and DL8 on eTDM_OUT2 — both 16 channels at 48 kHz, so
both draw on the same audio PLL timing domain — then stops one and measures
whether the other survives.

A `stop()` that released the shared domain unconditionally would take the other
stream's bit clock with it. The `clock_control` drivers underneath do not
reference count, so the AFE driver has to.

**No loopback sample can find this**, because all three stop both streams at
teardown — the one order in which the fault cannot show. Playback only, so no
wires are needed: the question is whether a memory interface keeps moving
frames, which needs the port clocked, not connected to anything.

## Run it

```bash
west build -b mt8370_genio_510_evk/mt8188/a55 -S mtk-afe samples/audio/twostream_dl11_dl8
adb push build/zephyr/zephyr.bin /root/zephyr.bin
adb shell 'echo on > /sys/devices/platform/soc/10b10000.afe/power/control'
adb shell 'sh /root/genio-inmate.sh /root/zephyr.bin genio-700-evk-zephyr-afe.cell'
```

See [the group README](../README.md) for why each of those steps is needed.

## Expected

```
  1. both started
    DL11                   ok     2401 frames in 50 ms, wanted ~2400
    DL8                    ok     2401 frames in 50 ms, wanted ~2400

  2. DL8 stopped -- DL11 must be untouched
    DL11 survivor          ok     2402 frames in 50 ms, wanted ~2400
    DL8 stopped            ok        0 frames in 50 ms, wanted 0

  3. DL8 restarted -- the domain must come back for it
    DL11                   ok     2403 frames in 50 ms, wanted ~2400
    DL8                    ok     2401 frames in 50 ms, wanted ~2400

  4. DL11 stopped -- the other order
    DL8 survivor           ok     2403 frames in 50 ms, wanted ~2400
    DL11 stopped           ok        0 frames in 50 ms, wanted 0

PASS -- the timing domain is reference counted
```

Phase 3 separates a count that never rises from one that never falls: a count
stuck at zero brings the domain up once and never again. Phase 4 repeats the
test in the other order, because a reference count can be wrong in one
direction only.

## Reboot afterwards

This sample stops every stream it started, which leaves the AFE in the state
that wedges the **next** inmate during init. Reboot the board before running
another audio sample. `api_reject` is the exception — it never starts a stream,
so it is safe to run straight after.
