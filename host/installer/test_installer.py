"""Installer checks use temporary XDG directories and mocked system commands."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('install_host', HERE/'install_host.py')
module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
root_spec = importlib.util.spec_from_file_location('root_support', HERE/'root_support.py')
support = importlib.util.module_from_spec(root_spec); root_spec.loader.exec_module(support)


class InstallerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='lg-tv-installer-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        env = {name: str(self.root/leaf) for name, leaf in
               [('XDG_CONFIG_HOME', 'config'), ('XDG_DATA_HOME', 'data with % spaces'), ('XDG_STATE_HOME', 'state')]}
        self.environment = patch.dict(os.environ, env)
        self.environment.start(); self.addCleanup(self.environment.stop)
        self.installer = module.Installer()
        self.calls = []

    def fake_run(self, *args, **kwargs):
        self.calls.append(tuple(str(x) for x in args))
        stdout = json.dumps({'created_hook': True}) if args[-1] == 'install' else ''
        return subprocess.CompletedProcess(args, 1 if 'is-enabled' in args else 0, stdout, '')

    def test_setup_preserves_existing_configuration_and_is_idempotent(self):
        config = self.installer.config/'sunshine/sunshine.conf'
        config.parent.mkdir(parents=True)
        original = b'sunshine_name = existing-host\n'
        config.write_bytes(original)
        with patch.object(module, 'run', self.fake_run), patch.object(module.subprocess, 'run') as system, \
             patch.object(self.installer, 'preflight'):
            self.installer.install(start=False)
            self.installer.install(start=False)
        self.assertEqual(config.read_bytes(), original)
        self.assertEqual(system.call_args.args[0], ['sudo', '--', '/usr/bin/pacman', '-S', '--needed', '--',
                                                  'sunshine', 'python', 'libpulse'])
        self.assertNotIn('-Sy', system.call_args.args[0])
        self.assertFalse(any('enable' in call for call in self.calls))
        self.assertEqual(self.installer.helper.read_bytes(), self.installer.source.read_bytes())
        self.assertEqual(self.installer.unit.stat().st_mode & 0o777, 0o600)
        self.assertIn('%%', self.installer.unit.read_text())
        self.assertEqual(set(self.installer.previous()['files']), {str(self.installer.helper), str(self.installer.unit)})

    def test_modified_helper_blocks_uninstall_before_service_changes(self):
        with patch.object(module, 'run', self.fake_run), patch.object(module.subprocess, 'run'), \
             patch.object(self.installer, 'preflight'):
            self.installer.install(start=False)
        self.installer.helper.write_text('user edit\n')
        self.calls.clear()
        with patch.object(module, 'run', self.fake_run), self.assertRaisesRegex(RuntimeError, 'Modified file'):
            self.installer.uninstall()
        self.assertFalse(self.calls)
        self.assertTrue(self.installer.manifest.exists())

    def test_manifest_cannot_delete_arbitrary_files(self):
        victim = self.root/'unrelated'; victim.write_bytes(b'keep')
        self.installer.publish(self.installer.manifest, json.dumps({'files': {str(victim): module.digest(b'keep')}}).encode())
        with self.assertRaisesRegex(RuntimeError, 'Unexpected paths'):
            self.installer.uninstall()
        self.assertEqual(victim.read_bytes(), b'keep')

    def test_root_setup_failure_writes_no_user_units(self):
        def fail(*args, **kwargs):
            if args[0] == 'sudo': raise RuntimeError('root step failed')
            return self.fake_run(*args, **kwargs)
        with patch.object(module, 'run', fail), patch.object(module.subprocess, 'run'), \
             patch.object(self.installer, 'preflight'), self.assertRaisesRegex(RuntimeError, 'root step'):
            self.installer.install()
        self.assertFalse(self.installer.unit.exists())
        self.assertFalse(self.installer.helper.exists())

    def test_uninstall_retains_sunshine_config_and_does_not_stop_game_host(self):
        with patch.object(module, 'run', self.fake_run), patch.object(module.subprocess, 'run'), \
             patch.object(self.installer, 'preflight'):
            self.installer.install(start=False)
        self.calls.clear()
        with patch.object(module, 'run', self.fake_run): self.installer.uninstall()
        self.assertTrue((self.installer.config/'sunshine/sunshine.conf').exists())
        self.assertFalse(self.installer.helper.exists())
        self.assertFalse(self.installer.unit.exists())
        self.assertFalse(any('--now' in call and module.SUNSHINE_UNIT in call for call in self.calls))

    def test_systemd_quoting_rejects_newlines(self):
        with self.assertRaises(ValueError): module.quote_unit('/tmp/bad\npath')
        self.assertEqual(module.quote_unit('a% b"c'), '"a%% b\\"c"')

    def test_root_hook_has_only_fixed_package_executable(self):
        content = support.HOOK_BYTES.decode()
        self.assertIn('Target = sunshine', content)
        self.assertIn('Exec = /usr/bin/setcap cap_sys_admin,cap_sys_nice+p /usr/bin/sunshine', content)
        self.assertNotIn('NOPASSWD', content)
        hook = self.root/'hook'; hook.write_text('unrelated')
        with patch.object(support, 'HOOK', hook), patch.object(support.os, 'geteuid', return_value=0), \
             patch.object(support, 'trusted'), self.assertRaisesRegex(RuntimeError, 'Unrelated'):
            support.configure('remove')
        self.assertEqual(hook.read_text(), 'unrelated')


if __name__ == '__main__': unittest.main()
