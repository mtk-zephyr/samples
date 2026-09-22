"""Host side of the runtime UART reconfigure test.

Samples the console at 115200, then at 9600, then at 115200 again, following the
firmware's phase announcements. Seeing readable text at 9600 and only at 9600
during phase 2 is the proof that uart_configure() actually changed the wire rate.
"""
import argparse
import os
import time

import serial

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--port', default=os.environ.get('PORT', '/dev/ttyUSB0'),
                help='serial device (default: $PORT, else /dev/ttyUSB0)')
PORT = ap.parse_args().port


def sample(baud, secs, label):
    ser = serial.Serial(PORT, baud, timeout=0.3)
    time.sleep(0.2)
    ser.reset_input_buffer()
    end = time.time() + secs
    buf = b''
    while time.time() < end:
        buf += ser.read(4096)
    ser.close()

    text = buf.decode('utf-8', 'replace')
    lines = [l.strip() for l in text.splitlines() if l.strip()]
    clean = [l for l in lines if l.startswith('RECONF')]
    print(f"  [{label}] listening at {baud} for {secs}s: "
          f"{len(lines)} line(s), {len(clean)} well-formed RECONF")
    for l in dict.fromkeys(clean[:4]):
        print(f"      {l}")
    if lines and not clean:
        print(f"      (garbled, e.g. {lines[0][:50]!r})")
    return clean


print("=== phase 1: expect RECONF at 115200 ===")
p1 = sample(115200, 3.5, "115200")

print()
print("=== phase 2: firmware switches to 9600 ===")
print("  first, confirm it is NO LONGER readable at 115200:")
p2_wrong = sample(115200, 2.0, "115200")
print("  now listen at 9600:")
p2 = sample(9600, 3.0, "9600")

print()
print("=== phase 3: firmware restores 115200 ===")
p3 = sample(115200, 4.0, "115200")

print()
ok_1 = any('phase1' in l for l in p1)
ok_2 = any('phase2' in l for l in p2)
ok_2neg = not any('phase2' in l for l in p2_wrong)
ok_3 = any('phase3' in l for l in p3)

print(f"  phase1 readable at 115200        : {'PASS' if ok_1 else 'FAIL'}")
print(f"  phase2 readable at 9600          : {'PASS' if ok_2 else 'FAIL'}")
print(f"  phase2 NOT readable at 115200    : {'PASS' if ok_2neg else 'FAIL'}")
print(f"  phase3 readable at 115200 again  : {'PASS' if ok_3 else 'FAIL'}")
print()
print("RESULT:", "runtime reconfigure works on the wire"
      if (ok_1 and ok_2 and ok_2neg and ok_3) else "reconfigure did NOT behave as expected")
