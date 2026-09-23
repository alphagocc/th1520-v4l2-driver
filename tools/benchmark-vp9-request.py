#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Measure the same VP9 input with FFmpeg Request and software decoding.

Hardware output stays in drm_prime; software output stays in yuv420p. Both
are wrapped_avframe to a null muxer. Runtime includes startup and probing.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import resource
import statistics
import subprocess
import time


def sha256(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ffmpeg', type=Path, required=True)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--frames', type=int, default=600)
    parser.add_argument('--rounds', type=int, default=3)
    parser.add_argument('--software-threads', type=int, default=4)
    parser.add_argument('--timeout', type=int, default=240)
    args = parser.parse_args()
    if min(args.frames, args.rounds, args.software_threads, args.timeout) < 1:
        parser.error('numeric arguments must be positive')
    args.output.mkdir(parents=True, exist_ok=True)
    if (args.output/'summary.json').exists():
        parser.error('output already contains summary.json')
    binary, source = args.ffmpeg.resolve(), args.input.resolve()
    version = subprocess.run([str(binary), '-version'], capture_output=True,
                             text=True, check=True, timeout=10).stdout
    (args.output/'ffmpeg-version.txt').write_text(version)
    provenance = {'ffmpeg': str(binary), 'ffmpeg_sha256': sha256(binary),
                  'input': str(source), 'input_sha256': sha256(source),
                  'frames': args.frames, 'rounds': args.rounds,
                  'hardware_threads': 1, 'software_threads': args.software_threads,
                  'cpu_percent_basis': '100% is one logical CPU',
                  'measurement': 'process elapsed time including startup and probing',
                  'uname': list(os.uname())}
    (args.output/'provenance.json').write_text(json.dumps(provenance, indent=2)+'\n')
    results = []

    def run(mode, number, warmup=False):
        label = f'{mode}-' + ('warmup' if warmup else f'round-{number}')
        command = [str(binary), '-hide_banner', '-loglevel', 'verbose',
                   '-nostdin', '-nostats', '-benchmark']
        if mode == 'hardware':
            command += ['-hwaccel', 'v4l2request', '-hwaccel_output_format', 'drm_prime']
        else:
            command += ['-hwaccel', 'none']
        command += ['-c:v', 'vp9', '-threads', str(1 if mode == 'hardware' else args.software_threads),
                    '-f', 'ivf', '-i', str(source), '-map', '0:v:0', '-an', '-sn', '-dn',
                    '-fps_mode', 'passthrough', '-c:v', 'wrapped_avframe', '-pix_fmt',
                    '+drm_prime' if mode == 'hardware' else '+yuv420p',
                    '-f', 'null', '-', '-progress', 'pipe:1']
        stdout, stderr = args.output/(label+'.progress'), args.output/(label+'.log')
        (args.output/(label+'.command.json')).write_text(json.dumps(command, indent=2)+'\n')
        usage0 = resource.getrusage(resource.RUSAGE_CHILDREN)
        start = time.perf_counter()
        with stdout.open('w') as out, stderr.open('w') as err:
            result = subprocess.run(command, stdout=out, stderr=err, timeout=args.timeout)
        elapsed = time.perf_counter()-start
        usage1 = resource.getrusage(resource.RUSAGE_CHILDREN)
        progress = dict(line.split('=', 1) for line in stdout.read_text().splitlines() if '=' in line)
        frames = int(progress.get('frame', '0'))
        row = {'mode': mode, 'round': number, 'warmup': warmup,
               'returncode': result.returncode, 'frames': frames,
               'progress_end': progress.get('progress') == 'end',
               'elapsed_seconds': elapsed,
               'cpu_user_seconds': usage1.ru_utime-usage0.ru_utime,
               'cpu_system_seconds': usage1.ru_stime-usage0.ru_stime}
        row['fps'] = frames/elapsed
        row['cpu_percent'] = 100*(row['cpu_user_seconds']+row['cpu_system_seconds'])/elapsed
        results.append(row)
        (args.output/'runs.json').write_text(json.dumps(results, indent=2)+'\n')
        print(f'{label}: rc={result.returncode} frames={frames} fps={row["fps"]:.2f} '
              f'cpu={row["cpu_percent"]:.1f}% elapsed={elapsed:.3f}s', flush=True)
        if result.returncode or frames != args.frames or not row['progress_end']:
            raise RuntimeError(f'{label} did not complete the expected frame count')

    run('hardware', 0, True)
    run('software', 0, True)
    for number in range(1, args.rounds+1):
        order = ('hardware', 'software') if number % 2 else ('software', 'hardware')
        for mode in order:
            run(mode, number)
    summary = {}
    for mode in ('hardware', 'software'):
        rows = [r for r in results if r['mode'] == mode and not r['warmup']]
        summary[mode] = {'median_fps': statistics.median(r['fps'] for r in rows),
                         'median_cpu_percent': statistics.median(r['cpu_percent'] for r in rows),
                         'frames_per_run': args.frames, 'runs': rows}
    summary['speed_ratio'] = summary['hardware']['median_fps']/summary['software']['median_fps']
    (args.output/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
    print(json.dumps(summary, indent=2), flush=True)


if __name__ == '__main__':
    main()
