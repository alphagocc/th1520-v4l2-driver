#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Generate continuous, synthetic 8-bit 4:2:0 decode-throughput inputs.
# Usage: sh tools/make-benchmark-fixtures.sh [OUTPUT_DIR]
# FRAMES_1080 defaults to 600; INCLUDE_4K=1 also creates the optional 4K inputs.
# Requires ffmpeg with libx264/libx265, ffprobe, python3, and sha256sum.
set -eu

OUT=${1:-test-results/benchmark-fixtures}
THREADS=${THREADS:-4}
FRAMES_1080=${FRAMES_1080:-600}
INCLUDE_4K=${INCLUDE_4K:-0}
case "$FRAMES_1080" in ''|*[!0-9]*|0) printf 'FRAMES_1080 must be positive.\n' >&2; exit 1;; esac
case "$INCLUDE_4K" in 0|1) ;; *) printf 'INCLUDE_4K must be 0 or 1.\n' >&2; exit 1;; esac
mkdir -p "$OUT"
ffmpeg -version > "$OUT/ffmpeg-version.txt"
printf 'name\tcodec\twidth\theight\tframes\tencoder_parameters\n' > "$OUT/manifest.tsv"

encode()
{
    codec=$1
    size=$2
    frames=$3
    width=${size%x*}
    height=${size#*x}
    name=$codec-$size
    if [ "$codec" = h264 ]; then
        ext=h264
        encoder=libx264
        preset=veryfast
        quality=-qp
        parameter_flag=-x264-params
        parameters="threads=$THREADS:keyint=60:min-keyint=60:scenecut=0:open-gop=0:repeat-headers=1:aud=1:bframes=2:b-adapt=0:ref=3"
        profile=high
        quantizer=24
    else
        ext=h265
        encoder=libx265
        preset=ultrafast
        quality=-crf
        parameter_flag=-x265-params
        parameters="pools=$THREADS:frame-threads=1:keyint=60:min-keyint=60:scenecut=0:open-gop=0:repeat-headers=1:aud=1:bframes=2:b-adapt=0:ref=3:ctu=64:wpp=1:sao=1"
        profile=main
        quantizer=28
    fi
    printf 'ENCODE %s: %s frames\n' "$name" "$frames"
    ffmpeg -hide_banner -loglevel info -nostdin -y \
        -f lavfi -i "testsrc2=size=$size:rate=30" -frames:v "$frames" \
        -an -pix_fmt yuv420p -c:v "$encoder" -preset "$preset" \
        -profile:v "$profile" "$quality" "$quantizer" \
        "$parameter_flag" "$parameters" -f "$codec" "$OUT/$name.$ext" \
        > "$OUT/$name.encode.log" 2>&1
    ffprobe -v error -f "$codec" -count_frames -select_streams v:0 \
        -show_entries stream=codec_name,profile,width,height,pix_fmt,nb_read_frames \
        -of json "$OUT/$name.$ext" > "$OUT/$name.probe.json"
    actual=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["streams"][0]["nb_read_frames"])' \
        "$OUT/$name.probe.json")
    [ "$actual" = "$frames" ] || { printf 'Unexpected frame count: %s\n' "$actual" >&2; exit 1; }
    printf '%s\t%s\t%s\t%s\t%s\t%s %s=%s %s\n' \
        "$name" "$codec" "$width" "$height" "$frames" "$preset" \
        "$quality" "$quantizer" "$parameters" >> "$OUT/manifest.tsv"
}

encode h264 1920x1080 "$FRAMES_1080"
encode hevc 1920x1080 "$FRAMES_1080"
if [ "$INCLUDE_4K" = 1 ]; then
    encode h264 3840x2160 300
    encode hevc 3840x2160 300
fi
set -- ./h264-1920x1080.h264 ./hevc-1920x1080.h265
if [ "$INCLUDE_4K" = 1 ]; then
    set -- "$@" ./h264-3840x2160.h264 ./hevc-3840x2160.h265
fi
(cd "$OUT" && sha256sum "$@" > SHA256SUMS)
