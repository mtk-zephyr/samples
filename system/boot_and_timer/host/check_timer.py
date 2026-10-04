#!/usr/bin/env python3
"""Host side of the boot and timer-accuracy sample.

Stamps every console line with the host's clock on arrival and measures the
five-second sleeps against it.  The target cannot do this for itself:
k_uptime_get() derives from the same timer, so a misconfigured one would be
measuring its own error as zero.

Owns the serial port for the duration -- nothing else may hold it, because a
second reader steals bytes and looks exactly like a dead console.
"""
import argparse
import os
import re
import sys
import time

import serial

RE_BOARD = re.compile(r'BOOT board=(\S+)')
RE_CNTFRQ = re.compile(r'BOOT cntfrq=(\d+)')
RE_START = re.compile(r'SLEEP_START (\d+)')
RE_END = re.compile(r'SLEEP_END\s+(\d+)')


def capture(port, baud, timeout):
    """Read lines until BOOT DONE, returning (arrival_time, text) pairs."""
    ser = serial.Serial(port, baud, timeout=0.05)
    lines, buf = [], b''
    end = time.time() + timeout
    while time.time() < end:
        # Read what has arrived, or block for one byte: read(n) waits for all
        # n bytes or the timeout, which would stamp a line at the end of the
        # wait rather than on arrival.
        chunk = ser.read(ser.in_waiting or 1)
        if not chunk:
            continue
        now = time.time()
        buf += chunk
        while b'\n' in buf:
            raw, buf = buf.split(b'\n', 1)
            lines.append((now, raw.decode('utf-8', 'replace').strip()))
        if any('BOOT DONE' in text for _, text in lines):
            break
    ser.close()
    return lines


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--port', default=os.environ.get('PORT', '/dev/ttyUSB0'),
                    help='serial device (default: $PORT, else /dev/ttyUSB0)')
    ap.add_argument('--baud', type=int, default=115200)
    ap.add_argument('--board', help='board target the image was built for')
    ap.add_argument('--cntfrq', type=int, default=13000000,
                    help='expected counter frequency in Hz (default: 13000000)')
    ap.add_argument('--tolerance-ms', type=int, default=100,
                    help='allowed deviation from 5 s (default: 100)')
    ap.add_argument('--timeout', type=float, default=45.0)
    args = ap.parse_args()

    print(f"listening on {args.port} @ {args.baud}; start or restart the cell now")
    lines = capture(args.port, args.baud, args.timeout)
    if not lines:
        print("\nFAIL  no console output at all.")
        print("      Check that the cell is running and that nothing else holds the port:")
        print(f"        fuser {args.port}")
        print("        adb shell 'jailhouse cell list'")
        return 1

    text = [t for _, t in lines]
    failures = []

    board = next((m.group(1) for m in map(RE_BOARD.search, text) if m), None)
    if board is None:
        failures.append("no BOOT board= line -- is this the right image?")
        print("  board target      not seen")
    elif args.board and board != args.board:
        failures.append(f"board target is {board}, expected {args.board}")
        print(f"  board target      {board}   WRONG, expected {args.board}")
    else:
        print(f"  board target      {board}")

    cntfrq = next((int(m.group(1)) for m in map(RE_CNTFRQ.search, text) if m), None)
    if cntfrq == args.cntfrq:
        print(f"  cntfrq            {cntfrq} Hz")
    else:
        failures.append(f"cntfrq is {cntfrq}, expected {args.cntfrq}")
        print(f"  cntfrq            {cntfrq}   WRONG, expected {args.cntfrq}")

    starts, ends = {}, {}
    for when, t in lines:
        m = RE_START.search(t)
        if m:
            starts[m.group(1)] = when
        m = RE_END.search(t)
        if m:
            ends[m.group(1)] = when

    print(f"  sleeps            {len(starts)} started, {len(ends)} completed")
    worst, worst_i = 0.0, None
    for key in sorted(starts):
        if key not in ends:
            continue
        dev_ms = (ends[key] - starts[key] - 5.0) * 1000.0
        flag = "" if abs(dev_ms) <= args.tolerance_ms else "   OUT OF TOLERANCE"
        print(f"    iteration {key}       {dev_ms:+7.1f} ms from 5 s{flag}")
        if abs(dev_ms) > abs(worst):
            worst, worst_i = dev_ms, key

    if not ends:
        failures.append("no completed sleeps")
    elif abs(worst) > args.tolerance_ms:
        failures.append(f"worst deviation {worst:+.1f} ms exceeds {args.tolerance_ms} ms")
        if worst_i == '0':
            print()
            print("  Note: only iteration 0 is out of tolerance.  On a first boot the")
            print("  banner, cntfrq and SLEEP_START 0 can arrive in one burst once the")
            print("  console comes up, which makes that interval read short.  Re-run")
            print("  before believing it; iterations 1 and 2 are the reliable ones.")

    print()
    if failures:
        print("RESULT: FAIL")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("RESULT: PASS")
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
