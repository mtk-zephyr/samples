# Argument rejection

Walks the guards on `mt8188_afe_configure()` and `mt8188_afe_set_buf()` and
checks each one returns what it promises: out-of-range and undriven memory
interfaces, a null configuration, zero and oversized channel counts, a word size
the block cannot carry, a rate with no timing value, and buffers that are zero
sized, mis-sized, unaligned, or run past the end of the address space.

These are worth holding still because of what sits behind them. Several eTDM
register fields are written as a count less one, so a zero channel count
underflows into an all-ones field and the block accepts it — leaving a stream
running at a width nothing asked for. The buffer registers steer a bus master
with no address translation in front of it, so a zero size puts the end below
the base and an unaligned address puts the wrap point mid-sample. The hardware
reports none of it.

Nothing is started, so no wires are needed and the AFE is left as it was found.

## Run it

```bash
west build -b mt8370_genio_510_evk/mt8188/a55 -S mtk-afe samples/audio/api_reject
adb push build/zephyr/zephyr.bin /root/zephyr.bin
adb shell 'echo on > /sys/devices/platform/soc/10b10000.afe/power/control'
adb shell 'sh /root/genio-inmate.sh /root/zephyr.bin genio-510-evk-zephyr-afe.cell'
```

See [the group README](../README.md) for why each of those steps is needed.

## Expected

```
  configure()
    valid, as a control                ok    got 0        wanted 0
    memory interface out of range      ok    got -EINVAL  wanted -EINVAL
    memory interface not driven        ok    got -ENOTSUP wanted -ENOTSUP
    ...
  set_buf()
    valid, as a control                ok    got 0        wanted 0
    zero size                          ok    got -EINVAL  wanted -EINVAL
    ...

PASS -- 15 of 15 checks
```

Each function is also called once with **valid** arguments. That matters: a
guard that has started rejecting everything would otherwise read as a clean
pass.

Safe to run immediately after a sample that stopped its streams, since it never
starts one.
