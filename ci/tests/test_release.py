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
import recover_release
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

    def test_full_rerun_produces_distinct_artifact_name_and_attempt_outputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source, workspace, artifact, report = fixture(root)
            outputs = root / 'outputs'
            with patch.dict(os.environ, {'GITHUB_OUTPUT': str(outputs)}):
                first = export_artifacts.export(workspace, root / 'bundle1', source, SOURCE, '123', '1')
                second = export_artifacts.export(workspace, root / 'bundle2', source, SOURCE, '123', '2')
            self.assertNotEqual(first['artifact_name'], second['artifact_name'])
            self.assertIn('artifact_name=Konoha-Android17-6.18-123-1', outputs.read_text())
            self.assertIn('artifact_name=Konoha-Android17-6.18-123-2', outputs.read_text())
            self.assertIn('build_attempt=1', outputs.read_text())
            self.assertIn('build_attempt=2', outputs.read_text())

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
    def exercise(self, failed_build=False, stale_source=False, upload_failure=False, incomplete=False, existing=False,
                 advance_during_upload=False, hidden_draft=False, draft_exists=False, later_page=False,
                 wrong_draft=False, drift_after_upload=False, wrong_digest=False, api_calls=None,
                 hidden_listing=False, bad_created_id=False, partial_upload=False, duplicate_uploaded=False,
                 published_during_upload=False):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source, workspace, artifact, report = fixture(root)
            bundle = root / 'bundle'
            export_artifacts.export(workspace, bundle, source, SOURCE, '123', '1')
            paths = sorted(p for p in bundle.rglob('*') if p.is_file())
            assets = {p.name: {'id': 100 + i, 'name': p.name, 'size': p.stat().st_size, 'state': 'uploaded',
                              'digest': 'sha256:' + verify_artifacts.digest(p)} for i, p in enumerate(paths)}
            stored = {name: dict(asset) for name, asset in assets.items()} if existing else {}
            if partial_upload:
                first, second = list(assets)[:2]
                stored[first] = dict(assets[first])
                stored[second] = {**assets[second], 'state': 'starter', 'size': 0, 'digest': None}
            commands = []
            head_reads = 0
            created, uploaded, published = draft_exists, False, existing
            tag = f'gki-6.18.38-s{SOURCE[:12]}-k{KSU[:12]}'
            release_path = 'repos/owner/repo/releases/77'

            def release():
                inventory = [dict(asset) for asset in stored.values()]
                if incomplete and uploaded:
                    inventory = inventory[:-1]
                if duplicate_uploaded and uploaded:
                    inventory.append(dict(inventory[0]))
                if wrong_digest and uploaded:
                    inventory[0]['digest'] = 'sha256:' + '0' * 64
                return {'id': 77, 'tag_name': tag, 'draft': not (published or (published_during_upload and uploaded)),
                        'target_commitish': 'd' * 40 if wrong_draft or (drift_after_upload and uploaded) else SOURCE,
                        'body': SOURCE + KERNEL + KSU, 'assets': inventory, 'html_url': 'release-url'}

            def api(path, optional=False, *, method='GET', data=None, input_file=None):
                nonlocal head_reads, created, uploaded, published
                if api_calls is not None:
                    api_calls.append((method, path))
                if method != 'GET':
                    commands.append({'method': method, 'path': path, 'data': data,
                                     'filename': input_file.name if input_file else None})
                if method == 'POST' and path == 'repos/owner/repo/releases':
                    self.assertFalse(created)
                    self.assertEqual(data['tag_name'], tag)
                    self.assertEqual(data['target_commitish'], SOURCE)
                    self.assertIs(data['draft'], True)
                    self.assertTrue(all(value in data['body'] for value in (SOURCE, KERNEL, KSU)))
                    created = True
                    result = release()
                    if bad_created_id:
                        result['id'] = True
                    return result
                if method == 'POST' and path.startswith('https://uploads.github.com/' + release_path + '/assets?name='):
                    self.assertTrue(created)
                    self.assertFalse(published)
                    self.assertTrue(path.endswith(input_file.name))
                    if upload_failure:
                        raise RuntimeError('Upload failed')
                    self.assertNotIn(input_file.name, stored)
                    uploaded = True
                    stored[input_file.name] = dict(assets[input_file.name])
                    return dict(stored[input_file.name])
                if method == 'DELETE' and path.startswith('repos/owner/repo/releases/assets/'):
                    asset_id = int(path.rsplit('/', 1)[1])
                    matches = [name for name, item in stored.items() if item['id'] == asset_id]
                    self.assertEqual(len(matches), 1)
                    del stored[matches[0]]
                    return None
                if method == 'PATCH' and path == release_path:
                    self.assertFalse(published)
                    self.assertEqual(stored, assets)
                    self.assertIs(data['draft'], False)
                    self.assertEqual(data['make_latest'], 'true')
                    self.assertTrue(all(value in data['body'] for value in (SOURCE, KERNEL, KSU)))
                    published = True
                    return release()
                self.assertEqual(method, 'GET', 'Unexpected release mutation: ' + path)
                if path.endswith('/attempts/1'):
                    return {'head_branch': 'ci', 'path': '.github/workflows/build.yml'}
                if path.endswith('/jobs'):
                    return {'jobs': [{'name': 'build', 'conclusion': 'failure' if failed_build else 'success'}]}
                if '/git/ref/heads/' in path:
                    head_reads += 1
                    changed = stale_source or (advance_during_upload and head_reads > 1)
                    return {'object': {'sha': 'd' * 40 if changed else SOURCE}}
                if '/releases/tags/' in path:
                    if hidden_draft:
                        self.assertTrue(optional)
                        return None
                    return release() if created or existing else None
                if '/releases?per_page=100&page=' in path:
                    if hidden_listing:
                        return []
                    if later_page and path.endswith('page=1'):
                        return [{'id': 1000 + i, 'tag_name': 'unrelated-' + str(i)} for i in range(100)]
                    return [release()] if created else []
                if path == release_path:
                    self.assertTrue(created or existing)
                    return release()
                raise AssertionError(path)

            with patch.object(publish_release, 'api', side_effect=api), \
                    patch.object(subprocess, 'check_output', side_effect=AssertionError('Unexpected tag-based CLI call')):
                try:
                    result = publish_release.publish('owner/repo', bundle, source, SOURCE, KERNEL, KSU, '123', '1')
                except (ValueError, RuntimeError) as error:
                    result = {'error': str(error)}
            return result, commands

    def test_release_stays_draft_until_complete_upload_verified(self):
        result, commands = self.exercise()
        self.assertEqual(result['status'], 'verified build released')
        self.assertEqual(commands[0]['path'], 'repos/owner/repo/releases')
        self.assertIs(commands[0]['data']['draft'], True)
        self.assertEqual(len([c for c in commands if c['filename']]), len(publish_release.FILES) + 2)
        self.assertEqual(commands[-1]['method'], 'PATCH')
        self.assertEqual(commands[-1]['path'], 'repos/owner/repo/releases/77')
        self.assertIs(commands[-1]['data']['draft'], False)

    def test_failed_build_cannot_create_release(self):
        result, commands = self.exercise(failed_build=True)
        self.assertIn('have not succeeded', result['error'])
        self.assertEqual(commands, [])

    def test_stale_source_cannot_create_release(self):
        result, commands = self.exercise(stale_source=True)
        self.assertIn('not promoted', result['error'])
        self.assertEqual(commands, [])

    def test_head_advance_during_upload_cannot_publish_an_older_latest_release(self):
        result, commands = self.exercise(advance_during_upload=True)
        self.assertIn('advanced during upload', result['error'])
        self.assertTrue(any(c['filename'] for c in commands))
        self.assertFalse(any(c['method'] == 'PATCH' for c in commands))

    def test_release_only_rerun_uses_original_verified_build_attempt(self):
        environment = {'GITHUB_REPOSITORY': 'owner/repo', 'KONOHA_SOURCE_ROOT': '/fixture/source',
                       'GITHUB_RUN_ID': '123', 'GITHUB_RUN_ATTEMPT': '2', 'BUILD_RUN_ATTEMPT': '1'}
        with patch.dict(os.environ, environment, clear=True), \
                patch.object(sys, 'argv', ['publish_release.py', 'bundle', SOURCE, KERNEL, KSU]), \
                patch.object(publish_release, 'publish', return_value={'tag': 'verified-tag'}) as publish, \
                patch('builtins.print'):
            publish_release.main()
        self.assertEqual(publish.call_args.args[-2:], ('123', '1'))

    def test_attempt_one_bundle_releases_while_current_run_attempt_is_two(self):
        with patch.dict(os.environ, {'GITHUB_RUN_ATTEMPT': '2', 'BUILD_RUN_ATTEMPT': '1'}):
            result, commands = self.exercise()
        self.assertEqual(result['status'], 'verified build released')
        self.assertIs(commands[-1]['data']['draft'], False)

    def test_upload_failure_does_not_publish_draft(self):
        result, commands = self.exercise(upload_failure=True)
        self.assertIn('Upload failed', result['error'])
        self.assertFalse(any(c['method'] == 'PATCH' for c in commands))

    def test_incomplete_or_duplicate_upload_does_not_publish_draft(self):
        for flags in ({'incomplete': True}, {'duplicate_uploaded': True}):
            with self.subTest(flags=flags):
                result, commands = self.exercise(**flags)
                self.assertIn('incomplete', result['error'])
                self.assertFalse(any(c['method'] == 'PATCH' for c in commands))

    def test_existing_complete_release_is_not_overwritten(self):
        result, commands = self.exercise(existing=True)
        self.assertEqual(result['status'], 'verified inputs already released')
        self.assertEqual(commands, [])

    def test_creation_response_id_works_when_tag_and_listing_never_expose_new_draft(self):
        calls = []
        result, commands = self.exercise(hidden_draft=True, hidden_listing=True, api_calls=calls)
        self.assertEqual(result['status'], 'verified build released')
        self.assertEqual(sum(c['path'] == 'repos/owner/repo/releases' for c in commands), 1)
        after_create = calls[calls.index(('POST', 'repos/owner/repo/releases')) + 1:]
        self.assertFalse(any('/releases/tags/' in path or '/releases?' in path for _, path in after_create))
        self.assertEqual(sum(method == 'GET' and path.endswith('/releases/77')
                             for method, path in after_create), 2)
        self.assertTrue(all('/releases/77/assets?' in c['path'] for c in commands if c['filename']))
        self.assertEqual(commands[-1]['path'], 'repos/owner/repo/releases/77')

    def test_existing_hidden_draft_retry_uses_paginated_inventory_without_recreate(self):
        calls = []
        result, commands = self.exercise(hidden_draft=True, draft_exists=True, later_page=True, api_calls=calls)
        self.assertEqual(result['status'], 'verified build released')
        self.assertFalse(any(c['path'] == 'repos/owner/repo/releases' for c in commands))
        self.assertTrue(any('/releases?per_page=100&page=2' in path for _, path in calls))

    def test_partial_upload_retry_keeps_verified_asset_and_replaces_only_failed_asset(self):
        result, commands = self.exercise(draft_exists=True, partial_upload=True)
        self.assertEqual(result['status'], 'verified build released')
        self.assertEqual([c['path'] for c in commands if c['method'] == 'DELETE'],
                         ['repos/owner/repo/releases/assets/101'])
        uploads = [c['filename'] for c in commands if c['filename']]
        self.assertEqual(len(uploads), len(publish_release.FILES) + 1)
        self.assertNotIn('SHA256SUMS', uploads)
        self.assertFalse(any(c['path'] == 'repos/owner/repo/releases' for c in commands))

    def test_wrong_hidden_draft_is_rejected_before_upload(self):
        result, commands = self.exercise(hidden_draft=True, draft_exists=True, wrong_draft=True)
        self.assertIn('different inputs', result['error'])
        self.assertEqual(commands, [])

    def test_invalid_created_release_id_is_rejected_before_upload(self):
        result, commands = self.exercise(bad_created_id=True)
        self.assertIn('different inputs', result['error'])
        self.assertEqual(len(commands), 1)
        self.assertFalse(any(c['filename'] for c in commands))

    def test_draft_input_drift_bad_digest_or_publication_race_prevents_publishing(self):
        for flags, reason in [({'drift_after_upload': True}, 'different inputs'),
                              ({'wrong_digest': True}, 'Uploaded release asset differs'),
                              ({'published_during_upload': True}, 'no longer the expected draft')]:
            with self.subTest(flags=flags):
                result, commands = self.exercise(hidden_draft=True, draft_exists=True, **flags)
                self.assertIn(reason, result['error'])
                self.assertFalse(any(c['method'] == 'PATCH' for c in commands))

    def test_ambiguous_hidden_draft_inventory_is_rejected(self):
        prefix, tag = 'repos/owner/repo', 'expected-tag'
        with patch.object(publish_release, 'api', side_effect=[None, [{'tag_name': tag}, {'tag_name': tag}]]), \
                self.assertRaisesRegex(ValueError, 'multiple releases'):
            publish_release.release_by_tag(prefix, tag)

    def test_recovery_cli_uses_original_build_run_without_overriding_ci_context(self):
        environment = {'GITHUB_REPOSITORY': 'owner/repo', 'KONOHA_SOURCE_ROOT': '/fixture/source',
                       'GITHUB_RUN_ID': '999', 'GITHUB_RUN_ATTEMPT': '3', 'GITHUB_SHA': 'd' * 40}
        with patch.dict(os.environ, environment, clear=True), \
                patch.object(sys, 'argv', ['publish_release.py', 'bundle', SOURCE, KERNEL, KSU,
                                          '--build-run-id', '123', '--build-run-attempt', '1']), \
                patch.object(publish_release, 'publish', return_value={'tag': 'verified-tag'}) as publish, \
                patch('builtins.print'):
            publish_release.main()
            self.assertEqual(os.environ['GITHUB_SHA'], 'd' * 40)
            self.assertEqual(os.environ['GITHUB_RUN_ID'], '999')
        self.assertEqual(publish.call_args.args[-2:], ('123', '1'))


