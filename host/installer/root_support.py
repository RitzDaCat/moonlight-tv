#!/usr/bin/python3 -I
"""One-time package permissions and a fixed pacman upgrade hook; no sudoers rule."""
import argparse
import json
import os
from pathlib import Path
import stat
import subprocess
import tempfile

HOOK = Path('/etc/pacman.d/hooks/90-lg-tv-sunshine-caps.hook')
HOOK_BYTES = b'''# Managed by lg-tv-streaming; fixed package-owned executable only.
[Trigger]
Operation = Install
Operation = Upgrade
Type = Package
Target = sunshine

[Action]
Description = Restore Sunshine KMS capture and scheduling capabilities
When = PostTransaction
Depends = libcap
Exec = /usr/bin/setcap cap_sys_admin,cap_sys_nice+p /usr/bin/sunshine
'''


def run(*args):
    return subprocess.run(args, check=True, text=True, capture_output=True, timeout=30,
                          env={'PATH': '/usr/bin:/usr/sbin', 'LANG': 'C'}).stdout.strip()


def trusted(path, mode=None):
    info = path.lstat()
    if not stat.S_ISREG(info.st_mode) or info.st_uid != 0 or info.st_gid != 0 or info.st_mode & 0o022:
        raise RuntimeError('Expected a root-owned, non-writable regular file: ' + str(path))
    if mode is not None and stat.S_IMODE(info.st_mode) != mode:
        raise RuntimeError('Unexpected permissions: ' + str(path))


def configure(action):
    if os.geteuid() != 0:
        raise RuntimeError('Run this installation step with sudo')
    existing = HOOK.exists() or HOOK.is_symlink()
    if existing:
        trusted(HOOK, 0o644)
        if HOOK.read_bytes() != HOOK_BYTES:
            raise RuntimeError('Unrelated pacman hook left untouched: ' + str(HOOK))
    if action == 'remove':
        if existing: HOOK.unlink()
        return {'removed_hook': existing, 'executable_capabilities': 'retained'}
    binary = Path('/usr/bin/sunshine')
    trusted(binary)
    if run('/usr/bin/pacman', '-Qoq', str(binary)) != 'sunshine':
        raise RuntimeError('Sunshine executable must belong to the sunshine package')
    parent = HOOK.parent
    parent.mkdir(mode=0o755, parents=True, exist_ok=True)
    info = parent.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o022:
        raise RuntimeError('Unsafe pacman hook directory')
    created = False
    if not existing:
        name = None
        try:
            with tempfile.NamedTemporaryFile(prefix='.lg-tv-', dir=parent, delete=False) as stream:
                name = stream.name
                stream.write(HOOK_BYTES); stream.flush(); os.fchmod(stream.fileno(), 0o644)
                os.fsync(stream.fileno())
            os.link(name, HOOK, follow_symlinks=False)
            created = True
        finally:
            if name is not None: os.unlink(name)
    try:
        run('/usr/bin/setcap', 'cap_sys_admin,cap_sys_nice+p', str(binary))
        run('/usr/bin/modprobe', 'uinput')
        run('/usr/bin/modprobe', 'uhid')
        run('/usr/bin/udevadm', 'control', '--reload-rules')
        for name in ('uinput', 'uhid'):
            run('/usr/bin/udevadm', 'trigger', '--action=change', '--sysname-match='+name)
    except BaseException:
        if created: HOOK.unlink()
        raise
    return {'created_hook': created, 'capabilities': run('/usr/bin/getcap', str(binary)),
            'passwordless_sudo': False}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('install', 'remove'))
    print(json.dumps(configure(parser.parse_args().action)))
