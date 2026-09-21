#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Bounded legal H.264 fixtures, verified headers, and software NV12 references.
# Requirements: python3, ffmpeg with libx264 and trace_headers, ffprobe.
# MATRIX_CASES selects comma-separated case names; MATRIX_THREADS defaults to 2.
set -eu
[ "$#" -le 1 ] || { printf 'Usage: %s [OUTPUT_DIRECTORY]\n' "$0" >&2; exit 2; }
TOOLS_DIR=${0%/*}
[ "$TOOLS_DIR" != "$0" ] || TOOLS_DIR=.
DRIVER_DIR=$(CDPATH= cd -- "$TOOLS_DIR/.." && pwd)
if [ "$#" -eq 1 ]; then OUT=$1; else OUT="$DRIVER_DIR/test-results/h264-matrix"; fi
exec python3 - "$OUT" <<'PY'
import collections
import csv
import dataclasses
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys


@dataclasses.dataclass(frozen=True)
class Case:
    name: str
    width: int = 320
    height: int = 240
    frames: int = 12
    profile: str = "high"
    cabac: int = 1
    refs: int = 4
    bframes: int = 2
    bpyramid: str = "none"
    slices: int = 1
    weightp: int = 0
    weightb: int = 0
    custom_cqm: bool = False
    fade: bool = False
    source: str = "testsrc2"


CASES = [
    Case("baseline-refs1", profile="baseline", cabac=0, refs=1, bframes=0),
    Case("baseline-refs4-slices4", frames=16, profile="baseline",
         cabac=0, refs=4, bframes=0, slices=4),
    Case("main-cabac-refs1", profile="main", refs=1, bframes=0),
    Case("main-cavlc-b2", frames=16, profile="main", cabac=0),
    Case("main-weighted-p", frames=16, profile="main", bframes=0,
         weightp=2, fade=True),
    Case("main-weighted-b", frames=16, profile="main", bframes=3, weightb=1),
    Case("high-cavlc-refs4", cabac=0, bframes=0),
    Case("high-bpyramid", frames=16, bframes=3, bpyramid="normal", weightb=1),
    Case("high-refs16", frames=16, refs=16, bframes=0),
    Case("high-slices4", width=640, height=360, slices=4),
    Case("high-custom-cqm", custom_cqm=True),
    Case("baseline-crop322x242", width=322, height=242, profile="baseline",
         cabac=0, refs=1, bframes=0),
    Case("main-crop640x360", width=640, height=360, profile="main"),
    Case("high-1920x1080", width=1920, height=1080, frames=8),
    Case("high-3840x2160", width=3840, height=2160, frames=8, refs=1,
         bframes=0, source="smptebars"),
]
PROFILE_IDC = {"baseline": 66, "main": 77, "high": 100}
CQM4I = [12 + 2 * (row + col) for row in range(4) for col in range(4)]
CQM4P = [16 + 2 * (row + col) for row in range(4) for col in range(4)]
SCAN4 = [0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15]
FIELD = re.compile(r"\b([a-zA-Z_]\w*(?:\[\d+\])*)\s+\S+\s+=\s+(-?\d+)\s*$")


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run(command, log, timeout):
    with log.open("wb") as handle:
        result = subprocess.run(command, stdout=handle, stderr=subprocess.STDOUT,
                                timeout=timeout, check=False)
    require(result.returncode == 0,
            f"command returned {result.returncode}; see {log.name}")


def parse_trace(text):
    fields = collections.defaultdict(list)
    packets = []
    matrices = []
    packet = None
    current_slice = None
    current_matrix = None
    for line in text.splitlines():
        if "Packet:" in line:
            packet = []
            packets.append(packet)
            current_slice = None
        if line.endswith("Slice Header"):
            require(packet is not None, "slice header appeared before a packet")
            current_slice = collections.defaultdict(list)
            packet.append(current_slice)
        match = FIELD.search(line)
        if not match:
            continue
        indexed_name, raw_value = match.groups()
        name = indexed_name.split("[", 1)[0]
        value = int(raw_value)
        fields[name].append(value)
        if current_slice is not None:
            current_slice[name].append(value)
        if name in ("pic_scaling_list_present_flag", "seq_scaling_list_present_flag"):
            current_matrix = {
                "kind": name.split("_", 1)[0],
                "index": int(re.search(r"\[(\d+)\]", indexed_name).group(1)),
                "present": value,
                "deltas": [],
            }
            matrices.append(current_matrix)
        elif name == "delta_scale" and current_matrix is not None:
            current_matrix["deltas"].append(value)
    return fields, packets, matrices


def single(fields, name, default=None):
    values = fields.get(name, [])
    if not values and default is not None:
        return default
    require(values and len(set(values)) == 1, f"missing or changing {name}: {values}")
    return values[0]


def has_nondefault_weight(slice_fields):
    for component in ("luma", "chroma"):
        denominator = single(slice_fields, component + "_log2_weight_denom", 0)
        for list_id in ("l0", "l1"):
            if any(value != (1 << denominator)
                   for value in slice_fields.get(f"{component}_weight_{list_id}", [])):
                return True
            if any(slice_fields.get(f"{component}_offset_{list_id}", [])):
                return True
    return False


def matrix_matches(group, expected):
    if not group["present"] or len(group["deltas"]) != 16:
        return False
    values = []
    previous = 8
    for delta in group["deltas"]:
        previous = (previous + delta) % 256
        if previous == 0:
            return False
        values.append(previous)
    return values == [expected[index] for index in SCAN4]


def verify(case, probe, headers, software):
    fields, packets, matrices = parse_trace(headers)
    streams = probe.get("streams", [])
    frames = probe.get("frames", [])
    require(len(streams) == 1, "expected one video stream")
    stream = streams[0]
    require(stream.get("codec_name") == "h264", "unexpected codec")
    require((stream.get("width"), stream.get("height")) == (case.width, case.height),
            "display dimensions differ from the requested dimensions")
    require(stream.get("pix_fmt") == "yuv420p", "fixture is not 8-bit 4:2:0")
    require(len(frames) == case.frames, "software-probed frame count differs")
    require(all(frame.get("interlaced_frame", 0) == 0 for frame in frames),
            "fixture contains interlaced pictures")
    require(single(fields, "frame_mbs_only_flag") == 1, "field-coded SPS")
    require(single(fields, "bit_depth_luma_minus8", 0) == 0 and
            single(fields, "bit_depth_chroma_minus8", 0) == 0, "unexpected bit depth")
    require(single(fields, "chroma_format_idc", 1) == 1, "unexpected chroma format")
    require(single(fields, "profile_idc") == PROFILE_IDC[case.profile],
            "libx264 selected a different profile")
    require(single(fields, "entropy_coding_mode_flag") == case.cabac,
            "CABAC/CAVLC request was not reflected in PPS")
    require(single(fields, "max_num_ref_frames") == case.refs,
            "reference limit request was not reflected in SPS")
    require(single(fields, "transform_8x8_mode_flag", 0) == int(case.profile == "high"),
            "8x8-transform enable differs from the requested High profile tools")

    packets = [packet for packet in packets if packet]
    require(len(packets) == case.frames, "trace packet/frame count differs")
    counts = [len(packet) for packet in packets]
    require(min(counts) == case.slices and max(counts) == case.slices,
            f"slice request differs from actual per-picture counts: {counts}")
    first_slices = [packet[0] for packet in packets]
    b_pictures = [s for s in first_slices if single(s, "slice_type") % 5 == 1]
    b_reference_pictures = sum(single(s, "nal_ref_idc") != 0 for s in b_pictures)
    require(bool(b_pictures) == bool(case.bframes), "B pictures were not encoded as requested")
    require(sum(frame.get("pict_type") == "B" for frame in frames) == len(b_pictures),
            "B picture counts disagree between headers and decoded frames")
    require((b_reference_pictures > 0) == (case.bpyramid != "none"),
            "B-pyramid request differs from actual reference B pictures")
    require(single(fields, "weighted_pred_flag") == int(case.weightp != 0),
            "weighted-P enable differs from PPS")
    require(single(fields, "weighted_bipred_idc") == (2 if case.weightb else 0),
            "weighted-B enable differs from PPS")
    weighted_slices = sum(has_nondefault_weight(s) for packet in packets for s in packet)
    if case.weightp:
        require(weighted_slices > 0, "weighted-P case contains only default weights")
    if case.custom_cqm:
        require(any(g["kind"] == "pic" and g["index"] == 0 and matrix_matches(g, CQM4I)
                    for g in matrices), "custom 4x4 intra-Y matrix was not encoded")
        require(any(g["kind"] == "pic" and g["index"] == 3 and matrix_matches(g, CQM4P)
                    for g in matrices), "custom 4x4 inter-Y matrix was not encoded")

    coded_width = (single(fields, "pic_width_in_mbs_minus1") + 1) * 16
    coded_height = (single(fields, "pic_height_in_map_units_minus1") + 1) * 16
    crop = [single(fields, "frame_crop_" + edge + "_offset", 0)
            for edge in ("left", "right", "top", "bottom")]
    require(coded_width - 2 * (crop[0] + crop[1]) == case.width and
            coded_height - 2 * (crop[2] + crop[3]) == case.height,
            "SPS coded/display crop dimensions disagree")
    require(software.stat().st_size == case.width * case.height * 3 // 2 * case.frames,
            "software NV12 file has an unexpected frame size or count")
    features = [
        case.profile, "CABAC" if case.cabac else "CAVLC",
        f"SPS max_num_ref_frames={case.refs}", f"{case.slices} slices/picture",
        f"B pictures={len(b_pictures)}", f"reference B pictures={b_reference_pictures}",
    ]
    if case.weightp:
        features.append(f"nondefault explicit weighted slices={weighted_slices}")
    if case.weightb:
        features.append("PPS weighted_bipred_idc=2 with B pictures")
    if case.custom_cqm:
        features.append("verified custom intra/inter 4x4 luma matrices")
    if (coded_width, coded_height) != (case.width, case.height):
        features.append(f"coded {coded_width}x{coded_height}, display {case.width}x{case.height}")
    return {
        "display_width": case.width, "display_height": case.height,
        "coded_width": coded_width, "coded_height": coded_height,
        "crop_offsets": crop, "frames": len(frames), "profile": stream["profile"],
        "profile_idc": single(fields, "profile_idc"), "level_idc": single(fields, "level_idc"),
        "pix_fmt": stream["pix_fmt"], "entropy_coding_mode_flag": case.cabac,
        "max_num_ref_frames": single(fields, "max_num_ref_frames"),
        "num_ref_idx_l0_default_active_minus1":
            single(fields, "num_ref_idx_l0_default_active_minus1"),
        "slice_count_min": min(counts), "slice_count_max": max(counts),
        "b_pictures": len(b_pictures), "b_reference_pictures": b_reference_pictures,
        "nondefault_weighted_slices": weighted_slices,
        "features": features,
    }


def write_manifest(out, manifest):
    (out / "matrix.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    with (out / "matrix.tsv").open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle, delimiter="\t", lineterminator="\n")
        writer.writerow(["case", "stream", "software_nv12", "width", "height",
                         "coded_width", "coded_height", "frames", "features"])
        for entry in manifest["cases"]:
            if entry["status"] != "verified":
                continue
            observed = entry["observed"]
            writer.writerow([entry["name"], entry["stream"], entry["software_nv12"],
                             observed["display_width"], observed["display_height"],
                             observed["coded_width"], observed["coded_height"],
                             observed["frames"], "; ".join(observed["features"])])


def main():
    out = pathlib.Path(sys.argv[1]).expanduser().resolve()
    threads = int(os.environ.get("MATRIX_THREADS", "2"))
    timeout = int(os.environ.get("MATRIX_TIMEOUT", "300"))
    require(1 <= threads <= 4, "MATRIX_THREADS must be between 1 and 4")
    require(30 <= timeout <= 1800, "MATRIX_TIMEOUT must be between 30 and 1800 seconds")
    ffmpeg = shutil.which(os.environ.get("FFMPEG", "ffmpeg"))
    ffprobe = shutil.which(os.environ.get("FFPROBE", "ffprobe"))
    require(ffmpeg and ffprobe, "ffmpeg and ffprobe are required")
    selected = {name.strip() for name in os.environ.get("MATRIX_CASES", "").split(",")
                if name.strip()}
    require(not selected - {case.name for case in CASES}, "unknown MATRIX_CASES entry")
    cases = [case for case in CASES if not selected or case.name in selected]
    out.mkdir(parents=True, exist_ok=True)
    versions = {}
    for label, executable in (("ffmpeg", ffmpeg), ("ffprobe", ffprobe)):
        result = subprocess.run([executable, "-version"], stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, check=True, timeout=30)
        versions[label] = result.stdout.splitlines()[0]
        (out / (label + "-version.txt")).write_text(result.stdout, encoding="utf-8")
    manifest = {"complete": False, "versions": versions, "encoder_threads": threads,
                "selected_cases": [case.name for case in cases], "cases": []}
    write_manifest(out, manifest)

    for case in cases:
        print(f"ENCODE {case.name}: {case.width}x{case.height}, {case.frames} frames", flush=True)
        source = f"{case.source}=size={case.width}x{case.height}:rate=12"
        if case.fade:
            source += ",fade=t=in:st=0:d=1"
        params = [
            f"threads={threads}", "lookahead-threads=1", "sync-lookahead=0",
            "rc-lookahead=0", "mbtree=0", "aq-mode=0", "me=dia", "subme=1",
            "trellis=0", "keyint=64", "scenecut=0", "open-gop=0",
            "aud=1", "repeat-headers=1", f"cabac={case.cabac}", f"ref={case.refs}",
            f"bframes={case.bframes}", "b-adapt=0", f"b-pyramid={case.bpyramid}",
            f"weightp={case.weightp}", f"weightb={case.weightb}",
            f"8x8dct={int(case.profile == 'high')}", f"slices={case.slices}",
        ]
        if case.custom_cqm:
            params += ["cqm4iy=" + ",".join(map(str, CQM4I)),
                       "cqm4py=" + ",".join(map(str, CQM4P))]
        stream = out / (case.name + ".h264")
        software = out / (case.name + ".sw.nv12")
        entry = {"name": case.name, "status": "pending", "requested": dataclasses.asdict(case),
                 "source_filter": source, "x264_params": ":".join(params),
                 "stream": stream.name, "software_nv12": software.name}
        manifest["cases"].append(entry)
        try:
            encode = [ffmpeg, "-hide_banner", "-loglevel", "info", "-nostdin", "-y",
                      "-filter_threads", "1", "-f", "lavfi", "-i", source,
                      "-frames:v", str(case.frames), "-an", "-pix_fmt", "yuv420p",
                      "-c:v", "libx264", "-preset", "veryfast", "-qp", "24",
                      "-profile:v", case.profile, "-x264-params", entry["x264_params"],
                      "-color_range", "tv", "-color_primaries", "bt709",
                      "-color_trc", "bt709", "-colorspace", "bt709", "-f", "h264", str(stream)]
            entry["encode_command"] = encode
            encode_log = out / (case.name + ".encode.log")
            run(encode, encode_log, timeout)
            require(not re.search(r"error parsing option|error setting option|unrecognized option|ignoring",
                                  encode_log.read_text(errors="replace"), re.I),
                    "encoder rejected or ignored an option; inspect the encode log")
            trace = [ffmpeg, "-hide_banner", "-loglevel", "info", "-nostdin",
                     "-c:v", "h264", "-threads", "1", "-i", str(stream),
                     "-map", "0:v:0", "-c:v", "copy", "-bsf:v", "trace_headers", "-f", "null", "-"]
            trace_log = out / (case.name + ".headers.log")
            run(trace, trace_log, timeout)
            probe_cmd = [ffprobe, "-v", "error", "-threads", "1", "-select_streams", "v:0",
                         "-count_frames", "-show_frames", "-show_streams", "-show_entries",
                         "stream=codec_name,profile,width,height,pix_fmt,level,nb_read_frames:"
                         "frame=pict_type,interlaced_frame,key_frame", "-of", "json", str(stream)]
            probe_result = subprocess.run(probe_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                          text=True, check=True, timeout=timeout)
            (out / (case.name + ".probe.json")).write_text(probe_result.stdout, encoding="utf-8")
            decode = [ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin", "-y",
                      "-hwaccel", "none", "-c:v", "h264", "-threads", "1",
                      "-err_detect", "explode", "-xerror", "-i", str(stream),
                      "-map", "0:v:0", "-an", "-sn", "-dn", "-fps_mode", "passthrough",
                      "-pix_fmt", "nv12", "-threads", "1", "-f", "rawvideo", str(software)]
            entry["software_decode_command"] = decode
            run(decode, out / (case.name + ".decode.log"), timeout)
            entry["observed"] = verify(case, json.loads(probe_result.stdout),
                                       trace_log.read_text(errors="replace"), software)
            entry["stream_sha256"] = sha256(stream)
            entry["software_sha256"] = sha256(software)
            entry["software_bytes"] = software.stat().st_size
            entry["status"] = "verified"
        except (RuntimeError, subprocess.SubprocessError, ValueError, KeyError) as error:
            entry["status"] = "failed"
            entry["error"] = str(error)
            write_manifest(out, manifest)
            raise
        (out / (case.name + ".case.json")).write_text(json.dumps(entry, indent=2) + "\n",
                                                     encoding="utf-8")
        write_manifest(out, manifest)
        print("VERIFIED " + case.name + ": " + "; ".join(entry["observed"]["features"]), flush=True)
    manifest["complete"] = True
    write_manifest(out, manifest)
    print(f"Verified {len(cases)} legal cases; manifest: {out / 'matrix.tsv'}", flush=True)


try:
    main()
except (RuntimeError, subprocess.SubprocessError, ValueError, OSError, KeyError) as error:
    print("FAIL: " + str(error), file=sys.stderr)
    sys.exit(1)
PY
