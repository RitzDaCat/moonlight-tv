#!/usr/bin/env python3
"""Isolated nibble/folding/HEVC experiments; no downloads, service changes or TV access."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import resource
import signal
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--residual', action='store_true', help='Lossless predicted residual transport, without NVENC')
    parser.add_argument('--rle', action='store_true', help='Compress the residual mode map; requires --residual')
    parser.add_argument('--sparse', action='store_true', help='Lossless indexed corrections with automatic directory compression')
    parser.add_argument('--estimate-motion',action='store_true',help='Sparse experiment: derive translation and brightness from captured pixels on the GPU')
    parser.add_argument('--mean-bias',action='store_true',help='Control experiment: mean instead of robust modal brightness')
    args = parser.parse_args()
    if args.sparse and args.rle:parser.error('--sparse already selects automatic directory compression')
    if args.estimate_motion and not args.sparse:parser.error('--estimate-motion requires --sparse')
    if args.mean_bias and not args.estimate_motion:parser.error('--mean-bias requires --estimate-motion')
    if args.sparse:args.residual=True
    if args.rle and not args.residual:
        parser.error('--rle requires --residual')
    tag = 'pixel-residual' if args.residual else 'surface-packing'
    if args.rle:
        tag += '-rle'
    if args.sparse:tag='pixel-sparse'
    if args.estimate_motion:tag+='-estimated-'+('mean'if args.mean_bias else 'modal')
    root = Path(__file__).resolve().parents[2]
    sources = root / 'host/tools'
    output = root / 'code-review/engine-bench'
    output.mkdir(parents=True, exist_ok=True)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    start = time.monotonic()
    deadline = start + 120
    env = dict(os.environ, CUDA_CACHE_DISABLE='1', __GL_SHADER_DISK_CACHE='0',
               MESA_SHADER_CACHE_DISABLE='true', PYTHONDONTWRITEBYTECODE='1')
    receipt = {
        'boundary': 'Synthetic ready NV12 transforms and separate encode-only service; no game, capture, network, LG decode or deployment',
        'deadline_seconds': 120, 'source_size': [3840, 2160], 'source_frames': 4,
        'encode_warmup': 10, 'encode_samples': 40, 'codec': 'hevc_nvenc',
        'encoder_policy': 'P1 ULL single-pass forced split CBR23Mbps 120FPS; no B frames/lookahead; maximum-rate submission',
        'fold_interpretation': 'Separable sinh/asinh coordinate companding, k=2, fixed center; not a claim about intrinsic geometric curvature',
        'quality_boundary': 'Limited-range NV12 code-value MAE/PSNR, not perceptual/game/TV quality acceptance',
        'sources': {name: hashlib.sha256((sources / name).read_bytes()).hexdigest()
                    for name in ('surface_packing.cu', 'pixel_residual.cuh', 'pixel_sparse.cuh', 'nvenc_latency.cpp', 'run_surface_packing_bench.py')},
        'commands': [], 'transforms': [], 'encodes': [], 'decoded_quality': [],
    }
    owned = []

    def stop(process):
        if process.poll() is None:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        try:
            return process.communicate(timeout=2)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            return process.communicate()

    def run(command, name, cwd):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError('120-second experiment deadline reached')
        command = [str(x) for x in command]
        began = time.monotonic()
        p = subprocess.Popen(command, cwd=cwd, env=env, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, text=True, start_new_session=True)
        owned.append(p)
        status = {'name': name, 'argv': command}
        receipt['commands'].append(status)
        stdout = stderr = ''
        try:
            stdout, stderr = p.communicate(timeout=remaining)
        except BaseException:
            stdout, stderr = stop(p)
            raise
        finally:
            status['seconds'] = time.monotonic() - began
            status['exit'] = p.returncode
            (output / (tag + '-' + name + '.stdout')).write_text(stdout)
            (output / (tag + '-' + name + '.stderr')).write_text(stderr)
        if p.returncode:
            raise RuntimeError(f'{name} failed: {stderr[-2000:]}')
        return stdout

    def rows(stdout):
        return [json.loads(line) for line in stdout.splitlines() if line.startswith('{')]

    try:
        with tempfile.TemporaryDirectory(prefix='sunshine-surface-packing-', dir='/tmp') as directory:
            temp = Path(directory)
            receipt['temporary_directory'] = str(temp)
            run(['nvidia-smi', '--query-gpu=name,driver_version,pstate,clocks.gr,clocks.mem',
                 '--format=csv,noheader'], 'gpu', temp)
            run(['/opt/cuda/bin/nvcc', '-O3', '-std=c++17', '-ccbin', '/usr/bin/g++-14',
                 '-arch=sm_89', *(['-DSUNSHINE_RESIDUAL_PROBE'] if args.residual else []),
                 *(['-DSUNSHINE_SPARSE_PROBE']if args.sparse else []),sources / 'surface_packing.cu', '-o', temp / 'packing'], 'build-gpu', temp)
            if args.residual:
                run(['/opt/cuda/bin/compute-sanitizer', '--tool', 'memcheck', '--error-exitcode', '7',
                     '--padding', '32', temp / 'packing', '--sparse-self-test'if args.sparse else '--residual-self-test'], 'memcheck', temp)
                receipt['boundary'] = 'Synthetic ready NV12/reference through complete CPU packet and independent GPU reconstruction; no network/TV/capture/deployment'
                receipt['residual'] = rows(run([temp / 'packing', '--sparse-mean-bench'if args.mean_bias else '--sparse-estimated-bench'if args.estimate_motion else '--sparse-bench'if args.sparse else '--residual-rle-bench' if args.rle else '--residual-bench'], 'bench', temp))
                receipt['lossless'] = True
            else:
                run(['clang++', '-O3', '-std=c++17', '-I/opt/cuda/include', sources / 'nvenc_latency.cpp',
                     '-lavcodec', '-lavutil', '-lcuda', '-o', temp / 'encode'], 'build-nvenc', temp)
                run(['/opt/cuda/bin/compute-sanitizer', '--tool', 'memcheck', '--error-exitcode', '7',
                     '--padding', '32', temp / 'packing', '--self-test'], 'memcheck', temp)
                transforms = rows(run([temp / 'packing', '--generate', temp], 'transforms', temp))
                receipt['self_test'] = transforms.pop(0)
                receipt['transforms'] = transforms
                names = [row['mode'] for row in transforms if row['round'] == 0]
                receipt['fixture_hashes'] = {name: hashlib.sha256((temp / (name + '.nv12')).read_bytes()).hexdigest()
                                              for name in names}
                if receipt['fixture_hashes']['copy'] != receipt['fixture_hashes']['nibble_lossless']:
                    raise RuntimeError('lossless nibble fixture differs from original')
                validated_streams = {}
                for round_number in range(2):
                    for name in names if round_number == 0 else reversed(names):
                        half = name in ('uniform_half', 'hyperbolic_half')
                        w, h = (1920, 1080) if half else (3840, 2160)
                        destination = temp / f'{round_number}-{name}'
                        destination.mkdir()
                        record = rows(run([temp / 'encode', '--fixture-probe', temp / (name + '.nv12'),
                                           w, h, destination], f'encode-{round_number}-{name}', temp))[0]
                        record.update(mode=name, round=round_number)
                        stream = destination / f'hevc_nvenc-{w}x{h}-disabled-forced.hevc'
                        stream_hash = hashlib.sha256(stream.read_bytes()).hexdigest()
                        record['stream_sha256'] = stream_hash
                        record['stream_bytes_including_warmup'] = stream.stat().st_size
                        receipt['encodes'].append(record)
                        reconstruction_key = (stream_hash, name if name in ('row_fold', 'uniform_half', 'hyperbolic_half') else 'identity')
                        if reconstruction_key in validated_streams:
                            record['decode_equal_to'] = validated_streams[reconstruction_key]
                            continue
                        decoded = destination / 'first-last.nv12'
                        run(['ffmpeg', '-v', 'error', '-nostdin', '-threads', '2', '-i', stream,
                             '-vf', 'select=eq(n\\,0)+eq(n\\,49)', '-fps_mode', 'passthrough',
                             '-pix_fmt', 'nv12', '-f', 'rawvideo', decoded], f'decode-{round_number}-{name}', temp)
                        if decoded.stat().st_size != w * h * 3:
                            raise RuntimeError('first/last decoded frame count or geometry mismatch')
                        quality = rows(run([temp / 'packing', '--decoded-quality', name, decoded, temp / 'copy.nv12'],
                                           f'quality-{round_number}-{name}', temp))
                        receipt['decoded_quality'].extend(dict(item, round=round_number) for item in quality)
                        # A folded stream's inverse is part of validation, so only reuse
                        # identical payloads with the same reconstruction operation.
                        validated_streams[reconstruction_key] = f'{round_number}-{name}'
            receipt['result'] = 'pass'
    except BaseException as error:
        receipt['result'] = 'failed'
        receipt['error'] = str(error)
        raise
    finally:
        for p in owned:
            if p.poll() is None:
                stop(p)
        receipt['seconds'] = time.monotonic() - start
        receipt['owned_processes_reaped'] = all(p.poll() is not None for p in owned)
        receipt['temporary_directory_removed'] = not Path(receipt.get('temporary_directory', '/does-not-exist')).exists()
        (output / (tag + '.json')).write_text(json.dumps(receipt, indent=2) + '\n')
        if receipt['result'] != 'pass':
            (output / (tag + '-failed.json')).write_text(json.dumps(receipt, indent=2) + '\n')
    print(f"{tag} experiments passed in {receipt['seconds']:.2f}s; {output / (tag + '.json')}")


if __name__ == '__main__':
    main()
