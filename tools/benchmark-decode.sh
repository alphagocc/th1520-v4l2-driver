#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Compare explicit GStreamer V4L2 stateless decoders with FFmpeg software decode.
# Usage: sh tools/benchmark-decode.sh [REPOSITORY_ROOT] [RESULT_DIR] [OPTIONS]
# Requires Linux, python3 and ffmpeg. The default comparison also needs gi/Gst
# and gst-inspect-1.0. --vendor-only uses h264_omx/hevc_omx without GStreamer.
# Loads/installs nothing. Output pixels are discarded without format conversion.
# Default inputs are the existing 1080p matrix clips. Complete Annex-B clips are
# repeated in one input/pipeline: this measures repeated short CVS, not a long GOP.
set -eu

TOOLS_DIR=${0%/*}
[ "$TOOLS_DIR" != "$0" ] || TOOLS_DIR=.
DRIVER_DIR=$(CDPATH= cd -- "$TOOLS_DIR/.." && pwd)
exec python3 - "$DRIVER_DIR" "$@" <<'PY'
import argparse
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import resource
import shlex
import shutil
import signal
import statistics
import subprocess
import sys
import tempfile
import time


GST_WORKER = r'''
import json
import sys
import time
import gi
gi.require_version("Gst", "1.0")
from gi.repository import Gst

Gst.init(None)
source, codec, timeout = sys.argv[1], sys.argv[2], float(sys.argv[3])
names = ("h264parse", "v4l2slh264dec") if codec == "h264" else ("h265parse", "v4l2slh265dec")
pipeline = Gst.Pipeline.new("benchmark")
elements = [Gst.ElementFactory.make(factory, name) for factory, name in
            [("filesrc", "input"), (names[0], "parser"), (names[1], "decoder"), ("fakesink", "output")]]
if any(element is None for element in elements):
    raise RuntimeError("required GStreamer element is unavailable: " + repr(names))
src, parser, decoder, sink = elements
src.set_property("location", source)
for name, value in [("sync", False), ("async", False), ("enable-last-sample", False),
                    ("silent", True), ("qos", False)]:
    sink.set_property(name, value)
if sink.find_property("stats") is None:
    raise RuntimeError("fakesink requires the GstBaseSink stats property (GStreamer >= 1.18)")
for element in elements:
    pipeline.add(element)
for left, right in zip(elements, elements[1:]):
    if not left.link(right):
        raise RuntimeError("could not link " + left.get_name() + " to " + right.get_name())
factory = decoder.get_factory()
if factory.get_name() != names[1] or factory.get_plugin_name() != "v4l2codecs":
    raise RuntimeError("unexpected hardware decoder factory/plugin")
started = time.perf_counter()
try:
    if pipeline.set_state(Gst.State.PLAYING) == Gst.StateChangeReturn.FAILURE:
        raise RuntimeError("GStreamer could not enter PLAYING")
    bus = pipeline.get_bus()
    message = bus.timed_pop_filtered(int(timeout * Gst.SECOND), Gst.MessageType.ERROR | Gst.MessageType.EOS)
    elapsed = time.perf_counter() - started
    if message is None:
        raise RuntimeError("GStreamer EOS timeout")
    if message.type == Gst.MessageType.ERROR:
        error, debug = message.parse_error()
        raise RuntimeError(str(error) + "; " + str(debug))
    stats = sink.get_property("stats")
    frames = int(stats.get_value("rendered"))
    dropped = int(stats.get_value("dropped"))
    caps = sink.get_static_pad("sink").get_current_caps()
    result = {"frames": frames, "dropped": dropped, "eos": True,
              "decoder": factory.get_name(), "plugin": factory.get_plugin_name(),
              "caps": caps.to_string() if caps else None, "pipeline_seconds": elapsed,
              "gstreamer": Gst.version_string()}
finally:
    pipeline.set_state(Gst.State.NULL)
print(json.dumps(result), flush=True)
'''


class BenchmarkFailure(Exception):
    pass


def require(condition, message):
    if not condition:
        raise BenchmarkFailure(message)


def stop(process):
    if process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
        process.wait(timeout=2)
    except ProcessLookupError:
        pass
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait(timeout=2)


def execute(command, log, environment, timeout):
    # communicate waits for pipe EOF, avoiding wait(timeout)'s polling interval
    # as an artificial component of short-process elapsed time.
    with log.open("wb") as output:
        output.write(("$ " + shlex.join(command) + "\n").encode())
        output.flush()
        before = resource.getrusage(resource.RUSAGE_CHILDREN)
        started = time.perf_counter()
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=output, env=environment, start_new_session=True)
        timed_out = False
        try:
            stdout, _ = process.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            stop(process)
            stdout, _ = process.communicate(timeout=2)
        except BaseException:
            stop(process)
            raise
        elapsed = time.perf_counter() - started
        after = resource.getrusage(resource.RUSAGE_CHILDREN)
        output.write(stdout)
        if timed_out:
            output.write(f"\nTimeout after {timeout} seconds.\n".encode())
    user = after.ru_utime - before.ru_utime
    system = after.ru_stime - before.ru_stime
    return {"rc": 124 if timed_out else process.returncode, "elapsed_seconds": elapsed,
            "user_seconds": user, "system_seconds": system,
            "cpu_percent": 100 * (user + system) / elapsed, "log": str(log)}, stdout.decode(errors="replace")


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def read_optional(path):
    try:
        return Path(path).read_text(errors="replace").strip().rstrip("\x00")
    except OSError:
        return None


def snapshot():
    patterns = ["/sys/devices/system/cpu/cpufreq/policy*/scaling_governor",
                "/sys/devices/system/cpu/cpufreq/policy*/scaling_cur_freq",
                "/sys/devices/system/cpu/cpufreq/policy*/scaling_min_freq",
                "/sys/devices/system/cpu/cpufreq/policy*/scaling_max_freq",
                "/sys/devices/system/cpu/cpufreq/policy*/scaling_driver",
                "/sys/class/thermal/thermal_zone*/type", "/sys/class/thermal/thermal_zone*/temp",
                "/sys/class/devfreq/*/cur_freq", "/sys/class/devfreq/*/governor"]
    files = ["/proc/loadavg", "/proc/meminfo", "/proc/cpuinfo", "/proc/modules",
             "/proc/device-tree/model"]
    for pattern in patterns:
        files += [str(path) for path in Path("/").glob(pattern.lstrip("/"))]
    return {name: read_optional(name) for name in files}


