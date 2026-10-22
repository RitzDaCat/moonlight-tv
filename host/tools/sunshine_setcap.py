#!/usr/bin/python3 -I
"""Grant fixed capture capabilities to the configured Sunshine runtime files.

Installed root-owned; never import project code or execute the target. This
trusts locally built code with CAP_SYS_ADMIN and CAP_SYS_NICE. ELF/path checks
establish file identity, not code safety.
"""

import ctypes
import json
import os
import stat
import struct
import sys


RUNTIME_ROOT = ""  # Filled once by the installer; the root-owned copy has a fixed path.
OWNER_UID = -1  # Filled once by the installer.
NAMES = frozenset(("sunshine", "sunshine-latency-candidate", "sunshine-latency-prototype"))
CAPABILITIES = b"cap_sys_admin,cap_sys_nice=p"
ELF_HEADER = struct.Struct("<16sHHIQQQIHHHHHH")


def check_directory(fd):
    """Accept trusted directory owners and refuse unsafe writable ancestry."""
    info = os.fstat(fd)
    sticky_root = info.st_uid == 0 and info.st_mode & stat.S_ISVTX
    if info.st_uid not in (0, OWNER_UID) or (info.st_mode & 0o022 and not sticky_root):
        raise ValueError("untrusted directory owner or writable directory ancestry")


def open_target(path):
    """Validate a fixed pathname and retain its inode through the capability write."""
    if path not in {RUNTIME_ROOT + "/" + name for name in NAMES}:
        raise ValueError("expected one of the three fixed LG_TV Sunshine runtime paths")
    directory = os.open("/", os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    target = None
    try:
        check_directory(directory)
        parts = path.split("/")[1:]
        for part in parts[:-1]:
            child = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC,
                            dir_fd=directory)
            os.close(directory)
            directory = child
            check_directory(directory)
        target = os.open(parts[-1], os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC,
                         dir_fd=directory)
        info = os.fstat(target)
        if not stat.S_ISREG(info.st_mode) or info.st_uid != OWNER_UID:
            raise ValueError("target must be a regular file owned by the configured user")
        if info.st_nlink != 1:
            raise ValueError("hard-linked files are refused")
        if not info.st_mode & stat.S_IXUSR or info.st_mode & 0o6022:
            raise ValueError("target must be executable without set-ID or group/other write bits")
        header = os.pread(target, ELF_HEADER.size, 0)
        if len(header) != ELF_HEADER.size:
            raise ValueError("truncated ELF header")
        ident, kind, machine, version, _, phoff, _, _, ehsize, phsize, phnum, *_ = ELF_HEADER.unpack(header)
        if (ident[:7] != b"\x7fELF\x02\x01\x01" or kind not in (2, 3) or machine != 62
                or version != 1 or ehsize != ELF_HEADER.size or phsize != 56 or not phnum
                or phoff < ehsize or phoff + phsize * phnum > info.st_size):
            raise ValueError("expected a complete x86-64 ELF executable")
        result, target = target, None
        return result
    finally:
        os.close(directory)
        if target is not None:
            os.close(target)


def grant_caps(target):
    """Set and verify exactly the fixed capability set on the held descriptor."""
    lib = ctypes.CDLL("/usr/lib/libcap.so.2", use_errno=True)
    signatures = (
        ("cap_from_text", [ctypes.c_char_p], ctypes.c_void_p),
        ("cap_get_fd", [ctypes.c_int], ctypes.c_void_p),
        ("cap_compare", [ctypes.c_void_p, ctypes.c_void_p], ctypes.c_int),
        ("cap_set_fd", [ctypes.c_int, ctypes.c_void_p], ctypes.c_int),
        ("cap_free", [ctypes.c_void_p], ctypes.c_int),
    )
    for name, arguments, result in signatures:
        method = getattr(lib, name)
        method.argtypes, method.restype = arguments, result
    expected = lib.cap_from_text(CAPABILITIES)
    if not expected:
        raise OSError(ctypes.get_errno(), "capability parsing failed")
    current = None
    try:
        current = lib.cap_get_fd(target)
        if current and lib.cap_compare(current, expected) == 0:
            return "already present"
        if current:
            lib.cap_free(current)
            current = None
        if lib.cap_set_fd(target, expected) != 0:
            raise OSError(ctypes.get_errno(), "capability write failed")
        current = lib.cap_get_fd(target)
        if not current or lib.cap_compare(current, expected) != 0:
            raise OSError("capability readback did not match")
        return "granted"
    finally:
        if current:
            lib.cap_free(current)
        lib.cap_free(expected)


def main(argv):
    """Accept a single fixed target, with no configurable roots or capabilities."""
    if os.geteuid() != 0:
        raise ValueError("run through the installed scoped sudoers entry")
    if len(argv) != 1:
        raise ValueError("usage: lg-tv-sunshine-setcap <fixed-runtime-path>")
    target = open_target(argv[0])
    try:
        result = grant_caps(target)
    finally:
        os.close(target)
    print(json.dumps({"path": argv[0], "capabilities": CAPABILITIES.decode(), "result": result}))


if __name__ == "__main__":
    try:
        main(sys.argv[1:])
    except (OSError, ValueError) as error:
        print(f"lg-tv-sunshine-setcap: {error}", file=sys.stderr)
        sys.exit(2)
