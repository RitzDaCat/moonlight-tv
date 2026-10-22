#!/usr/bin/env python3
"""Private virtual KWin/PipeWire + owned source + isolated Sunshine loopback."""
import argparse
import contextlib
import hashlib
import json
import os
from pathlib import Path
import resource
import re
import shlex
import signal
import subprocess
import sys
import tempfile
import time


def start_ticks(pid):
    try:
        return Path(f'/proc/{pid}/stat').read_text().rsplit(')',1)[1].split()[19]
    except FileNotFoundError:
        return None


def stop(process):
    if process.poll() is None:
        try:
            os.killpg(process.pid,signal.SIGTERM)
        except ProcessLookupError:
            pass
    try:
        return process.communicate(timeout=2)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid,signal.SIGKILL)
        return process.communicate()


def experiment_seconds(args):
    return max(75,(args.duration*3+25)*(3 if args.compare=='resolution'else 2 if args.compare else 1)+10)


def inside(args,root):
    work=Path(args.work)
    children=[];files=[];manifest={'children':[],'commands':[]}
    deadline=time.monotonic()+experiment_seconds(args)
    def launch(name,command,extra_env=None):
        out=(work/(name+'.stdout')).open('wb');err=(work/(name+'.stderr')).open('wb');files.extend((out,err))
        env=None
        if name in ('outputs-before','outputs-after','add-mode','set-mode'):
            env=dict(os.environ,QT_QPA_PLATFORM='wayland')
        if extra_env:env=dict(os.environ if env is None else env,**extra_env)
        p=subprocess.Popen([str(x)for x in command],env=env,stdout=out,stderr=err,start_new_session=True)
        children.append(p);manifest['children'].append({'name':name,'pid':p.pid,'start_ticks':start_ticks(p.pid)})
        (work/'owned.json').write_text(json.dumps(manifest['children']))
        return p
    def run(name,command,timeout=15,required=True,extra_env=None):
        p=launch(name,command,extra_env)
        try:p.wait(timeout=min(timeout,max(.1,deadline-time.monotonic())))
        except subprocess.TimeoutExpired:stop(p);raise RuntimeError(name+' timed out')
        manifest['commands'].append({'name':name,'exit':p.returncode,'argv':[str(x)for x in command]})
        if required and p.returncode:raise RuntimeError(name+' failed')
        return (work/(name+'.stdout')).read_text()
    try:
        run('desktop-cache',['kbuildsycoca6','--noincremental'])
        wire=launch('pipewire',['pipewire'])
        policy=launch('wireplumber',['wireplumber','--profile','policy'])
        kwin=launch('kwin',['kwin_wayland','--virtual','--width',args.resolution.split('x')[0],
            '--height',args.resolution.split('x')[1],'--output-count','1','--socket','streaming-owned',
            '--no-lockscreen','--no-global-shortcuts','--no-kactivities'])
        for _ in range(100):
            if (work/'run/streaming-owned').exists():break
            if kwin.poll() is not None:raise RuntimeError('Private KWin stopped during startup')
            time.sleep(.05)
        else:raise RuntimeError('Private Wayland socket timeout')
        run('wayland-info',['wayland-info'],required=False)
        # Every display command is inside this private bus and virtual backend.
        text=run('outputs-before',['kscreen-doctor','-j'],required=False)
        try:
            outputs=json.loads(text)['outputs']
            selected=outputs[0]
            manifest['virtual_output_before']=selected
            if args.source_fps!=60:
                w,h=args.resolution.split('x');identity=str(selected['id'])
                run('add-mode',['kscreen-doctor',f'output.{identity}.addCustomMode.{w}.{h}.{args.source_fps*1000}.reduced'],required=False)
                run('set-mode',['kscreen-doctor',f'output.{identity}.mode.{args.resolution}@{args.source_fps}'],required=False)
            manifest['virtual_outputs_after']=json.loads(run('outputs-after',['kscreen-doctor','-j'],required=False))
        except (KeyError,ValueError,IndexError) as e:
            manifest['output_metadata_error']=str(e)
        producer_command=[work/'producer',*args.resolution.split('x'),str(args.source_fps),str(experiment_seconds(args))]
        if args.quality:
            (work/'references').mkdir();producer_command+=[work/'references']
        producer=launch('producer',producer_command)
        for _ in range(160):
            text=(work/'producer.stdout').read_text()
            if '\n' in text and text.startswith('{'):
                ready=json.loads(text.splitlines()[0])
                if ready.get('ready'):manifest['producer']=ready;break
            if producer.poll() is not None:raise RuntimeError('Owned source failed to start')
            time.sleep(.05)
        else:raise RuntimeError('Owned source readiness timeout')
        stream_resolution=args.stream_resolution or args.resolution
        base_case=dict(cache_imports=args.cache_imports,window_capture=args.window_capture,
                       stream_resolution=stream_resolution,early_stream=args.early_stream,
                       lease_buffers=args.lease_buffers,codec=args.codec,legacy_policy=False,gfni_fec=args.gfni_fec)
        def variant(**changes):return dict(base_case,**changes)
        cases=[base_case]
        if args.compare=='cache':cases=[variant(cache_imports=False),variant(cache_imports=True)]
        if args.compare=='pipeline':cases=[variant(cache_imports=False,legacy_policy=True),variant(cache_imports=True)]
        if args.compare=='engine':cases=[variant(cache_imports=False,early_stream=False,lease_buffers=False,legacy_policy=True,gfni_fec=False),
                                         variant(cache_imports=True,early_stream=True,lease_buffers=True,gfni_fec=True)]
        if args.compare=='source':cases=[variant(window_capture=False),variant(window_capture=True)]
        if args.compare=='resolution':cases=[variant(stream_resolution=size)for size in dict.fromkeys((args.resolution,'1920x1080','1280x720'))]
        if args.compare=='stream':cases=[variant(early_stream=False),variant(early_stream=True)]
        if args.compare=='leases':cases=[variant(lease_buffers=False),variant(lease_buffers=True)]
        if args.compare=='fec':cases=[variant(gfni_fec=False),variant(gfni_fec=True)]
        if args.compare=='codec':cases=[variant(codec='hevc'),variant(codec='av1')]
        if args.reverse:cases.reverse()
        manifest['comparison_cases']=cases
        window_uuid=''
        if any(case['window_capture']for case in cases):
            # This script queries only the owned compositor on this private bus.
            # It neither changes window state nor executes inside the source app.
            script=work/'owned-window.js'
            script.write_text('function reportOwnedWindow() {\n'
                ' for (const w of workspace.windowList()) {\n'
                '  if (w.caption.indexOf("Owned streaming latency producer") === 0)\n'
                '   console.warn("OWNED_STREAM_WINDOW_UUID=" + w.internalId.toString());\n'
                ' }\n}\nworkspace.windowAdded.connect(reportOwnedWindow);\nreportOwnedWindow();\n')
            result=run('window-script-load',['gdbus','call','--session','--dest','org.kde.KWin',
                '--object-path','/Scripting','--method','org.kde.kwin.Scripting.loadScript',script,'owned-stream-query'])
            match=re.fullmatch(r'\((\d+),\)\s*',result)
            if not match:raise RuntimeError('Unexpected private script ID')
            run('window-script-run',['gdbus','call','--session','--dest','org.kde.KWin',
                '--object-path','/Scripting/Script'+match[1],'--method','org.kde.kwin.Script.run'])
            for _ in range(80):
                logs=(work/'kwin.stderr').read_text()+(work/'kwin.stdout').read_text()
                found=re.search(r'OWNED_STREAM_WINDOW_UUID=(\{?[0-9a-fA-F-]{36}\}?)',logs)
                if found:break
                time.sleep(.025)
            else:raise RuntimeError('Owned source window UUID was not reported')
            manifest['owned_window_uuid']=found[1]
            window_uuid=found[1]
        suffix=f'private-{args.source_fps}fps'+('-graphics'if args.graphics_contention else '')
        if args.tag_suffix:suffix+='-'+args.tag_suffix
        for index,case in enumerate(cases):
            cached=case['cache_imports'];window=case['window_capture'];stream_resolution=case['stream_resolution']
            early_stream=case['early_stream'];old_policy=case['legacy_policy']
            case_suffix=suffix+('-window'if window else '')
            if args.compare:case_suffix+=f'-compare-{args.compare}-'+('reverse-'if args.reverse else '')+str(index)
            command=[sys.executable,root/'host/tools/run_loopback_bench.py','--host-binary',args.host_binary,
                     '--codec',case['codec'],'--capture','kwin','--twopass','quarter_res'if old_policy else 'disabled','--resolution',stream_resolution,
                     '--split-policy','auto'if old_policy else 'forced','--profile','--duration',str(args.duration),
                     '--producer-log',work/'producer.stdout','--tag-suffix',case_suffix]
            if args.graphics_contention:command+=['--graphics-contention','--graphics-draw-ms',str(args.graphics_draw_ms),'--graphics-period-ms',str(args.graphics_period_ms)]
            if cached:command+=['--cache-imports']
            if early_stream:command+=['--early-stream']
            if case['lease_buffers']:command+=['--lease-buffers']
            if case['gfni_fec']:command+=['--gfni-fec']
            if args.validate_fec:command+=['--validate-fec']
            if args.quality:command+=['--source-references',work/'references']
            if args.trace_nvenc:command+=['--trace-nvenc','--nvenc-headers',args.nvenc_headers]
            run('loopback'+('-'+str(index)if args.compare else ''),command,timeout=args.duration*3+25,
                extra_env={'SUNSHINE_KWIN_WINDOW_UUID':window_uuid if window else ''})
        if producer.poll() is not None or kwin.poll() is not None or wire.poll() is not None or policy.poll() is not None:
            raise RuntimeError('Private source/compositor stopped before loopback completed')
        manifest['result']='completed'
    except BaseException as e:
        manifest.update(result='failed',error=str(e))
    finally:
        for p in reversed(children):stop(p)
        for f in files:f.close()
        manifest['owned_processes_reaped']=all(p.poll()is not None for p in children)
        (work/'inside.json').write_text(json.dumps(manifest,indent=2)+'\n')
    return 0 if manifest.get('result')=='completed'else 1


