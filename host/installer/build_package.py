#!/usr/bin/env python3
"""Build a small native Arch integration package from local source, without downloads."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    helper = here/'session_assist.py'
    if not helper.exists(): helper = here.parent/'tools/session_assist.py'
    output = args.output.resolve(); output.mkdir(parents=True, exist_ok=True)
    sources = {name: (here/name).read_bytes() for name in ('install_host.py', 'root_support.py', 'README.md')}
    sources['session_assist.py'] = helper.read_bytes()
    sources['LICENSE'] = (here/'LICENSE').read_bytes()
    sources['lg-tv-host-setup'] = b'#!/bin/sh\nexec /usr/bin/python3 /usr/lib/lg-tv-streaming/install_host.py "$@"\n'
    sources['lg-tv-host-setup.desktop'] = b'''[Desktop Entry]
Type=Application
Name=LG TV streaming host setup
Comment=Set up this PC for Moonlight streaming
Exec=lg-tv-host-setup install
Terminal=true
Icon=video-display
Categories=Game;Settings;
'''
    # The upgrade hook and one-time system setup come from the same source.
    import importlib.util
    spec = importlib.util.spec_from_file_location('root_support', here/'root_support.py')
    support = importlib.util.module_from_spec(spec); spec.loader.exec_module(support)
    sources['90-lg-tv-sunshine-caps.hook'] = support.HOOK_BYTES
    sources['lg-tv-streaming-host.install'] = b'''post_install() {
  /usr/bin/python3 -I /usr/lib/lg-tv-streaming/root_support.py install ||
    printf '%s\\n' 'System setup incomplete; run lg-tv-host-setup doctor after installation.'
  printf '%s\\n' 'Run LG TV streaming host setup from the desktop menu, or lg-tv-host-setup install as the desktop user.'
}
post_upgrade() { post_install; }
'''
    with tempfile.TemporaryDirectory(prefix='lg-tv-arch-package-') as scratch:
        work = Path(scratch)
        for name, data in sources.items(): (work/name).write_bytes(data)
        names = ' '.join("'"+name+"'" for name in sources)
        checksums = ' '.join("'"+hashlib.sha256(data).hexdigest()+"'" for data in sources.values())
        (work/'PKGBUILD').write_text(f'''pkgname=lg-tv-streaming-host
pkgver=0.1.0
pkgrel=1
pkgdesc='CachyOS/Arch Sunshine setup with portable controller, focus and audio integration'
arch=('any')
license=('GPL-3.0-only')
depends=('sunshine' 'python' 'libpulse')
install=lg-tv-streaming-host.install
source=({names})
sha256sums=({checksums})
options=('!debug')
package() {{
  for file in install_host.py root_support.py session_assist.py; do
    install -Dm644 "$srcdir/$file" "$pkgdir/usr/lib/lg-tv-streaming/$file"
  done
  install -Dm755 "$srcdir/lg-tv-host-setup" "$pkgdir/usr/bin/lg-tv-host-setup"
  install -Dm644 "$srcdir/lg-tv-host-setup.desktop" "$pkgdir/usr/share/applications/lg-tv-host-setup.desktop"
  install -Dm644 "$srcdir/90-lg-tv-sunshine-caps.hook" "$pkgdir/etc/pacman.d/hooks/90-lg-tv-sunshine-caps.hook"
  install -Dm644 "$srcdir/README.md" "$pkgdir/usr/share/doc/lg-tv-streaming/README.md"
  install -Dm644 "$srcdir/LICENSE" "$pkgdir/usr/share/licenses/lg-tv-streaming-host/LICENSE"
}}
''')
        result = subprocess.run(['makepkg', '--nodeps', '--cleanbuild', '--noconfirm'], cwd=work,
                                env={**os.environ, 'PACKAGER': 'lg-tv-streaming', 'PKGDEST': str(work)},
                                capture_output=True, text=True, timeout=120)
        if result.returncode: raise RuntimeError(result.stdout+result.stderr)
        packages = list(work.glob('lg-tv-streaming-host-*.pkg.tar.*'))
        if len(packages) != 1: raise RuntimeError('Unexpected package output')
        destination = output/packages[0].name
        shutil.copyfile(packages[0], destination)
        receipt = {'package': destination.name, 'sha256': hashlib.sha256(destination.read_bytes()).hexdigest(),
                   'source_sha256': {name: hashlib.sha256(data).hexdigest() for name, data in sources.items()},
                   'downloads': False, 'installed_on_this_pc': False,
                   'scope': 'integration package; Sunshine is a signed repository dependency'}
        (output/'package-manifest.json').write_text(json.dumps(receipt, indent=2)+'\n')
        print(json.dumps(receipt, indent=2))


if __name__ == '__main__': main()
