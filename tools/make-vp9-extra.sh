#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Small profile-0 fixtures for backward adaptation, alt-ref, display reuse,
# key-frame context reset and non-aligned dimensions.
# Requires Python 3, ffmpeg with libvpx-vp9/trace_headers, and ffprobe.
set -eu
[ "$#" -le 1 ] || { printf 'Usage: %s [OUTPUT_DIRECTORY]\n' "$0" >&2; exit 2; }
TOOLS_DIR=${0%/*}
[ "$TOOLS_DIR" != "$0" ] || TOOLS_DIR=.
DRIVER_DIR=$(CDPATH= cd -- "$TOOLS_DIR/.." && pwd)
OUT=${1:-"$DRIVER_DIR/test-results/vp9-extra"}
exec python3 - "$OUT" <<'PY'
import collections
import dataclasses
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


@dataclasses.dataclass(frozen=True)
class Case:
    name: str
    width: int
    height: int
    frames: int
    keyint: int
    altref: bool = False
    tile_columns: int = 0
    tile_rows: int = 0
    show_existing: bool = False


CASES = [
    Case("vp9-adapt-322x242", 322, 242, 20, 20),
    Case("vp9-altref-superframe", 640, 360, 40, 40, altref=True),
    Case("vp9-reset-keyframes", 320, 240, 24, 8),
    Case("vp9-show-existing-slots", 320, 240, 9, 1, show_existing=True),
]
FIELD = re.compile(r"\b([a-zA-Z_]\w*(?:\[\d+\]|\.\w+)*)\s+[01 ]*\s*=\s*(-?\d+)\s*$")


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def run(command, path):
    with path.open("wb") as log:
        log.write(("$ " + shlex.join(command) + "\n").encode())
        log.flush()
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                stdin=subprocess.DEVNULL, timeout=180)
    require(result.returncode == 0, f"command returned {result.returncode}: {path}")


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for data in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(data)
    return result.hexdigest()


def packets(path):
    data = path.read_bytes()
    require(data[:4] == b"DKIF" and data[8:12] == b"VP90", "expected VP9 IVF")
    require(struct.unpack_from("<H", data, 6)[0] == 32, "expected 32-byte IVF header")
    offset, result = 32, []
    while offset < len(data):
        require(offset + 12 <= len(data), "truncated IVF packet header")
        length, timestamp = struct.unpack_from("<IQ", data, offset)
        require(length > 0 and offset + 12 + length <= len(data), "truncated IVF payload")
        payload = data[offset + 12:offset + 12 + length]
        result.append((offset, timestamp, payload))
        offset += 12 + length
    require(len(result) == struct.unpack_from("<I", data, 24)[0], "IVF packet count differs")
    return data[:32], result


def packet_evidence(path):
    _, packet_list = packets(path)
    result = []
    for number, (offset, timestamp, payload) in enumerate(packet_list):
        marker = payload[-1]
        sizes = [len(payload)]
        index_bytes = 0
        if marker & 224 == 192:
            count, magnitude = (marker & 7) + 1, ((marker >> 3) & 3) + 1
            index_bytes = 2 + count * magnitude
            require(len(payload) >= index_bytes and payload[-index_bytes] == marker,
                    f"packet {number}: invalid superframe index")
            sizes = [int.from_bytes(payload[len(payload) - index_bytes + 1 + i * magnitude:
                                            len(payload) - index_bytes + 1 + (i + 1) * magnitude],
                                    "little") for i in range(count)]
            require(sum(sizes) + index_bytes == len(payload), "superframe size mismatch")
        frames, start = [], 0
        for size in sizes:
            require(size > 0, "empty VP9 subframe")
            first = payload[start]
            require(first >> 6 == 2 and ((first >> 4) & 3) == 0,
                    "expected profile-0 frame marker")
            show_existing = (first >> 3) & 1
            frame = {"offset": offset + 12 + start, "bytes": size,
                     "show_existing_frame": show_existing}
            if show_existing:
                frame["frame_to_show_map_idx"] = first & 7
                require(size == 1, "expected one-byte show-existing frame")
            else:
                frame.update(frame_type=(first >> 2) & 1, show_frame=(first >> 1) & 1,
                             error_resilient_mode=first & 1)
            frames.append(frame)
            start += size
        result.append({"packet": number, "offset": offset, "timestamp": timestamp,
                       "bytes": len(payload), "superframe_index_bytes": index_bytes,
                       "frames": frames})
    return result


def inspect_headers(case, path, evidence):
    fields = collections.defaultdict(list)
    frame_headers, current = [], None
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if re.search(r"\]\s+Frame\s*$", line):
            current = {}
            frame_headers.append(current)
        elif re.search(r"\]\s+(?:Packet:|Superframe Index)", line):
            current = None
        match = FIELD.search(line)
        if match and current is not None:
            name, value = match[1], int(match[2])
            fields[name].append(value)
            current[name] = value
    raw_frames = [frame for packet in evidence for frame in packet["frames"]]
    require(len(raw_frames) == len(frame_headers), "raw/trace frame count mismatch")
    for number, (raw, header) in enumerate(zip(raw_frames, frame_headers)):
        for name in ("show_existing_frame", "frame_type", "show_frame", "frame_to_show_map_idx"):
            if name in raw:
                require(header.get(name) == raw[name], f"frame {number}: raw/trace {name} mismatch")
    for field, value in (("profile_low_bit", 0), ("profile_high_bit", 0),
                         ("frame_parallel_decoding_mode", 0), ("error_resilient_mode", 0),
                         ("tile_cols_log2", case.tile_columns), ("tile_rows_log2", case.tile_rows)):
        require(fields[field] and set(fields[field]) == {value},
                f"{case.name}: expected {field}={value}, got {fields[field]}")
    require(1 in fields["refresh_frame_context"], "no frame-context refresh")
    shown = sum(header.get("show_existing_frame", 0) or header.get("show_frame", 0)
                for header in frame_headers)
    require(shown == case.frames, f"{case.name}: unexpected visible header count {shown}")
    invisible = sum(header.get("show_frame") == 0 for header in frame_headers)
    superframes = sum(packet["superframe_index_bytes"] > 0 for packet in evidence)
    keyframes = [i for i, header in enumerate(frame_headers) if header.get("frame_type") == 0]
    adaptation = sum(header.get("frame_parallel_decoding_mode") == 0 and
                     header.get("refresh_frame_context") == 1 for header in frame_headers)
    if case.altref:
        require(invisible > 0 and superframes > 0, "alt-ref encoding lacks invisible/superframe data")
        require(len(set(fields["frame_context_idx"])) > 1, "alt-ref context index never changes")
        require(1 in fields["ref_frame_sign_bias[3]"], "no future ALTREF sign bias")
    if case.name == "vp9-reset-keyframes":
        require(keyframes == [0, 8, 16], f"unexpected key-frame positions: {keyframes}")
    if case.show_existing:
        require(fields["show_existing_frame"].count(1) == 8 and
                fields["frame_to_show_map_idx"] == list(range(8)), "show-existing slots differ")
    return {"coded_headers": len(frame_headers), "visible_frames": shown,
            "invisible_frames": invisible, "superframe_packets": superframes,
            "show_existing_frames": fields["show_existing_frame"].count(1),
            "backward_adaptation_eligible_headers": adaptation,
            "keyframe_header_indices": keyframes,
            "observed": {name: sorted(set(values)) for name, values in sorted(fields.items())},
            "frame_headers": frame_headers}


def main():
    out = Path(sys.argv[1]).resolve()
    out.mkdir(parents=True, exist_ok=True)
    ffmpeg, ffprobe = shutil.which("ffmpeg"), shutil.which("ffprobe")
    require(ffmpeg and ffprobe, "ffmpeg and ffprobe must be available")
    threads = int(os.environ.get("MATRIX_THREADS", "2"))
    require(1 <= threads <= 64, "MATRIX_THREADS must be between 1 and 64")
    run([ffmpeg, "-version"], out / "ffmpeg-version.log")
    run([ffprobe, "-version"], out / "ffprobe-version.log")
    records, checksums = [], []
    for case in CASES:
        sample = out / f"{case.name}.ivf"
        reference = out / f"{case.name}.sw.nv12"
        if case.show_existing:
            header, seed = packets(out / "vp9-reset-keyframes.ivf")
            # The checked key frame refreshes every reference slot. Profile-0
            # show_existing_frame contains marker=2, profile=0, show_existing=1,
            # and the three-bit reference index, making one byte per packet.
            header = bytearray(header)
            struct.pack_into("<I", header, 24, case.frames)
            with sample.open("wb") as output:
                output.write(header)
                for timestamp, payload in enumerate([seed[0][2]] + [bytes([136 | i]) for i in range(8)]):
                    output.write(struct.pack("<IQ", len(payload), timestamp))
                    output.write(payload)
            (out / f"{case.name}.construction.json").write_text(json.dumps({
                "seed": "vp9-reset-keyframes.ivf", "seed_packet": 0,
                "seed_sha256": digest(out / "vp9-reset-keyframes.ivf"),
                "show_existing_payload_hex": [bytes([136 | i]).hex() for i in range(8)],
                "verification": "FFmpeg trace_headers, native VP9 decoder, and libvpx decoder"
            }, indent=2) + "\n", encoding="utf-8")
        else:
            command = [ffmpeg, "-hide_banner", "-loglevel", "info", "-nostdin", "-y",
                       "-f", "lavfi", "-i", f"testsrc2=size={case.width}x{case.height}:rate=30",
                       "-frames:v", str(case.frames), "-pix_fmt", "yuv420p", "-c:v", "libvpx-vp9",
                       "-profile:v", "0", "-threads", str(threads), "-deadline", "good", "-cpu-used", "4",
                       "-b:v", "800k" if case.altref else "0", "-crf", "32",
                       "-lag-in-frames", "25" if case.altref else "0",
                       "-auto-alt-ref", "1" if case.altref else "0", "-g", str(case.keyint),
                       "-tile-columns", str(case.tile_columns), "-tile-rows", str(case.tile_rows),
                       "-frame-parallel", "0", "-error-resilient", "0", "-aq-mode", "0"]
            if case.altref:
                passes = ["-passlogfile", str(out / f"{case.name}.pass")]
                run(command + passes + ["-pass", "1", "-f", "null", os.devnull],
                    out / f"{case.name}.pass1.log")
                command += passes + ["-pass", "2"]
            run(command + ["-f", "ivf", str(sample)], out / f"{case.name}.encode.log")
        headers = out / f"{case.name}.headers.log"
        run([ffmpeg, "-hide_banner", "-loglevel", "info", "-nostdin", "-i", str(sample),
             "-map", "0:v:0", "-c:v", "copy", "-bsf:v", "trace_headers", "-f", "null", "-"], headers)
        evidence = packet_evidence(sample)
        observations = inspect_headers(case, headers, evidence)
        (out / f"{case.name}.packets.json").write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
        (out / f"{case.name}.headers.json").write_text(json.dumps(observations, indent=2) + "\n", encoding="utf-8")
        probe = subprocess.run([ffprobe, "-v", "error", "-count_frames", "-select_streams", "v:0",
                                "-show_streams", "-of", "json", str(sample)], capture_output=True,
                               timeout=60, check=True)
        (out / f"{case.name}.probe.json").write_bytes(probe.stdout)
        stream = json.loads(probe.stdout)["streams"][0]
        require(stream["codec_name"] == "vp9" and stream["pix_fmt"] == "yuv420p", "unexpected format")
        require((stream["width"], stream["height"], int(stream["nb_read_frames"])) ==
                (case.width, case.height, case.frames), "decoded dimensions/frame count mismatch")
        decode = [ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-xerror", "-hwaccel", "none"]
        tail = ["-f", "ivf", "-i", str(sample), "-map", "0:v:0", "-fps_mode", "passthrough",
                "-c:v", "rawvideo", "-pix_fmt", "nv12", "-f", "rawvideo"]
        run(decode + ["-c:v", "vp9"] + tail + [str(reference)], out / f"{case.name}.software.log")
        frame_bytes = case.width * case.height * 3 // 2
        require(reference.stat().st_size == case.frames * frame_bytes, "unexpected NV12 byte count")
        if case.show_existing:
            independent = out / f"{case.name}.libvpx.nv12"
            run(decode + ["-c:v", "libvpx-vp9"] + tail + [str(independent)],
                out / f"{case.name}.libvpx.log")
            require(digest(reference) == digest(independent), "native/libvpx show-existing output differs")
            pixels = reference.read_bytes()
            require(all(pixels[i * frame_bytes:(i + 1) * frame_bytes] == pixels[:frame_bytes]
                        for i in range(case.frames)), "show-existing output differs from key frame")
        observations.pop("frame_headers")
        records.append({**dataclasses.asdict(case), **observations, "input": sample.name,
                        "software_frames": case.frames, "reference_bytes": reference.stat().st_size,
                        "sample_sha256": digest(sample), "reference_sha256": digest(reference)})
        for path in (sample, reference):
            checksums.append(f"{digest(path)}  {path.name}")
        (out / "encode-cases.json").write_text(json.dumps(records, indent=2) + "\n", encoding="utf-8")
        print(f"Generated {case.name}: visible={case.frames} "
              f"coded_headers={observations['coded_headers']} "
              f"invisible={observations['invisible_frames']} "
              f"superframes={observations['superframe_packets']}", flush=True)
    (out / "SHA256SUMS").write_text("\n".join(checksums) + "\n", encoding="utf-8")
    print(f"cases={len(records)} software_frames={sum(case.frames for case in CASES)}")


try:
    main()
except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
    print(f"VP9 extra fixture generation failed: {exc}", file=sys.stderr)
    sys.exit(1)
PY
