"""Execute bootstrap only with inert command mocks; never clean the local host."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
MOCK = r'''#!INTERPRETER
import json, os
from pathlib import Path
import sys
name = Path(sys.argv[0]).name
log = Path(os.environ['KONOHA_MOCK_LOG'])
previous = [json.loads(line) for line in log.read_text().splitlines()] if log.exists() else []
with log.open('a') as stream:
    stream.write(json.dumps([name, *sys.argv[1:]]) + '\n')
if name == 'df':
    if '--output=avail' in sys.argv:
        count = sum(call[0] == 'df' and '--output=avail' in call for call in previous)
        values = json.loads(os.environ['KONOHA_MOCK_FREE_GIB'])
        print('Avail\n' + str(values[min(count, len(values) - 1)] * 1024 ** 3))
    else:
        print('Filesystem Size Used Avail Use% Mounted on\nmock-root 72G 12G 60G 17% /')
elif name == 'nproc':
    print('8')
elif name == 'awk':
    print('12000')
elif name == 'lsblk':
    print('NAME FSTYPE SIZE MOUNTPOINTS\nmock-root ext4 72G /')
# sudo, curl, mkdir, chmod and git are deliberately inert. In particular,
# sudo never executes its arguments, including rm, apt and swap commands.
'''


class BootstrapTests(unittest.TestCase):
    def exercise(self, runner='github-hosted', actions='true', operating_system='Linux', disk=(33, 60)):
        with tempfile.TemporaryDirectory(prefix='konoha-bootstrap-test-') as temporary:
            root = Path(temporary)
            commands = root / 'commands'
            commands.mkdir()
            mock = commands / 'mock'
            mock.write_text(MOCK.replace('INTERPRETER', sys.executable))
            mock.chmod(0o755)
            for name in ('sudo', 'df', 'lsblk', 'mkdir', 'curl', 'chmod', 'git', 'free', 'nproc', 'awk'):
                (commands / name).symlink_to(mock)
            environment = dict(os.environ)
            environment.update({
                'PATH': str(commands) + ':/usr/bin:/bin',
                'GITHUB_ACTIONS': actions, 'RUNNER_ENVIRONMENT': runner, 'RUNNER_OS': operating_system,
                'GITHUB_WORKSPACE': str(root), 'GITHUB_PATH': str(root / 'github-path'),
                'GITHUB_ENV': str(root / 'github-env'), 'KONOHA_MOCK_LOG': str(root / 'calls'),
                'KONOHA_MOCK_FREE_GIB': json.dumps(disk),
            })
            result = subprocess.run(['bash', str(ROOT / 'ci/bootstrap_runner.sh')],
                                    env=environment, text=True, capture_output=True)
            calls = [json.loads(line) for line in (root / 'calls').read_text().splitlines()] if (root / 'calls').exists() else []
            exported = (root / 'github-env').read_text() if (root / 'github-env').exists() else ''
            return result, calls, exported

    def test_local_or_self_hosted_runner_rejected_before_any_cleanup(self):
        for runner, actions, platform in (('self-hosted', 'true', 'Linux'),
                                          ('github-hosted', 'false', 'Linux'),
                                          ('github-hosted', 'true', 'Windows')):
            with self.subTest(runner=runner, actions=actions, platform=platform):
                result, calls, exported = self.exercise(runner, actions, platform)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('disposable GitHub-hosted Linux', result.stderr)
                self.assertEqual(calls, [])

    def test_explicit_toolchain_cleanup_preserves_system_and_runner_tools(self):
        result, calls, exported = self.exercise()
        self.assertEqual(result.returncode, 0, result.stderr)
        cleanup = next(call for call in calls if call[:2] == ['sudo', 'rm'])
        self.assertEqual(cleanup[:4], ['sudo', 'rm', '-rf', '--'])
        for path in ('/opt/hostedtoolcache', '/usr/share/dotnet', '/usr/local/lib/android',
                     '/usr/share/swift', '/opt/microsoft/powershell', '/usr/lib/jvm', '/opt/az'):
            self.assertIn(path, cleanup)
        for path in cleanup[4:]:
            self.assertNotIn(path, ('/', '/usr', '/usr/bin', '/usr/lib', '/usr/bin/python3', '/usr/bin/gh'))
            self.assertNotIn('externals', path)
            self.assertNotIn('/usr/lib/python', path)
        self.assertIn('before=35433480192 after=64424509440 reclaimed=28991029248', result.stdout)
        self.assertIn('KONOHA_BUILD_JOBS=4', exported)
        self.assertIn('KONOHA_BUILD_MEMORY_MB=12000', exported)
        self.assertFalse(any(call[0] == 'lsblk' for call in calls))
        self.assertTrue(any(call[:2] == ['sudo', 'swapon'] for call in calls))

    def test_disk_gate_retained_with_diagnostics_and_no_swap_on_failure(self):
        result, calls, exported = self.exercise(disk=(33, 33))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('At least 55 GiB', result.stderr)
        self.assertTrue(any(call[0] == 'lsblk' for call in calls))
        self.assertTrue(any(call[:2] == ['sudo', 'du'] for call in calls))
        self.assertFalse(any(call[:2] == ['sudo', 'fallocate'] for call in calls))
        self.assertFalse(any(call[:2] == ['sudo', 'swapon'] for call in calls))
        self.assertEqual(exported, '')

    def test_exact_55_gib_gate_boundary_passes(self):
        result, calls, exported = self.exercise(disk=(33, 55))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(any(call[:2] == ['sudo', 'swapon'] for call in calls))


if __name__ == '__main__':
    unittest.main()
