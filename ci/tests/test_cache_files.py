import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import cache_files


class CacheFiles(unittest.TestCase):
    def fixture(self, parent):
        roots = [parent / 'objects', parent / 'bazel-out']
        for path in roots:
            path.mkdir(parents=True)
        payload = bytes(range(256)) * 32
        for index, path in enumerate((roots[0] / 'one.o', roots[1] / 'two.o', roots[1] / 'three.o')):
            path.write_bytes(payload)
            path.chmod(0o444)
            os.utime(path, ns=(1600000000000000000 + index, 1600000000000000123 + index * 101))
        return roots, parent / 'links.json'

    def test_real_tar_roundtrip_preserves_bytes_modes_and_nanosecond_timestamps_without_sharing_writes(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            before, after = base / 'before', base / 'after'
            roots, manifest = self.fixture(before)
            original = {str(p.relative_to(before)): (p.read_bytes(), p.stat().st_mtime_ns, stat.S_IMODE(p.stat().st_mode))
                        for root in roots for p in root.iterdir()}
            result = cache_files.deduplicate(before, roots, manifest)
            self.assertEqual(result['deduplicated_files'], 2)
            self.assertTrue(os.path.samefile(roots[0] / 'one.o', roots[1] / 'two.o'))
            archive = base / 'cache.tar'
            subprocess.run(['tar', '--posix', '-cf', str(archive), '-C', str(before),
                            'objects', 'bazel-out', 'links.json'], check=True, capture_output=True)
            after.mkdir()
            subprocess.run(['tar', '-xf', str(archive), '-C', str(after)], check=True, capture_output=True)
            result = cache_files.restore(after, [after / 'objects', after / 'bazel-out'], after / 'links.json', reserve_bytes=0)
            self.assertEqual(result['restored_independent_files'], 2)
            inodes = set()
            for name, (payload, mtime, mode) in original.items():
                path = after / name
                self.assertEqual(path.read_bytes(), payload)
                self.assertEqual(path.stat().st_mtime_ns, mtime)
                self.assertEqual(stat.S_IMODE(path.stat().st_mode), mode)
                inodes.add(path.stat().st_ino)
            self.assertEqual(len(inodes), 3)
            changed = after / 'bazel-out/two.o'
            changed.chmod(0o644)
            changed.write_bytes(b'new target content')
            self.assertEqual((after / 'objects/one.o').read_bytes(), original['objects/one.o'][0])
            self.assertEqual((after / 'bazel-out/three.o').read_bytes(), original['bazel-out/three.o'][0])
            self.assertFalse((after / 'links.json').exists())

    def test_other_bytes_modes_symlinks_and_existing_hardlinks_are_not_combined(self):
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary)
            roots, manifest = self.fixture(parent)
            other = roots[1] / 'other.o'
            other.write_bytes(b'X' * 8192)
            other.chmod(0o444)
            mode = roots[1] / 'different-mode.o'
            mode.write_bytes((roots[0] / 'one.o').read_bytes())
            mode.chmod(0o644)
            alias = roots[1] / 'symlink.o'
            alias.symlink_to(roots[0] / 'one.o')
            linked = roots[1] / 'existing.o'
            os.link(roots[1] / 'three.o', linked)
            outside = parent / 'outside'
            outside.mkdir()
            (outside / 'unrelated.o').write_bytes(other.read_bytes())
            (roots[0] / 'outside-link').symlink_to(outside, target_is_directory=True)
            result = cache_files.deduplicate(parent, roots, manifest)
            self.assertEqual(result['deduplicated_files'], 1)
            self.assertFalse(os.path.samefile(other, roots[0] / 'one.o'))
            self.assertFalse(os.path.samefile(mode, roots[0] / 'one.o'))
            self.assertTrue(os.path.samefile(linked, roots[1] / 'three.o'))
            self.assertEqual((outside / 'unrelated.o').stat().st_nlink, 1)
            self.assertTrue(alias.is_symlink())

    def test_bad_manifest_checksum_and_low_space_reject_before_expanding(self):
        for case in ('outside', 'duplicate', 'timestamp', 'checksum', 'space'):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as temporary:
                parent = Path(temporary)
                roots, manifest = self.fixture(parent)
                cache_files.deduplicate(parent, roots, manifest)
                record = json.loads(manifest.read_text())
                group = record['groups'][0]
                if case == 'outside':
                    group['files'][-1]['path'] = '../unrelated.o'
                elif case == 'duplicate':
                    group['files'][-1]['path'] = group['files'][0]['path']
                elif case == 'timestamp':
                    group['files'][-1]['mtime_ns'] = 'wrong'
                elif case == 'checksum':
                    group['sha256'] = '0' * 64
                manifest.write_text(json.dumps(record))
                with mock.patch('cache_files.shutil.disk_usage', return_value=SimpleNamespace(free=0 if case == 'space' else 2**40)):
                    with self.assertRaises(ValueError):
                        cache_files.restore(parent, roots, manifest)
                self.assertTrue(os.path.samefile(roots[0] / 'one.o', roots[1] / 'two.o'))
                self.assertTrue(os.path.samefile(roots[0] / 'one.o', roots[1] / 'three.o'))

    def test_readonly_output_directories_keep_their_permissions(self):
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary)
            roots, manifest = self.fixture(parent)
            roots[1].chmod(0o555)
            try:
                cache_files.deduplicate(parent, roots, manifest)
                self.assertEqual(stat.S_IMODE(roots[1].stat().st_mode), 0o555)
                cache_files.restore(parent, roots, manifest, reserve_bytes=0)
                self.assertEqual(stat.S_IMODE(roots[1].stat().st_mode), 0o555)
                self.assertFalse(os.path.samefile(roots[0] / 'one.o', roots[1] / 'two.o'))
            finally:
                roots[1].chmod(0o755)


if __name__ == '__main__':
    unittest.main()
