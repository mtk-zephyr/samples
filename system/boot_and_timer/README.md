# Boot, board identity and timer accuracy

Confirms the image boots on the board you think it does, that the kernel's idea
of the counter frequency is right, and that `k_sleep()` sleeps for as long as it
says.

## Run it

```bash
west build -b mt8370_genio_510_evk/mt8188/a55 samples/system/boot_and_timer
adb push build/zephyr/zephyr.bin /root/zephyr.bin
```

Start the host script **first** — the markers are printed once, so a listener
that attaches late misses them — then bring the cell up:

```bash
python3 samples/system/boot_and_timer/host/check_timer.py \
        --board mt8370_genio_510_evk/mt8188/a55 &
sleep 3
adb shell 'sh /root/genio-inmate.sh /root/zephyr.bin'
```

## Expected

```
  board target      mt8370_genio_510_evk/mt8188/a55
  cntfrq            13000000 Hz
  sleeps            3 started, 3 completed
    iteration 0          -8.7 ms from 5 s
    iteration 1          +6.9 ms from 5 s
    iteration 2          +7.2 ms from 5 s

RESULT: PASS
```

`cntfrq` is 13 MHz on both parts. Deviations are single-digit milliseconds in
practice; the script allows 100 ms (`--tolerance-ms`).

## Why the host does the timing

`k_uptime_get()` derives from the very timer under test, so the firmware cannot
detect a misconfigured one — it would be marking its own homework. The host
stamps each line as it arrives, giving an independent clock.

This is what catches a misdeclared interrupt trigger type. Zephyr's GIC
devicetree flags encode only level-versus-edge, and Linux's `IRQ_TYPE_LEVEL_HIGH`
shares a value with `IRQ_TYPE_EDGE`. Get it wrong and the architected timer's
PPIs are left edge-triggered — which builds, boots, and keeps time badly.

## If the board target is wrong

The parts are pin-compatible, so **a Genio 700 image runs quite happily on 510
hardware**. The board string is the only thing that catches a mismatched image.
Pass `--board` and the script will tell you.

## One artefact

On a first boot the banner, `cntfrq` and `SLEEP_START 0` can arrive in a single
burst once the console comes up, which makes iteration 0 read short. If only
iteration 0 is out of tolerance the script says so — re-run before believing it.
Iterations 1 and 2 are the reliable ones.
