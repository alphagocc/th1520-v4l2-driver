#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Bounded VP9 profile-0 IVF fixtures with checked headers and software NV12.
# Requirements: python3, ffmpeg with libvpx-vp9/trace_headers, and ffprobe.
# MATRIX_CASES selects comma-separated names; MATRIX_THREADS defaults to 2.
set -eu
[ "$#" -le 1 ] || { printf 'Usage: %s [OUTPUT_DIRECTORY]\n' "$0" >&2; exit 2; }
TOOLS_DIR=${0%/*}
[ "$TOOLS_DIR" != "$0" ] || TOOLS_DIR=.
DRIVER_DIR=$(CDPATH= cd -- "$TOOLS_DIR/.." && pwd)
OUT=${1:-"$DRIVER_DIR/test-results/vp9-matrix"}
exec python3 - "$OUT" <<'PY'
import collections
import csv
import dataclasses
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys


@dataclasses.dataclass(frozen=True)
class Case:
    name: str
    width: int = 640
    height: int = 360
    frames: int = 12
    keyint: int = 12
    tile_columns: int = 0
    tile_rows: int = 0
    frame_parallel: int = 0
    error_resilient: int = 0
    lossless: int = 0
    aq_mode: int = 0
    expected: str = "decode"


CASES = [
    Case("vp9-key", width=320, height=240, frames=1, keyint=1),
    Case("vp9-all-key", width=320, height=240, frames=8, keyint=1),
    Case("vp9-inter"),
    Case("vp9-frame-parallel", frame_parallel=1),
    Case("vp9-error-resilient", error_resilient=1, frame_parallel=1),
    Case("vp9-tiles4x1", width=1920, height=1080, frames=8, tile_columns=2),
    Case("vp9-odd-321x241", width=321, height=241, expected="reject-EINVAL"),
    Case("vp9-lossless", width=320, height=240, frames=8, lossless=1),
    Case("vp9-segmentation", aq_mode=3),
    Case("vp9-context-refresh", frames=24, keyint=24),
]
FIELD = re.compile(r"\b([a-zA-Z_]\w*(?:\[\d+\]|\.\w+)*)\s+[01 ]*\s*=\s*(-?\d+)\s*$")


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def run(command, log):
    with log.open("wb") as stream:
        stream.write(("$ " + shlex.join(command) + "\n").encode("utf-8"))
        stream.flush()
        result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT,
                                stdin=subprocess.DEVNULL, timeout=180, check=False)
    require(result.returncode == 0, f"command returned {result.returncode}: {log}")


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for data in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(data)
    return result.hexdigest()


def inspect_headers(case, path):
    fields = collections.defaultdict(list)
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = FIELD.search(line)
        if match:
            fields[match[1]].append(int(match[2]))
    for field, value in (("profile_low_bit", 0), ("profile_high_bit", 0),
                         ("show_frame", 1),
                         ("error_resilient_mode", case.error_resilient),
                         ("tile_cols_log2", case.tile_columns),
                         ("tile_rows_log2", case.tile_rows)):
        require(fields[field] and set(fields[field]) == {value},
                f"{case.name}: expected {field}={value}, got {fields[field]}")
    require(len(fields["frame_type"]) == case.frames,
            f"{case.name}: unexpected number of frame headers")
    if case.keyint == 1:
        require(set(fields["frame_type"]) == {0}, f"{case.name}: expected only key frames")
    else:
        require(fields["frame_type"][0] == 0 and 1 in fields["frame_type"],
                f"{case.name}: expected a key frame followed by inter frames")
    if not case.error_resilient:
        require(set(fields["frame_parallel_decoding_mode"]) == {case.frame_parallel},
                f"{case.name}: unexpected frame_parallel_decoding_mode")
        require(1 in fields["refresh_frame_context"],
                f"{case.name}: no probability-context refresh")
    if case.lossless:
        require(set(fields["base_q_idx"]) == {0}, f"{case.name}: lossless base_q_idx is nonzero")
        for field in ("delta_q_y_dc", "delta_q_uv_dc", "delta_q_uv_ac"):
            require(set(fields[field + ".delta_coded"]) == {0},
                    f"{case.name}: lossless quantizer delta is present")
    if case.aq_mode:
        require(1 in fields["segmentation_enabled"], f"{case.name}: segmentation is absent")
    return {name: sorted(set(values)) for name, values in sorted(fields.items())}


def main():
    out = Path(sys.argv[1]).resolve()
    out.mkdir(parents=True, exist_ok=True)
    ffmpeg, ffprobe = shutil.which("ffmpeg"), shutil.which("ffprobe")
    require(ffmpeg and ffprobe, "ffmpeg and ffprobe must be available")
    run([ffmpeg, "-version"], out / "ffmpeg-version.log")
    run([ffprobe, "-version"], out / "ffprobe-version.log")
    threads = int(os.environ.get("MATRIX_THREADS", "2"))
    require(1 <= threads <= 64, "MATRIX_THREADS must be between 1 and 64")
    selection = os.environ.get("MATRIX_CASES", "")
    names = set(selection.split(",")) if selection else {case.name for case in CASES}
    require(names <= {case.name for case in CASES}, "MATRIX_CASES contains an unknown name")
    cases = [case for case in CASES if case.name in names]
    records = []
    checksums = []
    for case in cases:
        case_out = out / "unsupported" if case.expected != "decode" else out
        case_out.mkdir(parents=True, exist_ok=True)
        sample = case_out / f"{case.name}.ivf"
        reference = case_out / f"{case.name}.sw.nv12"
        headers = case_out / f"{case.name}.headers.log"
        # RGB testsrc preserves odd dimensions; testsrc2 rounds them down for 4:2:0.
        command = [ffmpeg, "-hide_banner", "-loglevel", "info", "-nostdin", "-y",
                   "-f", "lavfi", "-i", f"testsrc=size={case.width}x{case.height}:rate=30",
                   "-frames:v", str(case.frames), "-vf", "format=yuv420p",
                   "-c:v", "libvpx-vp9", "-profile:v", "0", "-threads", str(threads),
                   "-deadline", "realtime", "-cpu-used", "6", "-b:v", "0",
                   "-crf", "0" if case.lossless else "32",
                   "-lag-in-frames", "0", "-auto-alt-ref", "0", "-g", str(case.keyint),
                   "-tile-columns", str(case.tile_columns), "-tile-rows", str(case.tile_rows),
                   "-frame-parallel", str(case.frame_parallel),
                   "-error-resilient", str(case.error_resilient),
                   "-lossless", str(case.lossless), "-aq-mode", str(case.aq_mode),
                   "-f", "ivf", str(sample)]
        run(command, case_out / f"{case.name}.encode.log")
        run([ffmpeg, "-hide_banner", "-loglevel", "info", "-nostdin", "-i", str(sample),
             "-map", "0:v:0", "-c:v", "copy", "-bsf:v", "trace_headers",
             "-f", "null", "-"], headers)
        observed = inspect_headers(case, headers)
        probe = subprocess.run([ffprobe, "-v", "error", "-count_frames", "-select_streams", "v:0",
                                "-show_streams", "-of", "json", str(sample)],
                               capture_output=True, timeout=60, check=True)
        (case_out / f"{case.name}.probe.json").write_bytes(probe.stdout)
        stream = json.loads(probe.stdout)["streams"][0]
        require(stream["codec_name"] == "vp9" and stream["pix_fmt"] == "yuv420p",
                f"{case.name}: unexpected codec or pixel format")
        require((stream["width"], stream["height"], int(stream["nb_read_frames"])) ==
                (case.width, case.height, case.frames), f"{case.name}: decoded dimensions/frames differ")
        run([ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-xerror",
             "-hwaccel", "none", "-c:v", "vp9", "-f", "ivf", "-i", str(sample),
             "-map", "0:v:0", "-fps_mode", "passthrough", "-c:v", "rawvideo",
             "-pix_fmt", "nv12", "-f", "rawvideo", str(reference)],
            case_out / f"{case.name}.software.log")
        # FFmpeg packs luma at width, interleaved UV at 2 * ceil(width / 2).
        frame_bytes = case.width * case.height + ((case.width + 1) // 2) * 2 * ((case.height + 1) // 2)
        require(reference.stat().st_size == case.frames * frame_bytes,
                f"{case.name}: unexpected software-reference byte count")
        record = {**dataclasses.asdict(case), "software_frames": int(stream["nb_read_frames"]),
                  "reference_bytes": reference.stat().st_size, "observed": observed,
                  "input": sample.relative_to(out).as_posix(),
                  "sample_sha256": digest(sample), "reference_sha256": digest(reference)}
        records.append(record)
        for item in (sample, reference):
            checksums.append(f"{digest(item)}  {item.relative_to(out).as_posix()}")
        (out / "encode-cases.json").write_text(json.dumps(records, indent=2) + "\n", encoding="utf-8")
        print(f"Generated {case.name}: {case.width}x{case.height}, {case.frames} frames, "
              f"hardware_expected={case.expected}", flush=True)
    with (out / "encode-cases.tsv").open("w", encoding="utf-8", newline="") as stream:
        fields = [field.name for field in dataclasses.fields(Case)] + ["software_frames", "reference_bytes"]
        writer = csv.DictWriter(stream, fields, delimiter="\t", extrasaction="ignore")
        writer.writeheader()
        writer.writerows(records)
    (out / "SHA256SUMS").write_text("\n".join(checksums) + "\n", encoding="utf-8")
    print(f"cases={len(records)} software_frames={sum(record['software_frames'] for record in records)}")


try:
    main()
except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
    print(f"VP9 fixture generation failed: {exc}", file=sys.stderr)
    sys.exit(1)
PY
