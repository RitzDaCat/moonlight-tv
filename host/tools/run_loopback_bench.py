#!/usr/bin/env python3
"""Run an isolated localhost Sunshine capture/encode stream without the TV."""
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
import socket
import ssl
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request
import uuid
import xml.etree.ElementTree as ET


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host-binary', type=Path, default=root/'host/runtime/sunshine')
    parser.add_argument('--codec', choices=('hevc', 'av1'), default='hevc')
    parser.add_argument('--twopass', choices=('quarter_res', 'disabled'), default='quarter_res')
    parser.add_argument('--duration', type=int, choices=range(2, 121), default=4)
    parser.add_argument('--resolution', choices=('1280x720', '1920x1080', '2560x1440', '3840x2160'), default='1920x1080')
    parser.add_argument('--capture', choices=('kms', 'kwin'), default='kms')
    parser.add_argument('--split-policy', choices=('auto', 'forced'), default='auto')
    parser.add_argument('--profile', action='store_true')
    parser.add_argument('--producer-log', type=Path, help='Owned private compositor barcode producer log')
    parser.add_argument('--source-references',type=Path,help='Instrumented owned GPU reference PNGs; color-error validation only')
    parser.add_argument('--tag-suffix', default='')
    parser.add_argument('--cache-imports',action='store_true')
    parser.add_argument('--early-stream',action='store_true')
    parser.add_argument('--lease-buffers',action='store_true')
    parser.add_argument('--gfni-fec',action='store_true')
    parser.add_argument('--validate-fec',action='store_true',help='Native client debug recovery with one synthetic missing shard per FEC block; instrumented correctness only')
    parser.add_argument('--graphics-draw-ms',type=float,default=4.)
    parser.add_argument('--graphics-period-ms',type=float,default=0.)
    parser.add_argument('--trace-nvenc',action='store_true',help='Instrument the private host NVENC API table')
    parser.add_argument('--nvenc-headers',type=Path,help='Directory containing ffnvcodec/nvEncodeAPI.h for the private tracing build')
    interference = parser.add_mutually_exclusive_group()
    interference.add_argument('--contention', action='store_true')
    interference.add_argument('--graphics-contention', action='store_true')
    args = parser.parse_args()
    if not(.05<=args.graphics_draw_ms<=10) or not(args.graphics_period_ms==0 or args.graphics_draw_ms<=args.graphics_period_ms<=100):
        parser.error('Graphics draw must be .05..10 ms; period must be zero or draw..100 ms')
    if not args.graphics_contention and (args.graphics_draw_ms!=4 or args.graphics_period_ms!=0):
        parser.error('Graphics timing controls require --graphics-contention')
    if args.source_references and not args.producer_log:parser.error('Source references require the owned producer log')
    if args.tag_suffix and not re.fullmatch('[a-z0-9-]+',args.tag_suffix):
        parser.error('Tag suffix must contain only lowercase letters, digits and hyphens')
    binary = args.host_binary.resolve(strict=True)
    if args.trace_nvenc and (args.nvenc_headers is None or not (args.nvenc_headers/'ffnvcodec/nvEncodeAPI.h').is_file()):
        parser.error('--trace-nvenc requires --nvenc-headers containing ffnvcodec/nvEncodeAPI.h')
    common = root/'core/moonlight-common-c'
    output = root/'code-review/engine-bench'
    output.mkdir(parents=True, exist_ok=True)
    tag = f'loopback-{args.codec}-{args.twopass}-'+('graphics' if args.graphics_contention else 'busy' if args.contention else 'idle')
    if args.resolution!='1920x1080':
        tag += '-'+args.resolution
    if args.capture!='kms':
        tag += '-'+args.capture
    if binary.name!='sunshine':
        tag += '-'+binary.name
    if args.split_policy!='auto':
        tag += '-split-'+args.split_policy
    if args.profile:
        tag += '-profile'
    if args.tag_suffix:
        tag += '-'+args.tag_suffix
    if args.cache_imports:
        tag += '-cache'
    if args.lease_buffers:tag += '-leases'
    if args.gfni_fec:tag += '-gfni-fec'
    if args.validate_fec:tag += '-fec-validated'
    if args.source_references:tag += '-quality'
    if args.graphics_contention and (args.graphics_draw_ms!=4 or args.graphics_period_ms!=0):
        tag += '-draw'+format(args.graphics_draw_ms,'g').replace('.','p')+'-period'+format(args.graphics_period_ms,'g').replace('.','p')
    if args.early_stream:tag += '-early-stream'
    if args.trace_nvenc:tag += '-trace'
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    start = time.monotonic()
    deadline = start+max(120,args.duration*3+60)
    env = dict(os.environ, CUDA_CACHE_DISABLE='1', __GL_SHADER_DISK_CACHE='0')
    if args.profile:
        env['SUNSHINE_PROFILE_VIDEO']='1'
    env['SUNSHINE_CACHE_DMABUF_IMPORTS']='1'if args.cache_imports else '0'
    env['SUNSHINE_EARLY_CUDA_STREAM']='1'if args.early_stream else '0'
    env['SUNSHINE_LEASE_PIPEWIRE_BUFFERS']='1'if args.lease_buffers else '0'
    env['SUNSHINE_GFNI_FEC']='1'if args.gfni_fec else '0'
    receipt = {'host_binary': str(binary), 'host_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
               'codec': args.codec, 'twopass': args.twopass, 'contention': args.contention or args.graphics_contention,
               'stream_resolution': args.resolution, 'source_capture': args.capture,
               'profile': args.profile, 'split_policy': args.split_policy,'cache_imports':args.cache_imports,
               'early_stream':args.early_stream,'lease_buffers':args.lease_buffers,'gfni_fec':args.gfni_fec,'quality_instrumented':bool(args.source_references),'fec_validation_instrumented':args.validate_fec,
               'graphics_draw_ms':args.graphics_draw_ms,'graphics_period_ms':args.graphics_period_ms,'nvenc_instrumented':args.trace_nvenc,
               'interference_kind': 'OpenGL fragment shader' if args.graphics_contention else 'CUDA kernel' if args.contention else 'none',
               'commands': [], 'boundary': 'Actual localhost capture/encode/packet host span; no TV, game input or presentation',
               'isolation': 'Own loopback ports, keys, trusted client state and absolute config paths; no live server API calls'}
    owned = []

    def stop(process):
        if process is None:
            return
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

    def run(argv, label, cwd=None):
        argv = [str(a) for a in argv]
        process = subprocess.Popen(argv, cwd=cwd, env=env, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, start_new_session=True)
        owned.append(process)
        cleanup.callback(stop, process)
        try:
            stdout, stderr = process.communicate(timeout=max(.1, deadline-time.monotonic()))
        except BaseException:
            stop(process)
            raise
        (output/(tag+'-'+label+'.stdout')).write_bytes(stdout)
        (output/(tag+'-'+label+'.stderr')).write_bytes(stderr)
        receipt['commands'].append({'label': label, 'argv': argv, 'exit': process.returncode})
        if process.returncode:
            raise RuntimeError(label+': '+(stderr or stdout).decode(errors='replace')[-1500:])
        return stdout.decode()

    try:
        with tempfile.TemporaryDirectory(prefix='sunshine-loopback-', dir='/tmp') as folder, contextlib.ExitStack() as cleanup:
            tmp = Path(folder)
            server_env=env
            if args.trace_nvenc:
                trace=tmp/'nvenc-trace';trace.mkdir()
                run(['clang++','-O2','-std=c++17','-shared','-fPIC','-Wl,-z,nodelete',
                     '-I'+str(args.nvenc_headers),root/'host/tools/nvenc_api_trace.cpp','-ldl',
                     '-o',trace/'libnvidia-encode.so.1'],'trace-build')
                server_env=dict(env,LD_LIBRARY_PATH=str(trace)+(':'+env['LD_LIBRARY_PATH']if env.get('LD_LIBRARY_PATH')else ''),
                    NVENC_REAL_LIBRARY='/usr/lib/libnvidia-encode.so.1')
                receipt['nvenc_trace_source_sha256']=hashlib.sha256((root/'host/tools/nvenc_api_trace.cpp').read_bytes()).hexdigest()
            # Reserve a disjoint free block; the existing server is never contacted.
            base = None
            for candidate in range(52000, 59000, 50):
                sockets = []
                try:
                    for delta in (-5, 0, 1, 9, 10, 11, 13, 21):
                        for kind in (socket.SOCK_STREAM, socket.SOCK_DGRAM):
                            s = socket.socket(socket.AF_INET, kind); sockets.append(s)
                            s.bind(('127.0.0.1', candidate+delta))
                    base = candidate
                    break
                except OSError:
                    pass
                finally:
                    for s in sockets:
                        s.close()
            if base is None:
                raise RuntimeError('No free isolated port block')
            receipt['base_port'] = base
            cmake = (f'cmake_minimum_required(VERSION 3.20)\nproject(LoopbackProbe LANGUAGES C CXX)\n'
                     f'set(BUILD_SHARED_LIBS OFF)\nadd_subdirectory("{common}" common)\n'
                     'target_compile_options(moonlight-common-c PRIVATE -Wno-unused-but-set-global)\n'
                     f'add_executable(receiver "{root}/host/tools/stream_latency_client.c")\n'
                     f'target_link_libraries(receiver PRIVATE moonlight-common-c)\n')
            if args.validate_fec:
                cmake += (f'set_source_files_properties("{common}/src/RtpVideoQueue.c" TARGET_DIRECTORY moonlight-common-c PROPERTIES COMPILE_DEFINITIONS LC_DEBUG)\n'
                          'target_compile_definitions(receiver PRIVATE OWNED_FEC_VALIDATION)\n'
                          'target_link_options(receiver PRIVATE \"-Wl,--wrap=reed_solomon_decode\")\n')
            (tmp/'CMakeLists.txt').write_text(cmake)
            run(['cmake', '-S', tmp, '-B', tmp/'build', '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
                 '-DCMAKE_C_COMPILER=clang', '-DCMAKE_CXX_COMPILER=clang++'], 'configure')
            run(['cmake', '--build', tmp/'build', '--target', 'receiver', '-j', '6'], 'build')
            receiver = tmp/'build/receiver'
            launch_query = run([receiver, '--launch-query'], 'launch-query').strip().lstrip('&')
            for who in ('client', 'server'):
                run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
                     '-subj', '/CN=localhost', '-addext', 'subjectAltName=IP:127.0.0.1',
                     '-keyout', tmp/(who+'.key'), '-out', tmp/(who+'.crt')], who+'-certificate')
            (tmp/'state.json').write_text(json.dumps({'root': {'uniqueid': str(uuid.uuid4()), 'named_devices': [
                {'name': 'Owned loopback probe', 'uuid': str(uuid.uuid4()), 'enabled': True,
                 'cert': (tmp/'client.crt').read_text()}]}}))
            (tmp/'apps.json').write_text(json.dumps({'env': {}, 'apps': [{'name': 'Owned loopback desktop'}]}))
            paths = {'pkey': 'server.key', 'cert': 'server.crt', 'file_state': 'state.json',
                     'credentials_file': 'credentials.json', 'file_apps': 'apps.json', 'log_path': 'host.log'}
            config = '\n'.join(f'{key} = {tmp/value}' for key, value in paths.items())+'\n'
            config += f'port = {base}\nbind_address = 127.0.0.1\naddress_family = ipv4\nsunshine_name = Owned loopback probe\nsystem_tray = disabled\nupnp = disabled\norigin_web_ui_allowed = pc\n'
            split_value='enabled' if args.split_policy=='forced' else 'driver_decides'
            config += f'capture = {args.capture}\nencoder = nvenc\nnvenc_preset = 1\nnvenc_twopass = {args.twopass}\nnvenc_split_encode = {split_value}\nstream_audio = disabled\n'
            config += 'keyboard = disabled\nmouse = disabled\ncontroller = disabled\n'
            (tmp/'sunshine.conf').write_text(config)
            with (output/(tag+'-server.stdout')).open('wb') as stdout, (output/(tag+'-server.stderr')).open('wb') as stderr:
                server = subprocess.Popen([str(binary), str(tmp/'sunshine.conf')], cwd=tmp, env=server_env,
                                          stdout=stdout, stderr=stderr, start_new_session=True)
                owned.append(server)
                cleanup.callback(stop, server)
                context = ssl.create_default_context(cafile=str(tmp/'server.crt'))
                context.load_cert_chain(str(tmp/'client.crt'), str(tmp/'client.key'))
                def request(action, params=None):
                    query = {'uniqueid': 'owned-loopback-client', **(params or {})}
                    url = f'https://127.0.0.1:{base-5}/{action}?'+urllib.parse.urlencode(query)
                    with urllib.request.urlopen(url, context=context, timeout=20 if action=='launch' else 3) as response:
                        tree = ET.fromstring(response.read())
                    if tree.get('status_code', '200') != '200':
                        raise RuntimeError(action+': '+tree.get('status_message', 'request rejected'))
                    return tree
                info = None
                ready_deadline = min(deadline, time.monotonic()+25)
                while time.monotonic()<ready_deadline and server.poll() is None:
                    try:
                        info = request('serverinfo'); break
                    except (OSError, urllib.error.URLError):
                        time.sleep(.1)
                if info is None:
                    raise RuntimeError('Isolated host did not become ready')
                apps = request('applist'); appid = apps.findtext('.//App/ID')
                if not appid:
                    raise RuntimeError('No owned test app advertised')
                load = None
                if args.contention or args.graphics_contention:
                    receipt['load_source_sha256'] = hashlib.sha256((root/'host/tools'/('gpu_graphics_load.cpp' if args.graphics_contention else 'gpu_conversion.cu')).read_bytes()).hexdigest()
                    if args.graphics_contention:
                        run(['clang++', '-O3', '-std=c++17', root/'host/tools/gpu_graphics_load.cpp',
                             '-lEGL', '-lGL', '-o', tmp/'load'], 'load-build')
                        load_args = [str(tmp/'load'), str(max(20,args.duration+10)), str(args.graphics_draw_ms), str(args.graphics_period_ms)]
                    else:
                        run(['/opt/cuda/bin/nvcc', '-O3', '-std=c++17', '-ccbin', '/usr/bin/g++-14', '-arch=sm_89',
                             root/'host/tools/gpu_conversion.cu', '-o', tmp/'load'], 'load-build')
                        load_args = [str(tmp/'load'), '--contention-seconds', str(max(20,args.duration+10))]
                    load = subprocess.Popen(load_args, cwd=tmp, env=env,
                                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, start_new_session=True)
                    owned.append(load)
                    cleanup.callback(stop, load)
                    ready = load.stdout.readline() if select.select([load.stdout], [], [], 5)[0] else ''
                    receipt['load_ready'] = ready.strip()
                    if 'ready' not in ready:
                        raise RuntimeError('Synthetic load not ready')
                key = os.urandom(16); iv = os.urandom(4)+bytes(12)
                params = dict(urllib.parse.parse_qsl(launch_query))
                params.update(appid=appid, mode=args.resolution+'x120', additionalStates='1', sops='0',
                              rikey=key.hex(), rikeyid=str(int.from_bytes(iv[:4], 'big')), surroundAudioInfo='196610',
                              localAudioPlayMode='1', remoteControllersBitmap='0', gcmap='0')
                launched = request('launch', params)
                session = launched.findtext('sessionUrl0')
                if not session or launched.findtext('gamesession') != '1':
                    raise RuntimeError('Owned stream launch failed')
                fields = [info.findtext('appversion', ''), info.findtext('GfeVersion', ''), session,
                          info.findtext('ServerCodecModeSupport', '0'), key.hex(), iv.hex(), args.codec, str(args.duration), args.resolution]
                descriptor = tmp/'session'; descriptor.write_text('\n'.join(fields)+'\n'); descriptor.chmod(0o600)
                sampler = subprocess.Popen([sys.executable, str(root/'host/tools/nvml_encoder_sample.py'), str(server.pid)],
                                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, start_new_session=True)
                owned.append(sampler); cleanup.callback(stop, sampler)
                try:
                    command=[receiver,descriptor,tmp/'stream.hevc' if args.codec=='hevc' else tmp/'stream.obu']
                    if args.producer_log:
                        command += [tmp/'frame-times.csv']
                    data = run(command, 'stream')
                finally:
                    sample_stdout, sample_stderr = stop(sampler)
                    (output/(tag+'-nvml.stdout')).write_text(sample_stdout)
                    (output/(tag+'-nvml.stderr')).write_text(sample_stderr)
                    receipt['nvml_exit'] = sampler.returncode
                    receipt['nvml_samples'] = [json.loads(row) for row in sample_stdout.splitlines() if row.startswith('{')]
                receipt['stats'] = [json.loads(line) for line in data.splitlines() if line.startswith('{')][-1]
                if load:
                    receipt['load_alive_at_end'] = load.poll() is None
                    remaining_stdout, load_stderr = stop(load)
                    (output/(tag+'-load.stdout')).write_text(ready+remaining_stdout)
                    (output/(tag+'-load.stderr')).write_text(load_stderr)
                    if not receipt['load_alive_at_end']:
                        raise RuntimeError('Load ended before measurement')
                request('cancel')
                stop(server)
            if (tmp/'host.log').exists():
                (output/(tag+'-host.log')).write_bytes((tmp/'host.log').read_bytes())
                receipt['conversion_setup_failures']=sum((tmp/'host.log').read_text().count(text)for text in ("Couldn't register Y plane texture","Couldn't register UV plane texture","Couldn't activate encoder CUDA context"))
                if receipt['conversion_setup_failures']:raise RuntimeError('Conversion setup failed or retried; exclude this comparison')
                host_text=(tmp/'host.log').read_text()
                receipt['feature_proof']={'early_stream':'CUDA conversion uses the encoder pre-open stream' in host_text,
                                          'buffer_leases':'Explicit CUDA source-buffer read ownership enabled' in host_text,
                                          'gfni_fec':bool(re.search(r'Register-blocked video FEC enabled; GFNI available=(?:true|1)\b',host_text))}
                if args.early_stream and not receipt['feature_proof']['early_stream']:raise RuntimeError('Requested early stream is absent from host logs')
                if args.lease_buffers and not receipt['feature_proof']['buffer_leases']:raise RuntimeError('Requested buffer leases are absent from host logs')
                if args.gfni_fec and not receipt['feature_proof']['gfni_fec']:raise RuntimeError('Requested native GFNI FEC is absent from host logs')
                if args.validate_fec and (not receipt['stats'].get('fec_recoveries')or receipt['stats'].get('fec_recovery_errors')):raise RuntimeError('Native client FEC validation did not complete successful recoveries')
            stream = tmp/('stream.hevc' if args.codec=='hevc' else 'stream.obu')
            receipt['stream_sha256']=hashlib.sha256(stream.read_bytes()).hexdigest()
            decoded = json.loads(run(['ffprobe', '-v', 'error', '-count_frames', '-show_entries',
                                      'stream=width,height,nb_read_frames', '-of', 'json', stream], 'decode'))
            receipt['decoded'] = decoded
            width, height = map(int, args.resolution.split('x'))
            if (decoded['streams'][0]['width'], decoded['streams'][0]['height']) != (width, height):
                raise RuntimeError('Decoded geometry differs from the requested stream')
            if int(decoded['streams'][0].get('nb_read_frames', 0)) != receipt['stats']['frames']:
                raise RuntimeError('Not every received frame decoded')
            if args.producer_log:
                import csv
                import numpy as np
                raw=tmp/'barcode.gray'
                producer_snapshot=args.producer_log.read_bytes()
                producer_info=json.loads(producer_snapshot.splitlines()[0])
                regions=producer_info.get('witness_regions',1)
                source_width,source_height=producer_info['width'],producer_info['height']
                scale_x,scale_y=width/source_width,height/source_height
                crop_width=int(np.ceil(512*scale_x))
                crop_height=max(1,int(16*scale_y)-2)
                source_origins=[(x,y)for y in(0,source_height//2-16,source_height-16)
                                for x in(0,source_width-512)]if regions==6 else [(0,source_height-16)]
                if regions in(1,6):
                    origins=[(int(np.floor(x*scale_x)),int(np.ceil(y*scale_y))+1)for x,y in source_origins]
                    if any(x+crop_width>width or y+crop_height>height for x,y in origins):raise RuntimeError('Scaled witness crop is outside the image')
                    crops=[f'[s{i}]crop={crop_width}:{crop_height}:{x}:{y}:exact=1[c{i}]'for i,(x,y)in enumerate(origins)]
                    filters=f'split={regions}'+''.join(f'[s{i}]'for i in range(regions))+';'+ ';'.join(crops)+';'+''.join(f'[c{i}]'for i in range(regions))+f'vstack=inputs={regions}'
                    if regions==1:
                        x,y=origins[0];filters=f'crop={crop_width}:{crop_height}:{x}:{y}:exact=1'
                else:raise RuntimeError('Unsupported owned witness region count')
                run(['ffmpeg','-v','error','-xerror','-threads','4','-i',stream,'-vf',filters,
                     '-fps_mode','passthrough','-pix_fmt','gray','-f','rawvideo',raw], 'barcode')
                pixels=np.frombuffer(raw.read_bytes(),dtype=np.uint8).reshape(-1,regions,crop_height,crop_width)
                region_ids=np.zeros(pixels.shape[:2],dtype=np.uint64)
                for region,((source_x,_),(crop_x,_))in enumerate(zip(source_origins,origins)):
                    fraction=source_x*scale_x-crop_x
                    for bit in range(32):
                        lo=int(np.ceil(fraction+bit*16*scale_x+.75))
                        hi=int(np.floor(fraction+(bit+1)*16*scale_x-.75))
                        if hi<=lo:raise RuntimeError('Scaled witness bit has no interior samples')
                        present=pixels[:,region,:,lo:hi].mean(axis=(1,2))>127
                        region_ids[:,region]|=present.astype(np.uint64)<<np.uint64(bit)
                if not np.all(region_ids==region_ids[:,0,None]):raise RuntimeError('Owned frame-ID witnesses disagree across image regions')
                ids=region_ids[:,0]
                if np.any(ids[1:]<ids[:-1]):raise RuntimeError('Owned source frame IDs went backwards')
                rows=list(csv.DictReader((tmp/'frame-times.csv').open()))
                if len(ids)!=len(rows):raise RuntimeError('Barcode and receive-frame count mismatch')
                source={int(r['frame']):int(r['begin_ns']) for line in producer_snapshot.decode().splitlines()
                        if line.startswith('{') and '"begin_ns"' in line for r in [json.loads(line)]}
                ages=[];missing_ids=0
                for identity,row in zip(ids,rows):
                    if int(identity) not in source:missing_ids+=1;continue
                    age=(int(row['arrival_ns'])-source[int(identity)])/1000
                    if age<0:raise RuntimeError('Decoded source ID is from the future')
                    ages.append(age)
                if not ages or missing_ids:raise RuntimeError('Decoded barcode does not match owned source log')
                values=np.asarray(ages)
                receipt['source_witness']={'frames':len(ids),'unique_source_frames':len(set(map(int,ids))),
                    'regions_per_frame':regions,'all_regions_agree':True,
                    'source_dimensions':[source_width,source_height],'crop_dimensions':[crop_width,crop_height],
                    'duplicate_source_frames':len(ids)-len(set(map(int,ids))), 'first_source_id':int(ids[0]),'last_source_id':int(ids[-1]),
                    'skipped_source_frames':int(ids[-1]-ids[0])+1-len(set(map(int,ids))),
                    'source_begin_to_receiver_ready_us':{key:float(np.percentile(values,p))for key,p in (('p50',50),('p95',95),('p99',99))},
                    'boundary':'Same-machine monotonic clock: owned source CPU draw start to complete received compressed frame, includes rendering/compositor/capture/encode/assembly; excludes TV decode/display/input'}
                (output/(tag+'-frame-times.csv')).write_bytes((tmp/'frame-times.csv').read_bytes())
                with (output/(tag+'-source-witness.csv')).open('w',newline='')as joined:
                    writer=csv.writer(joined)
                    writer.writerow(('frame','source_frame','arrival_ns','draw_begin_ns','source_begin_to_receiver_ready_us','regions_per_frame'))
                    for identity,row,age in zip(ids,rows,ages):
                        writer.writerow((row['frame'],int(identity),row['arrival_ns'],source[int(identity)],age,regions))
                receipt['source_witness']['producer_log_sha256']=hashlib.sha256(producer_snapshot).hexdigest()
                if args.source_references:
                    from PIL import Image
                    import math
                    import shutil
                    if (source_width,source_height)!=(width,height):raise RuntimeError('Quality reference geometry differs')
                    references={int(p.stem):p for p in args.source_references.glob('*.png')if p.stem.isdecimal()}
                    chosen=[];seen=set()
                    for index,identity in enumerate(map(int,ids)):
                        if index>=20 and identity in references and identity not in seen:
                            chosen.append((index,identity,references[identity]));seen.add(identity)
                        if len(chosen)==8:break
                    if len(chosen)<2:raise RuntimeError('Too few exact source-frame reference matches')
                    select_filter='select='+repr('+'.join(f'eq(n,{index})'for index,_,_ in chosen))
                    run(['ffmpeg','-v','error','-xerror','-threads','4','-i',stream,'-vf',select_filter,
                         '-fps_mode','passthrough','-pix_fmt','rgb24',tmp/'quality-%03d.png'],'quality-decode')
                    samples=[]
                    def errors(delta):
                        squared=delta.astype(np.float32)**2
                        mse=float(squared.mean(dtype=np.float64))
                        return dict(mae=float(np.abs(delta).mean(dtype=np.float64)),
                                    psnr_db=10*math.log10(255.*255./mse)if mse else None,
                                    maximum_error=int(np.abs(delta).max()))
                    artifacts=output/'quality'/tag;artifacts.mkdir(parents=True,exist_ok=True)
                    for order,(index,identity,reference)in enumerate(chosen,1):
                        candidate=tmp/f'quality-{order:03d}.png'
                        with Image.open(reference)as a,Image.open(candidate)as b:
                            expected=np.asarray(a.convert('RGB'),dtype=np.int16);actual=np.asarray(b.convert('RGB'),dtype=np.int16)
                        if actual.shape!=expected.shape:raise RuntimeError('Decoded quality frame has different dimensions')
                        delta=actual-expected
                        samples.append(dict(source_frame=identity,received_index=index,full_image=errors(delta),
                                            center_strip=errors(delta[height//2-32:height//2+32]),
                                            reference_sha256=hashlib.sha256(reference.read_bytes()).hexdigest(),
                                            decoded_sha256=hashlib.sha256(candidate.read_bytes()).hexdigest()))
                        if order==1:
                            shutil.copyfile(reference,artifacts/'reference.png');shutil.copyfile(candidate,artifacts/'decoded.png')
                    receipt['quality']={'samples':samples,'source':'Exact GLES backbuffer readback before source swap, top-down PNG',
                                        'boundary':'Instrumented correctness/quality run, never a clean latency comparison; no TV decoder or game content',
                                        'artifacts':str(artifacts),'mean_psnr_db':float(np.mean([s['full_image']['psnr_db']for s in samples])),
                                        'mean_mae':float(np.mean([s['full_image']['mae']for s in samples]))}

            receipt['result'] = 'completed'
    except BaseException as error:
        receipt.update(result='failed', error=str(error))
    finally:
        for process in reversed(owned):
            stop(process)
        receipt['seconds'] = time.monotonic()-start
        receipt['cleanup'] = 'Owned processes reaped; TemporaryDirectory removed'
        receipt['receiver_sha256'] = hashlib.sha256((root/'host/tools/stream_latency_client.c').read_bytes()).hexdigest()
        receipt['runner_sha256'] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
        (output/(tag+'.json')).write_text(json.dumps(receipt, indent=2)+'\n')
    print(json.dumps({k: receipt[k] for k in ('result', 'error', 'stats', 'seconds') if k in receipt}))
    return 0 if receipt['result']=='completed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