class ReleaseApiTests(unittest.TestCase):
    def test_creation_body_preserves_newlines_and_literal_shell_text(self):
        body = {'draft': True, 'body': 'first line\n`literal` $(literal)\\n'}
        result = subprocess.CompletedProcess([], 0, '{"id":77}', '')
        with patch.object(subprocess, 'run', return_value=result) as run:
            self.assertEqual(publish_release.api('repos/owner/repo/releases', method='POST', data=body), {'id': 77})
        self.assertEqual(json.loads(run.call_args.kwargs['input']), body)
        self.assertIn('--input', run.call_args.args[0])
        self.assertIn('-', run.call_args.args[0])

    def test_binary_upload_uses_file_input_and_binary_content_type(self):
        result = subprocess.CompletedProcess([], 0, '{"id":101}', '')
        with patch.object(subprocess, 'run', return_value=result) as run:
            publish_release.api('https://uploads.github.com/repos/owner/repo/releases/77/assets?name=archive',
                                method='POST', input_file=Path('/fixture/archive'))
        command = run.call_args.args[0]
        self.assertEqual(command[command.index('--input') + 1], '/fixture/archive')
        self.assertIn('Content-Type: application/octet-stream', command)
        self.assertIsNone(run.call_args.kwargs['input'])

    def test_delete_accepts_empty_response_and_optional_lookup_still_handles_404(self):
        with patch.object(subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, '', '')):
            self.assertIsNone(publish_release.api('repos/owner/repo/releases/assets/101', method='DELETE'))
        with patch.object(subprocess, 'run', return_value=subprocess.CompletedProcess([], 1, '', 'gh: Not Found (HTTP 404)')):
            self.assertIsNone(publish_release.api('repos/owner/repo/releases/tags/missing', optional=True))
            with self.assertRaises(RuntimeError):
                publish_release.api('repos/owner/repo/releases/77')


