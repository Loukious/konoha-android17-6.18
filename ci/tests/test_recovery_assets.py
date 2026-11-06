import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import export_artifacts
import publish_release
import recovery_assets
import test_release


class CompleteRecoveryAssets(unittest.TestCase):
    def fixture(self, root):
        source, workspace, artifact, report = test_release.fixture(root)
        image = bytearray(128)
        image[56:60] = b'ARM\x64'
        (artifact / 'gki/Image').write_bytes(image)
        (artifact / 'gki/modules.builtin').write_text('kernel/compiled-in.ko\n')
        (artifact / 'gki/modules.builtin.modinfo').write_bytes(b'compiled-in.alias=fixture_builtin\0')
        with tarfile.open(artifact / 'gki/modules_staging_dir.tar.gz', 'w:gz') as archive:
            data = b'exact native feature module'
            member = tarfile.TarInfo('lib/modules/' + test_release.RELEASE + '/kernel/native.ko')
            member.size = len(data)
            archive.addfile(member, io.BytesIO(data))
        test_release.write_json(artifact / 'inputs/nethunter-native-modules.json', {'modules': ['native.ko'], 'wireless_firmware': ['test.bin']})
        (artifact / 'inputs/boot-parameters.txt').write_text('android_arch_task_struct_size=512 sched_proxy_exec=0\n')
        (artifact / 'firmware/firmware').mkdir(parents=True)
        (artifact / 'firmware/firmware/test.bin').write_bytes(b'wireless firmware')
        test_release.write_json(artifact / 'firmware/manifest.json', {'blobs': [
            {'path': 'test.bin', 'sha256': recovery_assets.digest(b'wireless firmware')}]})
        report['sha256'] = {p.relative_to(artifact).as_posix(): recovery_assets.digest(p.read_bytes())
                            for p in artifact.rglob('*') if p.is_file() and p != artifact / 'report.json'}
        test_release.write_json(artifact / 'report.json', report)
        bundle = root / 'bundle'
        identity = export_artifacts.export(workspace, bundle, source, test_release.SOURCE, '123', '1')
        return source, artifact, bundle, identity

    def metadata(self, path):
        if path == recovery_assets.HERE / 'legacy-onyx.json':
            return {'format': 1, 'device': 'onyx', 'legacy_release_prefix': '6.6.',
                    'legacy_module_names': ['retired'], 'default_loads': {'vendor_dlkm': {'modules.load': ['one']}}}
        return json.loads(path.read_text())

    def module_info(self, path):
        return {'name': [path.stem], 'vermagic': [test_release.RELEASE + ' SMP modversions aarch64'],
                'depends': ['two' if path.stem == 'one' else ''],
                'softdep': ['pre: compiled_in'], 'alias': ['fixture:' + path.stem]}

    def test_complete_archive_payload_and_inventory(self):
        with tempfile.TemporaryDirectory() as temporary, \
                patch.object(recovery_assets, 'read', side_effect=self.metadata), \
                patch.object(recovery_assets, 'modinfo', side_effect=self.module_info):
            source, artifact, bundle, identity = self.fixture(Path(temporary))
            products = recovery_assets.build(artifact, identity)
            zip_name, meta_name = recovery_assets.names(identity['kernel_release'])
            self.assertEqual(set(products), {zip_name, meta_name})
            with zipfile.ZipFile(io.BytesIO(products[zip_name])) as archive:
                self.assertEqual(archive.read('payload/Image'), (artifact / 'gki/Image').read_bytes())
                self.assertEqual(archive.read('payload/modules/one.ko'), b'one module')
                self.assertEqual(archive.read('payload/modules/native.ko'), b'exact native feature module')
                self.assertEqual(archive.read('payload/firmware/test.bin'), b'wireless firmware')
                metadata = json.loads(archive.read('kernel.json'))
                self.assertEqual(metadata['modules']['one']['depends'], ['two'])
                self.assertEqual(metadata['native_additions'], ['native'])
                sums = dict(line.split('  ', 1)[::-1] for line in archive.read('SHA256SUMS').decode().splitlines())
                self.assertEqual(set(sums), set(archive.namelist()) - {'SHA256SUMS'})
                for name, checksum in sums.items():
                    self.assertEqual(recovery_assets.digest(archive.read(name)), checksum)
                for name in ('tools/onyx-recovery', 'tools/recovery-guard', 'install.sh', 'META-INF/com/google/android/update-binary'):
                    self.assertTrue(archive.getinfo(name).external_attr >> 16 & 0o111)
            meta = json.loads(products[meta_name])
            self.assertEqual(meta['modules'], 3)
            self.assertFalse(meta['device_tested'])
            self.assertEqual(len(meta['partitions']), 6)

    def test_complete_bundle_roundtrip_rejects_rehashed_zip_tampering(self):
        with tempfile.TemporaryDirectory() as temporary, \
                patch.object(recovery_assets, 'read', side_effect=self.metadata), \
                patch.object(recovery_assets, 'modinfo', side_effect=self.module_info):
            source, artifact, bundle, identity = self.fixture(Path(temporary))
            result = recovery_assets.add_to_bundle(bundle, source)
            self.assertEqual(set(result['full_recovery']), set(recovery_assets.names(identity['kernel_release'])))
            self.assertEqual(recovery_assets.add_to_bundle(bundle, source), result)
            filename = recovery_assets.names(identity['kernel_release'])[0]
            (bundle / filename).write_bytes(b'changed installer')
            result['sha256'][filename] = recovery_assets.digest(b'changed installer')
            test_release.write_json(bundle / 'release.json', result)
            (bundle / 'SHA256SUMS').write_text(''.join(
                recovery_assets.digest(path.read_bytes()) + '  ' + path.relative_to(bundle).as_posix() + '\n'
                for path in sorted(bundle.rglob('*')) if path.is_file() and path.name != 'SHA256SUMS'))
            with self.assertRaisesRegex(ValueError, 'Complete recovery installer differs'):
                publish_release.validate_bundle(bundle, source, test_release.SOURCE, test_release.KERNEL, test_release.KSU, '123', '1')

    def test_missing_or_duplicate_complete_asset_inventory_is_rejected(self):
        names = recovery_assets.names(test_release.RELEASE)
        for entries in (names[:1], names + names[:1], ['unexpected.zip']):
            with self.subTest(entries=entries), self.assertRaisesRegex(ValueError, 'recovery asset'):
                publish_release.bundle_files({'kernel_release': test_release.RELEASE, 'full_recovery': entries})

    def test_missing_dependency_and_changed_payload_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary, \
                patch.object(recovery_assets, 'read', side_effect=self.metadata), \
                patch.object(recovery_assets, 'modinfo', side_effect=self.module_info):
            source, artifact, bundle, identity = self.fixture(Path(temporary))
            def bad_info(path):
                info = self.module_info(path)
                info['depends'] = ['not_available']
                return info
            with patch.object(recovery_assets, 'modinfo', side_effect=bad_info), self.assertRaisesRegex(ValueError, 'Missing hard dependency'):
                recovery_assets.payload_files(artifact, identity)
            (artifact / 'devicetrees/dtb.img').write_bytes(b'changed dtb')
            with self.assertRaisesRegex(ValueError, 'Release input differs'):
                recovery_assets.payload_files(artifact, identity)

    def test_pinned_sources_and_static_arm64_tools_are_complete(self):
        files, provenance = recovery_assets.tooling_files()
        self.assertIn('installer-source/engine/prepare.go', files)
        self.assertIn('installer-source/busybox-notices/sources/busybox-1.36.1.tar.bz2', files)
        self.assertEqual(provenance['go_version'], 'go1.25.4')
        self.assertTrue(files['tools/onyx-recovery'][1])


if __name__ == '__main__':
    unittest.main()
