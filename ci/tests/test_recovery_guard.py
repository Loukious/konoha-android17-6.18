"""Run the recovery guard against host syscall and synthetic DM fixtures."""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parents[1] / 'recovery'


class RecoveryGuard(unittest.TestCase):
    def test_snapshot_supervision_and_device_mapper_guards(self):
        with tempfile.TemporaryDirectory(prefix='onyx-recovery-guard-') as directory:
            root = Path(directory)
            shutil.copyfile(HERE / 'recovery-guard.c', root / 'recovery-guard.c')
            shutil.copyfile(HERE / 'tests/guard_cases.py', root / 'test.py')
            shutil.copyfile(HERE / 'tools/recovery-guard', root / 'guard-aarch64')
            flags = ['-nostdlib', '-static', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
                     '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-fno-ident', '-Os',
                     '-Wall', '-Wextra', '-Werror', '-Wl,--build-id=none', '-Wl,-z,noexecstack']
            subprocess.run(['cc', *flags, '-DGUARD_TEST', str(root / 'recovery-guard.c'),
                            '-o', str(root / 'guard-test')], check=True, capture_output=True, timeout=30)
            subprocess.run(['python3', str(root / 'test.py')], check=True, capture_output=True, timeout=30)
            report = json.loads((root / 'review.json').read_text())
            self.assertEqual(report['status'], 'PASS')
            self.assertEqual(report['scenario_count'], 44)
            result = subprocess.run(['python3', str(HERE / 'tests/dm_cases.py'), str(root / 'guard-test'), str(root)],
                                    text=True, capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            report = json.loads((root / 'dm/review.json').read_text())
            self.assertTrue(report['status'].startswith('PASS:'))
            self.assertGreater(len(report['cases']), 20)


if __name__ == '__main__':
    unittest.main()
