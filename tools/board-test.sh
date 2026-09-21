#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# GStreamer performs V4L2 Request API hardware decoding. FFmpeg reference
# files must be generated with explicit software decoders by make-fixtures.sh.
set -eu
TOOLS_DIR=${0%/*}
[ "$TOOLS_DIR" != "$0" ] || TOOLS_DIR=.
DRIVER_DIR=$(CDPATH= cd -- "$TOOLS_DIR/.." && pwd)
RESULTS=${1:-"$DRIVER_DIR/test-results"}
FIXTURES="$RESULTS/fixtures"
mkdir -p "$RESULTS"
export GST_REGISTRY="$RESULTS/gstreamer-registry.bin"
export GST_DEBUG_NO_COLOR=1

decode()
{
    STEM=$1
    LABEL=$2
    case "$STEM" in
        h264-*) PARSER=h264parse; DECODER=v4l2slh264dec; EXT=h264 ;;
        hevc-*) PARSER=h265parse; DECODER=v4l2slh265dec; EXT=h265 ;;
        *) return 1 ;;
    esac
    # Planar round-trip removes padding represented by GstVideoMeta. It
    # performs a lossless NV12/I420 packing change, with no software decoder.
    timeout -k 5 30 gst-launch-1.0 -q filesrc location="$FIXTURES/$STEM.$EXT" \
        ! "$PARSER" ! identity sleep-time=1000 ! "$DECODER" \
        ! videoconvert ! video/x-raw,format=I420 \
        ! videoconvert ! video/x-raw,format=NV12 \
        ! filesink location="$RESULTS/$LABEL.hw.nv12" \
        > "$RESULTS/$LABEL.gst.log" 2>&1
    cmp "$RESULTS/$LABEL.hw.nv12" "$FIXTURES/$STEM.sw.nv12"
    printf 'PASS %s pixel comparison\n' "$LABEL"
}

for STEM in h264-i hevc-i h264-ipb hevc-ipb; do
    decode "$STEM" "$STEM"
done
for STEM in h264-1080 hevc-1080; do
    if [ -s "$FIXTURES/$STEM.sw.nv12" ]; then
        decode "$STEM" "$STEM"
    fi
done
decode h264-ipb h264-mixed &
H264_PID=$!
decode hevc-ipb hevc-mixed &
HEVC_PID=$!
STATUS=0
wait "$H264_PID" || STATUS=1
wait "$HEVC_PID" || STATUS=1
[ "$STATUS" -eq 0 ] || exit "$STATUS"

cc -std=c11 -O2 -Wall -Wextra -Werror -o "$DRIVER_DIR/tools/request-test" \
    "$DRIVER_DIR/tools/request-test.c"
"$DRIVER_DIR/tools/request-test" --workers 2 --iterations 10 --data-offset 13 \
    --malformed > "$RESULTS/request-concurrent.log" 2>&1
tail -1 "$RESULTS/request-concurrent.log"
sha256sum "$RESULTS/"*.hw.nv12
