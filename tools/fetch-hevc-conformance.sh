#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Fetch six small, original FFmpeg FATE conformance streams for 8-bit 4:2:0.
# The .h265 suffix is for decode-matrix.sh; every byte remains unchanged.
set -eu

TOOLS_DIR=${0%/*}
[ "$TOOLS_DIR" != "$0" ] || TOOLS_DIR=.
DRIVER_DIR=$(CDPATH= cd -- "$TOOLS_DIR/.." && pwd)
OUT="$DRIVER_DIR/test-results/hevc-conformance"
FATE_COMMIT=045c8006d1c10d7196e2e1c33d4de651a401960d
SAMPLE_BASE=https://fate-suite.ffmpeg.org/hevc-conformance
SOURCE_BASE="https://raw.githubusercontent.com/FFmpeg/FFmpeg/$FATE_COMMIT"
MAX_FILE_BYTES=524288

if [ "$#" -ne 0 ]; then
    printf 'Usage: sh tools/fetch-hevc-conformance.sh\n' >&2
    exit 2
fi

for TOOL in curl sha256sum wc tr; do
    command -v "$TOOL" >/dev/null 2>&1 || {
        printf 'Required tool missing: %s\n' "$TOOL" >&2
        exit 1
    }
done
mkdir -p "$OUT"

download()
{
    curl --fail --silent --show-error --location --retry 2 \
        --connect-timeout 15 --max-time 60 --max-filesize "$MAX_FILE_BYTES" \
        "$1" -o "$2"
}

verify_sample()
{
    [ -f "$1" ] &&
        [ "$(wc -c < "$1" | tr -d '[:space:]')" = "$2" ] &&
        printf '%s  %s\n' "$3" "$1" | sha256sum --check --status
}

cat > "$OUT/SELECTION.txt" <<EOF
Selection checked on 2026-09-21.
Official FATE list: $SOURCE_BASE/tests/fate/hevc.mak
Official reference results: $SOURCE_BASE/tests/ref/fate/hevc-conformance-<NAME>
Original streams: $SAMPLE_BASE/<NAME>.bit
Saved streams: <NAME>.h265, identical bytes and SHA-256.
Total compressed bytes: 1425179. Total decoded frames: 739.
All six streams: Main profile, chroma_format_idc=1, both bit_depth_minus8=0.
FFmpeg 9.0.1 software decoding matched the official FATE per-frame size and
pixel CRC for all 739 frames; timestamps were not part of that comparison.
This script verifies file size and SHA-256. Hardware results are recorded
separately by the board decoder tests. See docs/hevc-conformance-samples.md.
EOF

printf 'name\tbytes\tsha256\twidth\theight\tframes\tfeatures\tsample_url\tfate_reference_url\n' > "$OUT/SOURCES.tsv"
: > "$OUT/SHA256SUMS"

while read -r NAME BYTES HASH WIDTH HEIGHT FRAMES FEATURES; do
    MEDIA="$OUT/$NAME.h265"
    REF_URL="$SOURCE_BASE/tests/ref/fate/hevc-conformance-$NAME"
    if ! verify_sample "$MEDIA" "$BYTES" "$HASH"; then
        PART="$MEDIA.part.$$"
        download "$SAMPLE_BASE/$NAME.bit" "$PART"
        if ! verify_sample "$PART" "$BYTES" "$HASH"; then
            printf 'File size or SHA-256 mismatch: %s\n' "$NAME" >&2
            exit 1
        fi
        mv "$PART" "$MEDIA"
    fi
    download "$REF_URL" "$OUT/$NAME.fate.framecrc.part.$$"
    mv "$OUT/$NAME.fate.framecrc.part.$$" "$OUT/$NAME.fate.framecrc"
    printf '%s  %s.h265\n' "$HASH" "$NAME" >> "$OUT/SHA256SUMS"
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$NAME" "$BYTES" "$HASH" "$WIDTH" "$HEIGHT" "$FRAMES" \
        "$FEATURES" "$SAMPLE_BASE/$NAME.bit" "$REF_URL" >> "$OUT/SOURCES.tsv"
    printf '%s: %s bytes, %sx%s, %s frames, SHA-256 verified\n' \
        "$NAME" "$BYTES" "$WIDTH" "$HEIGHT" "$FRAMES"
done <<'SAMPLES'
TILES_A_Cisco_2 484767 eff78a401ecccc21d995345988f1be60ee76604cf10fa39d421c3e00668a94d6 1920 1080 100 tiles-5x5;nonuniform
SLICES_A_Rovi_3 65943 7440908beaa68768ee66b7af5823a28ce0716d90aa1b49de630d2c5aa555d955 640 480 9 20-independent-slices-per-picture
WPP_D_ericsson_MAIN_2 22474 30eec63f2324aa982fb91bd4c1c551c833c253ba711ee131aa9cc4d322398caf 64 240 48 WPP;dependent-slices;SAO;temporal-MVP
TMVP_A_MS_3 17238 33556aa42355ba43575a6e6f1b579420a9217f576fc0ae07ee57fa2750a38d52 416 240 17 temporal-MVP-toggle;SAO
SLIST_B_Sony_8 344203 94ac8ba55b5a528618720aab5f1a2a192a044a760c007d2f33dc1f89bc0e9833 832 480 65 SPS-and-PPS-scaling-lists
LTRPSPS_A_Qualcomm_1 490554 8f8e50fa408b7e96e76e6936b380fc34c208271efb8c283b15a42cb1f8c78ae7 416 240 500 long-term-references;SAO;temporal-MVP
SAMPLES

download "$SOURCE_BASE/tests/fate/hevc.mak" "$OUT/hevc.mak.part.$$"
mv "$OUT/hevc.mak.part.$$" "$OUT/hevc.mak"
printf 'Samples and source records: %s\n' "$OUT"
