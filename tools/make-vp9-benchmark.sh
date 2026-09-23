#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Continuous, unique 1080p VP9 frames plus an exact eight-packet prefix.
# No looping and no full raw-video reference. Requires ffmpeg/libvpx, ffprobe,
# trace_headers, and Python 3. THREADS defaults to 4.
set -eu
[ "$#" -le 1 ] || { printf 'Usage: %s [OUTPUT_DIRECTORY]\n' "$0" >&2; exit 2; }
TOOLS_DIR=${0%/*}
[ "$TOOLS_DIR" != "$0" ] || TOOLS_DIR=.
DRIVER_DIR=$(CDPATH= cd -- "$TOOLS_DIR/.." && pwd)
OUT=${1:-"$DRIVER_DIR/test-results/vp9-benchmark"}
exec python3 - "$OUT" <<'PY'
import collections
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess
import sys
import time


WIDTH, HEIGHT, FRAMES, PREFIX_FRAMES = 1920, 1080, 600, 8
FIELD = re.compile(r"\b([a-zA-Z_]\w*(?:\[\d+\]|\.\w+)*)\s+[01 ]*\s*=\s*(-?\d+)\s*$")


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def run(command, path):
    started = time.monotonic()
    with path.open("wb") as log:
        log.write(("$ " + shlex.join(command) + "\n").encode())
        log.flush()
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                stdin=subprocess.DEVNULL, timeout=600)
    require(result.returncode == 0, f"command returned {result.returncode}: {path}")
    return time.monotonic() - started


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def inspect_headers(path):
    fields = collections.defaultdict(list)
    frames, current = [], None
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if re.search(r"\]\s+Frame\s*$", line):
            current = {}
            frames.append(current)
        match = FIELD.search(line)
        if match:
            name, value = match[1], int(match[2])
            fields[name].append(value)
            if current is not None:
                current[name] = value
    require(len(frames) == FRAMES, "unexpected coded-header count")
    for name, value in (("profile_low_bit", 0), ("profile_high_bit", 0),
                        ("show_existing_frame", 0), ("show_frame", 1),
                        ("frame_parallel_decoding_mode", 0), ("refresh_frame_context", 1),
                        ("error_resilient_mode", 0), ("tile_cols_log2", 2), ("tile_rows_log2", 0)):
        require(len(fields[name]) == FRAMES and set(fields[name]) == {value},
                f"unexpected {name}: {sorted(set(fields[name]))}")
    keyframes = [index for index, frame in enumerate(frames) if frame.get("frame_type") == 0]
    require(keyframes == list(range(0, FRAMES, 60)), f"unexpected key-frame positions: {keyframes}")
    return {"coded_headers": len(frames), "keyframe_indices": keyframes,
            "backward_adaptation_eligible_headers": sum(
                frame.get("frame_parallel_decoding_mode") == 0 and
                frame.get("refresh_frame_context") == 1 for frame in frames),
            "observed": {name: sorted(set(values)) for name, values in sorted(fields.items())},
            "histograms": {name: dict(sorted(collections.Counter(values).items()))
                           for name, values in sorted(fields.items())}}


def make_prefix(source, prefix):
    data = source.read_bytes()
    require(data[:4] == b"DKIF" and data[8:12] == b"VP90", "expected VP9 IVF")
    require(struct.unpack_from("<H", data, 6)[0] == 32, "unexpected IVF header length")
    require(struct.unpack_from("<I", data, 24)[0] == FRAMES, "unexpected IVF frame count")
    offset, packets = 32, []
    while offset < len(data):
        require(offset + 12 <= len(data), "truncated IVF packet header")
        length, timestamp = struct.unpack_from("<IQ", data, offset)
        require(length > 0 and offset + 12 + length <= len(data), "truncated IVF packet")
        require(timestamp == len(packets), "IVF timestamps are not sequential")
        packets.append({"packet": len(packets), "offset": offset, "bytes": length,
                        "timestamp": timestamp})
        offset += 12 + length
    require(len(packets) == FRAMES, "unexpected IVF packet count")
    prefix_end = packets[PREFIX_FRAMES]["offset"]
    header = bytearray(data[:32])
    struct.pack_into("<I", header, 24, PREFIX_FRAMES)
    prefix.write_bytes(header + data[32:prefix_end])
    require(prefix.read_bytes()[32:] == data[32:prefix_end], "prefix packet bytes differ")
    return {"source": source.name, "source_sha256": digest(source),
            "source_byte_start": 32, "source_byte_end_exclusive": prefix_end,
            "prefix_packets": packets[:PREFIX_FRAMES],
            "modification": "Only IVF frame-count field changes from 600 to 8; payload bytes are identical."}


def main():
    out = Path(sys.argv[1]).resolve()
    validation = out / "validation"
    validation.mkdir(parents=True, exist_ok=True)
    ffmpeg, ffprobe = shutil.which("ffmpeg"), shutil.which("ffprobe")
    require(ffmpeg and ffprobe, "ffmpeg and ffprobe must be available")
    threads = int(os.environ.get("THREADS", "4"))
    require(1 <= threads <= 64, "THREADS must be between 1 and 64")
    run([ffmpeg, "-version"], out / "ffmpeg-version.log")
    run([ffprobe, "-version"], out / "ffprobe-version.log")
    sample = out / "vp9-1920x1080.ivf"
    source = "testsrc2=size=1920x1080:rate=30"
    command = [ffmpeg, "-hide_banner", "-loglevel", "info", "-nostdin", "-y",
               "-f", "lavfi", "-i", source, "-frames:v", str(FRAMES), "-an",
               "-pix_fmt", "yuv420p", "-c:v", "libvpx-vp9", "-profile:v", "0",
               "-threads", str(threads), "-deadline", "good", "-cpu-used", "4",
               "-b:v", "0", "-crf", "32", "-lag-in-frames", "0", "-auto-alt-ref", "0",
               "-g", "60", "-tile-columns", "2", "-tile-rows", "0",
               "-frame-parallel", "0", "-error-resilient", "0", "-aq-mode", "0",
               "-f", "ivf", str(sample)]
    print("Encoding 600 continuous VP9 frames at 1920x1080...", flush=True)
    encode_seconds = run(command, out / "vp9-1920x1080.encode.log")
    probe = subprocess.run([ffprobe, "-v", "error", "-count_frames", "-select_streams", "v:0",
                            "-show_streams", "-of", "json", str(sample)], capture_output=True,
                           timeout=600, check=True)
    (out / "vp9-1920x1080.probe.json").write_bytes(probe.stdout)
    stream = json.loads(probe.stdout)["streams"][0]
    require(stream["codec_name"] == "vp9" and stream["profile"] == "Profile 0" and
            stream["pix_fmt"] == "yuv420p", "unexpected codec, profile, or pixel format")
    require((stream["width"], stream["height"], int(stream["nb_read_frames"])) ==
            (WIDTH, HEIGHT, FRAMES), "decoded dimensions/frame count differ")
    header_log = out / "vp9-1920x1080.headers.log"
    run([ffmpeg, "-hide_banner", "-loglevel", "info", "-nostdin", "-i", str(sample),
         "-c:v", "copy", "-bsf:v", "trace_headers", "-f", "null", "-"], header_log)
    headers = inspect_headers(header_log)
    (out / "vp9-1920x1080.headers.json").write_text(json.dumps(headers, indent=2) + "\n", encoding="utf-8")
    framehash = out / "vp9-1920x1080.software.framemd5"
    decode = [ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-xerror",
              "-hwaccel", "none", "-c:v", "vp9", "-f", "ivf"]
    run(decode + ["-i", str(sample), "-map", "0:v:0", "-fps_mode", "passthrough",
                  "-c:v", "rawvideo", "-pix_fmt", "nv12", "-f", "framemd5", str(framehash)],
        out / "vp9-1920x1080.software-hash.log")
    hashes = [line.split(",")[-1].strip() for line in framehash.read_text().splitlines()
              if line and not line.startswith("#")]
    require(len(hashes) == FRAMES and len(set(hashes)) == FRAMES,
            "decoded frames are missing or contain repeated pixels")
    prefix = validation / "vp9-1920x1080-first8.ivf"
    prefix_info = make_prefix(sample, prefix)
    reference = validation / "vp9-1920x1080-first8.sw.nv12"
    run(decode + ["-i", str(prefix), "-map", "0:v:0", "-fps_mode", "passthrough",
                  "-c:v", "rawvideo", "-pix_fmt", "nv12", "-f", "rawvideo", str(reference)],
        validation / "vp9-1920x1080-first8.software.log")
    frame_bytes = WIDTH * HEIGHT * 3 // 2
    require(reference.stat().st_size == PREFIX_FRAMES * frame_bytes, "unexpected prefix NV12 size")
    with reference.open("rb") as raw:
        for index in range(PREFIX_FRAMES):
            require(hashlib.md5(raw.read(frame_bytes)).hexdigest() == hashes[index],
                    "prefix pixels differ from corresponding full-stream pixels")
    prefix_info.update(software_frames=PREFIX_FRAMES, reference_bytes=reference.stat().st_size,
                       prefix_sha256=digest(prefix), reference_sha256=digest(reference))
    (validation / "provenance.json").write_text(json.dumps(prefix_info, indent=2) + "\n", encoding="utf-8")
    manifest = {"name": sample.name, "codec": "vp9", "profile": 0, "bit_depth": 8,
                "chroma": "4:2:0", "width": WIDTH, "height": HEIGHT, "rate": "30/1", "frames": FRAMES,
                "source": source, "encoding_command": command, "encode_seconds": encode_seconds,
                "software_frames": len(hashes), "unique_decoded_frame_hashes": len(set(hashes)),
                "sample_bytes": sample.stat().st_size, "sample_sha256": digest(sample),
                "prefix_input": prefix.relative_to(out).as_posix(),
                "prefix_reference": reference.relative_to(out).as_posix(), "headers": headers}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    (out / "SHA256SUMS").write_text("".join(f"{digest(path)}  {path.relative_to(out).as_posix()}\n"
                                             for path in (sample, framehash, prefix, reference)), encoding="utf-8")
    print(f"Generated {FRAMES} unique frames, {sample.stat().st_size} IVF bytes; "
          f"prefix={PREFIX_FRAMES} frames / {reference.stat().st_size} NV12 bytes", flush=True)


try:
    main()
except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
    print(f"VP9 benchmark fixture generation failed: {exc}", file=sys.stderr)
    sys.exit(1)
PY
