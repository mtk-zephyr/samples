# UART interrupt-driven RX

Echoes every received byte and counts both bytes and ISR entries, so the host
can verify the echo byte-for-byte and see how the receive path behaves under
sustained load.

## Run it

```bash
west build -b mt8370_genio_510_evk/mt8188/a55 samples/uart/rx_interrupt
adb push build/zephyr/zephyr.bin /root/zephyr.bin
adb shell 'sh /root/genio-inmate.sh /root/zephyr.bin'
python3 samples/uart/rx_interrupt/host/verify_echo.py
```

The host script owns the serial port, so start it after the cell is up and make
sure no terminal is attached.

## Expected

Three phases — 64 bytes, 2000 bytes in 64-byte chunks, 8000 bytes in 256-byte
chunks — each echoed back identically:

```
=== test 3: 8000 bytes, sustained (interrupt load) ===
  8000 bytes in 256-byte chunks
    sent 8000 bytes, received 8000, 1.22 s (6.4 KiB/s)
    echo byte-for-byte IDENTICAL

=== firmware counters ===
    STATS rx=10065 isr=10065
    host sent 10065 bytes total (including the 0x04 request)

RESULT: all echo tests passed
```

`rx` must equal what the host sent: **zero bytes lost across 10,065
interrupts**.

## Reading the counters

`isr == rx` means one interrupt per byte — the RX FIFO trigger level is
effectively 1 and nothing batches. That is not a defect, but it does make
receive interrupt-bound: about 6.5 KiB/s against an 11.5 KiB/s line rate, with
the echo done inside the ISR via `uart_poll_out`. If RX throughput ever matters,
the FIFO trigger level is where to look.

## The out-of-band request byte

`0x04` is counted but not echoed; it asks for the `STATS` line instead. Without
that, the counter output would interleave into the very stream being compared.
