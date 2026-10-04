"""Host side of the runtime UART reconfigure test.

Samples the console at 115200, then at 9600, then at 115200 again, following the
firmware's phase announcements. Seeing readable text at 9600 and only at 9600
during phase 2 is the proof that uart_configure() actually changed the wire rate.

Start this before the cell: it follows phase 1 until the announcements stop,
which is the moment the firmware switches to 9600, and times the rest from
there. A listener that attaches after phase 1 has nothing to follow.
"""
import argparse
import os
import time

import serial

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--port', default=os.environ.get('PORT', '/dev/ttyUSB0'),
                help='serial device (default: $PORT, else /dev/ttyUSB0)')
ap.add_argument('--timeout', type=float, default=60.0,
                help='seconds to wait for phase 1 (default: 60)')
args = ap.parse_args()
PORT, TIMEOUT = args.port, args.timeout

# Phase 1 is announced every 250 ms; this much silence means it has ended.
PHASE1_GAP = 0.6


class Listener:
    """The console at one rate, split into well-formed RECONF lines."""

    def __init__(self, baud):
        self.baud = baud
        self.ser = serial.Serial(PORT, baud, timeout=0.05)
        self.buf = b''
        self.lines = []

    def poll(self):
        """Return the lines completed since the last call."""
        # Read what has arrived, or block for one byte: read(n) waits for all
        # n bytes or the timeout, which would delay noticing a line.
        self.buf += self.ser.read(self.ser.in_waiting or 1)
        *done, self.buf = self.buf.split(b'\n')
        new = [line.decode('utf-8', 'replace').strip() for line in done]
        new = [line for line in new if line]
        self.lines += new
        return new

    def listen(self, secs):
        end = time.time() + secs
        while time.time() < end:
            self.poll()

    def close(self, label):
        self.ser.close()
        clean = [line for line in self.lines if line.startswith('RECONF')]
        print(f"  [{label}] at {self.baud}: "
              f"{len(self.lines)} line(s), {len(clean)} well-formed RECONF")
        for line in dict.fromkeys(clean[:4]):
            print(f"      {line}")
        if self.lines and not clean:
            print(f"      (garbled, e.g. {self.lines[0][:50]!r})")
        return clean


def sample(baud, secs, label):
    rx = Listener(baud)
    time.sleep(0.2)
    rx.ser.reset_input_buffer()
    rx.listen(secs)
    return rx.close(label)


print(f"listening on {PORT} at 115200; start or restart the cell now")
print()
print("=== phase 1: expect RECONF at 115200 until the firmware switches ===")
rx = Listener(115200)
end = time.time() + TIMEOUT
last = None
while time.time() < end:
    if any('phase1' in line for line in rx.poll()):
        last = time.time()
    if last is not None and time.time() - last > PHASE1_GAP:
        break
p1 = rx.close("115200")
if last is None:
    print()
    print("FAIL  no phase 1 at 115200. Start this script before the cell, or")
    print("      restart the cell while it listens. Check nothing else holds the")
    print(f"      port: fuser {PORT}")
    raise SystemExit(1)

print()
print("=== phase 2: firmware switches to 9600 ===")
print("  first, confirm it is NO LONGER readable at 115200:")
p2_wrong = sample(115200, 1.5, "115200")
print("  now listen at 9600:")
p2 = sample(9600, 1.6, "9600")

print()
print("=== phase 3: firmware restores 115200 ===")
p3 = sample(115200, 5.0, "115200")

failed = [line for line in p1 + p2_wrong + p2 + p3 if 'FAILED' in line]
if failed:
    print()
    for line in failed:
        print(f"  firmware: {line}")

print()
ok_1 = any('phase1' in line for line in p1)
ok_2 = any('phase2' in line for line in p2)
ok_2neg = not any('phase2' in line for line in p1 + p2_wrong)
ok_3 = any('phase3' in line for line in p3)

print(f"  phase1 readable at 115200        : {'PASS' if ok_1 else 'FAIL'}")
print(f"  phase2 readable at 9600          : {'PASS' if ok_2 else 'FAIL'}")
print(f"  phase2 NOT readable at 115200    : {'PASS' if ok_2neg else 'FAIL'}")
print(f"  phase3 readable at 115200 again  : {'PASS' if ok_3 else 'FAIL'}")
print()
print("RESULT:", "runtime reconfigure works on the wire"
      if (ok_1 and ok_2 and ok_2neg and ok_3) else "reconfigure did NOT behave as expected")
