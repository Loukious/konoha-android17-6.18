import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import dispatch_build
import export_artifacts
import publish_release
import track_upstream
import verify_artifacts

SOURCE, KERNEL, KSU = 'a' * 40, 'b' * 40, 'c' * 40
RELEASE = '6.18.38-android17-5-maybe-dirty-4k'


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + '\n')


def fixture(root):
    source = root / 'source'
    workspace = root / 'workspace'
    artifact = workspace / 'artifacts/build'
    pins = {'common': {'revision': KERNEL, 'kernel': '6.18.38'}}
    catalog = [{'output': 'one.ko'}, {'output': 'two.ko'}]
    dt = {key: True for key in ('overlay_engines_agree', 'bootloader_ids_preserved', 'dtbo_table_verified')}
    ksu_data = b'archived KernelSU source'
    ksu = {'revision': KSU, 'sha256': hashlib.sha256(ksu_data).hexdigest()}
    write_json(source / 'port/sources.json', pins)
    write_json(source / 'port/modules.json', catalog)
    write_json(source / 'port/devicetrees.json', {'dtb_order': ['tuna.dtb', 'tuna7.dtb']})
    write_json(artifact / 'inputs/sources.json', pins)
    write_json(artifact / 'inputs/modules.json', catalog)
    write_json(artifact / 'inputs/kernelsu-next/source.json', ksu)
    write_json(artifact / 'devicetrees/report.json', {**dt, 'trees': {'tuna.dtb': {}, 'tuna7.dtb': {}}})
    for name, data in {
        'build.log': b'build and CRC checks passed', 'gki/Image': b'kernel Image',
        'gki/kernel_aarch64_Module.symvers': b'0x12345678 symbol module EXPORT_SYMBOL\n',
        'gki/kernel.release': (RELEASE + '\n').encode(),
        'gki/kernel_aarch64_dot_config': b'CONFIG_KSU=y\nCONFIG_MODVERSIONS=y\n',
        'platform/one.ko': b'one module', 'platform/two.ko': b'two module',
        'devicetrees/dtb.img': b'dtb', 'devicetrees/dtbo.img': b'dtbo',
        'devicetrees/checked/tuna.dtb': b'tuna', 'devicetrees/checked/tuna7.dtb': b'tuna7',
        'inputs/kernelsu-next/source.tar': ksu_data,
    }.items():
        path = artifact / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    report = {
        'kernel_release': RELEASE, 'qdss_diagnostic': False, 'kernelsu_next': ksu,
        'devicetree_verification': dt,
        'module_verification': [
            {'module': name, 'sha256': verify_artifacts.digest(artifact / 'platform' / name),
             'vermagic': RELEASE + ' SMP modversions aarch64', 'imports_checked': 5}
            for name in ('one.ko', 'two.ko')],
        'sha256': {p.relative_to(artifact).as_posix(): verify_artifacts.digest(p)
                   for p in artifact.rglob('*') if p.is_file()},
    }
    write_json(artifact / 'report.json', report)
    return source, workspace, artifact, report


