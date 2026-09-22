#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Run one command with the project-local HEVC crop plugin. Installs nothing.
set -eu
TOOLS_DIR=${0%/*}
[ "$TOOLS_DIR" != "$0" ] || TOOLS_DIR=.
DRIVER_DIR=$(CDPATH= cd -- "$TOOLS_DIR/.." && pwd)
WORK=${GST_CROP_BUILD:-"$DRIVER_DIR/build/gstreamer-crop"}
[ "$#" -gt 0 ] || { printf 'Usage: sh tools/with-gst-crop.sh COMMAND [ARG...]\n' >&2; exit 2; }
[ -f "$WORK/plugin/libgstv4l2codecs.so" ] || {
    printf 'Build the local plugin with tools/build-gstreamer-crop.sh first.\n' >&2
    exit 1
}
export GST_PLUGIN_PATH_1_0="$WORK/plugin${GST_PLUGIN_PATH_1_0:+:$GST_PLUGIN_PATH_1_0}"
export GST_REGISTRY="$WORK/registry.bin"
exec "$@"
