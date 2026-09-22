#!/bin/sh
# Bring a Zephyr image up as a Jailhouse inmate on a Genio EVK.
#
# Runs ON THE BOARD, not on your workstation:
#
#   adb push zephyr.bin           /root/zephyr.bin
#   adb push tools/genio-inmate.sh /root/genio-inmate.sh
#   adb shell 'sh /root/genio-inmate.sh /root/zephyr.bin'
#
# The board is detected from its hostname, so the same script serves the
# Genio 700 EVK and the Genio 510 EVK.
#
# Two things here cost a debugging cycle each, and are why this is a script
# rather than a line in a README:
#
#  1. `jailhouse cell list` exits 0 even when Jailhouse is NOT enabled -- it
#     just prints nothing.  A guard written as
#     `jailhouse cell list >/dev/null || enable` therefore skips the enable,
#     and the failure surfaces two commands later as
#     "JAILHOUSE_CELL_CREATE: Invalid argument", which points at the cell
#     config rather than at the missing root cell.  Test for an actual cell
#     row instead.
#  2. `jailhouse enable` is required from cold on both boards.  Nothing in the
#     image brings the root cell up for you.
set -e

IMAGE=${1:?usage: genio-inmate.sh <image.bin> [inmate-cell]}
CELLS=${CELL_DIR:-/usr/share/jailhouse/cells}

case "$(uname -n)" in
genio-700-evk) ROOT_CELL=genio-700-evk.cell;   DEFAULT_INMATE=genio-700-evk-zephyr.cell ;;
genio-510-evk) ROOT_CELL=genio-510-evk.cell;   DEFAULT_INMATE=genio-510-evk-zephyr.cell ;;
*) echo "unknown board '$(uname -n)' -- name the cells by hand" >&2; exit 1 ;;
esac

INMATE=${2:-$DEFAULT_INMATE}

modprobe jailhouse 2>/dev/null || true

if ! jailhouse cell list 2>/dev/null | grep -qE "^[0-9]"; then
	jailhouse enable "$CELLS/$ROOT_CELL"
fi

jailhouse cell shutdown zephyr 2>/dev/null || true
jailhouse cell destroy  zephyr 2>/dev/null || true

jailhouse cell create "$CELLS/$INMATE"
# 0x8000 is where the inmate cell maps the window the image is linked for.
jailhouse cell load   zephyr "$IMAGE" -a 0x00008000
jailhouse cell start  zephyr

jailhouse cell list
