#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Extend an existing FFmpeg 8.1 Request configuration in a separate directory.
# Usage: sh tools/build-ffmpeg-vp9-request.sh ORIGINAL_DRIVER_DIR [BUILD_DIR]
# Reuses the original source and dependency sysroot; preserves original binaries.
# This script compiles only. No installation, module loading, or device access.
set -eu
[ "$#" -ge 1 ] && [ "$#" -le 2 ] || {
    printf 'Usage: %s ORIGINAL_DRIVER_DIR [BUILD_DIR]\n' "$0" >&2
    exit 2
}
TOOLS_DIR=${0%/*}
[ "$TOOLS_DIR" != "$0" ] || TOOLS_DIR=.
DRIVER_DIR=$(CDPATH= cd -- "$TOOLS_DIR/.." && pwd)
BASE_DRIVER=$(CDPATH= cd -- "$1" && pwd)
BASE_BUILD="$BASE_DRIVER/build/ffmpeg-request-build"
SOURCE="$BASE_DRIVER/build/ffmpeg-request-source/ffmpeg"
SYSROOT="$BASE_DRIVER/build/ffmpeg-request-runtime/sysroot"
BUILD=${2:-"$DRIVER_DIR/build/ffmpeg-request-vp9"}
JOBS=${JOBS:-2}
case "$JOBS" in 1|2) ;; *) printf 'JOBS must be 1 or 2.\n' >&2; exit 2;; esac
[ "$(uname -s)" = Linux ] || { printf 'Run this build on Linux.\n' >&2; exit 1; }
for COMMAND in python3 pkg-config make cc sha256sum; do
    command -v "$COMMAND" >/dev/null 2>&1 || { printf 'Missing command: %s\n' "$COMMAND" >&2; exit 1; }
done

python3 - "$DRIVER_DIR" "$BASE_BUILD" "$SOURCE" "$BUILD" <<'PY'
import hashlib
import json
from pathlib import Path
import sys


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


driver, base, source, build = [Path(arg).resolve() for arg in sys.argv[1:]]
if not build.is_relative_to(driver / "build") or build in (base, source):
    raise SystemExit("New build must be separate and remain inside this driver's build directory")
release = (source / "RELEASE").read_text().strip()
if release != "8.1":
    raise SystemExit(f"Expected FFmpeg 8.1 source, got {release}")
for name in ("configure", "libavcodec/v4l2_request_h264.c", "libavcodec/v4l2_request_hevc.c",
             "libavcodec/v4l2_request_vp9.c", "libavcodec/vp9.c", "libavformat/ivfdec.c",
             "libavutil/hwcontext_v4l2request.c"):
    if not (source / name).is_file():
        raise SystemExit(f"Missing source file: {name}")
arguments = (base / "configure-arguments.txt").read_text().splitlines()
if "--disable-asm" not in arguments or "--enable-v4l2-request" not in arguments:
    raise SystemExit("Original configuration lacks --disable-asm or V4L2 Request")
additions = {"--enable-decoder=": "vp9", "--enable-hwaccel=": "vp9_v4l2request",
             "--enable-parser=": "vp9", "--enable-demuxer=": "ivf"}
changes = []
for prefix, addition in additions.items():
    positions = [i for i, value in enumerate(arguments) if value.startswith(prefix)]
    if len(positions) != 1:
        raise SystemExit(f"Expected one original argument starting with {prefix}")
    index = positions[0]
    values = arguments[index][len(prefix):].split(",")
    original = arguments[index]
    if addition not in values:
        values.append(addition)
    arguments[index] = prefix + ",".join(values)
    changes.append({"before": original, "after": arguments[index]})
build.mkdir(parents=True, exist_ok=True)
(build / "configure-arguments.txt").write_text("\n".join(arguments) + "\n")
(build / "original-configure-arguments.txt").write_text((base / "configure-arguments.txt").read_text())
original_hashes = {name: digest(base / name) for name in ("ffmpeg", "ffprobe")}
(build / "original-binaries.json").write_text(json.dumps(original_hashes, indent=2) + "\n")
source_hashes = [f"{digest(path)}  {path.relative_to(source).as_posix()}"
                 for path in sorted(source.rglob("*")) if path.is_file() and ".git" not in path.parts]
(build / "source-SHA256SUMS").write_text("\n".join(source_hashes) + "\n")
provenance = {"source_directory": str(source), "source_release": release,
              "source_file_count": len(source_hashes),
              "source_manifest_sha256": digest(build / "source-SHA256SUMS"),
              "original_build": str(base), "original_binary_sha256": original_hashes,
              "original_provenance": json.loads((base / "source-provenance.json").read_text()),
              "configuration_changes": changes,
              "policy": "Same existing source and sysroot; separate build; preserve --disable-asm and original binaries."}
(build / "source-provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
print(f"Source={source}; source files={len(source_hashes)}; build={build}", flush=True)
PY

for DEPENDENCY in \
    "$SYSROOT/usr/lib/riscv64-linux-gnu/pkgconfig/libdrm.pc" \
    "$SYSROOT/usr/lib/riscv64-linux-gnu/pkgconfig/libudev.pc" \
    "$SYSROOT/usr/lib/riscv64-linux-gnu/libdrm.so" \
    "$SYSROOT/usr/lib/riscv64-linux-gnu/libudev.so"; do
    [ -r "$DEPENDENCY" ] || { printf 'Missing dependency: %s\n' "$DEPENDENCY" >&2; exit 1; }
done
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/riscv64-linux-gnu/pkgconfig:$SYSROOT/usr/share/pkgconfig"
unset PKG_CONFIG_PATH
cd "$BUILD"
pkg-config --modversion libdrm libudev > dependency-versions.txt
pkg-config --cflags --libs libdrm libudev > dependency-flags.txt
python3 - "$SOURCE" <<'PY'
from pathlib import Path
import subprocess
import sys
arguments = Path("configure-arguments.txt").read_text().splitlines()
with Path("configure.log").open("wb") as log:
    subprocess.run(["sh", str(Path(sys.argv[1]) / "configure"), *arguments],
                   stdout=log, stderr=subprocess.STDOUT, check=True)
PY
printf 'Configuration complete; compiling with make -j%s.\n' "$JOBS"
make -j"$JOBS" > make.log 2>&1
./ffmpeg -version > version.txt 2>&1
./ffmpeg -hide_banner -hwaccels > hwaccels.txt 2>&1
./ffmpeg -hide_banner -decoders > decoders.txt 2>&1
./ffmpeg -hide_banner -demuxers > demuxers.txt 2>&1
sha256sum ffmpeg ffprobe > SHA256SUMS
python3 - "$BASE_BUILD" "$SOURCE" <<'PY'
import hashlib
import json
from pathlib import Path
import re
import sys


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


base, source = map(Path, sys.argv[1:])
configuration = Path("config.h").read_text() + Path("config_components.h").read_text()
required = [f"CONFIG_{codec}_{kind}" for codec in ("H264", "HEVC", "VP9")
            for kind in ("DECODER", "PARSER", "V4L2REQUEST_HWACCEL")]
required += ["CONFIG_IVF_DEMUXER", "CONFIG_V4L2_REQUEST", "CONFIG_VP9_SUPERFRAME_SPLIT_BSF"]
for macro in required:
    if not re.search(rf"^#define {macro} 1$", configuration, flags=re.M):
        raise SystemExit(f"Missing enabled configuration: {macro}")
if "v4l2request" not in Path("hwaccels.txt").read_text():
    raise SystemExit("Built binary does not list v4l2request")
original_hashes = json.loads(Path("original-binaries.json").read_text())
for name, expected in original_hashes.items():
    if digest(base / name) != expected:
        raise SystemExit(f"Original binary changed: {base / name}")
for line in Path("source-SHA256SUMS").read_text().splitlines():
    expected, name = line.split("  ", 1)
    if digest(source / name) != expected:
        raise SystemExit(f"Source file changed while compiling: {name}")
Path("verification.json").write_text(json.dumps({
    "enabled_configuration": required,
    "original_binaries_unchanged": True, "recorded_source_files_unchanged": True,
    "ffmpeg_sha256": digest(Path("ffmpeg")), "ffprobe_sha256": digest(Path("ffprobe")),
    "software_asm_disabled": "--disable-asm" in Path("configure-arguments.txt").read_text().splitlines()
}, indent=2) + "\n")
print(f"Verified FFmpeg and ffprobe in {Path.cwd()}; original binaries preserved.")
PY
printf 'Build and verification completed.\n'
