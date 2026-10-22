#!/usr/bin/python3 -I
"""One-time installation/removal of the scoped LG_TV capability helper."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import pwd
import re
import stat
import subprocess
import tempfile


HELPER = Path("/usr/local/libexec/lg-tv-sunshine-setcap")
POLICY = Path("/etc/sudoers.d/lg-tv-sunshine")
SOURCE = Path(__file__).with_name("sunshine_setcap.py")


def installation_payload():
    """Freeze the checkout owner and three allowed paths into root-owned files."""
    project = SOURCE.resolve().parents[2]
    owner = pwd.getpwuid(project.stat().st_uid)
    runtime = str(project / "host/runtime")
    # Sudoers treats whitespace and punctuation as syntax. Refuse ambiguous
    # locations rather than widening the installed command authorization.
    if owner.pw_uid == 0 or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_.-]*", owner.pw_name):
        raise RuntimeError("Checkout must belong to an ordinary local user")
    if not re.fullmatch(r"/[A-Za-z0-9_./+-]+", runtime):
        raise RuntimeError("Checkout path must contain only letters, digits, /, _, ., + or -")
    payload = SOURCE.read_bytes()
    for marker, value in ((b'RUNTIME_ROOT = ""', ("RUNTIME_ROOT = " + repr(runtime)).encode()),
                          (b"OWNER_UID = -1", ("OWNER_UID = " + str(owner.pw_uid)).encode())):
        if payload.count(marker) != 1:
            raise RuntimeError("Unexpected helper configuration template")
        payload = payload.replace(marker, value, 1)
    policy = (
        "# LG_TV fixed Sunshine capture capabilities; no daemon or shell grant.\n"
        + owner.pw_name + " ALL=(root) NOSETENV: NOPASSWD: "
        + ", ".join(str(HELPER) + " " + runtime + "/" + name for name in (
            "sunshine", "sunshine-latency-candidate", "sunshine-latency-prototype"))
        + "\n"
    ).encode()
    return payload, policy


def run(*command):
    """Use fixed system utilities without a shell or user-controlled environment."""
    return subprocess.run(command, check=True, capture_output=True, text=True, timeout=20,
                          env={"PATH": "/usr/sbin:/usr/bin:/sbin:/bin", "LANG": "C"}).stdout.strip()


def existing(path, payload, mode):
    """Refuse unrelated files, symlinks, owners and permissions."""
    try:
        info = path.lstat()
    except FileNotFoundError:
        return False
    if (not stat.S_ISREG(info.st_mode) or info.st_uid != 0 or info.st_gid != 0
            or stat.S_IMODE(info.st_mode) != mode or path.read_bytes() != payload):
        raise RuntimeError(f"Unrecognized existing file; left untouched: {path}")
    return True


def install_file(path, payload, mode):
    """Publish a root-owned file atomically and remove temporary files on failure."""
    directory = path.parent.lstat()
    if not stat.S_ISDIR(directory.st_mode) or directory.st_uid != 0 or directory.st_mode & 0o022:
        raise RuntimeError(f"Unsafe installation directory: {path.parent}")
    name = None
    try:
        with tempfile.NamedTemporaryFile(prefix=".lg-tv-install-", dir=path.parent, delete=False) as stream:
            name = stream.name
            stream.write(payload)
            stream.flush()
            os.fchown(stream.fileno(), 0, 0)
            os.fchmod(stream.fileno(), mode)
            os.fsync(stream.fileno())
        # Never overwrite a pre-existing name, even if another installer races.
        os.link(name, path, follow_symlinks=False)
    finally:
        if name is not None:
            os.unlink(name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("plan", "install", "remove"))
    args = parser.parse_args()
    if SOURCE.is_symlink():
        raise RuntimeError("Helper source must not be a symlink")
    payload, policy_bytes = installation_payload()
    if len(payload) > 65536 or not payload.startswith(b"#!/usr/bin/python3 -I\n"):
        raise RuntimeError("Unexpected helper source")
    compile(payload, str(HELPER), "exec")
    files = ((HELPER, payload, 0o755), (POLICY, policy_bytes, 0o440))
    if args.action == "plan":
        print(json.dumps({"files": [{"path": str(path), "mode": oct(mode),
                                      "owner": "root:root", "sha256": hashlib.sha256(data).hexdigest()}
                                     for path, data, mode in files],
                          "sudoers": policy_bytes.decode(), "background_services": False}, indent=2))
        return
    if os.geteuid() != 0:
        raise RuntimeError("One administrator authorization is required to install/remove this policy")
    os.umask(0o077)
    present = [existing(path, data, mode) for path, data, mode in files]
    run("/usr/bin/visudo", "-c")
    if args.action == "remove":
        # Revoke passwordless access first. Existing executable capabilities remain.
        for (path, _, _), found in reversed(list(zip(files, present))):
            if found:
                path.unlink()
        run("/usr/bin/visudo", "-c")
        print("Removed LG_TV helper and policy; no other sudoers files or services changed")
        return
    created = []
    try:
        for (path, data, mode), found in zip(files, present):
            if not found:
                install_file(path, data, mode)
                created.append(path)
        run("/usr/bin/visudo", "-cf", str(POLICY))
        run("/usr/bin/visudo", "-c")
    except BaseException:
        for path in reversed(created):
            path.unlink()
        raise
    print("Installed LG_TV fixed-capability helper and passwordless policy; no service started")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(str(error), file=__import__("sys").stderr)
        raise SystemExit(1)