def software_command(ffmpeg, source, codec, threads, decoder=None):
    # Explicit *_omx selects the vendor hardware decoder even with hwaccel=none.
    # OMX library worker threads are internal; do not present -threads as a
    # hardware/library thread limit. Software reference decoding always uses 1.
    loglevel = "warning" if decoder in ("h264_omx", "hevc_omx") else "error"
    command = [ffmpeg, "-hide_banner", "-loglevel", loglevel, "-nostdin", "-nostats",
               "-xerror", "-err_detect", "explode", "-hwaccel", "none", "-c:v", decoder or codec]
    if threads is not None:
        command += ["-threads", str(threads)]
    return command + ["-f", codec, "-i", str(source),
            "-map", "0:v:0", "-an", "-sn", "-dn", "-fps_mode", "passthrough",
            "-c:v", "wrapped_avframe", "-f", "null", "-", "-progress", "pipe:1"]


def write_reports(folder, records, summaries):
    (folder / "runs.json").write_text(json.dumps(records, indent=2) + "\n")
    (folder / "summary.json").write_text(json.dumps(summaries, indent=2) + "\n")
    for name, rows in [("runs", records), ("summary", summaries)]:
        if rows:
            fields = list(dict.fromkeys(key for row in rows for key in row))
            with (folder / (name + ".tsv")).open("w", newline="") as stream:
                writer = csv.DictWriter(stream, fieldnames=fields, delimiter="\t")
                writer.writeheader()
                writer.writerows({key: json.dumps(value) if isinstance(value, (list, dict)) else value
                                  for key, value in row.items()} for row in rows)


