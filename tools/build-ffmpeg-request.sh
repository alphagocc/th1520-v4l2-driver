#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Build the supplied FFmpeg V4L2 Request archive in the project directory.
# Usage: sh tools/build-ffmpeg-request.sh [ARCHIVE.zip]
# Requires the temporary dependency sysroot described in docs/ffmpeg-request.md.
# No package installation, module loading, or system configuration changes.
set -eu

TOOLS_DIR=${0%/*}
[ "$TOOLS_DIR" != "$0" ] || TOOLS_DIR=.
DRIVER_DIR=$(CDPATH= cd -- "$TOOLS_DIR/.." && pwd)
[ "$#" -le 1 ] || { printf 'Usage: %s [ARCHIVE.zip]\n' "$0" >&2; exit 1; }
ARCHIVE=${1:-"$DRIVER_DIR/build/FFmpeg-v4l2-request-n8.1.zip"}
SOURCE_ROOT="$DRIVER_DIR/build/ffmpeg-request-source"
SOURCE="$SOURCE_ROOT/ffmpeg"
BUILD="$DRIVER_DIR/build/ffmpeg-request-build"
SYSROOT="$DRIVER_DIR/build/ffmpeg-request-runtime/sysroot"
JOBS=${JOBS:-4}

case "$JOBS" in
    ''|*[!0-9]*|0) printf 'JOBS must be a positive integer.\n' >&2; exit 1 ;;
esac
[ "$(uname -s)" = Linux ] || { printf 'Run this build on Linux.\n' >&2; exit 1; }
for COMMAND in python3 pkg-config make cc sha256sum; do
    command -v "$COMMAND" >/dev/null 2>&1 || {
        printf 'Required command is missing: %s\n' "$COMMAND" >&2
        exit 1
    }
done

python3 - "$DRIVER_DIR" "$ARCHIVE" <<'PY'
import hashlib
import json
from pathlib import Path, PurePosixPath
import stat
import sys
import zipfile

expected = "d98b03c25d755015aa0e6a943069241b2c54e5d3ba30369aea76176540f5c352"
driver = Path(sys.argv[1]).resolve()
archive = Path(sys.argv[2]).resolve(strict=True)
build_root = (driver / "build").resolve()
if not build_root.is_relative_to(driver):
    raise SystemExit("Build directory must remain inside the driver directory")

source_root = build_root / "ffmpeg-request-source"
source = source_root / "ffmpeg"
build = build_root / "ffmpeg-request-build"
sysroot = build_root / "ffmpeg-request-runtime" / "sysroot"
for directory in (source_root, source, build, sysroot):
    if not directory.resolve().is_relative_to(build_root):
        raise SystemExit(f"Build destination escapes the project build directory: {directory}")

digest = hashlib.sha256()
with archive.open("rb") as stream:
    for chunk in iter(lambda: stream.read(1024 * 1024), b""):
        digest.update(chunk)
if digest.hexdigest() != expected:
    raise SystemExit(f"Archive SHA-256 mismatch: {digest.hexdigest()}")

reused = (source / "configure").is_file()
if not reused:
    if source_root.exists() and any(source_root.iterdir()):
        raise SystemExit("Existing source directory is incomplete; preserve it and choose a complete source tree")
    with zipfile.ZipFile(archive) as package:
        for entry in package.infolist():
            name = PurePosixPath(entry.filename)
            if (not name.parts or name.parts[0] != "ffmpeg" or
                    name.is_absolute() or ".." in name.parts or
                    "\\" in entry.filename or
                    stat.S_ISLNK(entry.external_attr >> 16)):
                raise SystemExit(f"Unsafe archive entry: {entry.filename}")
            target = source_root.joinpath(*name.parts).resolve()
            if not target.is_relative_to(source_root.resolve()):
                raise SystemExit(f"Archive entry escapes its destination: {entry.filename}")
        source_root.mkdir(parents=True, exist_ok=True)
        package.extractall(source_root)
        # ZipFile does not restore Unix executable bits; FFmpeg's Makefiles
        # invoke ffbuild/version.sh and other helper scripts as executables.
        for entry in package.infolist():
            if not entry.is_dir():
                mode = (entry.external_attr >> 16) & 0o777
                if mode:
                    source_root.joinpath(*PurePosixPath(entry.filename).parts).chmod(mode)

release = (source / "RELEASE").read_text(encoding="utf-8").strip()
if release != "8.1":
    raise SystemExit(f"Unexpected source RELEASE: {release}")
for filename in ("configure", "libavcodec/v4l2_request_h264.c",
                 "libavcodec/v4l2_request_hevc.c",
                 "libavutil/hwcontext_v4l2request.c"):
    if not (source / filename).is_file():
        raise SystemExit(f"Required source file is missing: {filename}")

build.mkdir(parents=True, exist_ok=True)
(build / "source-provenance.json").write_text(json.dumps({
    "archive": str(archive),
    "archive_sha256": digest.hexdigest(),
    "source_directory": str(source),
    "source_release": release,
    "source_reused": reused,
    "existing_source_policy": "Preserve all existing source changes; archive hash does not attest the current source tree",
    "branch_url": "https://code.ffmpeg.org/Kwiboo/FFmpeg/src/branch/v4l2-request-n8.1",
    "commit": None,
    "commit_status": "Unconfirmed: supplied archive has no verified commit identity; branch endpoint returned an anti-bot page during source review",
}, indent=2) + "\n", encoding="utf-8")
print(f"Source: {source}; RELEASE={release}; reused={reused}")
PY

for DEPENDENCY in \
    "$SYSROOT/usr/lib/riscv64-linux-gnu/pkgconfig/libdrm.pc" \
    "$SYSROOT/usr/lib/riscv64-linux-gnu/pkgconfig/libudev.pc" \
    "$SYSROOT/usr/lib/riscv64-linux-gnu/libdrm.so" \
    "$SYSROOT/usr/lib/riscv64-linux-gnu/libudev.so"; do
    [ -r "$DEPENDENCY" ] || {
        printf 'Temporary dependency is missing: %s\nSee %s/docs/ffmpeg-request.md\n' \
            "$DEPENDENCY" "$DRIVER_DIR" >&2
        exit 1
    }
done

export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/riscv64-linux-gnu/pkgconfig:$SYSROOT/usr/share/pkgconfig"
unset PKG_CONFIG_PATH
pkg-config --exists libdrm libudev

cd "$BUILD"
pkg-config --modversion libdrm libudev > dependency-versions.txt
pkg-config --cflags --libs libdrm libudev > dependency-flags.txt

set -- \
    --disable-autodetect --disable-doc --disable-debug --disable-network \
    --disable-everything --disable-asm \
    --enable-ffmpeg --enable-ffprobe --disable-ffplay \
    --enable-libdrm --enable-libudev --enable-v4l2-request \
    --enable-decoder=h264,hevc \
    --enable-hwaccel=h264_v4l2request,hevc_v4l2request \
    --enable-parser=h264,hevc --enable-demuxer=h264,hevc,mov,matroska \
    --enable-protocol=file,pipe --enable-muxer=null,rawvideo,framehash \
    --enable-encoder=rawvideo,wrapped_avframe \
    --enable-filter=buffer,buffersink,null,format,hwdownload,scale \
    --enable-swscale
printf '%s\n' "$@" > configure-arguments.txt
# Use sh because ZIP archives need not preserve configure's executable bit.
sh "$SOURCE/configure" "$@"
make -j"$JOBS"
./ffmpeg -hide_banner -hwaccels > hwaccels.txt
./ffmpeg -version > version.txt
sha256sum ffmpeg ffprobe > SHA256SUMS
printf 'Built %s/ffmpeg and %s/ffprobe; no installation was performed.\n' "$BUILD" "$BUILD"
