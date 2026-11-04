#!/usr/bin/env python3
"""Check raw CPIO preservation and adversarial archive/ZIP handling."""
import hashlib
import io
from pathlib import Path
import stat
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile
import json

import generate as g


class Tests(unittest.TestCase):
    def test_softdep_groups_reset_for_each_record(self):
        words, ignored = g.softdep_words('ecb\npre: one post: two\nnls\npre: aes\nraw pre: three\n')
        self.assertEqual(words, ['one', 'two', 'aes', 'three'])
        self.assertEqual(ignored, ['ecb', 'nls', 'raw'])

    def test_nonroot_erofs_is_rejected(self):
        with tempfile.TemporaryDirectory() as path:
            root = Path(path)
            image = root / 'baseline.img'
            image.write_bytes(b'fixture')
            tree = root / 'tree'
            tree.mkdir()
            (tree / 'nonroot-file').write_bytes(b'fixture')
            builder = g.Builder(root, root)
            def output(command):
                return b'Uid: 1000   Gid: 0 Access: 0644\n' if '--path=/nonroot-file' in command else b'Uid: 0 Gid: 0 Access: 0755\n'
            with patch.object(builder, 'tool', return_value='dump.erofs'), patch.object(builder, 'run', side_effect=output):
                with self.assertRaisesRegex(ValueError, 'Only root-owned'):
                    builder.check_ownership(image, tree, 'vendor_dlkm')

    def test_new_native_and_platform_modules_keep_system_placement(self):
        # Orchestration test: ELF/CRC validation itself is covered by the real
        # release run; fake metadata makes the new517-vs-old471 choice explicit.
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            artifact, layout = root / 'artifact', root / 'layout'
            (artifact / 'gki').mkdir(parents=True)
            (artifact / 'platform').mkdir()
            (artifact / 'inputs').mkdir()
            release = '6.18.38-android17-5-maybe-dirty-4k'
            for name in ['modules.builtin', 'modules.builtin.modinfo', 'kernel_aarch64_Module.symvers']:
                (artifact / 'gki' / name).write_text('')
            (artifact / 'gki/kernel.release').write_text(release)
            platform = [f'platform{i}' for i in range(391)]
            generic = [f'generic{i}' for i in range(80)]
            new_platform = [f'wireless{i}' for i in range(9)]
            new_native = [f'newnative{i}' for i in range(37)]
            catalog = root / 'fake-catalog'
            catalog.mkdir()
            paths = {}
            for name in platform + generic + new_platform + new_native:
                paths[name] = catalog / (name + '.ko')
                paths[name].write_bytes(name.encode())
            kinds = {n: 'platform' if n in platform + new_platform else 'gki' for n in paths}
            (artifact / 'inputs/nethunter-native-modules.json').write_text(json.dumps({'modules': new_native, 'required_existing_modules': [generic[0]]}))
            (artifact / 'report.json').write_text(json.dumps({'sha256': {'inputs/nethunter-native-modules.json': 'bound-by-outer-archive-test'}}))
            trees = {}
            selections = {'vendor_ramdisk': platform + generic[:4], 'vendor_dlkm': platform + generic[:4], 'system_dlkm': generic[2:]}
            for part, selection in selections.items():
                original = root / ('original-' + part)
                directory = original / 'lib/modules'
                directory.mkdir(parents=True)
                target = layout / part / 'lib/modules'
                target.mkdir(parents=True)
                trees[part] = original
                for name in selection:
                    for dest in [directory, target]:
                        (dest / (name + '.ko')).write_bytes(name.encode())
                (directory / 'modules.load').write_text(selection[0] + '.ko\n')
                if part == 'vendor_ramdisk': (directory / 'modules.load.recovery').write_text(selection[0] + '.ko\n')
            (layout / 'prebuilt.json').write_text(json.dumps({'device': 'onyx', 'kernel_release': '6.18.37-old-layout'}))
            (layout / 'SHA256SUMS').write_text(''.join(g.sha(p) + '  ' + str(p.relative_to(layout)) + '\n' for p in layout.rglob('*') if p.is_file()))
            output = root / 'output'
            output.mkdir()
            builder = g.Builder(output, root)
            def run(command):
                if command[0] == 'depmod':
                    part = Path(command[command.index('-b') + 1]) / 'lib/modules' / release
                    (part / 'modules.dep').write_text('')
                return b''
            with patch.object(g, 'artifact_catalog', return_value=(paths, kinds, set())), patch.object(g, 'verify', return_value=[]), patch.object(builder, 'run', side_effect=run):
                reports, _ = g.assemble_modules(builder, artifact, trees,
                    {'module_layout': {'source': str(layout), 'sha256': g.sha(layout / 'SHA256SUMS')}})
            self.assertEqual(reports['system_dlkm']['modules'], 78)
            self.assertEqual(reports['vendor_ramdisk']['modules'], 404)
            self.assertEqual(reports['vendor_dlkm']['modules'], 441)
            autoload = set(reports['vendor_dlkm']['load_lists']['modules.load'])
            self.assertTrue(set(new_native + new_platform) <= autoload)
            unique = {p.name for part in trees.values() for p in part.glob('*.ko')}
            self.assertEqual(len(unique), 517)

    def test_raw_metadata_preserved(self):
        with tempfile.TemporaryDirectory() as path:
            modules = Path(path)
            (modules / 'new.ko').write_bytes(b'NEW')
            (modules / 'new.ko').chmod(0o644)
            records = [('lib', b'', stat.S_IFDIR | 0o751),
                       ('firmware-link', b'vendor/firmware', stat.S_IFLNK | 0o777),
                       ('init.rc', b'original-init', stat.S_IFREG | 0o641),
                       ('lib/modules', b'', stat.S_IFDIR | 0o755),
                       ('lib/modules/old.ko', b'OLD', stat.S_IFREG | 0o644)]
            raw = b''
            for inode, (name, payload, mode) in enumerate(records, 1):
                entry = g.cpio_record(name, payload, mode, inode)
                if name == 'init.rc':
                    fields = [int(entry[6 + i * 8:14 + i * 8], 16) for i in range(13)]
                    fields[2:6] = [1000, 1007, 3, 12345678]
                    fields[7:11] = [7, 9, 11, 13]
                    entry = b'070701' + ''.join(f'{n:08x}' for n in fields).encode() + entry[110:]
                raw += entry
            raw += g.cpio_record('TRAILER!!!', b'', 0, 0)
            before = g.cpio_entries(raw)
            after = g.cpio_entries(g.rebuild_cpio(before, modules))
            self.assertEqual({n: v[0] for n, v in before.items() if g.nonmodules(n)},
                             {n: v[0] for n, v in after.items() if g.nonmodules(n)})
            self.assertNotIn('lib/modules/old.ko', after)
            self.assertEqual(after['lib/modules/new.ko'][1], b'NEW')

    def test_cpio_bounds_and_duplicates(self):
        trailer = g.cpio_record('TRAILER!!!', b'', 0, 0)
        record = g.cpio_record('x', b'payload', stat.S_IFREG | 0o644, 1)
        for raw in [record[:117], record + record + trailer,
                    g.cpio_record('../escape', b'', 0, 1) + trailer, record + trailer + b'bad']:
            with self.assertRaises(ValueError):
                g.cpio_entries(raw)

    def test_archive_traversal_link_duplicates(self):
        for case in ['traversal', 'link', 'duplicate']:
            with self.subTest(case=case), tempfile.TemporaryDirectory() as path:
                root = Path(path)
                archive = root / 'bad.tar.gz'
                with tarfile.open(archive, 'w:gz') as tar:
                    info = tarfile.TarInfo('../escape' if case == 'traversal' else 'root/x')
                    if case == 'link':
                        info.type = tarfile.SYMTYPE
                        info.linkname = '/etc/passwd'
                    tar.addfile(info, io.BytesIO(b''))
                    if case == 'duplicate':tar.addfile(info, io.BytesIO(b''))
                with self.assertRaises(ValueError):
                    g.extract_release(archive, root / 'out', g.sha(archive))
                self.assertFalse((root / 'escape').exists())

    def test_zip_readback_and_modes(self):
        with tempfile.TemporaryDirectory() as path:
            root = Path(path)
            stage = root / 'stage'
            stage.mkdir()
            p = stage / 'install.sh'
            p.write_text('#!/sbin/sh\n')
            p.chmod(0o755)
            g.make_zip(stage, root / 'test.zip')
            with zipfile.ZipFile(root / 'test.zip') as archive:
                self.assertEqual(archive.read('install.sh'), p.read_bytes())
                self.assertEqual((archive.getinfo('install.sh').external_attr >> 16) & 0o777, 0o755)
                self.assertIn(g.sha(p), archive.read('SHA256SUMS').decode())


if __name__ == '__main__':
    unittest.main()
