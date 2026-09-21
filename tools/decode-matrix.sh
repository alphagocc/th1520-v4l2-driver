#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Decode regular *.h264/*.h265 samples through V4L2 and compare every NV12 byte.
# Usage: sh tools/decode-matrix.sh INPUT_DIR [RESULT_DIR] [--timeout 60]
# Requires python3, ffmpeg, gst-launch-1.0 and cmp. Installs or loads nothing.
# Existing NAME.EXT.sw.nv12 or NAME.sw.nv12 references are read-only inputs.
# The software decoder is always explicit: ffmpeg -hwaccel none -c:v h264/hevc.
# A shared per-case deadline covers reference preparation, hardware decoding,
# packing, comparison and hashing; process termination adds at most 3 seconds.
# summary.json is an array of case records; summary.tsv contains the same fields.
set -eu

exec python3 - "$@" <<'PY'
import argparse
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time


class CaseFailure(Exception):
    pass


def remaining(deadline):
    value = deadline - time.monotonic()
    if value <= 0:
        raise CaseFailure("case deadline expired")
    return value


def stop_process(process):
    if process.poll() is not None:
        return True
    try:
        if os.name == "posix":
            os.killpg(process.pid, signal.SIGTERM)
        else:
            process.terminate()
    except ProcessLookupError:
        return True
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        try:
            if os.name == "posix":
                os.killpg(process.pid, signal.SIGKILL)
            else:
                process.kill()
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=1)
        except subprocess.TimeoutExpired:
            return False
    return True


def run_command(command, log_path, deadline, environment):
    with log_path.open("wb") as log:
        log.write(("$ " + shlex.join(command) + "\n").encode("utf-8"))
        log.flush()
        try:
            timeout = remaining(deadline)
        except CaseFailure:
            log.write(b"Command was not started: case deadline expired.\n")
            return 124
        try:
            process = subprocess.Popen(
                command, stdin=subprocess.DEVNULL, stdout=log,
                stderr=subprocess.STDOUT, env=environment,
                start_new_session=(os.name == "posix"),
            )
        except OSError as exc:
            log.write((str(exc) + "\n").encode("utf-8"))
            return 127
        try:
            return process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            stopped = stop_process(process)
            log.write(b"Case deadline expired; command terminated.\n")
            if stopped is False:
                log.write(f"Process {process.pid} has not exited after SIGKILL.\n".encode("ascii"))
            return 124
        except BaseException:
            stop_process(process)
            raise


def frame_metadata(path):
    dimensions = None
    sizes = []
    for line in path.read_text(encoding="utf-8").splitlines():
        match = re.match(r"#dimensions\s+0:\s*(\d+)x(\d+)", line)
        if match:
            dimensions = tuple(map(int, match.groups()))
        elif line and not line.startswith("#"):
            fields = line.split(",")
            if len(fields) >= 6 and fields[0].strip() == "0":
                sizes.append(int(fields[4]))
    if not dimensions or not sizes:
        raise CaseFailure("software decoder produced no frame dimensions/samples")
    width, height = dimensions
    if not width or not height or width % 2 or height % 2:
        raise CaseFailure("comparison requires positive even NV12 dimensions")
    frame_bytes = width * height * 3 // 2
    if any(size != frame_bytes for size in sizes):
        raise CaseFailure("software frame sizes do not match a fixed NV12 layout")
    return width, height, len(sizes), frame_bytes


