#!/usr/bin/env python3
"""Install, inspect or remove the portable CachyOS/Arch Sunshine host setup."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile

SUNSHINE_UNIT = 'app-dev.lizardbyte.app.Sunshine.service'
ASSIST_UNIT = 'lg-tv-session-assist.service'
MARKER = '# Managed by lg-tv-streaming'
BASE_CONFIG = '''# Initial configuration only; existing Sunshine configuration is preserved.
origin_web_ui_allowed = pc
upnp = disabled
system_tray = disabled
# Sunshine selects the hardware encoder and streaming audio monitor automatically.
# Fast NVIDIA policy is ignored when a different encoder is selected.
nvenc_preset = 1
nvenc_twopass = disabled
'''


def run(*args, check=True, timeout=30):
    return subprocess.run([str(x) for x in args], check=check, text=True, capture_output=True, timeout=timeout)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def quote_unit(value):
    if any(ord(c) < 32 for c in value): raise ValueError('Control character in installation path')
    return '"'+value.replace('%', '%%').replace('\\', '\\\\').replace('"', '\\"')+'"'


class Installer:
    def __init__(self):
        self.config = Path(os.environ.get('XDG_CONFIG_HOME', str(Path.home()/'.config')))
        self.data = Path(os.environ.get('XDG_DATA_HOME', str(Path.home()/'.local/share')))/'lg-tv-streaming'
        self.state = Path(os.environ.get('XDG_STATE_HOME', str(Path.home()/'.local/state')))/'lg-tv-streaming'
        self.manifest = self.state/'installation.json'
        if not all(path.is_absolute() for path in (self.config, self.data, self.state)):
            raise RuntimeError('XDG installation directories must be absolute paths')
        self.unit = self.config/'systemd/user'/ASSIST_UNIT
        self.helper = self.data/'session_assist.py'
        here = Path(__file__).resolve().parent
        self.root_support = here/'root_support.py'
        self.source = here/'session_assist.py'
        if not self.source.exists(): self.source = here.parent/'tools/session_assist.py'

    def payloads(self):
        service = f'''{MARKER}
[Unit]
Description=Streaming game audio and Xwayland focus handoff
After=graphical-session.target pipewire-pulse.service
PartOf=graphical-session.target

[Service]
Environment={quote_unit('LG_TV_SESSION_STATE_DIR='+str(self.state/'session-assist'))}
ExecStart=/usr/bin/python3 -B {quote_unit(str(self.helper))} run
Restart=on-failure
RestartSec=3
TimeoutStopSec=10

[Install]
WantedBy=graphical-session.target
'''
        return {self.helper: self.source.read_bytes(), self.unit: service.encode()}

    def previous(self):
        if not self.manifest.exists(): return {}
        if self.manifest.is_symlink(): raise RuntimeError('Installation manifest must not be a symlink')
        return json.loads(self.manifest.read_text())

    def preflight(self):
        if os.geteuid() == 0: raise RuntimeError('Run as the desktop user; the installer requests sudo only for system setup')
        release = Path('/etc/os-release').read_text()
        if not any(line.strip() in ('ID=arch', 'ID=cachyos', 'ID="arch"', 'ID="cachyos"') for line in release.splitlines()):
            raise RuntimeError('This installer currently supports CachyOS and Arch Linux')
        if not shutil.which('pacman'): raise RuntimeError('pacman was not found')
        old = self.previous().get('files', {})
        for path, payload in self.payloads().items():
            if not path.exists() and not path.is_symlink(): continue
            info = path.lstat()
            if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid():
                raise RuntimeError('Unrecognized existing file: '+str(path))
            current = digest(path.read_bytes())
            if current != digest(payload) and old.get(str(path)) != current:
                raise RuntimeError('Existing file is not managed by this installer: '+str(path))

    @staticmethod
    def publish(path, data):
        path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        name = None
        try:
            with tempfile.NamedTemporaryFile(prefix='.lg-tv-', dir=path.parent, delete=False) as stream:
                name = stream.name
                stream.write(data); stream.flush(); os.fchmod(stream.fileno(), 0o600); os.fsync(stream.fileno())
            os.replace(name, path)
        finally:
            if name is not None and os.path.exists(name): os.unlink(name)

    def install(self, start=True):
        self.preflight()
        old = self.previous()
        before_enabled = run('systemctl', '--user', 'is-enabled', SUNSHINE_UNIT, check=False).returncode == 0
        # Use configured signed repositories. Do not run pacman -Sy or silently
        # add repositories, change drivers, or perform a full distribution upgrade.
        subprocess.run(['sudo', '--', '/usr/bin/pacman', '-S', '--needed', '--',
                        'sunshine', 'python', 'libpulse'], check=True)
        support = json.loads(run('sudo', '--', '/usr/bin/python3', '-I', self.root_support, 'install').stdout)
        receipt = {'format': 1, 'files': {}, 'sunshine_was_enabled': old.get('sunshine_was_enabled', before_enabled),
                   'created_root_hook': old.get('created_root_hook', False) or support['created_hook']}
        config = self.config/'sunshine/sunshine.conf'
        try:
            if not config.exists() and not config.is_symlink():
                config.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
                fd = os.open(config, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
                with os.fdopen(fd, 'w') as stream: stream.write(BASE_CONFIG)
                receipt['initial_config_created'] = True
            for path, payload in self.payloads().items():
                self.publish(path, payload); receipt['files'][str(path)] = digest(payload)
            self.publish(self.manifest, (json.dumps(receipt, indent=2)+'\n').encode())
            run('systemctl', '--user', 'daemon-reload')
            if start:
                # Enabling an already running unit does not restart its game session.
                run('systemctl', '--user', 'enable', '--now', SUNSHINE_UNIT)
                run('systemctl', '--user', 'enable', '--now', ASSIST_UNIT)
        except BaseException:
            # Persist the exact owned resources so a failed setup is removable.
            self.publish(self.manifest, (json.dumps(receipt, indent=2)+'\n').encode())
            raise
        print('Host setup installed. Create local Sunshine credentials at https://localhost:47990/ and pair this PC in Moonlight.')
        print('Existing game settings, pairing, audio devices and firewall rules were preserved.')

    def uninstall(self):
        receipt = self.previous()
        if not receipt: raise RuntimeError('No managed installation found')
        allowed = {str(self.helper), str(self.unit)}
        files = receipt.get('files', {})
        if not set(files).issubset(allowed): raise RuntimeError('Unexpected paths in installation manifest')
        for name, expected in files.items():
            path = Path(name)
            if not path.exists() and not path.is_symlink(): continue
            if path.is_symlink() or not path.is_file() or digest(path.read_bytes()) != expected:
                raise RuntimeError('Modified file left untouched: '+name)
        run('systemctl', '--user', 'disable', '--now', ASSIST_UNIT, check=False)
        if not receipt.get('sunshine_was_enabled', True):
            run('systemctl', '--user', 'disable', SUNSHINE_UNIT, check=False)
        if receipt.get('created_root_hook'):
            run('sudo', '--', '/usr/bin/python3', '-I', self.root_support, 'remove')
        for name in files: Path(name).unlink(missing_ok=True)
        self.manifest.unlink()
        run('systemctl', '--user', 'daemon-reload')
        print('Removed owned integration files. Sunshine package, live service, credentials, pairing and game configuration were retained.')

    def doctor(self):
        report = {'package': run('pacman', '-Q', 'sunshine', check=False).stdout.strip(),
                  'capabilities': run('getcap', '/usr/bin/sunshine', check=False).stdout.strip(),
                  'uinput_access': os.access('/dev/uinput', os.R_OK | os.W_OK),
                  'uhid_access': os.access('/dev/uhid', os.R_OK | os.W_OK),
                  'sunshine_active': run('systemctl', '--user', 'is-active', SUNSHINE_UNIT, check=False).stdout.strip(),
                  'helper_active': run('systemctl', '--user', 'is-active', ASSIST_UNIT, check=False).stdout.strip(),
                  'configuration_present': (self.config/'sunshine/sunshine.conf').is_file(),
                  'pairing_action': 'Use this PC\'s local Sunshine web UI and the PIN shown by Moonlight'}
        print(json.dumps(report, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('plan', 'install', 'doctor', 'uninstall'), nargs='?', default='plan')
    parser.add_argument('--no-start', action='store_true')
    args = parser.parse_args()
    installer = Installer()
    if args.action == 'plan':
        print(json.dumps({'packages': ['sunshine', 'python', 'libpulse'], 'user_files': [str(x) for x in installer.payloads()],
                          'system_setup': 'KMS capabilities, uinput/uhid and a fixed package upgrade hook',
                          'configuration': 'create only when absent', 'firewall': 'unchanged',
                          'backend': 'signed repository Sunshine; experimental patches are source-only',
                          'start_services': not args.no_start}, indent=2))
    elif args.action == 'install': installer.install(not args.no_start)
    elif args.action == 'doctor': installer.doctor()
    else: installer.uninstall()


if __name__ == '__main__':
    main()
