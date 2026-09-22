"""Host side of the UART RX / interrupt-load test.

Sends known data to the board and verifies the echo byte-for-byte, then asks the
firmware for its counters. Owns the serial port for the duration, so nothing else may
hold it -- a second reader steals bytes and looks exactly like a dead console.
"""
import argparse
import os
import sys
import time

import serial

BAUD = 115200
STATS_REQ = b'\x04'


def drain(ser, secs=1.0):
    end = time.time() + secs
    out = b''
    while time.time() < end:
        out += ser.read(4096)
    return out


def read_until(ser, needle, timeout=5.0):
    end = time.time() + timeout
    buf = b''
    while time.time() < end:
        buf += ser.read(4096)
        if needle in buf:
            return buf
    return buf


def echo_test(ser, payload, chunk, pause, label):
    ser.reset_input_buffer()
    got = bytearray()
    t0 = time.time()
    for i in range(0, len(payload), chunk):
        part = payload[i:i + chunk]
        ser.write(part)
        ser.flush()
        if pause:
            time.sleep(pause)
        got += ser.read(len(part))
    # allow stragglers
    end = time.time() + 2.0
    while len(got) < len(payload) and time.time() < end:
        got += ser.read(4096)
    dt = time.time() - t0

    ok = bytes(got) == payload
    print(f"  {label}")
    print(f"    sent {len(payload)} bytes, received {len(got)}, {dt:.2f} s "
          f"({len(payload)/dt/1024:.1f} KiB/s)")
    if ok:
        print("    echo byte-for-byte IDENTICAL")
    else:
        # characterise the mismatch rather than just failing
        n = min(len(got), len(payload))
        first = next((i for i in range(n) if got[i] != payload[i]), n)
        print(f"    MISMATCH: {len(payload)-len(got)} byte(s) missing, "
              f"first difference at offset {first}")
    return ok, len(got)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--port', default=os.environ.get('PORT', '/dev/ttyUSB0'),
                    help='serial device (default: $PORT, else /dev/ttyUSB0)')
    args = ap.parse_args()

    ser = serial.Serial(args.port, BAUD, timeout=0.3)
    print(f"opened {args.port} @ {BAUD}")

    banner = drain(ser, 2.0)
    if banner:
        print("banner//pending output:")
        for line in banner.decode('utf-8', 'replace').splitlines():
            if line.strip():
                print(f"    {line.strip()}")

    print()
    print("=== test 1: short echo, 64 bytes ===")
    p1 = bytes(range(0x20, 0x60))
    ok1, _ = echo_test(ser, p1, 64, 0.05, "printable ASCII 0x20-0x5f")

    print()
    print("=== test 2: 2000 bytes, chunked (RX path under normal load) ===")
    p2 = bytes((0x21 + (i % 0x5e)) for i in range(2000))
    ok2, _ = echo_test(ser, p2, 64, 0.01, "2000 bytes in 64-byte chunks")

    print()
    print("=== test 3: 8000 bytes, sustained (interrupt load) ===")
    p3 = bytes((0x21 + (i % 0x5e)) for i in range(8000))
    ok3, got3 = echo_test(ser, p3, 256, 0.004, "8000 bytes in 256-byte chunks")

    print()
    print("=== firmware counters ===")
    ser.reset_input_buffer()
    ser.write(STATS_REQ)
    ser.flush()
    buf = read_until(ser, b'STATS', 5.0)
    stats = [l.strip() for l in buf.decode('utf-8', 'replace').splitlines() if 'STATS' in l]
    for s in stats:
        print(f"    {s}")

    total_sent = len(p1) + len(p2) + len(p3) + 1  # +1 for the STATS_REQ byte
    print(f"    host sent {total_sent} bytes total (including the 0x04 request)")

    ser.close()
    print()
    print("RESULT:", "all echo tests passed" if (ok1 and ok2 and ok3) else "at least one echo test FAILED")
    return 0 if (ok1 and ok2 and ok3) else 1


if __name__ == '__main__':
    sys.exit(main())