class RecoveryTests(unittest.TestCase):
    def fixture(self):
        repository = {'id': 10, 'full_name': 'owner/repo'}
        run = {'id': 123, 'run_attempt': 1, 'repository': repository, 'head_repository': repository,
               'head_branch': 'ci', 'head_sha': 'd' * 40, 'path': '.github/workflows/build.yml',
               'status': 'completed', 'conclusion': 'failure'}
        jobs = [{'name': 'build', 'status': 'completed', 'conclusion': 'success'},
                {'name': 'release', 'status': 'completed', 'conclusion': 'failure'}]
        artifact = {'id': 456, 'name': 'Konoha-Android17-6.18-123-1', 'expired': False, 'size_in_bytes': 100,
                    'workflow_run': {'id': 123, 'repository_id': 10, 'head_repository_id': 10,
                                     'head_branch': 'ci', 'head_sha': 'd' * 40}}
        return run, jobs, artifact

    def select(self, run=None, jobs=None, artifacts=None, paginate=False):
        original_run, original_jobs, artifact = self.fixture()
        run = original_run if run is None else run
        jobs = original_jobs if jobs is None else jobs
        artifacts = [artifact] if artifacts is None else artifacts
        def api(path, optional=False):
            if path.endswith('/attempts/1'):
                return run
            if '/jobs?' in path:
                if paginate and path.endswith('page=1'):
                    return {'jobs': [{'name': 'other'}] * 100}
                return {'jobs': jobs}
            if '/artifacts?' in path:
                if paginate and path.endswith('page=1'):
                    return {'artifacts': [{'name': 'other'}] * 100}
                return {'artifacts': artifacts}
            raise AssertionError(path)
        with patch.object(publish_release, 'api', side_effect=api):
            return recover_release.select('owner/repo', '123', '1')

    def test_failed_release_run_can_recover_successful_build_and_later_page_artifact(self):
        result = self.select(paginate=True)
        self.assertEqual(result, {'build_run_id': '123', 'build_attempt': '1', 'artifact_id': '456',
                                  'artifact_name': 'Konoha-Android17-6.18-123-1', 'ci_revision': 'd' * 40})

    def test_wrong_run_attempt_workflow_branch_or_fork_is_rejected(self):
        for changed in [{'id': 999}, {'run_attempt': 2}, {'head_branch': 'android17-6.18'},
                        {'path': '.github/workflows/other.yml'},
                        {'repository': {'id': 11, 'full_name': 'other/repo'}},
                        {'head_repository': {'id': 11, 'full_name': 'fork/repo'}},
                        {'repository': {'full_name': 'owner/repo'}},
                        {'head_repository': {'id': True, 'full_name': 'owner/repo'}}, {'head_sha': None}]:
            with self.subTest(changed=changed), self.assertRaisesRegex(ValueError, 'owned CI workflow'):
                self.select(run={**self.fixture()[0], **changed})

    def test_failed_missing_duplicate_or_running_build_job_is_rejected(self):
        for jobs in [[], [{'name': 'build', 'status': 'completed', 'conclusion': 'failure'}],
                     [{'name': 'build', 'status': 'in_progress', 'conclusion': None}],
                     [self.fixture()[1][0]] * 2]:
            with self.subTest(jobs=jobs), self.assertRaisesRegex(ValueError, 'successful full build'):
                self.select(jobs=jobs)

    def test_expired_duplicate_wrong_attempt_or_wrong_owner_artifact_is_rejected(self):
        artifact = self.fixture()[2]
        changed = [{**artifact, 'expired': True}, {**artifact, 'size_in_bytes': 0},
                   {**artifact, 'size_in_bytes': '100'}, {**artifact, 'size_in_bytes': True},
                   {**artifact, 'id': '456'}, {**artifact, 'name': 'Konoha-Android17-6.18-123-2'}]
        for field, value in [('id', 999), ('head_sha', 'e' * 40), ('repository_id', 11),
                             ('head_repository_id', 11), ('head_branch', 'other')]:
            changed.append({**artifact, 'workflow_run': {**artifact['workflow_run'], field: value}})
        for artifacts in [[], [artifact, artifact], *[[value] for value in changed]]:
            with self.subTest(artifacts=artifacts), self.assertRaises(ValueError):
                self.select(artifacts=artifacts)

    def test_inputs_binds_exact_attempt_and_requires_full_revisions(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source, workspace, _, _ = fixture(root)
            bundle = root / 'bundle'
            identity = export_artifacts.export(workspace, bundle, source, SOURCE, '123', '1')
            self.assertEqual(recover_release.inputs(bundle, '123', '1'),
                             {'source_revision': SOURCE, 'kernel_revision': KERNEL, 'kernelsu_revision': KSU})
            for changed in [{'run_id': '999'}, {'run_attempt': '2'}, {'artifact_name': 'other'},
                            {'source_revision': 'main'}, {'common_revision': 'bad'}, {'kernelsu_revision': None}]:
                with self.subTest(changed=changed), self.assertRaises(ValueError):
                    write_json(bundle / 'release.json', {**identity, **changed})
                    recover_release.inputs(bundle, '123', '1')

    def test_invalid_run_numbers_are_rejected_before_api_calls(self):
        for run_id, attempt in [('0', '1'), ('123', '0'), ('../123', '1'), ('123', '-1'), ('123', '01')]:
            with self.subTest(values=(run_id, attempt)), patch.object(publish_release, 'api') as api, \
                    self.assertRaises(ValueError):
                recover_release.select('owner/repo', run_id, attempt)
            api.assert_not_called()


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


class WorkflowWiringTests(unittest.TestCase):
    def test_release_and_promotion_share_one_serial_job_and_bound_artifact_outputs(self):
        workflow = (Path(__file__).resolve().parents[2] / '.github/workflows/build.yml').read_text()
        release_job = workflow.split('\n  release:\n', 1)[1]
        self.assertIn('concurrency:\n      group: release-android17-6.18\n      cancel-in-progress: false', release_job)
        self.assertIn('BUILD_RUN_ATTEMPT: ${{ needs.build.outputs.build_attempt }}', release_job)
        self.assertIn('name: ${{ needs.build.outputs.artifact_name }}', release_job)
        self.assertIn('name: ${{ steps.export.outputs.artifact_name }}', workflow)
        self.assertIn('build_attempt: ${{ steps.export.outputs.build_attempt }}', workflow)

    def test_recovery_reuses_publication_and_state_under_same_lock_and_original_identity(self):
        workflow = (Path(__file__).resolve().parents[2] / '.github/workflows/recover-release.yml').read_text()
        self.assertIn('if: github.ref == \'refs/heads/ci\'', workflow)
        self.assertIn('concurrency:\n      group: release-android17-6.18\n      cancel-in-progress: false', workflow)
        self.assertIn('artifact-ids: ${{ steps.build.outputs.artifact_id }}', workflow)
        self.assertIn('run-id: ${{ steps.build.outputs.build_run_id }}', workflow)
        self.assertIn('--build-run-id "$BUILD_RUN_ID" --build-run-attempt "$BUILD_RUN_ATTEMPT"', workflow)
        self.assertIn('python3 ci/record_success.py', workflow)
        self.assertNotIn('source/port/build.py', workflow)
        self.assertNotIn('GITHUB_SHA:', workflow)
        self.assertNotIn('GITHUB_RUN_ID:', workflow)


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
