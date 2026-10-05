#!/bin/sh
# Copyright (c) 2026 MediaTek Inc.
# SPDX-License-Identifier: Apache-2.0
#
# Runs ON THE BOARD, as root, under Linux without Jailhouse:
#
#   run_linux.sh <cpu> <log> [routes-script]
#
# For 2, 16 and 32 channels: sets the eTDM mixer routes, runs
# etdm_latency_bench -S pinned to <cpu> at SCHED_FIFO 90, and appends the
# BENCH lines to <log>.  Both cpufreq policies are held at "performance" for
# the run and put back afterwards.  The routes script takes 2, 16, 32 or off.
# BENCH names the bench binary (default: etdm_latency_bench on the PATH).
set -e

CPU=${1:?usage: run_linux.sh <cpu> <log> [routes-script]}
LOG=${2:?usage: run_linux.sh <cpu> <log> [routes-script]}
ROUTES=${3:-/root/yocto-agent/etdm-routes.sh}
BENCH=${BENCH:-etdm_latency_bench}

if [ -d /sys/devices/jailhouse ]; then
	echo "Jailhouse is enabled; this bench is for Linux native" >&2
	exit 1
fi

saved=""
for p in /sys/devices/system/cpu/cpufreq/policy*; do
	saved="$saved $p:$(cat "$p/scaling_governor")"
	echo performance > "$p/scaling_governor"
done
restore() {
	for e in $saved; do
		echo "${e#*:}" > "${e%%:*}/scaling_governor"
	done
	sh "$ROUTES" off >/dev/null 2>&1 || true
}
trap restore EXIT

: > "$LOG"
for ch in 2 16 32; do
	sh "$ROUTES" "$ch"
	"$BENCH" -S -c "$ch" -a "$CPU" -r 90 | tee -a "$LOG"
done
echo "BENCH done" | tee -a "$LOG"
