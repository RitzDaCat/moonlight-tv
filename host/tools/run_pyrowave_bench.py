#!/usr/bin/env python3
"""Build pinned small upstream sources in /tmp and measure completed PyroWave work."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import resource
import signal
import subprocess
import tarfile
import tempfile
import time
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--graphics-contention', action='store_true')
    parser.add_argument('--codec', choices=('hevc-vulkan', 'h264-vulkan'), help='Test standard PyroEnc Vulkan Video instead of PyroWave')
    parser.add_argument('--video-policy', choices=('baseline', 'tu16', 'tiles2', 'merge1', 'tu16-tiles2', 'fast-all'), default='baseline')
    parser.add_argument('--video-cqp28', action='store_true')
    parser.add_argument('--draw-ms', type=float, default=4.)
    parser.add_argument('--half-math', action='store_true', help='Use upstream FP16 arithmetic variant')
    parser.add_argument('--full-storage', action='store_true', help='Use upstream FP32 storage variant')
    parser.add_argument('--occupancy', choices=('0', '0.5', '1'), help='Explicit command/device control for VK_NV_compute_occupancy_priority')
    parser.add_argument('--gpu-timestamps', action='store_true', help='Instrument explicit GPU command spans and calibrated queue delay')
    parser.add_argument('--only-4k', action='store_true')
    parser.add_argument('--rate', choices=('23', '100', '200', '400'))
    args = parser.parse_args()
    if not .05 <= args.draw_ms <= 10:
        parser.error('--draw-ms must be .05..10')
    if args.half_math and args.full_storage:
        parser.error('Select half math or full storage, not both')
    if args.codec and (args.half_math or args.full_storage or args.occupancy or args.gpu_timestamps or args.only_4k or args.rate):
        parser.error('PyroEnc codec probe uses its own fixed matrix and precision')
    if (args.video_policy != 'baseline' or args.video_cqp28) and not args.codec:
        parser.error('Video policy controls require --codec')
    if args.video_policy != 'baseline' and args.codec != 'hevc-vulkan':
        parser.error('Transform/tile/merge policies are HEVC-only experiments')
    if args.gpu_timestamps and not args.occupancy:
        parser.error('--gpu-timestamps requires --occupancy')
    root = Path(__file__).resolve().parents[2]
    source = root/'host/tools'
    output = root/'code-review/engine-bench'
    output.mkdir(parents=True, exist_ok=True)
    tag = ('pyroenc-'+args.codec if args.codec else 'pyrowave')+'-'+('graphics' if args.graphics_contention else 'idle')
    if args.graphics_contention:
        tag += '-draw'+str(args.draw_ms).replace('.', 'p')
    if args.half_math:
        tag += '-half-math'
    if args.full_storage:
        tag += '-full-storage'
    if args.occupancy:
        tag += '-occupancy'+args.occupancy.replace('.', 'p')
    if args.gpu_timestamps:
        tag += '-timestamps'
    if args.only_4k:
        tag += '-4k'
    if args.rate:
        tag += '-rate'+args.rate
    if args.video_policy != 'baseline':
        tag += '-'+args.video_policy
    if args.video_cqp28:
        tag += '-cqp28'
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    started = time.monotonic()
    deadline = started+240
    receipt = {'boundary': 'Owned synthetic GPU source through complete CPU packets; separate local GPU decode. No game/capture/network/TV/deployment.',
               'deadline_seconds': 240, 'source_files': {name: hashlib.sha256((source/name).read_bytes()).hexdigest()
                    for name in ('run_pyrowave_bench.py', 'pyrowave_latency.cpp', 'pyroenc_latency.cpp', 'pyroenc_policy.patch', 'gpu_graphics_load.cpp')},
               'downloads': [], 'commands': [], 'graphics_contention': args.graphics_contention,
               'half_math': args.half_math, 'full_storage': args.full_storage, 'draw_target_ms': args.draw_ms,
               'codec': args.codec or 'pyrowave', 'video_policy': args.video_policy, 'video_cqp28': args.video_cqp28,
               'compute_occupancy_priority': args.occupancy, 'instrumented': args.gpu_timestamps,
               'selected_rate_mbps': args.rate, 'only_4k': args.only_4k}
    owned = []

    def stop(p):
        if p.poll() is None:
            try:
                os.killpg(p.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        try:
            return p.communicate(timeout=2)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            return p.communicate()

    def run(argv, label, timeout=90):
        p = subprocess.Popen([str(a) for a in argv], env=env, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, start_new_session=True)
        owned.append(p)
        begin = time.monotonic()
        try:
            stdout, stderr = p.communicate(timeout=max(.1, min(timeout, deadline-begin)))
        except BaseException:
            stop(p)
            raise
        (output/(tag+'-'+label+'.stdout')).write_bytes(stdout)
        (output/(tag+'-'+label+'.stderr')).write_bytes(stderr)
        receipt['commands'].append({'argv': [str(a) for a in argv], 'label': label,
                                    'exit': p.returncode, 'seconds': time.monotonic()-begin})
        if p.returncode:
            raise RuntimeError(label+': '+(stderr or stdout).decode(errors='replace')[-1800:])
        return stdout.decode()

    def fetch(repo, revision, destination, excluded=(), skip_cpp_headers=False):
        url = f'https://codeload.github.com/{repo}/tar.gz/{revision}'
        with urllib.request.urlopen(url, timeout=20) as response:
            data = response.read(12*1024*1024+1)
        if len(data) > 12*1024*1024:
            raise RuntimeError('Source archive exceeds 12 MiB bound')
        files = size = 0
        with tarfile.open(fileobj=io.BytesIO(data), mode='r:gz') as archive:
            for entry in archive:
                path = PurePosixPath(entry.name)
                relative = PurePosixPath(*path.parts[1:])
                if not entry.isfile() or not relative.parts or relative.parts[0] in excluded:
                    continue
                if skip_cpp_headers and relative.suffix == '.hpp':
                    continue
                if relative.is_absolute() or '..' in relative.parts:
                    raise RuntimeError('Unsafe archive path')
                size += entry.size
                if size > 30*1024*1024 or entry.size > 4*1024*1024:
                    raise RuntimeError('Extracted source exceeds bound')
                target = destination/str(relative)
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(archive.extractfile(entry).read())
                files += 1
        receipt['downloads'].append({'url': url, 'sha256': hashlib.sha256(data).hexdigest(),
                                     'compressed_bytes': len(data), 'source_bytes': size, 'files': files})

    try:
        with tempfile.TemporaryDirectory(prefix='sunshine-pyrowave-', dir='/tmp') as directory:
            receipt['temporary_directory'] = directory
            work = Path(directory)
            env = dict(os.environ, CUDA_CACHE_DISABLE='1', __GL_SHADER_DISK_CACHE='0',
                       MESA_SHADER_CACHE_DISABLE='true', PYTHONDONTWRITEBYTECODE='1',
                       XDG_CONFIG_HOME=str(work/'config'), XDG_CACHE_HOME=str(work/'cache'),
                       XDG_DATA_HOME=str(work/'data'), TMPDIR=str(work))
            try:
                if args.codec:
                    upstream = work/'pyroenc'
                    upstream.mkdir()
                    for name in ('pyroenc.cpp', 'pyroenc.hpp', 'pyroenc_vk_table.inl', 'shaders/rgb_to_yuv.inc', 'LICENSE'):
                        url = 'https://raw.githubusercontent.com/HansKristian-Work/pyroenc/05b1dfd4d9d19eee3b604165fda9a790ea2667d0/'+name
                        with urllib.request.urlopen(url, timeout=15) as response:
                            data = response.read(256*1024+1)
                        if len(data)>256*1024:
                            raise RuntimeError('PyroEnc source file exceeds bound')
                        destination = upstream/name
                        destination.parent.mkdir(parents=True, exist_ok=True)
                        destination.write_bytes(data)
                        receipt['downloads'].append({'url':url,'sha256':hashlib.sha256(data).hexdigest(),'source_bytes':len(data)})
                    policy_flags=[]
                    if args.video_policy != 'baseline':
                        run(['patch','-p1','--batch','--forward','-d',upstream,'-i',source/'pyroenc_policy.patch'], 'policy-apply')
                        if args.video_policy in ('tu16', 'tu16-tiles2', 'fast-all'):
                            policy_flags+=['-DSUNSHINE_PYROENC_TU16']
                        if args.video_policy in ('tiles2', 'tu16-tiles2', 'fast-all'):
                            policy_flags+=['-DSUNSHINE_PYROENC_TILES2']
                        if args.video_policy in ('merge1', 'fast-all'):
                            policy_flags+=['-DSUNSHINE_PYROENC_MERGE1']
                        receipt['patched_upstream_sha256']=hashlib.sha256((upstream/'pyroenc.cpp').read_bytes()).hexdigest()
                    run(['clang++', '-O3', '-std=c++17', *policy_flags, '-I'+str(upstream), source/'pyroenc_latency.cpp',
                         upstream/'pyroenc.cpp', '-lvulkan', '-o', work/'pyroenc-probe'], 'build')
                else:
                    pyro = work/'pyrowave'
                    fetch('Themaister/pyrowave', '21f099d24df0ab0e4c10c3288c924b6b1717e581', pyro,
                          excluded=('eval-results', 'assets', 'metal', 'metrics'))
                    granite = pyro/'Granite'
                    fetch('Themaister/Granite', '53090429539bf3404250d18fe58db971f0907c04', granite,
                          excluded=('assets', 'tests', 'renderer', 'viewer', 'audio', 'physics', 'slangmosh', 'tools'))
                    fetch('zeux/volk', '47cddf7ed97b94118a08aacb548a411188e016cc', granite/'third_party/volk')
                    fetch('KhronosGroup/Vulkan-Headers', '6802bb4733b63ed5efd3adb308a6c885ef180ea1',
                          granite/'third_party/khronos/vulkan-headers', excluded=('registry', 'tests'), skip_cpp_headers=True)
                    # Add only our isolated probe; no upstream install or setup script is run.
                    with (pyro/'CMakeLists.txt').open('a') as f:
                        f.write('\nadd_executable(latency-probe "'+str(source/'pyrowave_latency.cpp')+'")\n'
                                'target_include_directories(latency-probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})\n'
                                'target_link_libraries(latency-probe PRIVATE pyrowave-shared Vulkan::Headers vulkan)\n')
                    build = work/'build'
                    run(['cmake', '-S', pyro, '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
                         '-DCMAKE_POLICY_VERSION_MINIMUM=3.5', '-DGRANITE_RENDERDOC_CAPTURE=OFF',
                         '-DPYROWAVE_FP32_MATH='+('OFF' if args.half_math else 'ON'),
                         '-DPYROWAVE_FP32_STORAGE='+('ON' if args.full_storage else 'OFF')], 'configure')
                    run(['cmake', '--build', build, '--target', 'latency-probe', 'pyrowave-c-test', '-j8'], 'build')
                    run([build/'pyrowave-c-test'], 'upstream-tests', 40)
                load = None
                if args.graphics_contention:
                    import select
                    run(['clang++', '-O3', '-std=c++17', source/'gpu_graphics_load.cpp',
                         '-lEGL', '-lGL', '-o', work/'load'], 'load-compile')
                    load = subprocess.Popen([str(work/'load'), '60', str(args.draw_ms)], env=env, stdout=subprocess.PIPE,
                                            stderr=subprocess.PIPE, start_new_session=True)
                    owned.append(load)
                    if not select.select([load.stdout], [], [], 8)[0]:
                        raise RuntimeError('Graphics readiness timeout')
                    ready = load.stdout.readline()
                    receipt['graphics_calibration'] = json.loads(ready)
                try:
                    command = [work/'pyroenc-probe', 'h264' if args.codec.startswith('h264') else 'hevc', work] if args.codec else [build/'latency-probe']
                    if args.video_cqp28:
                        command += ['--cqp28']
                    if args.occupancy:
                        command += ['--occupancy', args.occupancy]
                    if args.gpu_timestamps:
                        command += ['--gpu-timestamps']
                    if args.only_4k:
                        command += ['--only-4k']
                    if args.rate:
                        command += ['--rate', args.rate]
                    stdout = run(command, 'probe', 50)
                    receipt['load_alive_at_probe_end'] = load.poll() is None if load else None
                    if load and not receipt['load_alive_at_probe_end']:
                        raise RuntimeError('Graphics load expired during the probe; exclude mixed coverage')
                    receipt['samples'] = [json.loads(line) for line in stdout.splitlines() if line.startswith('{')]
                finally:
                    if load is not None:
                        stdout, stderr = stop(load)
                        (output/(tag+'-load.stdout')).write_bytes(ready+stdout)
                        (output/(tag+'-load.stderr')).write_bytes(stderr)
                        receipt['graphics_draws'] = json.loads(stdout.splitlines()[-1]) if stdout else None
                if args.codec:
                    import numpy as np
                    decoded = []
                    for fixture in sorted(work.glob('*.h264' if args.codec.startswith('h264') else '*.hevc')):
                        w,h=map(int,fixture.stem.split('-')[0].split('x'))
                        metadata=json.loads(run(['ffprobe','-v','error','-select_streams','v:0','-show_entries',
                            'stream=width,height,profile,pix_fmt,level,has_b_frames','-of','json',fixture],fixture.stem+'-geometry'))
                        stream=metadata['streams'][0]
                        if stream['width']!=w or stream['height']!=h or stream['has_b_frames']:
                            raise RuntimeError('Wrong decoded geometry or unexpected B frames')
                        md5=run(['ffmpeg','-v','error','-xerror','-threads','4','-i',fixture,'-fps_mode','passthrough',
                                 '-f','framemd5','-'],fixture.stem+'-decode')
                        hashes=[line.split(',')[-1].strip() for line in md5.splitlines() if line and not line.startswith('#')]
                        if len(hashes)!=120:
                            raise RuntimeError('Wrong complete decoded frame count')
                        # Read only requested first/last pixels into Python; all frames were decoded above.
                        rawpath=work/(fixture.stem+'.nv12')
                        run(['ffmpeg','-v','error','-xerror','-threads','4','-i',fixture,'-vf',r'select=eq(n\,0)+eq(n\,119)',
                             '-fps_mode','passthrough','-pix_fmt','nv12','-f','rawvideo',rawpath],fixture.stem+'-pixels')
                        pixels=np.frombuffer(rawpath.read_bytes(),dtype=np.uint8).reshape(2,h*3//2,w)
                        yy,xx=np.indices((h,w),dtype=np.uint16);reference=(16+((xx//8+yy//8)%220)).astype(np.int16)
                        errors=[{'frame':frame,'luma_mae':float(np.abs(pixels[i,:h].astype(np.int16)-reference).mean()),
                                 'chroma_mae':float(np.abs(pixels[i,h:].astype(np.int16)-128).mean())}for i,frame in enumerate((0,119))]
                        if any(e['luma_mae']>=2 or e['chroma_mae']>=1 for e in errors):
                            raise RuntimeError('Decoded known-source quality check failed')
                        decoded.append({'name':fixture.name,'metadata':stream,'frames':len(hashes),'source_errors':errors,
                                        'stream_sha256':hashlib.sha256(fixture.read_bytes()).hexdigest(),
                                        'decoded_hashes_sha256':hashlib.sha256('\n'.join(hashes).encode()).hexdigest()})
                        run(['ffmpeg','-v','verbose','-i',fixture,'-map','0:v:0','-c','copy',
                             '-bsf:v','trace_headers','-frames:v','1','-f','null','-'],fixture.stem+'-headers')
                        trace=(output/(tag+'-'+fixture.stem+'-headers.stderr')).read_text()
                        fields=('log2_min_luma_coding_block_size_minus3','log2_diff_max_min_luma_coding_block_size',
                                'log2_min_luma_transform_block_size_minus2','log2_diff_max_min_luma_transform_block_size',
                                'max_transform_hierarchy_depth_inter','max_transform_hierarchy_depth_intra',
                                'tiles_enabled_flag','num_tile_rows_minus1','num_tile_columns_minus1','five_minus_max_num_merge_cand')
                        import re
                        decoded[-1]['coded_syntax']={field:[int(value) for value in re.findall(r'\b'+field+r'\s+[^\n]*?=\s*(-?\d+)',trace)] for field in fields}
                    if len(decoded)!=4:
                        raise RuntimeError('Missing Vulkan Video fixtures')
                    receipt['decoded']=decoded
                    receipt['quality_boundary']='First/last known gray source checked; all480 frames decoded; no game/color/LG quality proof'
                receipt['result'] = 'completed'
            finally:
                for p in owned:
                    if p.poll() is None:
                        stop(p)
    except BaseException as error:
        receipt['result'] = 'failed'
        receipt['error'] = str(error)
    receipt['elapsed_seconds'] = time.monotonic()-started
    receipt['temporary_directory_removed'] = not Path(receipt.get('temporary_directory', '/does-not-exist')).exists()
    receipt['owned_processes_reaped'] = all(p.poll() is not None for p in owned)
    path = output/(tag+'.json')
    path.write_text(json.dumps(receipt, indent=2)+'\n')
    print(json.dumps({k: receipt[k] for k in ('result', 'elapsed_seconds')}))
    if 'error' in receipt:
        print(receipt['error'])
    for row in receipt.get('samples', []):
        print(json.dumps(row))
    print(path)
    return 0 if receipt['result'] == 'completed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
