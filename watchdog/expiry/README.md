# Watchdog expiry

Arms the SoC watchdog for 5 s, kicks it ten times at one-second intervals, then
stops kicking and reports that it is still alive every 250 ms until the
watchdog takes effect. Bare-metal, that is a reset of the SoC; in a Jailhouse
cell that provides the watchdog, the hypervisor stops the cell.

## Where it may run

**Only in a cell that provides the watchdog.** No cell of mtk-jailhouse
`mtk-v1.0.0` does:

- the plain cells do not grant the watchdog's block, and the inmate is stopped
  on its first access;
- the AFE cells grant the real watchdog, which Linux uses as its own, so this
  application would reset the whole board.

## Run it

The watchdog is off in the boards; the `mtk-wdt` snippet turns it on.

```bash
west build -b mt8370_genio_510_evk/mt8188/a55 -S mtk-wdt samples/watchdog/expiry
adb push build/zephyr/zephyr.bin /root/zephyr.bin
adb shell 'sh /root/genio-inmate.sh /root/zephyr.bin <a cell that provides the watchdog>'
```

## Expected

```
WDTEXPIRY armed for 5000 ms, counter at 13000000 Hz
WDTEXPIRY arm: 0 ms since the arm, cycle <n>
WDTEXPIRY kick 1: 1000 ms since the arm, cycle <n>
...
WDTEXPIRY kick 10: 10000 ms since the arm, cycle <n>
WDTEXPIRY stopped kicking; the watchdog is due at 15000 ms
WDTEXPIRY alive: 10250 ms since the arm, cycle <n>
...
WDTEXPIRY alive: 14750 ms since the arm, cycle <n>
```

and then nothing more from this run. The deadline is 5 s after the last kick,
about 15000 ms after the arm, so the last `alive` line is the one at 14750 ms,
or the one at 15000 ms when it wins the race with the deadline. Bare-metal, the
board then reboots. In a cell, the hypervisor stops the cell and `jailhouse cell
list` shows it failed; it checks deadlines every 100 ms, so the cell may run up
to that much longer. Whatever restarts failed cells starts it again from the
top.

`cycle` is the counter Zephyr's clock runs on, so the arm and the last kick can
be compared with the time at which the hypervisor reports the expiry. A `FAIL`
line means the driver refused the configuration and shows the error it returned.
