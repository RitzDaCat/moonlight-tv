#!/usr/bin/env python3
"""Prepare, enable, inspect, or remove the reversible Sunshine TV test profile."""

import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import time
import urllib.request
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[2]
UNIT = "app-dev.lizardbyte.app.Sunshine.service"
BINARY = ROOT / "host/runtime/sunshine-latency-prototype"
EXPECTED_SHA256 = "2f0802ef93c7b1eb380b4d1e498cc7d0907acd85e7a6ca74f15bc20a701cfe0f"
STAGED = ROOT / "host/runtime/tv-test/50-lg-tv-latency-test.conf"
DROPIN = Path.home() / ".config/systemd/user" / (UNIT + ".d") / STAGED.name
CAPABILITIES = "cap_sys_admin,cap_sys_nice=p"
PERMISSION_HELPER = Path("/usr/local/libexec/lg-tv-sunshine-setcap")
PROFILE = f"""# Managed by LG_TV/host/tools/tv_test.py; remove with its disable command.
[Service]
Environment=SUNSHINE_PROFILE_VIDEO=1
Environment=SUNSHINE_CACHE_DMABUF_IMPORTS=1
Environment=SUNSHINE_EARLY_CUDA_STREAM=1
Environment=SUNSHINE_LEASE_PIPEWIRE_BUFFERS=1
Environment=SUNSHINE_GFNI_FEC=1
ExecStart=
ExecStart={BINARY} {Path.home() / '.config/sunshine/sunshine.conf'} encoder=nvenc capture=kms nvenc_preset=1 nvenc_twopass=disabled nvenc_split_encode=enabled
"""


def run(*args):
    """Run a bounded command, preserving its diagnostic output on failure."""
    result = subprocess.run(args, text=True, capture_output=True, timeout=30)
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or result.stdout.strip() or f"Command failed: {args[0]}")
    return result.stdout.strip()


def validate_binary():
    """Require the exact executable that passed the retained validation."""
    if hashlib.sha256(BINARY.read_bytes()).hexdigest() != EXPECTED_SHA256:
        raise RuntimeError("Prototype hash differs from the validated build; profile not changed")


def check_dropin():
    """Never replace or remove an unrelated service override."""
    if DROPIN.exists() and DROPIN.read_text() != PROFILE:
        raise RuntimeError(f"Unrecognized override; left untouched: {DROPIN}")


def ensure_capabilities(capabilities):
    """Use only the installed fixed-purpose helper; never trigger a password prompt."""
    if capabilities.endswith(" " + CAPABILITIES):
        return
    if not PERMISSION_HELPER.is_file():
        raise RuntimeError("Capture permissions missing. Install the one-time scoped helper: "
                           f"sudo /usr/bin/python3 -I {ROOT}/host/tools/install_sunshine_permissions.py install")
    print(run("sudo", "-n", str(PERMISSION_HELPER), str(BINARY)))
    if not run("getcap", str(BINARY)).endswith(" " + CAPABILITIES):
        raise RuntimeError("Capture capabilities did not match after the helper ran")


def server_state():
    """Read only public local host status, without exposing credentials."""
    with urllib.request.urlopen("http://127.0.0.1:47989/serverinfo", timeout=2) as response:
        root = ET.fromstring(response.read())
    return {key: root.findtext(key) for key in ("state", "currentgame")}


def restore():
    """Remove this profile only, restoring the original service override."""
    check_dropin()
    if not DROPIN.exists():
        print("Test profile already disabled; service not restarted")
        return
    DROPIN.unlink()
    run("systemctl", "--user", "daemon-reload")
    run("systemctl", "--user", "restart", UNIT)
    print("Original Sunshine service restored")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("prepare", "enable", "disable", "status"))
    args = parser.parse_args()
    if args.action == "disable":
        restore()
        return
    validate_binary()
    capabilities = run("getcap", str(BINARY))
    check_dropin()
    if args.action == "status":
        print("Test profile:", "enabled" if DROPIN.exists() else "disabled")
        print("Prototype capture capabilities:", capabilities or "missing")
        print("Passwordless permission helper:", "installed" if PERMISSION_HELPER.is_file() else "not installed")
        print(run("systemctl", "--user", "show", UNIT,
                  "--property=ActiveState,SubState,MainPID,ExecStart"))
        print("Local server:", server_state())
        return
    STAGED.parent.mkdir(parents=True, exist_ok=True)
    STAGED.write_text(PROFILE)
    if args.action == "prepare":
        print("Prepared:", STAGED)
        print("Live service unchanged")
        if not capabilities.endswith(" " + CAPABILITIES):
            print("Enable will use the scoped passwordless helper once it is installed")
        return
    ensure_capabilities(capabilities)
    # Profile switches deliberately restart Sunshine; disconnect Moonlight first.
    DROPIN.parent.mkdir(parents=True, exist_ok=True)
    DROPIN.write_text(PROFILE)
    try:
        run("systemctl", "--user", "daemon-reload")
        run("systemctl", "--user", "restart", UNIT)
        deadline = time.monotonic() + 15
        while True:
            try:
                server_state()
                break
            except (OSError, ET.ParseError):
                if time.monotonic() >= deadline:
                    raise RuntimeError("Prototype server did not become ready")
                time.sleep(0.5)
        command = run("systemctl", "--user", "show", UNIT, "--property=ExecStart", "--value")
        if "path=" + str(BINARY) + " ;" not in command:
            raise RuntimeError("Service did not select the expected prototype")
    except BaseException:
        restore()
        raise
    print("Test profile active; original config, pairing and runtime preserved")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
