#!/usr/bin/env python3
"""Exercise capability-helper filesystem boundaries without granting privileges."""

import importlib.util
import os
from pathlib import Path
import shutil
import stat
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("sunshine_setcap", Path(__file__).with_name("sunshine_setcap.py"))
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)
installer_spec = importlib.util.spec_from_file_location(
    "install_sunshine_permissions", Path(__file__).with_name("install_sunshine_permissions.py"))
installer = importlib.util.module_from_spec(installer_spec)
installer_spec.loader.exec_module(installer)


class CapabilityBoundaryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="lg-tv-permission-test-", dir="/tmp")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "runtime"
        self.root.mkdir()
        self.target = self.root / "sunshine-latency-prototype"
        shutil.copyfile("/usr/bin/true", self.target)
        self.target.chmod(0o755)
        for name, value in (("RUNTIME_ROOT", str(self.root)), ("OWNER_UID", os.getuid())):
            context = patch.object(helper, name, value)
            context.start()
            self.addCleanup(context.stop)

    def opened(self):
        descriptor = helper.open_target(str(self.target))
        self.addCleanup(os.close, descriptor)
        return descriptor

    def refused(self):
        with self.assertRaises((ValueError, OSError)):
            self.opened()

    def test_accepts_real_elf_and_pins_inode_after_path_replacement(self):
        descriptor = self.opened()
        inode = os.fstat(descriptor).st_ino
        self.target.rename(self.root / "retired")
        shutil.copyfile("/usr/bin/true", self.target)
        self.assertNotEqual(inode, self.target.stat().st_ino)
        self.assertEqual(inode, os.fstat(descriptor).st_ino)

    def test_refuses_arbitrary_paths_and_traversal(self):
        for path in ("/usr/bin/true", str(self.root / "sunshine-other"),
                     str(self.root) + "/../runtime/sunshine-latency-prototype"):
            with self.subTest(path=path), self.assertRaises(ValueError):
                helper.open_target(path)

    def test_refuses_target_symlink(self):
        self.target.unlink()
        self.target.symlink_to("/usr/bin/true")
        self.refused()

    def test_refuses_symlink_directory(self):
        self.root.rename(self.root.with_name("moved"))
        self.root.symlink_to(self.root.with_name("moved"), target_is_directory=True)
        self.refused()

    def test_refuses_hardlinks(self):
        os.link(self.target, self.root / "other")
        self.refused()

    def test_refuses_fifo_without_blocking(self):
        self.target.unlink()
        os.mkfifo(self.target, 0o755)
        self.refused()

    def test_refuses_unsafe_file_modes(self):
        for mode in (0o644, 0o775, 0o757, 0o4755, 0o2755):
            with self.subTest(mode=oct(mode)):
                self.target.chmod(mode)
                self.refused()

    def test_refuses_writable_ancestry(self):
        self.root.chmod(0o777)
        self.refused()

    def test_refuses_wrong_owner(self):
        original = os.fstat

        def wrong_file_owner(fd):
            info = original(fd)
            if stat.S_ISREG(info.st_mode):
                fields = list(info)
                fields[4] = os.getuid() + 1
                return os.stat_result(fields)
            return info

        with patch.object(helper.os, "fstat", side_effect=wrong_file_owner):
            with self.assertRaisesRegex(ValueError, "regular file owned by the configured user"):
                self.opened()

    def test_refuses_script_truncated_or_wrong_architecture(self):
        good = self.target.read_bytes()
        wrong_arch = bytearray(good)
        wrong_arch[18:20] = (183).to_bytes(2, "little")
        for contents in (b"#!/bin/sh\nexit 0\n", good[:30], bytes(wrong_arch)):
            with self.subTest(size=len(contents)):
                self.target.write_bytes(contents)
                self.refused()

    def test_rejected_targets_do_not_leak_descriptors(self):
        self.target.chmod(0o777)
        before = len(os.listdir("/proc/self/fd"))
        for _ in range(100):
            self.refused()
        self.assertEqual(before, len(os.listdir("/proc/self/fd")))

    def test_nonroot_cannot_enter_grant_path(self):
        with patch.object(helper.os, "geteuid", return_value=1000):
            with self.assertRaisesRegex(ValueError, "scoped sudoers"):
                helper.main([str(self.target)])

    def test_installed_helper_freezes_checkout_owner_and_exact_targets(self):
        project = self.root / "checkout"
        source = project / "host/tools/sunshine_setcap.py"
        source.parent.mkdir(parents=True)
        source.write_bytes(Path(helper.__file__).read_bytes())
        with patch.object(installer, "SOURCE", source):
            payload, policy = installer.installation_payload()
        namespace = {"__name__": "installed_helper"}
        exec(compile(payload, "installed_helper", "exec"), namespace)
        self.assertEqual(namespace["RUNTIME_ROOT"], str(project / "host/runtime"))
        self.assertEqual(namespace["OWNER_UID"], os.getuid())
        self.assertEqual(policy.count(b"NOPASSWD:"), 1)
        self.assertEqual(policy.count(str(project / "host/runtime").encode()), 3)
        self.assertNotIn(b"*", policy)

    def test_installer_refuses_ambiguous_sudoers_paths(self):
        project = self.root / "checkout with spaces"
        source = project / "host/tools/sunshine_setcap.py"
        source.parent.mkdir(parents=True)
        source.write_bytes(Path(helper.__file__).read_bytes())
        with patch.object(installer, "SOURCE", source), \
             self.assertRaisesRegex(RuntimeError, "Checkout path"):
            installer.installation_payload()


if __name__ == "__main__":
    unittest.main()
