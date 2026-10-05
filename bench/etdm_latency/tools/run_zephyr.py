#!/usr/bin/env python3
# Copyright (c) 2026 MediaTek Inc.
# SPDX-License-Identifier: Apache-2.0
"""Run the Zephyr eTDM latency bench on a Genio EVK and capture its output.

Pushes the image and samples/tools/genio-inmate.sh, holds the audio power
domain up, starts the image in the given cell, and copies UART1 to a log until
the bench prints "BENCH done" (or the timeout passes).  Needs adb and pyserial,
and the board's Jailhouse enabled or enable-able by genio-inmate.sh.

  run_zephyr.py zephyr.bin genio-510-evk-zephyr-afe.cell bench-a55.log
"""
import argparse
import os
import subprocess
import sys
import time

import serial

HERE = os.path.dirname(os.path.abspath(__file__))
INMATE = os.path.normpath(os.path.join(HERE, '..', '..', '..', 'tools', 'genio-inmate.sh'))
BOARD_DIR = '/root/etdm-bench'
AFE_POWER = '/sys/devices/platform/soc/10b10000.afe/power/control'


def adb(*args):
    return subprocess.run(['adb', *args], check=True, capture_output=True, text=True).stdout


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('image')
    ap.add_argument('cell', help='cell file name in /usr/share/jailhouse/cells')
    ap.add_argument('log')
    ap.add_argument('--port', default=os.environ.get('PORT', '/dev/ttyUSB0'))
    ap.add_argument('--timeout', type=float, default=1800, help='seconds (default 1800)')
    args = ap.parse_args()

    adb('shell', f'mkdir -p {BOARD_DIR}')
    adb('push', args.image, f'{BOARD_DIR}/bench.bin')
    adb('push', INMATE, f'{BOARD_DIR}/genio-inmate.sh')
    adb('shell', f'echo on > {AFE_POWER}')

    ser = serial.Serial(args.port, 115200, timeout=0.05)
    ser.reset_input_buffer()
    out = subprocess.run(['adb', 'shell',
                          f'sh {BOARD_DIR}/genio-inmate.sh {BOARD_DIR}/bench.bin {args.cell}'],
                         capture_output=True, text=True)
    print(out.stdout.strip())
    if out.returncode:
        print(out.stderr.strip(), file=sys.stderr)
        return 1

    end = time.time() + args.timeout
    buf = b''
    done = False
    with open(args.log, 'w') as log:
        while time.time() < end and not done:
            buf += ser.read(ser.in_waiting or 1)
            *lines, buf = buf.split(b'\n')
            for raw in lines:
                line = raw.decode('utf-8', 'replace').rstrip()
                log.write(line + '\n')
                log.flush()
                if line.startswith('BENCH'):
                    print(line, flush=True)
                if line.startswith('BENCH done'):
                    done = True
    ser.close()
    subprocess.run(['adb', 'shell', 'jailhouse cell shutdown zephyr; jailhouse cell destroy zephyr'],
                   capture_output=True)
    print('done' if done else 'TIMEOUT: no "BENCH done"', '->', args.log)
    return 0 if done else 1


if __name__ == '__main__':
    sys.exit(main())