def main():
    parser = argparse.ArgumentParser(prog="benchmark-decode.sh", description=(
        "Decode-only process-throughput benchmark. Inputs must be complete, independently "
        "decodable Annex-B H.264/HEVC clips with parameter sets and random-access start."))
    parser.add_argument("repository_root", type=Path, nargs="?", default=Path(sys.argv[1]))
    parser.add_argument("result_dir", type=Path, nargs="?")
    parser.add_argument("--input", type=Path, action="append", help="complete .h264/.h265/.hevc clip; repeat option for multiple inputs")
    parser.add_argument("--ffmpeg", default="ffmpeg", help="FFmpeg executable used for software and optional vendor decoding")
    parser.add_argument("--vendor-only", action="store_true", help="measure only vendor h264_omx/hevc_omx; software reference is still checked, GStreamer is unused")
    parser.add_argument("--no-calibration", action="store_true", help="use each input file unchanged, once per pass; skip calibration and ignore min-frames")
    parser.add_argument("--threads", default=None, help="comma-separated positive FFmpeg decoder thread counts; default 1 and available CPU count")
    parser.add_argument("--warmup", type=int, default=1, help="unmeasured complete passes per backend after calibration (default 1)")
    parser.add_argument("--rounds", type=int, default=3, help="measured passes per backend (default 3)")
    parser.add_argument("--min-frames", type=int, default=300, help="minimum frames in a repeated input (default 300)")
    parser.add_argument("--target-seconds", type=float, default=2.0, help="calibrate fastest backend toward this process duration (default 2)")
    parser.add_argument("--timeout", type=float, default=120, help="seconds allowed per process (default 120)")
    parser.add_argument("--max-input-mib", type=int, default=256, help="maximum generated repeated input size (default 256 MiB)")
    args = parser.parse_args(sys.argv[2:])
    require(sys.platform.startswith("linux"), "benchmark requires Linux process CPU accounting")
    require(args.warmup >= 1 and args.rounds >= 2 and args.min_frames >= 1,
            "warmup >= 1, rounds >= 2, and min-frames >= 1 are required")
    require(math.isfinite(args.target_seconds) and math.isfinite(args.timeout)
            and 0 < args.target_seconds < args.timeout and args.timeout >= 5 and args.max_input_mib > 0,
            "require 0 < target-seconds < timeout, timeout >= 5, and max-input-mib > 0")
    cpus = sorted(os.sched_getaffinity(0))
    try:
        threads = list(dict.fromkeys(int(value) for value in (args.threads or f"1,{len(cpus)}").split(",")))
    except ValueError as exc:
        raise BenchmarkFailure("--threads requires comma-separated positive integers") from exc
    require(threads and all(value > 0 for value in threads), "--threads counts must be positive; automatic thread selection is excluded")
    root = args.repository_root.resolve()
    sources = [path.resolve() for path in args.input] if args.input else [
        root / "test-results/h264-matrix/high-1920x1080.h264",
        root / "test-results/hevc-matrix/hevc-slices4-1080p.h265"]
    require(len(set(sources)) == len(sources), "input files must be unique")
    for source in sources:
        require(source.is_file() and source.stat().st_size > 0, f"missing input: {source}")
        require(source.suffix.lower() in (".h264", ".h265", ".hevc"), f"expected Annex-B input: {source}")
    commands = {"ffmpeg": shutil.which(args.ffmpeg)}
    if not args.vendor_only:
        commands["gst-inspect-1.0"] = shutil.which("gst-inspect-1.0")
    require(all(commands.values()), "required commands are unavailable: " + ", ".join(name for name, command in commands.items() if command is None))
    output = (args.result_dir or root / "test-results/benchmark-decode").resolve()
    output.mkdir(parents=True, exist_ok=True)
    folder = Path(tempfile.mkdtemp(prefix=time.strftime("run-%Y%m%d-%H%M%S-"), dir=output))
    print(f"Evidence: {folder}", flush=True)
    worker = folder / "gstreamer-worker.py"
    if not args.vendor_only:
        worker.write_text(GST_WORKER)
    environment = os.environ.copy()
    environment.update({"LC_ALL": "C", "GST_DEBUG": "0", "GST_DEBUG_NO_COLOR": "1",
                        "GST_REGISTRY": str(folder / "gstreamer-registry.bin")})
    metadata = {"started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                "uname": list(platform.uname()), "python": sys.version, "cpu_affinity": cpus,
                "decoder_thread_counts": [] if args.vendor_only else threads, "arguments": vars(args).copy(),
                "ffmpeg_executable": commands["ffmpeg"],
                "dynamic_loader_environment": {key: environment[key] for key in ("LD_LIBRARY_PATH", "LD_PRELOAD", "OMX_BELLAGIO_REGISTRY") if key in environment},
                "clock": vars(time.get_clock_info("perf_counter")), "environment_before": snapshot(),
                "gstreamer_environment": {key: value for key, value in environment.items() if key.startswith("GST_")},
                "method": {
                    "timing": "whole child process: startup, input probing, decode, drain and shutdown; perf_counter",
                    "cpu": "RUSAGE_CHILDREN user + system seconds; 100 percent is one logical CPU; includes all child threads",
                    "hardware_frames": "fakesink native stats.rendered after EOS, before NULL; dropped must equal zero",
                    "software_frames": "FFmpeg final progress frame with passthrough frame timing and wrapped_avframe null output",
                    "vendor_frames": "FFmpeg final progress frame using explicit h264_omx/hevc_omx vendor hardware decoder; wrapped_avframe null output",
                    "output": "native decoded frames discarded; no pixel-format conversion, display, clock synchronization or raw output I/O",
                    "scope": "end-to-end application decode throughput; hardware and software frontends differ",
                    "input": "original Annex-B file, unchanged" if args.no_calibration else "same cached complete Annex-B file for every backend; concatenated complete CVS if repeated",
                    "cache": "input generated/read and each backend prewarmed; filesystem caches are not flushed",
                    "frequency": "CPU/VPU governors, affinities and frequencies are observed, never changed",
                    "thread_policy": "vendor OMX library threads are internal; software reference uses 1 thread" if args.vendor_only else "positive explicit FFmpeg input decoder -threads; default 1 and process-available CPU count",
                    "order": "backend order rotates each measured round; no simultaneous decoders"}}
    (folder / "environment.json").write_text(json.dumps(metadata, indent=2, default=str) + "\n")
    (folder / "README.txt").write_text(
        "Whole-process decode throughput, including startup and drain. CPU 100% = one logical CPU.\n"
        "All backends read identical Annex-B bytes and discard their native decoded buffers.\n"
        "GStreamer: filesrc ! parser ! explicit V4L2 stateless decoder ! fakesink sync=false.\n"
        "FFmpeg: explicit software decoder, explicit input threads, wrapped_avframe null output.\n"
        "Vendor-only: explicit h264_omx/hevc_omx hardware decoder, software 1-thread reference, no GStreamer.\n"
        "Vendor decoder availability is checked with ffmpeg -decoders; OMX library threads are internal.\n"
        "Native output memory/pixel layouts and application frontends differ. No display or color conversion is measured.\n"
        "Short clips repeat complete coded video sequences in one process; results are specific to that workload.\n"
        "Use --input with a longer pre-encoded clip for sustained performance. Encoding is outside measurements.\n"
        "--no-calibration reads the supplied file unchanged exactly once per pass, ignoring min-frames.\n"
        "Each run verifies EOS/progress=end, exact software-reference frame count, and zero GStreamer drops.\n"
        "Calibration/warmup are excluded from summary; rounds rotate backend order. Cache is warm.\n"
        "Short measurements are flagged; startup cost is retained, never estimated or subtracted.\n"
        "This benchmark verifies counts and process completion, not decoded pixel correctness.\n")
    preflights = [("ffmpeg-version", [commands["ffmpeg"], "-version"])]
    if args.vendor_only:
        preflights.append(("ffmpeg-decoders", [commands["ffmpeg"], "-hide_banner", "-decoders"]))
    else:
        preflights += [("gst-version", [commands["gst-inspect-1.0"], "--version"]),
                      ("python-gst", [sys.executable, "-c", "import gi; gi.require_version('Gst','1.0'); from gi.repository import Gst; Gst.init(None); print(Gst.version_string())"])]
    codecs = {"h264" if source.suffix.lower() == ".h264" else "hevc" for source in sources}
    if not args.vendor_only:
        for codec in sorted(codecs):
            element = "v4l2slh264dec" if codec == "h264" else "v4l2slh265dec"
            preflights.append((element, [commands["gst-inspect-1.0"], element]))
    for name, command in preflights:
        result, _ = execute(command, folder / (name + ".log"), environment, min(args.timeout, 30))
        require(result["rc"] == 0, f"preflight failed: {name}; see {result['log']}")
    if args.vendor_only:
        listing = (folder / "ffmpeg-decoders.log").read_text(errors="replace")
        available = set(re.findall(r"^\s*V\S*\s+(\S+)\s+", listing, re.MULTILINE))
        required = {codec + "_omx" for codec in codecs}
        require(required <= available, "FFmpeg lacks required vendor decoders: " + ", ".join(sorted(required - available))
                + f"; see {folder / 'ffmpeg-decoders.log'}")

    records, summaries = [], []
    modes = ([("vendor", None)] if args.vendor_only else
             [("hardware", None)] + [(f"software-{count}t", count) for count in threads])
    failures = 0
    for index, source in enumerate(sources):
        label = re.sub(r"[^A-Za-z0-9_.-]", "_", source.name)
        case_dir = folder / f"{index:02d}-{label}"
        case_dir.mkdir()
        codec = "h264" if source.suffix.lower() == ".h264" else "hevc"
        repeated = case_dir / ("input.h264" if codec == "h264" else "input.h265")
        expected = None
        repetitions = 1

        def run(mode, count, phase, number, input_file, expected_frames):
            command = ([sys.executable, str(worker), str(input_file), codec, str(args.timeout - 1)]
                       if mode == "hardware" else software_command(commands["ffmpeg"], input_file, codec, count,
                                                                     codec + "_omx" if mode == "vendor" else codec))
            row, stdout = execute(command, case_dir / f"{phase}-{number:02d}-{mode}.log", environment, args.timeout)
            row.update({"input": str(source), "codec": codec, "backend": mode, "threads": count,
                        "decoder": codec + "_omx" if mode == "vendor" else codec,
                        "phase": phase, "round": number, "repetitions": repetitions,
                        "expected_frames": expected_frames, "frames": None, "fps": None,
                        "status": "FAIL", "error": None})
            try:
                require(row["rc"] == 0, f"decoder exited with status {row['rc']}")
                if mode == "hardware":
                    data = json.loads(stdout)
                    row.update(data)
                    require(data.get("eos") and data.get("dropped") == 0, "missing EOS or dropped GStreamer frames")
                else:
                    matches = re.findall(r"^frame=\s*(\d+)\s*$", stdout, re.MULTILINE)
                    require(matches and re.search(r"^progress=end\s*$", stdout, re.MULTILINE), "missing final FFmpeg frame/progress record")
                    row["frames"] = int(matches[-1])
                require(row["frames"] > 0, "decoder produced no frames")
                require(expected_frames is None or row["frames"] == expected_frames,
                        f"frame count {row['frames']} != expected {expected_frames}")
                row["fps"] = row["frames"] / row["elapsed_seconds"]
                row["status"] = "PASS"
            except (BenchmarkFailure, ValueError, KeyError, TypeError) as exc:
                row["error"] = str(exc)
            records.append(row)
            write_reports(folder, records, summaries)
            print(f"{row['status']} {source.name} {phase} {number} {mode}: "
                  f"frames={row['frames']} seconds={row['elapsed_seconds']:.3f} "
                  f"fps={row['fps'] or 0:.2f} CPU={row['cpu_percent']:.1f}%", flush=True)
            require(row["status"] == "PASS", f"{mode}: {row['error']}; see {row['log']}")
            return row

        try:
            base = run("software-reference", 1, "reference", 0, source, None)
            expected = base["frames"]
            limit_warning = None
            if args.no_calibration:
                repeated = source
            else:
                max_repetitions = args.max_input_mib * 1024 * 1024 // source.stat().st_size
                require(max_repetitions >= 1, "source exceeds --max-input-mib")
                repetitions = max(1, math.ceil(args.min_frames / expected))
                require(repetitions <= max_repetitions, "--min-frames requires more than --max-input-mib; use a longer or smaller sample")
                for calibration in range(6):
                    with repeated.open("wb") as output_file:
                        for _ in range(repetitions):
                            with source.open("rb") as input_file:
                                shutil.copyfileobj(input_file, output_file, 1024 * 1024)
                    trials = [run(mode, count, "calibration", calibration, repeated, expected * repetitions)
                              for mode, count in modes]
                    fastest = min(row["elapsed_seconds"] for row in trials)
                    if fastest >= args.target_seconds:
                        break
                    if repetitions == max_repetitions or calibration == 5:
                        limit_warning = "calibration input/attempt limit reached before target duration"
                        break
                    repetitions = min(max_repetitions, repetitions * max(2, min(8, math.ceil(args.target_seconds / fastest))))
            sample_info = {"source": str(source), "source_sha256": digest(source), "source_frames": expected,
                           "source_bytes": source.stat().st_size, "input_sha256": digest(repeated),
                           "input_bytes": repeated.stat().st_size, "input_path": str(repeated), "repetitions": repetitions,
                           "calibration": "disabled" if args.no_calibration else "automatic",
                           "expected_frames": expected * repetitions, "duration_warning": limit_warning,
                           "workload": "repeated complete CVS" if repetitions > 1 else "single input clip"}
            (case_dir / "input.json").write_text(json.dumps(sample_info, indent=2) + "\n")
            for number in range(args.warmup):
                for mode, count in modes:
                    run(mode, count, "warmup", number, repeated, expected * repetitions)
            for number in range(args.rounds):
                offset = number % len(modes)
                for mode, count in modes[offset:] + modes[:offset]:
                    run(mode, count, "measured", number, repeated, expected * repetitions)
            for mode, count in modes:
                rows = [row for row in records if row["input"] == str(source) and row["backend"] == mode and row["phase"] == "measured"]
                seconds = [row["elapsed_seconds"] for row in rows]
                rates = [row["fps"] for row in rows]
                summary = {**sample_info, "backend": mode, "threads": count, "status": "PASS",
                           "decoder": rows[0]["decoder"],
                           "rounds": len(rows), "median_seconds": statistics.median(seconds),
                           "median_fps": statistics.median(rates), "min_fps": min(rates), "max_fps": max(rates),
                           "median_cpu_percent": statistics.median(row["cpu_percent"] for row in rows),
                           "median_cpu_seconds": statistics.median(row["user_seconds"] + row["system_seconds"] for row in rows),
                           "short_measurement": min(seconds) < args.target_seconds,
                           "throughput_cv_percent": 100 * statistics.stdev(rates) / statistics.mean(rates)}
                summaries.append(summary)
                print(f"SUMMARY {source.name} {mode}: {summary['median_fps']:.2f} fps "
                      f"CPU={summary['median_cpu_percent']:.1f}% "
                      f"range={summary['min_fps']:.2f}..{summary['max_fps']:.2f}", flush=True)
            if not args.vendor_only:
                hardware_fps = next(row["median_fps"] for row in summaries if row.get("source") == str(source) and row["backend"] == "hardware")
                for row in summaries:
                    if row.get("source") == str(source) and row["backend"] != "hardware":
                        row["hardware_speedup"] = hardware_fps / row["median_fps"]
        except (OSError, BenchmarkFailure) as exc:
            failures += 1
            summaries.append({"source": str(source), "status": "FAIL", "error": str(exc)})
            print(f"FAIL {source.name}: {exc}", file=sys.stderr, flush=True)
        write_reports(folder, records, summaries)
    metadata["environment_after"] = snapshot()
    metadata["finished_utc"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    (folder / "environment.json").write_text(json.dumps(metadata, indent=2, default=str) + "\n")
    print(f"Evidence: {folder / 'summary.json'}", flush=True)
    return 1 if failures else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("benchmark interrupted", file=sys.stderr)
        sys.exit(130)
    except (OSError, BenchmarkFailure) as exc:
        print(str(exc), file=sys.stderr)
        sys.exit(2)
PY
