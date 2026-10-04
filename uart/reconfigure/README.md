# UART runtime reconfigure

Changes the console baud rate at runtime and puts it back, proving the change
reached the wire rather than just the driver's idea of it.

## Run it

```bash
west build -b mt8370_genio_510_evk/mt8188/a55 samples/uart/reconfigure
adb push build/zephyr/zephyr.bin /root/zephyr.bin
```

Start the host script **first**, then bring the cell up:

```bash
python3 samples/uart/reconfigure/host/check_baud.py &
sleep 2
adb shell 'sh /root/genio-inmate.sh /root/zephyr.bin'
```

The firmware announces each phase repeatedly, giving the host a window to
resynchronise at the new rate. The script follows phase 1 until the
announcements stop, which is when the firmware switches to 9600, and times the
rest from there. Phase 1 lasts three seconds, so a script started later than
that has nothing to follow and says so.

## Expected

```
  phase1 readable at 115200        : PASS
  phase2 readable at 9600          : PASS
  phase2 NOT readable at 115200    : PASS
  phase3 readable at 115200 again  : PASS

RESULT: runtime reconfigure works on the wire
```

## The third line is the one that matters

Phase 2 being **unreadable at 115200** is the actual evidence. A
`uart_configure()` that returns 0 and changes no divisor produces a perfectly
convincing run of the other three checks — the console keeps working, the
firmware keeps printing, and nothing looks wrong. Only the negative control
distinguishes a real rate change from a no-op.

## Requires

`CONFIG_UART_USE_RUNTIME_CONFIGURE=y`, which this sample's `prj.conf` sets. It
is off in the board defconfig.