def normalize_nv12(source, target, width, height, deadline, log_path):
    # The two videoconvert elements remove decoder GstVideoMeta padding.
    # Ordinary GStreamer NV12 can still round the row stride to four bytes;
    # strip that remaining padding for an exact FFmpeg rawvideo comparison.
    stride = (width + 3) & ~3
    gst_frame_bytes = stride * height * 3 // 2
    size = source.stat().st_size
    if not size or size % gst_frame_bytes:
        raise CaseFailure(
            f"GStreamer output size {size} is not a positive multiple of "
            f"its NV12 frame size {gst_frame_bytes}"
        )
    frames = size // gst_frame_bytes
    with source.open("rb") as src, target.open("wb") as dst:
        if stride == width:
            while True:
                remaining(deadline)
                block = src.read(1024 * 1024)
                if not block:
                    break
                dst.write(block)
        else:
            for _ in range(frames):
                remaining(deadline)
                for _ in range(height * 3 // 2):
                    row = src.read(stride)
                    if len(row) != stride:
                        raise CaseFailure("short read while packing GStreamer NV12")
                    dst.write(row[:width])
    log_path.write_text(
        f"width={width} height={height} gst_stride={stride} "
        f"frames={frames} gst_bytes={size} packed_bytes={target.stat().st_size}\n",
        encoding="utf-8",
    )
    return frames


def sha256(path, deadline):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while True:
            remaining(deadline)
            block = stream.read(1024 * 1024)
            if not block:
                break
            digest.update(block)
    return digest.hexdigest()


def software_command(ffmpeg, sample, codec, reference, metadata, provided):
    command = [
        ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin", "-y",
        "-xerror", "-hwaccel", "none", "-c:v", codec, "-f", codec,
        "-i", str(sample),
    ]
    output_options = [
        "-map", "0:v:0", "-an", "-sn", "-dn", "-fps_mode", "passthrough",
        "-c:v", "rawvideo", "-pix_fmt", "nv12",
    ]
    if not provided:
        command += output_options + ["-f", "rawvideo", str(reference)]
    command += output_options
    if provided:
        # The sidecar supplies the complete reference and its frame count.
        # Decode one frame in software to obtain dimensions independently.
        command += ["-frames:v", "1"]
    command += ["-f", "framehash", str(metadata)]
    return command


def decode_case(sample, number, results, timeout, commands, environment):
    started = time.monotonic()
    deadline = started + timeout
    codec = "h264" if sample.suffix == ".h264" else "hevc"
    parser = "h264parse" if codec == "h264" else "h265parse"
    decoder = "v4l2slh264dec" if codec == "h264" else "v4l2slh265dec"
    label = re.sub(r"[^A-Za-z0-9_.-]", "_", sample.name)[:80]
    case_dir = Path(tempfile.mkdtemp(prefix=f"{number:04d}-{label}-", dir=results))
    raw_hw = case_dir / "hardware.gst.nv12"
    packed_hw = case_dir / "hardware.hw.nv12"
    metadata = case_dir / "reference.framehash"
    sidecars = [Path(str(sample) + ".sw.nv12"), sample.with_suffix(".sw.nv12")]
    reference = next((path for path in sidecars if path.is_file()), None)
    provided = reference is not None
    if reference is None:
        reference = case_dir / "reference.sw.nv12"
    row = {
        "name": sample.name, "codec": codec, "status": "FAIL",
        "pipeline_rc": None, "software_rc": None, "normalize_rc": None,
        "cmp": "not-run", "cmp_rc": None, "width": None, "height": None,
        "hw_frames": None, "sw_frames": None, "size": 0, "sw_size": 0,
        "gst_size": 0, "hash": None, "sw_hash": None,
        "reference": str(reference),
        "reference_source": "sidecar" if provided else "software-decoded",
        "software_decoder": codec, "frame_count_source": "reference-bytes",
        "input": str(sample), "case_dir": str(case_dir),
        "gst_log": str(case_dir / "gstreamer.log"),
        "software_log": str(case_dir / "software.log"),
        "cmp_log": str(case_dir / "cmp.log"),
        "timeout_seconds": timeout, "duration_ms": None, "errors": [],
    }
    try:
        row["software_rc"] = run_command(
            software_command(commands["ffmpeg"], sample, codec, reference,
                             metadata, provided),
            Path(row["software_log"]), deadline, environment,
        )
        if row["software_rc"]:
            raise CaseFailure(f"software reference metadata returned {row['software_rc']}")
        width, height, decoded_frames, frame_bytes = frame_metadata(metadata)
        row["width"], row["height"] = width, height
        row["sw_size"] = reference.stat().st_size
        if not row["sw_size"] or row["sw_size"] % frame_bytes:
            raise CaseFailure("reference size is not a positive whole number of NV12 frames")
        row["sw_frames"] = row["sw_size"] // frame_bytes
        if not provided:
            row["frame_count_source"] = "software-decoded"
            if row["sw_frames"] != decoded_frames:
                raise CaseFailure("software rawvideo and framehash frame counts differ")
        row["sw_hash"] = sha256(reference, deadline)

        # This is the board-test.sh packing sequence, with explicit V4L2
        # decoder elements and no decodebin/software fallback.
        pipeline = [
            commands["gst-launch-1.0"], "-q", "filesrc", f"location={sample}",
            "!", parser, "!", "identity", "sleep-time=1000", "!", decoder,
            "!", "videoconvert", "!", "video/x-raw,format=I420",
            "!", "videoconvert", "!", "video/x-raw,format=NV12",
            "!", "filesink", f"location={raw_hw}",
        ]
        row["pipeline_rc"] = run_command(
            pipeline, Path(row["gst_log"]), deadline, environment,
        )
        if row["pipeline_rc"]:
            row["errors"].append(f"hardware pipeline returned {row['pipeline_rc']}")
        if raw_hw.is_file():
            row["gst_size"] = raw_hw.stat().st_size
        try:
            row["hw_frames"] = normalize_nv12(
                raw_hw, packed_hw, width, height, deadline,
                case_dir / "packing.log",
            )
            row["normalize_rc"] = 0
        except (OSError, CaseFailure) as exc:
            row["normalize_rc"] = 1
            (case_dir / "packing.log").write_text(str(exc) + "\n", encoding="utf-8")
            raise CaseFailure(f"NV12 packing failed: {exc}") from exc
        row["size"] = packed_hw.stat().st_size
        row["hash"] = sha256(packed_hw, deadline)
        row["cmp_rc"] = run_command(
            [commands["cmp"], "--", str(packed_hw), str(reference)],
            Path(row["cmp_log"]), deadline, environment,
        )
        row["cmp"] = {0: "equal", 1: "different"}.get(row["cmp_rc"], "error")
        if row["cmp_rc"]:
            row["errors"].append(f"byte comparison returned {row['cmp_rc']}")
        if row["hw_frames"] != row["sw_frames"]:
            row["errors"].append("hardware and software frame counts differ")
        if not row["errors"]:
            row["status"] = "PASS"
    except (OSError, ValueError, CaseFailure) as exc:
        row["errors"].append(str(exc))
    row["duration_ms"] = round((time.monotonic() - started) * 1000)
    (case_dir / "result.json").write_text(json.dumps(row, indent=2) + "\n", encoding="utf-8")
    return row


def write_summaries(results, rows):
    json_tmp = results / "summary.json.tmp"
    json_tmp.write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")
    json_tmp.replace(results / "summary.json")
    tsv_tmp = results / "summary.tsv.tmp"
    with tsv_tmp.open("w", encoding="utf-8", newline="") as stream:
        fields = list(rows[0]) if rows else ["name", "codec", "status", "pipeline_rc", "cmp", "size", "hash"]
        writer = csv.DictWriter(stream, fieldnames=fields, delimiter="\t")
        writer.writeheader()
        for row in rows:
            writer.writerow({**row, "errors": json.dumps(row["errors"])})
    tsv_tmp.replace(results / "summary.tsv")


def main():
    parser = argparse.ArgumentParser(
        description="Compare legal H.264/HEVC files through V4L2 and explicit software decoding."
    )
    parser.add_argument("input_dir", type=Path)
    parser.add_argument("result_dir", type=Path, nargs="?")
    parser.add_argument("--timeout", type=float, default=60,
                        help="total seconds per case, plus up to 3 seconds to terminate a process")
    args = parser.parse_args()
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be a finite positive number")
    source = args.input_dir.resolve()
    if not source.is_dir():
        parser.error("input_dir must be an existing directory")
    samples = sorted(
        [path for pattern in ("*.h264", "*.h265") for path in source.glob(pattern)
         if path.is_file()], key=lambda path: path.name,
    )
    if not samples:
        parser.error("input_dir contains no .h264 or .h265 files")
    commands = {name: shutil.which(name) for name in ("ffmpeg", "gst-launch-1.0", "cmp")}
    missing = [name for name, command in commands.items() if command is None]
    if missing:
        parser.error("required commands are unavailable: " + ", ".join(missing))
    results = (args.result_dir or source / "decode-matrix-results").resolve()
    results.mkdir(parents=True, exist_ok=True)
    environment = os.environ.copy()
    environment["GST_REGISTRY"] = str(results / "gstreamer-registry.bin")
    environment["GST_DEBUG_NO_COLOR"] = "1"
    environment.setdefault("GST_DEBUG", "2")
    rows = []
    write_summaries(results, rows)
    for number, sample in enumerate(samples, 1):
        row = decode_case(sample, number, results, args.timeout, commands, environment)
        rows.append(row)
        write_summaries(results, rows)
        name = json.dumps(row["name"])
        print(f"{row['status']} {name} codec={row['codec']} pipeline_rc={row['pipeline_rc']} "
              f"cmp={row['cmp']} frames={row['hw_frames']}/{row['sw_frames']} "
              f"bytes={row['size']}/{row['sw_size']} sha256={row['hash'] or '-'}", flush=True)
    failed = sum(row["status"] != "PASS" for row in rows)
    print(f"cases={len(rows)} passed={len(rows) - failed} failed={failed}; "
          f"summaries: {results / 'summary.tsv'} and {results / 'summary.json'}", file=sys.stderr)
    return 1 if failed else 0


try:
    sys.exit(main())
except KeyboardInterrupt:
    print("decode matrix interrupted", file=sys.stderr)
    sys.exit(130)
PY