class ArtifactTests(unittest.TestCase):
    def exercise(self, change=None):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source, workspace, artifact, report = fixture(root)
            if change:
                change(source, artifact, report)
                write_json(artifact / 'report.json', report)
            bundle = root / 'bundle'
            identity = export_artifacts.export(workspace, bundle, source, SOURCE, '123', '1')
            publish_release.validate_bundle(bundle, source, SOURCE, KERNEL, KSU, '123', '1')
            return identity

    def test_complete_bundle_round_trip_rechecks_build_and_abi(self):
        identity = self.exercise()
        self.assertEqual(identity['modules'], 2)
        self.assertEqual(identity['source_revision'], SOURCE)

    def test_same_count_wrong_module_set_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'inventory'):
            self.exercise(lambda s, a, r: r['module_verification'][1].update(module='wrong.ko'))

    def test_duplicate_module_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'duplicated'):
            self.exercise(lambda s, a, r: r['module_verification'].__setitem__(1, r['module_verification'][0]))

    def test_missing_hash_coverage_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'cover every'):
            self.exercise(lambda s, a, r: r['sha256'].pop('gki/Image'))

    def test_changed_module_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'changed'):
            self.exercise(lambda s, a, r: (a / 'platform/one.ko').write_bytes(b'changed'))

    def test_wrong_vermagic_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'vermagic'):
            self.exercise(lambda s, a, r: r['module_verification'][0].update(vermagic='6.6.144 SMP'))

    def test_missing_crc_evidence_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'import verification'):
            self.exercise(lambda s, a, r: r['module_verification'][0].update(imports_checked=None))

    def test_failed_device_tree_verification_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'Device-tree verification failed'):
            self.exercise(lambda s, a, r: r['devicetree_verification'].update(dtbo_table_verified=False))

    def test_source_pin_mismatch_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'source pins differ'):
            self.exercise(lambda s, a, r: write_json(s / 'port/sources.json', {'common': {'revision': 'd' * 40}}))

    def test_diagnostic_archive_cannot_be_released(self):
        with self.assertRaisesRegex(ValueError, 'Diagnostic'):
            self.exercise(lambda s, a, r: r.update(qdss_diagnostic=True))

    def test_exported_bundle_tampering_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source, workspace, artifact, report = fixture(root)
            bundle = root / 'bundle'
            export_artifacts.export(workspace, bundle, source, SOURCE, '123', '1')
            (bundle / 'kernel-abi/Module.symvers').write_bytes(b'other ABI')
            with self.assertRaisesRegex(ValueError, 'asset changed'):
                publish_release.validate_bundle(bundle, source, SOURCE, KERNEL, KSU, '123', '1')
            with self.assertRaisesRegex(ValueError, 'different build inputs'):
                publish_release.validate_bundle(bundle, source, 'd' * 40, KERNEL, KSU, '123', '1')

    def test_extra_build_cannot_be_selected_by_timestamp(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source, workspace, artifact, report = fixture(root)
            write_json(workspace / 'artifacts/older/report.json', report)
            with self.assertRaisesRegex(ValueError, 'one complete build'):
                export_artifacts.export(workspace, root / 'bundle', source, SOURCE, '123', '1')

    def test_unsafe_archive_member_is_rejected_even_with_updated_bundle_hashes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source, workspace, artifact, report = fixture(root)
            bundle = root / 'bundle'
            identity = export_artifacts.export(workspace, bundle, source, SOURCE, '123', '1')
            archive_path = bundle / 'konoha-gki-and-modules.tar.gz'
            with tarfile.open(archive_path, 'w:gz') as archive:
                entry = tarfile.TarInfo('../escaped')
                entry.size = 3
                archive.addfile(entry, io.BytesIO(b'bad'))
            identity['sha256']['konoha-gki-and-modules.tar.gz'] = verify_artifacts.digest(archive_path)
            write_json(bundle / 'release.json', identity)
            hashes = {p.relative_to(bundle).as_posix(): verify_artifacts.digest(p)
                      for p in bundle.rglob('*') if p.is_file() and p.name != 'SHA256SUMS'}
            (bundle / 'SHA256SUMS').write_text(''.join(f'{checksum}  {name}\n' for name, checksum in hashes.items()))
            with self.assertRaisesRegex(ValueError, 'Unsafe artifact path'):
                publish_release.validate_bundle(bundle, source, SOURCE, KERNEL, KSU, '123', '1')


class ReleaseTests(unittest.TestCase):
    def exercise(self, failed_build=False, stale_source=False, upload_failure=False, incomplete=False, existing=False):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source, workspace, artifact, report = fixture(root)
            bundle = root / 'bundle'
            export_artifacts.export(workspace, bundle, source, SOURCE, '123', '1')
            paths = [p for p in bundle.rglob('*') if p.is_file()]
            assets = [{'name': p.name, 'size': p.stat().st_size,
                       'digest': 'sha256:' + verify_artifacts.digest(p)} for p in paths]
            commands = []
            def api(path, optional=False):
                if path.endswith('/attempts/1'):
                    return {'head_branch': 'ci', 'path': '.github/workflows/build.yml'}
                if path.endswith('/jobs'):
                    return {'jobs': [{'name': 'build', 'conclusion': 'failure' if failed_build else 'success'}]}
                if '/git/ref/heads/' in path:
                    return {'object': {'sha': 'd' * 40 if stale_source else SOURCE}}
                if '/releases/tags/' in path:
                    if optional and not existing:
                        return None
                    return {'draft': not existing, 'target_commitish': SOURCE, 'body': SOURCE + KERNEL + KSU,
                            'assets': assets[:-1] if incomplete else assets, 'html_url': 'release-url'}
                raise AssertionError(path)
            def run(args):
                commands.append(args)
                if upload_failure and args[:3] == ['gh', 'release', 'upload']:
                    raise subprocess.CalledProcessError(1, args)
                return ''
            with patch.object(publish_release, 'api', side_effect=api), patch.object(publish_release, 'run', side_effect=run):
                try:
                    result = publish_release.publish('owner/repo', bundle, source, SOURCE, KERNEL, KSU, '123', '1')
                except (ValueError, subprocess.CalledProcessError) as error:
                    result = {'error': str(error)}
            return result, commands

    def test_release_stays_draft_until_complete_upload_verified(self):
        result, commands = self.exercise()
        self.assertEqual([args[2] for args in commands], ['create', 'upload', 'edit'])
        self.assertIn('--draft', commands[0])
        self.assertIn('--target', commands[0])
        self.assertIn(SOURCE, commands[0])
        self.assertIn('--draft=false', commands[-1])
        self.assertEqual(result['status'], 'verified build released')

    def test_failed_build_cannot_create_release(self):
        result, commands = self.exercise(failed_build=True)
        self.assertIn('have not succeeded', result['error'])
        self.assertEqual(commands, [])

    def test_stale_source_cannot_create_release(self):
        result, commands = self.exercise(stale_source=True)
        self.assertIn('not promoted', result['error'])
        self.assertEqual(commands, [])

    def test_upload_failure_does_not_publish_draft(self):
        result, commands = self.exercise(upload_failure=True)
        self.assertIn('error', result)
        self.assertFalse(any(args[2] == 'edit' for args in commands))

    def test_incomplete_upload_does_not_publish_draft(self):
        result, commands = self.exercise(incomplete=True)
        self.assertIn('incomplete', result['error'])
        self.assertFalse(any(args[2] == 'edit' for args in commands))

    def test_existing_complete_release_is_not_overwritten(self):
        result, commands = self.exercise(existing=True)
        self.assertEqual(result['status'], 'verified inputs already released')
        self.assertEqual(commands, [])


class DispatchTests(unittest.TestCase):
    def test_retry_policy_handles_unchanged_inputs_and_failures(self):
        build = lambda status, conclusion: {'status': status, 'conclusion': conclusion}
        self.assertTrue(dispatch_build.should_dispatch([]))
        self.assertFalse(dispatch_build.should_dispatch([build('in_progress', None)]))
        self.assertFalse(dispatch_build.should_dispatch([build('queued', None)]))
        self.assertFalse(dispatch_build.should_dispatch([build('completed', 'success')]))
        self.assertTrue(dispatch_build.should_dispatch([build('completed', 'failure')]))
        self.assertTrue(dispatch_build.should_dispatch([build('completed', 'cancelled')] * 2))
        self.assertFalse(dispatch_build.should_dispatch([build('completed', 'failure')] * 3))

    def test_dispatch_binds_source_and_identifies_kernelsu(self):
        with patch.object(subprocess, 'check_output', return_value='[]'), patch.object(subprocess, 'run') as run:
            dispatch_build.dispatch(SOURCE, KSU)
        self.assertIn('source_revision=' + SOURCE, run.call_args.args[0])
        self.assertIn('kernelsu_revision=' + KSU, run.call_args.args[0])


class RealGitDetectionTests(unittest.TestCase):
    def git(self, repo, *args):
        return subprocess.check_output(['git', '-C', str(repo), *args], text=True).strip()

    def create(self, root, name, branch):
        repo = root / name
        repo.mkdir()
        self.git(repo, 'init', '-q', '-b', branch)
        self.git(repo, 'config', 'user.name', 'CI fixture')
        self.git(repo, 'config', 'user.email', 'ci@example.invalid')
        self.commit(repo, 'initial')
        return repo

    def commit(self, repo, text):
        (repo / 'fixture.txt').write_text(text)
        self.git(repo, 'add', '.')
        self.git(repo, 'commit', '-qm', text)
        return self.git(repo, 'rev-parse', 'HEAD')

    def exercise(self, scenario):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            kernel = self.create(root, 'kernel', 'android17-6.18-lts')
            ksu = self.create(root, 'ksu', 'dev')
            source = self.create(root, 'source', 'android17-6.18')
            current = self.git(kernel, 'rev-parse', 'HEAD')
            write_json(source / 'port/sources.json', {'common': {'revision': current}})
            accepted = self.commit(source, 'pinned kernel')
            last = {'common_revision': current, 'kernelsu_revision': self.git(ksu, 'rev-parse', 'HEAD'),
                    'source_revision': accepted, 'release_tag': 'previous-release'}
            if scenario == 'kernel': self.commit(kernel, 'upstream GKI bump')
            if scenario == 'ksu': self.commit(ksu, 'KernelSU dev bump')
            if scenario == 'source': self.commit(source, 'module source change')
            if scenario == 'missing-release': last.pop('release_tag')
            if scenario == 'failure': self.git(ksu, 'branch', '-m', 'renamed')
            write_json(root / 'ci/last-built.json', last)
            before = (root / 'ci/last-built.json').read_bytes()
            tracking = {'repository': str(kernel), 'branch': 'android17-6.18-lts',
                        'kernelsu_repository': str(ksu), 'kernelsu_branch': 'dev'}
            with patch.object(track_upstream, 'ROOT', root), patch.object(track_upstream, 'PORT', source / 'port'), \
                    patch.object(track_upstream, 'TRACK', tracking), patch.dict(os.environ, {}, clear=True):
                if scenario == 'failure':
                    with self.assertRaises(subprocess.CalledProcessError): track_upstream.detect()
                    result = None
                else: result = track_upstream.detect()
            self.assertEqual((root / 'ci/last-built.json').read_bytes(), before)
            return result

    def test_real_unchanged_remote_refs_do_not_trigger_build(self):
        result = self.exercise('unchanged')
        self.assertFalse(any(result[key] for key in ('kernel_changed', 'kernelsu_changed', 'source_changed', 'release_needed')))

    def test_real_gki_update_is_detected(self):
        self.assertTrue(self.exercise('kernel')['kernel_changed'])

    def test_real_kernelsu_update_is_detected(self):
        result = self.exercise('ksu')
        self.assertTrue(result['kernelsu_changed'])
        self.assertFalse(result['kernel_changed'])

    def test_real_source_update_is_detected(self):
        self.assertTrue(self.exercise('source')['source_changed'])

    def test_existing_build_state_without_release_triggers_migration_build(self):
        self.assertTrue(self.exercise('missing-release')['release_needed'])

    def test_failed_remote_lookup_does_not_record_success(self):
        self.assertIsNone(self.exercise('failure'))


if __name__ == '__main__':
    unittest.main()
