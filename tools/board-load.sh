#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Temporary module replacement for tests; no boot configuration changes.
set -eu
[ "$(id -u)" -eq 0 ] || { printf 'Run with sudo.\n' >&2; exit 1; }
TOOLS_DIR=${0%/*}
[ "$TOOLS_DIR" != "$0" ] || TOOLS_DIR=.
DRIVER_DIR=$(CDPATH= cd -- "$TOOLS_DIR/.." && pwd)
HELPERS="$DRIVER_DIR/build/helpers-$(uname -r)"

if [ -d /sys/module/th1520_vdec ]; then
    rmmod th1520_vdec
fi
if [ -d /sys/module/hantrodec ]; then
    modprobe -r hantrodec
fi
modprobe videodev
modprobe videobuf2-dma-contig
modprobe videobuf2-v4l2
if [ ! -d /sys/module/v4l2_mem2mem ]; then
    insmod "$HELPERS/v4l2-mem2mem.ko"
fi
if [ ! -d /sys/module/v4l2_h264 ]; then
    insmod "$HELPERS/v4l2-h264.ko"
fi
if [ ! -d /sys/module/v4l2_vp9 ]; then
    insmod "$HELPERS/v4l2-vp9.ko"
fi
insmod "$DRIVER_DIR/th1520-vdec.ko" "$@"
v4l2-ctl --list-devices
