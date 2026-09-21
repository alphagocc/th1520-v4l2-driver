#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Legal container and coded-video-sequence resolution-change checks.
# Usage: sh tools/test-containers.sh [REPOSITORY_ROOT] [RESULT_DIR]
# REPOSITORY_ROOT defaults to the repository containing this script.
# Inputs are under REPOSITORY_ROOT/test-results/{fixtures,h264-matrix,hevc-matrix}.
# Requires python3, ffmpeg, gst-launch-1.0 and cmp; installs or loads nothing.
# Commands are bounded to 30 seconds, or 60 seconds for changing-resolution CVS.
set -eu

TOOLS_DIR=${0%/*}
[ "$TOOLS_DIR" != "$0" ] || TOOLS_DIR=.
DRIVER_DIR=$(CDPATH= cd -- "$TOOLS_DIR/.." && pwd)
exec python3 - "$DRIVER_DIR" "$@" <<'PY'
import argparse
import csv
import hashlib
import json
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


class TestFailure(Exception):
    pass


def stop(process):
    if process.poll() is not None:
        return
    try:
        if os.name == "posix":
            os.killpg(process.pid, signal.SIGTERM)
        else:
            process.terminate()
    except ProcessLookupError:
        return
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
            pass


def run(command, log_path, environment, timeout=30):
    with log_path.open("wb") as log:
        log.write(("$ " + shlex.join(command) + "\n").encode("utf-8"))
        log.flush()
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL,
                                   stdout=log, stderr=subprocess.STDOUT,
                                   env=environment, start_new_session=(os.name == "posix"))
        try:
            return process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            stop(process)
            log.write(f"Command exceeded {timeout} seconds.\n".encode("ascii"))
            return 124
        except BaseException:
            stop(process)
            raise


def require(condition, message):
    if not condition:
        raise TestFailure(message)


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def metadata(path):
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
    require(dimensions is not None and bool(sizes), "software decoder produced no frames")
    width, height = dimensions
    require(width % 4 == 0 and height % 2 == 0,
            "these application checks require four-byte-aligned width and even height")
    frame_bytes = width * height * 3 // 2
    require(all(size == frame_bytes for size in sizes), "software output changed dimensions within one segment")
    return width, height, len(sizes), frame_bytes


def software_decode(command, sample, codec, nv12, framehash, raw_input):
    args = [command, "-hide_banner", "-loglevel", "error", "-nostdin", "-y",
            "-xerror", "-err_detect", "explode", "-hwaccel", "none", "-c:v", codec]
    if raw_input:
        args += ["-f", codec]
    args += ["-i", str(sample)]
    output = ["-map", "0:v:0", "-an", "-sn", "-dn", "-fps_mode", "passthrough",
              "-c:v", "rawvideo", "-pix_fmt", "nv12"]
    return args + output + ["-f", "rawvideo", str(nv12)] + output + ["-f", "framehash", str(framehash)]


def concatenate(paths, output):
    deadline = time.monotonic() + 60
    with output.open("wb") as target:
        for path in paths:
            with path.open("rb") as source:
                while True:
                    require(time.monotonic() < deadline, "concatenation exceeded 60 seconds")
                    data = source.read(1024 * 1024)
                    if not data:
                        break
                    target.write(data)


def pipeline(command, source, target, codec, demux=None):
    parser = "h264parse" if codec == "h264" else "h265parse"
    decoder = "v4l2slh264dec" if codec == "h264" else "v4l2slh265dec"
    args = [command, "-q", "filesrc", f"location={source}"]
    if demux:
        args += ["!", demux]
    # Fixed pixel formats, with width and height free to renegotiate at each CVS.
    return args + ["!", parser, "!", "identity", "sleep-time=1000", "!", decoder,
                   "!", "videoconvert", "!", "video/x-raw,format=I420",
                   "!", "videoconvert", "!", "video/x-raw,format=NV12",
                   "!", "filesink", f"location={target}"]


def write_summary(results, rows):
    (results / "summary.json").write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")
    if rows:
        with (results / "summary.tsv").open("w", encoding="utf-8", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(rows[0]), delimiter="\t")
            writer.writeheader()
            for row in rows:
                writer.writerow({**row, "segments": json.dumps(row["segments"]),
                                 "errors": json.dumps(row["errors"])})


def main():
    parser = argparse.ArgumentParser(prog="test-containers.sh",
                                     description="Legal V4L2 container and resolution-change application tests")
    parser.add_argument("repository_root", type=Path, nargs="?", default=Path(sys.argv[1]),
                        help="standalone driver repository root; defaults to the script's repository")
    parser.add_argument("result_dir", type=Path, nargs="?")
    args = parser.parse_args(sys.argv[2:])
    root = args.repository_root.resolve()
    require(root.is_dir(), f"repository root does not exist: {root}")
    results = (args.result_dir or root / "test-results/containers").resolve()
    results.mkdir(parents=True, exist_ok=True)
    commands = {name: shutil.which(name) for name in ("ffmpeg", "gst-launch-1.0", "cmp")}
    require(all(commands.values()), "required commands: ffmpeg, gst-launch-1.0, cmp")
    environment = os.environ.copy()
    environment["GST_REGISTRY"] = str(results / "gstreamer-registry.bin")
    environment["GST_DEBUG_NO_COLOR"] = "1"
    environment.setdefault("GST_DEBUG", "2")
    reference_root = Path(tempfile.mkdtemp(prefix="references-", dir=results))
    # Concrete names produced by make-h264-matrix.sh, make-hevc-matrix.sh and
    # make-fixtures.sh. Each input is a complete independently decodable clip.
    sources = {
        "h264-baseline": ("h264-matrix/baseline-refs1.h264", "h264", 320, 240, 66),
        "h264-main": ("h264-matrix/main-crop640x360.h264", "h264", 640, 360, 77),
        "h264-high": ("h264-matrix/high-1920x1080.h264", "h264", 1920, 1080, 100),
        "hevc-320": ("fixtures/hevc-i.h265", "hevc", 320, 240, None),
        "hevc-720": ("hevc-matrix/hevc-refs6-720p.h265", "hevc", 1280, 720, None),
        "hevc-1080": ("hevc-matrix/hevc-slices4-1080p.h265", "hevc", 1920, 1080, None),
    }
    cache = {}

    def reference(key):
        if key in cache:
            require("error" not in cache[key], cache[key].get("error", ""))
            return cache[key]
        relative, codec, width, height, profile = sources[key]
        sample = root / "test-results" / relative
        folder = reference_root / key
        folder.mkdir()
        nv12 = folder / "reference.sw.nv12"
        framehash = folder / "reference.framehash"
        try:
            require(sample.is_file() and sample.stat().st_size > 0, f"missing complete sample: {sample}")
            rc = run(software_decode(commands["ffmpeg"], sample, codec, nv12, framehash, True),
                     folder / "software.log", environment)
            require(rc == 0, f"software decode failed for {sample.name}: rc={rc}")
            actual_width, actual_height, frames, frame_bytes = metadata(framehash)
            require((actual_width, actual_height) == (width, height), f"unexpected dimensions in {sample.name}")
            require(nv12.stat().st_size == frames * frame_bytes, "software frame count/byte size mismatch")
            if profile is not None:
                headers = folder / "headers.log"
                rc = run([commands["ffmpeg"], "-hide_banner", "-loglevel", "info", "-nostdin",
                          "-hwaccel", "none", "-c:v", codec, "-f", codec, "-i", str(sample),
                          "-map", "0:v:0", "-c:v", "copy", "-bsf:v", "trace_headers", "-f", "null", "-"],
                         headers, environment)
                require(rc == 0, f"header validation failed: {sample.name}")
                profiles = set(map(int, re.findall(r"\bprofile_idc\s+\S+\s+=\s+(\d+)",
                                                  headers.read_text(encoding="utf-8", errors="replace"))))
                require(profiles == {profile}, f"unexpected H.264 profile in {sample.name}: {profiles}")
            sidecar = sample.with_suffix(".sw.nv12")
            if sidecar.is_file():
                rc = run([commands["cmp"], "--", str(nv12), str(sidecar)], folder / "baseline-cmp.log", environment)
                require(rc == 0, f"fresh software decode differs from existing baseline: {sample.name}")
            item = {"key": key, "input": str(sample), "codec": codec,
                    "width": width, "height": height, "profile_idc": profile,
                    "frames": frames, "bytes": nv12.stat().st_size, "hash": digest(nv12),
                    "reference": str(nv12), "directory": str(folder)}
            cache[key] = item
        except (OSError, ValueError, TestFailure) as exc:
            cache[key] = {"key": key, "input": str(sample), "error": str(exc), "directory": str(folder)}
        (folder / "result.json").write_text(json.dumps(cache[key], indent=2) + "\n", encoding="utf-8")
        require("error" not in cache[key], cache[key].get("error", ""))
        return cache[key]

    cases = [
        ("h264-mp4", "container", "h264", ["h264-baseline"], "mp4", "qtdemux"),
        ("hevc-matroska", "container", "hevc", ["hevc-320"], "matroska", "matroskademux"),
        ("h264-resolution-change", "cvs", "h264", ["h264-baseline", "h264-main", "h264-high"], None, None),
        ("hevc-resolution-change", "cvs", "hevc", ["hevc-320", "hevc-720", "hevc-1080"], None, None),
    ]
    rows = []
    for name, kind, codec, keys, container_format, demux in cases:
        started = time.monotonic()
        folder = Path(tempfile.mkdtemp(prefix=name + "-", dir=results))
        hw = folder / "hardware.hw.nv12"
        expected = folder / "expected.sw.nv12"
        row = {"name": name, "kind": kind, "codec": codec, "status": "FAIL",
               "mux_rc": None, "container_sw_cmp_rc": None, "pipeline_rc": None,
               "cmp_rc": None, "cmp": "not-run", "expected_frames": None,
               "size": 0, "sw_size": 0, "hash": None, "sw_hash": None,
               "segments": [], "case_dir": str(folder), "duration_ms": None, "errors": []}
        try:
            segments = [reference(key) for key in keys]
            offset = 0
            for segment in segments:
                row["segments"].append({**segment, "output_byte_offset": offset})
                offset += segment["bytes"]
            row["expected_frames"] = sum(segment["frames"] for segment in segments)
            # Always concatenate independently decoded segment references.
            # Never decode the complete changing-resolution stream in software.
            concatenate([Path(segment["reference"]) for segment in segments], expected)
            row["sw_size"], row["sw_hash"] = expected.stat().st_size, digest(expected)
            if kind == "container":
                source = Path(segments[0]["input"])
                wrapped = folder / ("video.mp4" if container_format == "mp4" else "video.mkv")
                # Baseline AVC has no B pictures; the HEVC fixture has one I
                # picture. PTS=DTS is valid for these chosen inputs. setts
                # supplies timestamps absent from elementary-stream packets.
                if codec == "hevc":
                    require(segments[0]["frames"] == 1, "HEVC container fixture must contain exactly one I picture")
                mux = [commands["ffmpeg"], "-hide_banner", "-loglevel", "warning", "-nostdin", "-y",
                       "-hwaccel", "none", "-c:v", codec, "-r", "30", "-f", codec, "-i", str(source),
                       "-map", "0:v:0", "-an", "-sn", "-dn", "-c:v", "copy",
                       "-bsf:v", "setts=pts=N/(30*TB):dts=N/(30*TB)", "-f", container_format, str(wrapped)]
                row["mux_rc"] = run(mux, folder / "mux.log", environment)
                require(row["mux_rc"] == 0, f"stream-copy remux failed: rc={row['mux_rc']}")
                decoded = folder / "container.sw.nv12"
                rc = run(software_decode(commands["ffmpeg"], wrapped, codec, decoded,
                                         folder / "container.framehash", False),
                         folder / "container-software.log", environment)
                require(rc == 0, f"container software verification failed: rc={rc}")
                row["container_sw_cmp_rc"] = run([commands["cmp"], "--", str(decoded), str(expected)],
                                                  folder / "container-software-cmp.log", environment)
                require(row["container_sw_cmp_rc"] == 0, "container changed software-decoded pixels/frame count")
                stream = wrapped
            else:
                stream = folder / ("three-cvs.h264" if codec == "h264" else "three-cvs.h265")
                concatenate([Path(segment["input"]) for segment in segments], stream)
            (folder / "segments.json").write_text(json.dumps(row["segments"], indent=2) + "\n", encoding="utf-8")
            row["pipeline_rc"] = run(pipeline(commands["gst-launch-1.0"], stream, hw, codec, demux),
                                      folder / "gstreamer.log", environment, 60 if kind == "cvs" else 30)
            if row["pipeline_rc"]:
                row["errors"].append(f"hardware pipeline returned {row['pipeline_rc']}")
            require(hw.is_file(), "hardware pipeline produced no NV12 file")
            row["size"], row["hash"] = hw.stat().st_size, digest(hw)
            row["cmp_rc"] = run([commands["cmp"], "--", str(hw), str(expected)], folder / "cmp.log", environment)
            row["cmp"] = {0: "equal", 1: "different"}.get(row["cmp_rc"], "error")
            if row["cmp_rc"]:
                row["errors"].append(f"hardware pixels/frame count differ: cmp rc={row['cmp_rc']}")
            if not row["errors"]:
                row["status"] = "PASS"
        except (OSError, ValueError, TestFailure) as exc:
            row["errors"].append(str(exc))
        row["duration_ms"] = round((time.monotonic() - started) * 1000)
        (folder / "result.json").write_text(json.dumps(row, indent=2) + "\n", encoding="utf-8")
        rows.append(row)
        write_summary(results, rows)
        print(f"{row['status']} {name} pipeline_rc={row['pipeline_rc']} cmp={row['cmp']} "
              f"expected_frames={row['expected_frames']} bytes={row['size']}/{row['sw_size']}", flush=True)
    print(f"Evidence: {results / 'summary.json'} and {results / 'summary.tsv'}", file=sys.stderr)
    return 1 if any(row["status"] != "PASS" for row in rows) else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("application tests interrupted", file=sys.stderr)
        sys.exit(130)
    except (OSError, TestFailure) as exc:
        print(str(exc), file=sys.stderr)
        sys.exit(2)
PY
