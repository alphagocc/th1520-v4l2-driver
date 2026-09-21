#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Valid 8-bit 4:2:0 HEVC streams, with software-decoded pixel references.
set -eu
[ "$#" -le 1 ] || { printf 'Usage: %s [OUTPUT_DIRECTORY]\n' "$0" >&2; exit 2; }
TOOLS_DIR=${0%/*}
[ "$TOOLS_DIR" != "$0" ] || TOOLS_DIR=.
DRIVER_DIR=$(CDPATH= cd -- "$TOOLS_DIR/.." && pwd)
OUT=${1:-"$DRIVER_DIR/test-results/hevc-matrix"}
mkdir -p "$OUT"
MANIFEST="$OUT/encode-cases.tsv"
printf 'case\tsize\tframes\tprofile\tx265_params\n' > "$MANIFEST"

encode()
{
    NAME=$1 SIZE=$2 FRAMES=$3 PROFILE=$4 PARAMS=$5
    ALL_PARAMS="pools=1:frame-threads=1:log-level=info:$PARAMS"
    printf '%s\t%s\t%s\t%s\t%s\n' "$NAME" "$SIZE" "$FRAMES" "$PROFILE" "$ALL_PARAMS" >> "$MANIFEST"
    ffmpeg -hide_banner -loglevel info -y -f lavfi \
        -i "testsrc2=size=$SIZE:rate=30" -frames:v "$FRAMES" -pix_fmt yuv420p \
        -c:v libx265 -preset ultrafast -profile:v "$PROFILE" \
        -x265-params "$ALL_PARAMS" -f hevc "$OUT/$NAME.h265" \
        > "$OUT/$NAME.encode.log" 2>&1
    if grep -qi 'unknown option\|unrecognized option\|error parsing option' "$OUT/$NAME.encode.log"; then
        printf 'Encoder did not accept parameters for %s\n' "$NAME" >&2
        exit 1
    fi
    ffmpeg -hide_banner -loglevel error -y -c:v hevc -i "$OUT/$NAME.h265" \
        -pix_fmt nv12 -f rawvideo "$OUT/$NAME.sw.nv12" \
        > "$OUT/$NAME.software.log" 2>&1
    ffmpeg -hide_banner -loglevel info -c:v hevc -i "$OUT/$NAME.h265" \
        -map 0:v:0 -c:v copy -bsf:v trace_headers -f null - \
        > "$OUT/$NAME.headers.log" 2>&1
    printf 'Generated %s\n' "$NAME"
}

encode hevc-still-128x96 128x96 1 mainstillpicture 'total-frames=1:keyint=1:bframes=0:ref=1:wpp=0:sao=0'
encode hevc-crop-322x242 322x242 12 main 'keyint=12:bframes=3:ref=4:ctu=32:min-cu-size=8:wpp=0:sao=1'
encode hevc-ctu16-wpp 640x360 12 main 'keyint=12:bframes=2:ref=3:ctu=16:min-cu-size=8:max-tu-size=16:wpp=1:sao=1'
encode hevc-ctu32-nosao 720x576 12 main 'keyint=12:bframes=3:ref=4:ctu=32:wpp=1:sao=0'
encode hevc-refs6-720p 1280x720 16 main 'keyint=16:bframes=3:ref=6:wpp=1:sao=1'
encode hevc-slices4-1080p 1920x1080 8 main 'keyint=8:bframes=2:ref=3:slices=4:wpp=1:sao=1'
encode hevc-uhd 3840x2160 4 main 'keyint=4:bframes=2:ref=3:ctu=64:wpp=1:sao=1'
encode hevc-scaling-default 640x480 12 main 'keyint=12:bframes=2:ref=3:scaling-list=default:wpp=1:sao=1'
encode hevc-lossless 320x240 8 main 'keyint=8:bframes=2:ref=3:lossless=1:wpp=0:sao=0'
sha256sum "$OUT/"*.h265 "$OUT/"*.sw.nv12 > "$OUT/SHA256SUMS"
