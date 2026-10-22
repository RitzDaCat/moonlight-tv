#!/usr/bin/env python3
"""Package the Windows host helper without downloads or local machine configuration."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import zipfile


def build(output):
    here = Path(__file__).resolve().parent
    root = here.parents[1]
    sources = {name: (here / "windows" / name).read_bytes()
               for name in ("install-host.cmd", "windows-host.ps1", "README.md")}
    sources["LICENSE"] = (here / "LICENSE").read_bytes()
    commit = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"],
                            capture_output=True, text=True, check=True).stdout.strip()
    manifest = {"format": 1, "source_repository": "https://github.com/RitzDaCat/moonlight-tv",
                "source_commit": commit, "package": "moonlight-tv-windows-host-setup.zip",
                "source_sha256": {name: hashlib.sha256(data).hexdigest() for name, data in sources.items()},
                "target": "Windows 10/11 x64", "sunshine_bundled": False,
                "sunshine_install_source": "official LizardByte.Sunshine WinGet package or official MSI",
                "contains_personal_host_configuration": False}
    sources["source-manifest.json"] = (json.dumps(manifest, indent=2) + "\n").encode()
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(prefix=".windows-host-", dir=output, delete=False) as stream:
            temporary = Path(stream.name)
        with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for name, data in sources.items():
                item = zipfile.ZipInfo("moonlight-tv-windows-host-setup/" + name, (1980, 1, 1, 0, 0, 0))
                item.compress_type = zipfile.ZIP_DEFLATED
                item.external_attr = 0o100644 << 16
                archive.writestr(item, data)
        destination = output / manifest["package"]
        temporary.replace(destination)
        manifest["sha256"] = hashlib.sha256(destination.read_bytes()).hexdigest()
        (output / "windows-host-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        return manifest
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    print(json.dumps(build(parser.parse_args().output), indent=2))


if __name__ == "__main__":
    main()
