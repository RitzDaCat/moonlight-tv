#!/usr/bin/env python3
"""Bounded native surface experiment; temporary SDK header/build/streams are removed."""
import argparse
import contextlib
import hashlib
import json
import os
from pathlib import Path
import resource
import re
import select
import signal
import sqlite3
import subprocess
import tempfile
import time
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kind', choices=('native', 'native-paired', 'native-policy', 'native-output', 'native-rgb', 'gl-semaphore', 'shared', 'motion'), default='native')
    parser.add_argument('--motion-size', choices=('4k', '1080p'), default='4k')
    parser.add_argument('--motion-case', choices=('internal', 'oracle', 'wrong', 'pixel-flow'))
    parser.add_argument('--green-sm', type=int, choices=(0, 2, 4, 8, 16, 32, 64, 128), default=0)
    parser.add_argument('--codec', choices=('hevc', 'h264'), default='hevc')
    parser.add_argument('--h264-cavlc', action='store_true')
    parser.add_argument('--bitstream-wait',choices=('blocking','poll'),default='blocking')
    parser.add_argument('--context-wait', choices=('blocking', 'spin', 'yield', 'auto'), default='blocking')
    parser.add_argument('--sdk-version', choices=('13.0', '13.1'))
    parser.add_argument('--split-mode', choices=('forced', 'disabled', 'auto'), default='forced')
    parser.add_argument('--io-stream', choices=('explicit', 'legacy'), default='explicit')
    parser.add_argument('--no-filler',action='store_true',help='Disable preset bitstream padding, required for GPU output')
    parser.add_argument('--profile-nsys', type=Path, help='Optional installed Nsight Systems executable; instrumented diagnostic run')
    parser.add_argument('--graphics-contention', action='store_true')
    parser.add_argument('--paced-120', action='store_true', help='Sleep before measurement to submit at 120 FPS')
    parser.add_argument('--unpaired', action='store_true', help='Shared mode: separate copy/direct encoder sessions')
    args = parser.parse_args()
    if args.kind=='shared'and args.bitstream_wait!='blocking':parser.error('Completion polling is restricted to native controlled inputs')
    if args.unpaired and args.kind != 'shared':
        parser.error('--unpaired requires --kind shared')
    if args.motion_case and args.kind != 'motion':
        parser.error('--motion-case requires --kind motion')
    if args.codec != 'hevc' and args.kind != 'native':
        parser.error('H.264 requires --kind native')
    if args.h264_cavlc and args.codec != 'h264':
        parser.error('--h264-cavlc requires --codec h264')
    if args.green_sm and args.context_wait != 'blocking':
        parser.error('Green contexts cannot use the regular context wait control')
    if args.kind=='native-output'and(args.io_stream!='legacy'or args.split_mode!='disabled'):
        parser.error('Native GPU output requires --io-stream legacy --split-mode disabled')
    if args.kind=='native-output':args.no_filler=True
    sdk = args.sdk_version or ('13.1' if args.kind.startswith('native') or args.kind in ('motion','gl-semaphore') else '13.0')
    root = Path(__file__).resolve().parents[2]
    sources = root/'host/tools'
    output = root/'code-review/engine-bench'
    output.mkdir(parents=True, exist_ok=True)
    tag = 'native-surface-'+args.kind+'-sdk'+sdk+'-'+args.split_mode+('-graphics' if args.graphics_contention else '-idle')
    if args.profile_nsys:
        args.profile_nsys = args.profile_nsys.resolve(strict=True)
        tag += '-profile'
    if args.io_stream != 'explicit':
        tag += '-'+args.io_stream
    if args.no_filler:tag+='-no-filler'
    if args.paced_120:
        tag += '-120fps'
    if args.unpaired:
        tag += '-unpaired'
    if args.kind == 'motion':
        tag += '-'+args.motion_size+('-'+args.motion_case if args.motion_case else '')
    if args.green_sm:
        tag += '-green'+str(args.green_sm)
    if args.codec != 'hevc':
        tag += '-'+args.codec+('-cavlc' if args.h264_cavlc else '-cabac')
    if args.context_wait != 'blocking':
        tag += '-wait-'+args.context_wait
    if args.bitstream_wait!='blocking':tag+='-completion-'+args.bitstream_wait
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    started = time.monotonic()
    deadline = started+120
    env = dict(os.environ, CUDA_CACHE_DISABLE='1', __GL_SHADER_DISK_CACHE='0',
               MESA_SHADER_CACHE_DISABLE='true')
    owned = []
    receipt = {'kind': args.kind, 'sdk_version': sdk, 'split_mode': args.split_mode, 'graphics_contention': args.graphics_contention,
               'io_stream': args.io_stream,
               'preset_filler_data_disabled':args.no_filler,
               'green_sm': args.green_sm,
               'codec': args.codec, 'h264_entropy': 'cavlc' if args.h264_cavlc else 'cabac',
               'context_wait': args.context_wait,'bitstream_wait':args.bitstream_wait,
               'pace_fps': 120 if args.paced_120 else 0,
               'unpaired': args.unpaired,
               'instrumented': bool(args.profile_nsys),
               'boundary': 'Standalone native input/encode; no capture, game, TV or deployment',
               'warmup': 20, 'frames_per_fixture': 120, 'deadline_seconds': 120,
               'cycle_boundary': 'Before source update through packet completion and input retirement; fixture packet memory copy included, file I/O excluded',
               'sources': {name: hashlib.sha256((sources/name).read_bytes()).hexdigest()
                           for name in ('run_native_surface_bench.py', 'nvenc_gl_probe.cpp', 'vulkan_shared_surface.h',
                                        'shared_nv12.comp', 'gpu_graphics_load.cpp', 'pixel_motion.h')},
               'commands': []}

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

    def run(argv, label, timeout=35, binary=False):
        argv = [str(arg) for arg in argv]
        process = subprocess.Popen(argv, env=env, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, start_new_session=True)
        owned.append(process)
        try:
            stdout, stderr = process.communicate(timeout=max(.1, min(timeout, deadline-time.monotonic())))
        except subprocess.TimeoutExpired:
            stop(process)
            raise RuntimeError(label+' timed out')
        if not binary:
            (output/(tag+'-'+label+'.stdout')).write_bytes(stdout)
        (output/(tag+'-'+label+'.stderr')).write_bytes(stderr)
        receipt['commands'].append({'label': label, 'argv': argv, 'exit': process.returncode})
        if process.returncode:
            raise RuntimeError(label+': '+stderr.decode(errors='replace')[-1500:])
        return stdout if binary else stdout.decode()

    try:
        # Finally reaps subprocesses inside this scope, before TemporaryDirectory cleanup.
        with tempfile.TemporaryDirectory(prefix='sunshine-native-surface-') as temporary:
            try:
                work = Path(temporary)
                if args.profile_nsys:
                    env.update(XDG_CONFIG_HOME=str(work/'config'), XDG_CACHE_HOME=str(work/'cache'),
                               XDG_DATA_HOME=str(work/'data'), TMPDIR=str(work))
                revision, expected_digest = (
                    ('n13.0.19.0', '4fe4094541ef0f8a13249d97a8692dc5f835a6e9dd42eeadb3e2f7321d54dc7e') if sdk == '13.0' else
                    ('71acc268a541f7d7f33cf0387229e3015cfb4977', '8776fddcb8febc6aec4d73989b1f21831eb30306bc583da55b4bf0c14a1dc228'))
                url = f'https://raw.githubusercontent.com/FFmpeg/nv-codec-headers/{revision}/include/ffnvcodec/nvEncodeAPI.h'
                with urllib.request.urlopen(url, timeout=15) as response:
                    header = response.read()
                digest = hashlib.sha256(header).hexdigest()
                if digest != expected_digest:
                    raise RuntimeError('SDK header checksum mismatch')
                (work/'nvEncodeAPI.h').write_bytes(header)
                receipt['header'] = {'url': url, 'sha256': digest}
                flags = ['-O3', '-std=c++17', '-I/opt/cuda/include', '-I'+str(work)]
                if args.profile_nsys:
                    flags += ['-DSUNSHINE_TRACE_NVTX']
                libraries = ['-lEGL', '-lGL', '-lcuda', '-ldl']
                probe_args = ['--4k-native-paired-probe' if args.kind == 'native-paired' else '--4k-native-array-probe']
                if args.kind == 'native-policy':
                    probe_args = ['--4k-policy-probe']
                if args.kind == 'native-output':
                    probe_args = ['--4k-output-probe']
                if args.kind == 'native-rgb':
                    probe_args = ['--rgb-array-probe']
                if args.kind == 'motion':
                    flags += ['-DSUNSHINE_MOTION_PROBE']
                    probe_args = ['--motion-probe', '--motion-size', args.motion_size]
                    if args.motion_case:
                        probe_args += ['--motion-case', args.motion_case]
                    revision = 'edb50da3cf849840d680249aa6dbef248ebce2ca'
                    headers = {'nvOpticalFlowCommon.h': 'a83f6045e5c470b35a6c50672f92e082b78d55752fe51cc20eb1a2738be05b9d',
                               'nvOpticalFlowCuda.h': '07defc79637fb9893f2a06204972eaa6bcf9e5bea400c88440439ebaaa39f115'}
                    receipt['optical_flow_headers'] = []
                    for name, expected in headers.items():
                        url = f'https://raw.githubusercontent.com/NVIDIA/NVIDIAOpticalFlowSDK/{revision}/{name}'
                        with urllib.request.urlopen(url, timeout=15) as response:
                            data = response.read()
                        digest = hashlib.sha256(data).hexdigest()
                        if digest != expected:
                            raise RuntimeError('Optical-flow header checksum mismatch: '+name)
                        (work/name).write_bytes(data)
                        receipt['optical_flow_headers'].append({'url': url, 'sha256': digest})
                    receipt['motion_boundary'] = 'Completed native input copy + optional hardware flow, readback, hint construction + encode/packet retirement; source frames uploaded before timing'
                if args.kind=='gl-semaphore':
                    flags+=['-DSUNSHINE_VULKAN_SHARED_PROBE'];libraries+=['-lvulkan'];probe_args=['--4k-semaphore-probe']
                probe_args+=['--bitstream-wait',args.bitstream_wait]
                if args.kind == 'shared':
                    flags += ['-DSUNSHINE_VULKAN_SHARED_PROBE']
                    libraries += ['-lvulkan']
                    shader = sources/'shared_nv12.comp'
                    run(['glslangValidator', '-V', shader, '-o', work/'producer.spv'], 'shader')
                    probe_args = ['--4k-shared-probe' if args.unpaired else '--4k-shared-paired-probe', '--shared-shader', work/'producer.spv']
                run(['clang++', *flags, sources/'nvenc_gl_probe.cpp', *libraries, '-o', work/'probe'], 'compile')
                load = None
                if args.graphics_contention:
                    run(['clang++', '-O3', '-std=c++17', sources/'gpu_graphics_load.cpp',
                         '-lEGL', '-lGL', '-o', work/'load'], 'load-compile')
                    load = subprocess.Popen([str(work/'load'), '60'], env=env, stdout=subprocess.PIPE,
                                            stderr=subprocess.PIPE, start_new_session=True)
                    owned.append(load)
                    if not select.select([load.stdout], [], [], 8)[0]:
                        raise RuntimeError('Graphics load readiness timeout')
                    ready = load.stdout.readline()
                    receipt['graphics_calibration'] = json.loads(ready)
                    if not receipt['graphics_calibration'].get('ready'):
                        raise RuntimeError('Graphics load did not become ready')
                try:
                    command = [work/'probe', *probe_args, '--split-mode', args.split_mode,
                               '--io-stream', args.io_stream, '--green-sm', args.green_sm,
                               '--native-codec', args.codec, '--context-wait', args.context_wait,
                               '--pace-fps', '120' if args.paced_120 else '0', '--write-streams', work]
                    if args.h264_cavlc:
                        command += ['--h264-cavlc']
                    if args.no_filler:command+=['--no-filler']
                    if args.profile_nsys:
                        command = [args.profile_nsys, 'profile', '--sample=none', '--cpuctxsw=none',
                                   '--trace=cuda,nvtx,nvvideo', '--wait=all', '--force-overwrite=true',
                                   '--output', work/'trace', *command]
                    stdout = run(command, 'probe', 40)
                    receipt['samples'] = [json.loads(line) for line in stdout.splitlines() if line.startswith('{')]
                    receipt['load_alive_at_probe_end'] = load.poll() is None if load else None
                    if load and not receipt['load_alive_at_probe_end']:
                        raise RuntimeError('Graphics load expired during measurement; exclude mixed coverage')
                finally:
                    if load is not None:
                        stdout, stderr = stop(load)
                        (output/(tag+'-load.stdout')).write_bytes(ready+stdout)
                        (output/(tag+'-load.stderr')).write_bytes(stderr)
                        receipt['graphics_draws'] = json.loads(stdout.splitlines()[-1]) if stdout else None
                if args.profile_nsys:
                    run([args.profile_nsys, 'export', '--type=sqlite', '--force-overwrite=true',
                         '--output', work/'trace.sqlite', work/'trace.nsys-rep'], 'profile-export')
                    records = {}
                    with contextlib.closing(sqlite3.connect(work/'trace.sqlite')) as connection:
                        connection.row_factory = sqlite3.Row
                        tables = [row[0] for row in connection.execute("SELECT name FROM sqlite_master WHERE type='table'")]
                        receipt['profile_tables'] = {table: connection.execute('SELECT count(*) FROM "'+table+'"').fetchone()[0]
                                                     for table in tables}
                        for table in tables:
                            if table in ('StringIds', 'NVTX_EVENTS', 'CUPTI_ACTIVITY_KIND_KERNEL',
                                         'CUPTI_ACTIVITY_KIND_MEMCPY', 'CUPTI_ACTIVITY_KIND_RUNTIME',
                                         'CUPTI_ACTIVITY_KIND_SYNCHRONIZATION', 'DIAGNOSTIC_EVENT') or table.startswith('NVVIDEO'):
                                records[table] = [dict(row) for row in connection.execute('SELECT * FROM "'+table+'"')]
                    string_fields = {'nameId', 'demangledName', 'shortName', 'mangledName', 'textId', 'jsonTextId'}
                    referenced = {value for table, rows in records.items() if table != 'StringIds'
                                  for row in rows for key, value in row.items()
                                  if key in string_fields and isinstance(value, int)}
                    records['StringIds'] = [row for row in records.get('StringIds', []) if row['id'] in referenced]
                    # Retain scoped traces, excluding environment/process inventories. Raw reports stay temporary.
                    (output/(tag+'-records.json')).write_text(json.dumps(records, indent=1,
                        default=lambda value: value.hex() if isinstance(value, bytes) else str(value))+'\n')
                decoded = []
                fixtures = sorted(work.glob('*.h264' if args.codec == 'h264' else '*.hevc'))
                for fixture in fixtures:
                    metadata = json.loads(run(['ffprobe', '-v', 'error', '-select_streams', 'v:0',
                                              '-show_entries', 'stream=width,height', '-of', 'json', fixture],
                                             fixture.stem+'-geometry'))
                    w, h = (1920, 1080) if args.kind == 'motion' and args.motion_size == '1080p' else (3840, 2160)
                    if metadata['streams'][0] != {'width': w, 'height': h}:
                        raise RuntimeError('Wrong decoded geometry: '+fixture.name)
                    md5 = run(['ffmpeg', '-v', 'error', '-xerror', '-threads', '4', '-i', fixture,
                               '-map', '0:v:0', '-fps_mode', 'passthrough', '-f', 'framemd5', '-'], fixture.stem+'-decode')
                    hashes = [line.split(',')[-1].strip() for line in md5.splitlines()
                              if line and not line.startswith('#')]
                    if len(hashes) != 120 or (args.kind in('shared','native-rgb') and len(set(hashes)) != 120):
                        raise RuntimeError('Wrong decoded frame sequence: '+fixture.name)
                    bitstream = fixture.read_bytes()
                    nal_types = [(nal[0] & 31) if args.codec == 'h264' else (nal[0] >> 1) & 63
                                 for nal in re.split(b'\x00\x00\x00?\x01', bitstream)[1:] if len(nal) >= 2]
                    decoded.append({'name': fixture.name, 'frames': len(hashes),
                                    'vcl_nal_units': sum(1 <= kind <= 5 if args.codec == 'h264' else kind < 32 for kind in nal_types),
                                    'decoded_hashes_sha256': hashlib.sha256('\n'.join(hashes).encode()).hexdigest(),
                                    'stream_sha256': hashlib.sha256(bitstream).hexdigest()})
                expected = 8 if args.kind in('native-policy','native-output','native-rgb') else (2 if args.motion_case else 8) if args.kind == 'motion' else 6 if args.kind == 'native' else 4 if args.unpaired else 2
                if len(decoded) != expected:
                    raise RuntimeError('Missing encoder fixtures')
                receipt['decoded'] = decoded
                receipt['all_decoded_frames_equal_across_paths'] = len({item['decoded_hashes_sha256'] for item in decoded}) == 1
                if not receipt['all_decoded_frames_equal_across_paths'] and not args.kind.startswith('native') and args.kind != 'motion':
                    raise RuntimeError('Decoder mismatch between input paths')
                if args.kind == 'motion':
                    import numpy as np
                    reference_path = work/f'pixel-motion-{w}x{h}-source.nv12'
                    reference = np.frombuffer(reference_path.read_bytes(), dtype=np.uint8).reshape(2,h*3//2,w).astype(np.int16)
                    receipt['source_fixture_sha256'] = hashlib.sha256(reference_path.read_bytes()).hexdigest()
                    for fixture, result in zip(fixtures, decoded):
                        raw = run(['ffmpeg', '-v', 'error', '-xerror', '-threads', '4', '-i', fixture,
                                   '-vf', r'select=eq(n\,0)+eq(n\,119)', '-fps_mode', 'passthrough',
                                   '-pix_fmt', 'nv12', '-f', 'rawvideo', '-'], fixture.stem+'-pixels', binary=True)
                        pixels = np.frombuffer(raw,dtype=np.uint8).reshape(2,h*3//2,w).astype(np.int16)
                        result['source_errors'] = []
                        for i, frame in enumerate((0,119)):
                            delta = pixels[i].astype(np.float64)-reference[i]
                            mse = float(np.square(delta[:h]).mean())
                            result['source_errors'].append({'frame':frame, 'luma_mae':float(np.abs(delta[:h]).mean()),
                                'chroma_mae':float(np.abs(delta[h:]).mean()),'luma_psnr_db':10*np.log10(255**2/mse) if mse else None})
                    receipt['quality_boundary'] = 'First/last source error recorded for all moving colored fixtures; no matched game-quality acceptance or error threshold assumed'
                elif not receipt['all_decoded_frames_equal_across_paths'] or args.kind in('shared','native-rgb'):
                    # Native layouts may choose different compressed coding decisions.
                    # Check source error before classifying a bitwise mismatch as corruption.
                    import numpy as np
                    yy, xx = np.indices((2160, 3840), dtype=np.uint16)
                    reference = (16+((xx//8+yy//8)%220)).astype(np.int16)
                    if args.kind=='native-rgb':reference=(16+(((xx//8+yy//8)%256)*219+127)//255).astype(np.int16)
                    del yy, xx
                    for fixture, result in zip(fixtures, decoded):
                        raw = run(['ffmpeg', '-v', 'error', '-xerror', '-threads', '4', '-i', fixture,
                                   '-vf', r'select=eq(n\,0)+eq(n\,119)', '-fps_mode', 'passthrough',
                                   '-pix_fmt', 'nv12', '-f', 'rawvideo', '-'], fixture.stem+'-pixels', binary=True)
                        pixels = np.frombuffer(raw, dtype=np.uint8).reshape(2, 3240, 3840)
                        errors = []
                        for i, frame in enumerate((0, 119)):
                            expected = reference.copy()
                            if args.kind == 'shared':
                                row = (frame*8) % (2160-64)
                                expected[row:row+64] = 16+(frame*13) % 220
                            if args.kind=='native-rgb':
                                for f in range(frame+1):
                                    row=(f*8)%(2160-64)
                                    expected[row:row+64]=16+(((f*13)%256)*219+127)//255
                            errors.append({'frame': frame,
                                           'luma_mae': float(np.abs(pixels[i, :2160].astype(np.int16)-expected).mean()),
                                           'chroma_mae': float(np.abs(pixels[i, 2160:].astype(np.int16)-128).mean())})
                        result['source_errors'] = errors
                    receipt['source_error_check_passed'] = all(error['luma_mae'] < 2 and error['chroma_mae'] < 1
                                                              for item in decoded for error in item['source_errors'])
                    if not receipt['source_error_check_passed']:
                        raise RuntimeError('Decoded native input fails known-source error check')
                    receipt['quality_boundary'] = 'First/last synthetic gray source error checked; not matched game/color quality. See the sequence equality field separately.'
                receipt['result'] = 'completed'
            finally:
                for process in owned:
                    if process.poll() is None:
                        stop(process)
    except Exception as error:
        receipt['result'] = 'failed'
        receipt['error'] = str(error)
    receipt['elapsed_seconds'] = time.monotonic()-started
    path = output/(tag+'.json')
    path.write_text(json.dumps(receipt, indent=2)+'\n')
    print(json.dumps({key: receipt[key] for key in ('result', 'elapsed_seconds')}))
    if 'error' in receipt:
        print(receipt['error'])
    for row in receipt.get('samples', []):
        print(json.dumps(row))
    print(path)
    return 0 if receipt['result'] == 'completed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