def main():
    root=Path(__file__).resolve().parents[2]
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--resolution',choices=('1920x1080','3840x2160'),default='1920x1080')
    parser.add_argument('--stream-resolution',choices=('1280x720','1920x1080','2560x1440','3840x2160'),help='Encode this geometry while preserving the owned source resolution')
    parser.add_argument('--source-fps',type=int,choices=(60,120),default=120)
    parser.add_argument('--duration',type=int,choices=range(2,121),default=5)
    parser.add_argument('--graphics-contention',action='store_true')
    parser.add_argument('--cache-imports',action='store_true')
    parser.add_argument('--early-stream',action='store_true')
    parser.add_argument('--lease-buffers',action='store_true')
    parser.add_argument('--gfni-fec',action='store_true')
    parser.add_argument('--validate-fec',action='store_true')
    parser.add_argument('--quality',action='store_true',help='Instrumented owned GPU readbacks and matched-frame color error; excludes clean latency timing')
    parser.add_argument('--codec',choices=('hevc','av1'),default='hevc')
    parser.add_argument('--graphics-draw-ms',type=float,default=4.)
    parser.add_argument('--graphics-period-ms',type=float,default=0.)
    parser.add_argument('--trace-nvenc',action='store_true')
    parser.add_argument('--nvenc-headers',type=Path)
    parser.add_argument('--window-capture',action='store_true',help='Experimental explicit capture of the owned fullscreen window')
    parser.add_argument('--compare',choices=('cache','pipeline','engine','source','resolution','stream','leases','codec','fec'),help='Compare cases in one persistent private compositor')
    parser.add_argument('--reverse',action='store_true',help='Reverse the selected comparison order')
    parser.add_argument('--tag-suffix',default='')
    parser.add_argument('--host-binary',type=Path,default=root/'host/runtime/sunshine-latency-candidate')
    parser.add_argument('--inside',action='store_true',help=argparse.SUPPRESS)
    parser.add_argument('--work',help=argparse.SUPPRESS)
    args=parser.parse_args()
    if not(.05<=args.graphics_draw_ms<=10) or not(args.graphics_period_ms==0 or args.graphics_draw_ms<=args.graphics_period_ms<=100):
        parser.error('Graphics draw must be .05..10 ms; period must be zero or draw..100 ms')
    if not args.graphics_contention and (args.graphics_draw_ms!=4 or args.graphics_period_ms!=0):
        parser.error('Graphics timing controls require --graphics-contention')
    if args.tag_suffix and not re.fullmatch('[a-z0-9-]+',args.tag_suffix):
        parser.error('Tag suffix must contain only lowercase letters, digits and hyphens')
    if args.trace_nvenc and (args.nvenc_headers is None or not (args.nvenc_headers/'ffnvcodec/nvEncodeAPI.h').is_file()):
        parser.error('--trace-nvenc requires --nvenc-headers containing ffnvcodec/nvEncodeAPI.h')
    if args.quality and ((args.stream_resolution and args.stream_resolution!=args.resolution) or args.compare=='resolution'):
        parser.error('Quality comparison currently requires equal source and stream geometry')
    if args.reverse and not args.compare:parser.error('--reverse requires --compare')
    if experiment_seconds(args)>1200:parser.error('Selected experiment exceeds its 1200-second source lifetime bound')
    args.host_binary=args.host_binary.resolve(strict=True)
    resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    if args.inside:return inside(args,root)
    output=root/'code-review/engine-bench';output.mkdir(parents=True,exist_ok=True)
    tag=f'private-compositor-{args.resolution}-{args.source_fps}fps'+('-graphics'if args.graphics_contention else '-idle')
    if args.host_binary.name!='sunshine-latency-candidate':tag+='-'+args.host_binary.name
    if args.cache_imports:tag+='-cache'
    if args.window_capture:tag+='-window'
    if args.early_stream:tag+='-early-stream'
    if args.lease_buffers:tag+='-leases'
    if args.gfni_fec:tag+='-gfni-fec'
    if args.validate_fec:tag+='-fec-validated'
    if args.quality:tag+='-quality'
    if args.codec!='hevc':tag+='-'+args.codec
    if args.graphics_contention and (args.graphics_draw_ms!=4 or args.graphics_period_ms!=0):
        tag+='-draw'+format(args.graphics_draw_ms,'g').replace('.','p')+'-period'+format(args.graphics_period_ms,'g').replace('.','p')
    if args.trace_nvenc:tag+='-trace'
    if args.tag_suffix:tag+='-'+args.tag_suffix
    if args.stream_resolution:tag+='-to-'+args.stream_resolution
    if args.compare:tag+='-compare-'+args.compare+('-reverse'if args.reverse else '')
    started=time.monotonic();receipt={'boundary':'Owned virtual compositor and source; no game/live desktop/service/TV changes',
        'source_files':{name:hashlib.sha256((root/'host/tools'/name).read_bytes()).hexdigest()for name in
            ('run_compositor_bench.py','owned_wayland_producer.cpp','run_loopback_bench.py','stream_latency_client.c')},
        'quality_instrumented':args.quality,
        'graphics_draw_ms':args.graphics_draw_ms,'graphics_period_ms':args.graphics_period_ms,
        'host_sha256':hashlib.sha256(args.host_binary.read_bytes()).hexdigest(),'commands':[]}
    owned=[]
    try:
        with tempfile.TemporaryDirectory(prefix='sunshine-private-compositor-',dir='/tmp')as directory,contextlib.ExitStack()as cleanup:
            work=Path(directory);receipt['temporary_directory']=directory
            (work/'run').mkdir(mode=0o700)
            env=dict(os.environ,XDG_RUNTIME_DIR=str(work/'run'),PIPEWIRE_RUNTIME_DIR=str(work/'run'),
                     PIPEWIRE_REMOTE='pipewire-0',XDG_CONFIG_HOME=str(work/'config'),XDG_CACHE_HOME=str(work/'cache'),
                     XDG_DATA_HOME=str(work/'data'),XDG_STATE_HOME=str(work/'state'),WAYLAND_DISPLAY='streaming-owned',DISPLAY='',QT_QPA_PLATFORM='offscreen',
                     KWIN_COMPOSE='O2',CUDA_CACHE_DISABLE='1',__GL_SHADER_DISK_CACHE='0',MESA_SHADER_CACHE_DISABLE='true',
                     QT_LOGGING_TO_CONSOLE='1',QT_FORCE_STDERR_LOGGING='1',
                     KDE_SKIP_KGLOBALACCEL_AUTOSTART='1')
            def run(name,command):
                p=subprocess.Popen([str(x)for x in command],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,start_new_session=True);owned.append(p);cleanup.callback(stop,p)
                try:out,err=p.communicate(timeout=30)
                except BaseException:stop(p);raise
                receipt['commands'].append({'name':name,'argv':[str(x)for x in command],'exit':p.returncode})
                (output/(tag+'-'+name+'.stdout')).write_bytes(out);(output/(tag+'-'+name+'.stderr')).write_bytes(err)
                if p.returncode:raise RuntimeError(name+' failed: '+err.decode(errors='replace')[-1200:])
                return out.decode()
            protocol=Path('/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml')
            receipt['protocol_sha256']=hashlib.sha256(protocol.read_bytes()).hexdigest()
            run('protocol-header',['wayland-scanner','client-header',protocol,work/'xdg-shell-client.h'])
            run('protocol-code',['wayland-scanner','private-code',protocol,work/'xdg-shell.c'])
            run('protocol-build',['clang','-O2','-I'+str(work),'-c',work/'xdg-shell.c','-o',work/'xdg-shell.o'])
            flags=shlex.split(run('dependencies',['pkg-config','--cflags','--libs','wayland-client','wayland-egl','egl','glesv2']))
            quality_flags=[]
            if args.quality:quality_flags=['-DOWNED_REFERENCE_CAPTURE',*shlex.split(run('quality-dependencies',['pkg-config','--cflags','--libs','libpng']))]
            run('producer-build',['clang++','-O3','-std=c++17','-I'+str(work),root/'host/tools/owned_wayland_producer.cpp',work/'xdg-shell.o',*flags,*quality_flags,'-o',work/'producer'])
            applications=work/'data/applications';applications.mkdir(parents=True)
            (applications/'dev.lizardbyte.app.Sunshine.kwin.desktop').write_text('[Desktop Entry]\nType=Application\nName=Owned private streaming capture\nNoDisplay=true\nExec='+str(args.host_binary)+'\nX-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1\n')
            command=['dbus-run-session','--',sys.executable,Path(__file__),'--inside','--work',work,
                     '--resolution',args.resolution,'--source-fps',args.source_fps,'--duration',args.duration,'--host-binary',args.host_binary]
            if args.graphics_contention:command+=['--graphics-contention','--graphics-draw-ms',str(args.graphics_draw_ms),'--graphics-period-ms',str(args.graphics_period_ms)]
            if args.cache_imports:command+=['--cache-imports']
            if args.window_capture:command+=['--window-capture']
            if args.early_stream:command+=['--early-stream']
            if args.lease_buffers:command+=['--lease-buffers']
            if args.gfni_fec:command+=['--gfni-fec']
            if args.validate_fec:command+=['--validate-fec']
            if args.quality:command+=['--quality']
            command+=['--codec',args.codec]
            if args.trace_nvenc:command+=['--trace-nvenc','--nvenc-headers',args.nvenc_headers]
            if args.tag_suffix:command+=['--tag-suffix',args.tag_suffix]
            if args.compare:command+=['--compare',args.compare]
            if args.stream_resolution:command+=['--stream-resolution',args.stream_resolution]
            if args.reverse:command+=['--reverse']
            p=subprocess.Popen([str(x)for x in command],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,start_new_session=True);owned.append(p);cleanup.callback(stop,p)
            try:out,err=p.communicate(timeout=experiment_seconds(args)+10)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid,signal.SIGINT)
                try:out,err=p.communicate(timeout=6)
                except subprocess.TimeoutExpired:out,err=stop(p)
                raise RuntimeError('Private compositor experiment deadline')
            finally:
                # Recover only recorded own process groups if an inner wrapper failed unexpectedly.
                if (work/'owned.json').exists():
                    for child in reversed(json.loads((work/'owned.json').read_text())):
                        if start_ticks(child['pid'])==child['start_ticks']and child['start_ticks']is not None:
                            try:os.killpg(child['pid'],signal.SIGTERM)
                            except ProcessLookupError:pass
                for log in work.glob('*.stdout'):(output/(tag+'-'+log.name)).write_bytes(log.read_bytes())
                for log in work.glob('*.stderr'):(output/(tag+'-'+log.name)).write_bytes(log.read_bytes())
            (output/(tag+'-dbus.stdout')).write_bytes(out);(output/(tag+'-dbus.stderr')).write_bytes(err)
            receipt['inside']=json.loads((work/'inside.json').read_text())if(work/'inside.json').exists()else{}
            if p.returncode or receipt['inside'].get('result')!='completed':raise RuntimeError(receipt['inside'].get('error','Private compositor failed'))
            receipt['result']='completed'
    except BaseException as e:receipt.update(result='failed',error=str(e))
    finally:
        for p in reversed(owned):stop(p)
        receipt['elapsed_seconds']=time.monotonic()-started
        receipt['temporary_directory_removed']=not Path(receipt.get('temporary_directory','/does-not-exist')).exists()
        (output/(tag+'.json')).write_text(json.dumps(receipt,indent=2)+'\n')
    print(json.dumps({k:receipt[k]for k in('result','error','elapsed_seconds')if k in receipt}));print(output/(tag+'.json'))
    return 0 if receipt.get('result')=='completed'else 1


if __name__=='__main__':raise SystemExit(main())
