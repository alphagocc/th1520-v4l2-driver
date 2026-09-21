#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Deterministic inputs and software-decoded NV12 references for board tests.
set -eu
[ "$#" -le 1 ] || { printf 'Usage: %s [OUTPUT_DIRECTORY]\n' "$0" >&2; exit 2; }
TOOLS_DIR=${0%/*}
[ "$TOOLS_DIR" != "$0" ] || TOOLS_DIR=.
DRIVER_DIR=$(CDPATH= cd -- "$TOOLS_DIR/.." && pwd)
OUT=${1:-"$DRIVER_DIR/test-results/fixtures"}
mkdir -p "$OUT"

ffmpeg -hide_banner -loglevel error -y -f lavfi \
    -i testsrc2=size=320x240:rate=30 -frames:v 1 -pix_fmt yuv420p \
    -c:v libx264 -profile:v baseline -x264-params keyint=1:cabac=0:bframes=0 \
    -f h264 "$OUT/h264-i.h264"
ffmpeg -hide_banner -loglevel error -y -f lavfi \
    -i testsrc2=size=320x240:rate=30 -frames:v 1 -pix_fmt yuv420p \
    -c:v libx265 -x265-params pools=1:frame-threads=1:wpp=0:sao=0:keyint=1:bframes=0:log-level=error \
    -f hevc "$OUT/hevc-i.h265"
ffmpeg -hide_banner -loglevel error -y -f lavfi \
    -i testsrc2=size=320x240:rate=30 -frames:v 30 -pix_fmt yuv420p \
    -c:v libx264 -profile:v high -x264-params keyint=30:cabac=1:bframes=2:ref=3 \
    -f h264 "$OUT/h264-ipb.h264"
ffmpeg -hide_banner -loglevel error -y -f lavfi \
    -i testsrc2=size=320x240:rate=30 -frames:v 30 -pix_fmt yuv420p \
    -c:v libx265 -x265-params pools=1:frame-threads=1:keyint=30:bframes=2:log-level=error \
    -f hevc "$OUT/hevc-ipb.h265"

for INPUT in "$OUT/"*.h264 "$OUT/"*.h265; do
    case "$INPUT" in
        *.h264) DECODER=h264 ;;
        *.h265) DECODER=hevc ;;
    esac
    ffmpeg -hide_banner -loglevel error -y -c:v "$DECODER" -i "$INPUT" \
        -pix_fmt nv12 -f rawvideo "${INPUT%.*}.sw.nv12"
done
sha256sum "$OUT/"*
