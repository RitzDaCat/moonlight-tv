#!/usr/bin/env python3
"""Bounded, removable two-decoder test using Moonlight's installed SMP module.

Uses local SDK/FFmpeg/CLI only. Temporarily foregrounds a separate synthetic-video
app, then removes it and relaunches Moonlight. Does not change game/host settings.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
CLIENT = ROOT
APP_ID = 'com.limelight.webos.splitprobe'
REMOTE = '/media/developer/apps/usr/palm/applications/' + APP_ID


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', default='tv')
    parser.add_argument('--deadline-monotonic', type=float, help='Reuse an existing test deadline')
    parser.add_argument('--fps', type=int, choices=(30, 60), default=30)
    args = parser.parse_args()
    started = time.monotonic()
    deadline = started + 240  # Reserve the final minute for three bounded cleanup commands.
    if args.deadline_monotonic is not None:
        deadline = min(deadline, args.deadline_monotonic - 60)
    receipt = {'source_sha256': {}, 'commands': [], 'requested_fps': args.fps, 'boundary':
               'Synthetic video-only decoder overlap; not two-PC gameplay, audio, or input latency.'}
    output = ROOT / 'code-review/tv-test/two-decoder-probe.json'
    output.parent.mkdir(parents=True, exist_ok=True)
    for name in ('tv_split_probe.c', 'run_tv_split_probe.py'):
        receipt['source_sha256'][name] = hashlib.sha256((ROOT/'host/tools'/name).read_bytes()).hexdigest()

    def save():
        receipt['elapsed_seconds'] = time.monotonic() - started
        output.write_text(json.dumps(receipt, indent=2) + '\n')

    def run(label, command, timeout=30, cleanup=False):
        remaining = deadline-time.monotonic()
        if remaining <= 0 and not cleanup:
            raise TimeoutError('Interactive testing deadline reached')
        limit = min(timeout, 20) if cleanup else min(timeout, max(1, remaining))
        result = subprocess.run([str(x) for x in command], text=True, capture_output=True, timeout=limit)
        receipt['commands'].append({'label': label, 'exit': result.returncode,
                                    'stdout': result.stdout, 'stderr': result.stderr})
        save()
        if result.returncode:
            raise RuntimeError(label + ': ' + result.stderr.strip() + result.stdout.strip())
        return result.stdout

    cli = ROOT/'node_modules/.bin'
    def ares(label, tool, *options, **kw):
        return run(label, [cli/tool, '-d', args.device, *options], **kw)

    installed = launched = False
    try:
        apps = ares('installed-apps', 'ares-install', '-l')
        if APP_ID in apps:
            raise RuntimeError('Probe app already exists; refusing to overwrite an unrelated install')
        before = ares('moonlight-before', 'ares-novacom', '--run',
                      'pgrep -f ' + shlex.quote('^/media/developer/apps/usr/palm/applications/com.limelight.webos/bin/moonlight'))
        receipt['moonlight_was_running'] = bool(re.search(r'^\d+$', before, re.M))
        with tempfile.TemporaryDirectory(prefix='lg-tv-split-') as scratch:
            work = Path(scratch)
            stage = work/'app'; stage.mkdir()
            sdk = CLIENT/'build/arm-webos-linux-gnueabi_sdk-buildroot'
            sysroot = sdk/'arm-webos-linux-gnueabi/sysroot'
            run('cross-build', [sdk/'bin/arm-webos-linux-gnueabi-gcc', '-std=gnu11', '-D_GNU_SOURCE',
                '-O2', '-Wall', '-Wextra', '-Werror', ROOT/'host/tools/tv_split_probe.c',
                '-I'+str(CLIENT/'third_party/ss4s/include'),
                '-I'+str(CLIENT/'third_party/ss4s/modules/interface/include'),
                '-I'+str(sysroot/'usr/include/SDL2'), '-lSDL2', '-ldl',
                '-Wl,--no-as-needed', '-lcurl',
                '-o', stage/'probe'])
            for index in range(2):
                raw = work/f'player{index+1}.h264'
                run('fixture-'+str(index), ['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i',
                    f'testsrc2=size=1920x1080:rate={args.fps}', '-frames:v', '60', '-vf', f'hue=h={index*90}',
                    '-c:v', 'libx264', '-threads', '4', '-preset', 'ultrafast', '-tune', 'zerolatency',
                    '-pix_fmt', 'yuv420p', '-x264-params', 'aud=1:keyint=60:scenecut=0',
                    '-an', '-f', 'h264', raw])
                packets = json.loads(run('parse-fixture-'+str(index), ['ffprobe', '-v', 'error',
                    '-show_packets', '-show_entries', 'packet=pos,size', '-of', 'json', raw]))['packets']
                if len(packets) != 60:
                    raise RuntimeError('Unexpected fixture frame count')
                data = raw.read_bytes()
                with (stage/f'player{index+1}.frames').open('wb') as frame_file:
                    frame_file.write(struct.pack('<5I', 0x50534c4d, 1920, 1080, args.fps, len(packets)))
                    for packet in packets:
                        pos, size = int(packet['pos']), int(packet['size'])
                        frame_file.write(struct.pack('<I', size)); frame_file.write(data[pos:pos+size])
            shutil.copyfile(CLIENT/'third_party/ss4s/samples/av-threaded/icon.png', stage/'icon.png')
            (stage/'appinfo.json').write_text(json.dumps({'id': APP_ID, 'version': '1.0.0',
                'vendor': 'RitzDaCat', 'type': 'native', 'main': 'probe', 'title': 'Two-player decoder test',
                'icon': 'icon.png', 'requiredACG': []}))
            run('package', [cli/'ares-package', stage, '-o', work])
            package = next(work.glob(APP_ID+'*.ipk'))
            ares('install', 'ares-install', package); installed = True
            ares('launch', 'ares-launch', APP_ID); launched = True
            # The test exits after14s. Poll with bounded waits; never leave it running.
            for _ in range(24):
                result = ares('read-log', 'ares-novacom', '--run',
                              'cat ' + shlex.quote(REMOTE+'/probe.log') + ' 2>/dev/null || true')
                if 'probe_exit=' in result:
                    break
                if time.monotonic()+1 > deadline:
                    raise TimeoutError('Probe deadline reached')
                time.sleep(1)
            receipt['probe_log'] = result
            receipt['concurrent_frame_callbacks'] = 'overlap_callbacks=yes' in result
            receipt['normal_probe_exit'] = 'probe_exit=0' in result
            if 'probe_exit=' not in result:
                receipt['probe_status'] = 'did not exit within24s; stop required'
            else:
                receipt['probe_status'] = 'overlap confirmed by callbacks' if receipt['concurrent_frame_callbacks'] else 'overlap unproven'
    except BaseException as error:
        receipt['error'] = str(error)
        raise
    finally:
        if launched:
            try: ares('close-probe', 'ares-launch', '--close', APP_ID, cleanup=True)
            except (RuntimeError, subprocess.SubprocessError): pass
        if installed:
            try: ares('remove-probe', 'ares-install', '-r', APP_ID, cleanup=True)
            except (RuntimeError, subprocess.SubprocessError) as error: receipt['cleanup_error'] = str(error)
        if launched and receipt.get('moonlight_was_running'):
            try: ares('restore-moonlight', 'ares-launch', 'com.limelight.webos', cleanup=True)
            except (RuntimeError, subprocess.SubprocessError) as error: receipt['restore_error'] = str(error)
        save()
    print(json.dumps({k: receipt.get(k) for k in ('probe_status', 'concurrent_frame_callbacks',
                     'normal_probe_exit', 'elapsed_seconds', 'cleanup_error', 'restore_error')}, indent=2))


if __name__ == '__main__':
    main()
