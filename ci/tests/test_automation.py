import importlib.util
import base64
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'ci'))


def module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'ci' / (name + '.py'))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


class TrackingTests(unittest.TestCase):
    def test_unchanged_inputs_do_not_rebuild(self):
        tracker = module('track_upstream')
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'port').mkdir()
            (root / 'ci').mkdir()
            kernel, ksu = 'a' * 40, 'b' * 40
            (root / 'port/sources.json').write_text(json.dumps({'common': {'revision': kernel}}))
            (root / 'ci/last-built.json').write_text(json.dumps(
                {'common_revision': kernel, 'kernelsu_revision': ksu, 'source_revision': kernel}))
            with patch.object(tracker, 'ROOT', root), patch.object(tracker, 'PORT', root / 'port'), \
                    patch.object(tracker, 'remote_head', side_effect=[kernel, ksu]), \
                    patch.object(tracker, 'git', return_value=kernel), \
                    patch.dict(os.environ, {}, clear=True):
                result = tracker.detect()
            self.assertFalse(result['kernel_changed'])
            self.assertFalse(result['kernelsu_changed'])
            self.assertFalse(result['source_changed'])

    def test_kernelsu_only_change_rebuilds_accepted_kernel(self):
        tracker = module('track_upstream')
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'port').mkdir()
            (root / 'ci').mkdir()
            kernel = 'a' * 40
            (root / 'port/sources.json').write_text(json.dumps({'common': {'revision': kernel}}))
            (root / 'ci/last-built.json').write_text(json.dumps(
                {'common_revision': kernel, 'kernelsu_revision': 'b' * 40}))
            with patch.object(tracker, 'ROOT', root), patch.object(tracker, 'PORT', root / 'port'), \
                    patch.object(tracker, 'remote_head', side_effect=[kernel, 'c' * 40]), \
                    patch.object(tracker, 'git', return_value=kernel), \
                    patch.dict(os.environ, {}, clear=True):
                result = tracker.detect()
            self.assertFalse(result['kernel_changed'])
            self.assertTrue(result['kernelsu_changed'])

    def test_manual_source_change_requires_a_build(self):
        tracker = module('track_upstream')
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'port').mkdir()
            (root / 'ci').mkdir()
            kernel, ksu = 'a' * 40, 'b' * 40
            (root / 'port/sources.json').write_text(json.dumps({'common': {'revision': kernel}}))
            (root / 'ci/last-built.json').write_text(json.dumps(
                {'common_revision': kernel, 'kernelsu_revision': ksu, 'source_revision': 'c' * 40}))
            with patch.object(tracker, 'ROOT', root), patch.object(tracker, 'PORT', root / 'port'), \
                    patch.object(tracker, 'remote_head', side_effect=[kernel, ksu]), \
                    patch.object(tracker, 'git', return_value='d' * 40), \
                    patch.dict(os.environ, {}, clear=True):
                result = tracker.detect()
            self.assertFalse(result['kernel_changed'])
            self.assertFalse(result['kernelsu_changed'])
            self.assertTrue(result['source_changed'])

    def test_remote_head_rejects_wrong_ref(self):
        tracker = module('track_upstream')
        with patch.object(tracker.subprocess, 'check_output', return_value='a' * 40 + '\trefs/heads/other\n'):
            with self.assertRaises(ValueError):
                tracker.remote_head('https://example.org/kernel', 'android17-6.18-lts')

    def test_remote_head_accepts_exact_branch(self):
        tracker = module('track_upstream')
        with patch.object(tracker.subprocess, 'check_output', return_value='a' * 40 + '\trefs/heads/android17-6.18-lts\n'):
            self.assertEqual(tracker.remote_head('https://example.org/kernel', 'android17-6.18-lts'), 'a' * 40)

    def test_common_pin_and_blob_refresh(self):
        tracker = module('track_upstream')
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            port = root / 'port'
            port.mkdir()
            (port / 'sources.json').write_text(json.dumps({'common': {'revision': 'old'}}))
            (port / 'imports.json').write_text(json.dumps([
                {'project': 'common', 'path': 'include/core.h', 'revision': 'old', 'blob': 'old'},
                {'project': 'vendor', 'path': 'vendor.c', 'revision': 'keep', 'blob': 'keep'}]))
            (port / 'onyx-port.xml').write_text('<manifest><extend-project name="kernel/common"/></manifest>')
            revision = 'b' * 40
            def git(repo, *args):
                if args == ('rev-parse', 'HEAD'):
                    return revision
                if args[0] == 'show':
                    return 'VERSION = 6\nPATCHLEVEL = 18\nSUBLEVEL = 38\n'
                return 'c' * 40
            with patch.object(tracker, 'PORT', port), patch.object(tracker, 'git', side_effect=git):
                result = tracker.update(root, revision)
            self.assertEqual(result['kernel'], '6.18.38')
            imports = json.loads((port / 'imports.json').read_text())
            self.assertEqual(imports[0]['blob'], 'c' * 40)
            self.assertEqual(imports[1]['revision'], 'keep')
            self.assertIn('android17-6.18-lts', (port / 'onyx-port.xml').read_text())

    def test_refuses_wrong_kernel_before_writing(self):
        tracker = module('track_upstream')
        with patch.object(tracker, 'git', side_effect=['a' * 40, 'VERSION = 6\nPATCHLEVEL = 19\nSUBLEVEL = 0\n']):
            with self.assertRaises(ValueError):
                tracker.update(Path('/unused'), 'a' * 40)


class RebaseTests(unittest.TestCase):
    def setup_fixture(self, root, integrated=False, conflict=False):
        port = root / 'port'
        patches = port / 'common-patches'
        evidence = port / 'validation/clock_suspend_resume'
        patches.mkdir(parents=True)
        (evidence / 'candidate/drivers').mkdir(parents=True)
        common = root / 'common'
        (common / 'drivers').mkdir(parents=True)
        (common / 'drivers/core.c').write_text('fixed\n' if integrated else 'conflict\n' if conflict else 'old\n')
        (common / 'drivers/diag.c').write_text('original\n')
        (patches / 'upstream-platform-remove.patch').write_text(
            'Subject: test\n\ndiff --git a/drivers/core.c b/drivers/core.c\n'
            '--- a/drivers/core.c\n+++ b/drivers/core.c\n@@ -1 +1 @@\n-old\n+fixed\n')
        (patches / 'manifest.json').write_text(json.dumps({'patch': 'effective.patch'}))
        (evidence / 'common-qdss-diagnostic.patch').write_text(
            'diff --git a/drivers/diag.c b/drivers/diag.c\n'
            '--- a/drivers/diag.c\n+++ b/drivers/diag.c\n@@ -1 +1 @@\n-original\n+logged\n')
        (evidence / 'source-validation.json').write_text(json.dumps({'files': {'drivers/diag.c': {}}}))
        return port, common

    def test_forward_rebase_preserves_pristine_common(self):
        refresh = module('refresh_recipe')
        with tempfile.TemporaryDirectory() as temporary:
            port, common = self.setup_fixture(Path(temporary))
            with patch.object(refresh, 'PORT', port):
                result = refresh.refresh(common)
            self.assertEqual(result['active_common_patches'], 1)
            self.assertEqual((common / 'drivers/core.c').read_text(), 'old\n')
            self.assertEqual((port / 'validation/clock_suspend_resume/candidate/drivers/diag.c').read_text(), 'logged\n')

    def test_integrated_fix_is_not_applied_twice(self):
        refresh = module('refresh_recipe')
        with tempfile.TemporaryDirectory() as temporary:
            port, common = self.setup_fixture(Path(temporary), integrated=True)
            with patch.object(refresh, 'PORT', port):
                result = refresh.refresh(common)
            self.assertEqual(result['already_integrated_upstream'], ['drivers/core.c'])
            self.assertEqual((port / 'common-patches/effective.patch').read_bytes(), b'')

    def test_manifest_selects_local_and_upstream_replay_patches(self):
        refresh = module('refresh_recipe')
        with tempfile.TemporaryDirectory() as temporary:
            port, common = self.setup_fixture(Path(temporary))
            patches = port / 'common-patches'
            (common / 'drivers/thermal.c').write_text('private class\n')
            (patches / 'required.patch').write_text(
                (patches / 'upstream-platform-remove.patch').read_text() +
                'diff --git a/drivers/thermal.c b/drivers/thermal.c\n'
                '--- a/drivers/thermal.c\n+++ b/drivers/thermal.c\n'
                '@@ -1 +1 @@\n-private class\n+typed registration\n')
            (patches / 'manifest.json').write_text(json.dumps(
                {'patch': 'effective.patch', 'replay_patch': 'required.patch'}))
            with patch.object(refresh, 'PORT', port):
                result = refresh.refresh(common)
            self.assertEqual(result['active_common_patches'], 2)
            self.assertIn('typed registration', (patches / 'effective.patch').read_text())
            self.assertEqual((common / 'drivers/thermal.c').read_text(), 'private class\n')

    def test_conflict_does_not_mutate_recipe(self):
        refresh = module('refresh_recipe')
        with tempfile.TemporaryDirectory() as temporary:
            port, common = self.setup_fixture(Path(temporary), conflict=True)
            before = {p.relative_to(port): p.read_bytes() for p in port.rglob('*') if p.is_file()}
            with patch.object(refresh, 'PORT', port), self.assertRaises(ValueError):
                refresh.refresh(common)
            after = {p.relative_to(port): p.read_bytes() for p in port.rglob('*') if p.is_file()}
            self.assertEqual(before, after)


class SnapshotTests(unittest.TestCase):
    def test_snapshot_rejects_missing_or_changed_module_source(self):
        snapshot = module('snapshot')
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name in ['modules', 'ci', 'port', 'staged']:
                (root / name).mkdir()
            (root / 'port/modules.json').write_text('[{"output": "driver.ko"}]')
            (root / 'staged/BUILD.bazel').write_text('build rules\n')
            (root / 'staged/driver.c').write_text('driver source\n')
            with patch.object(snapshot, 'ROOT', root):
                snapshot.snapshot(root / 'staged', write=True)
                self.assertEqual(snapshot.snapshot(root / 'staged')['files_verified'], 2)
                (root / 'modules/driver.c').write_text('changed\n')
                with self.assertRaises(ValueError):
                    snapshot.snapshot(root / 'staged')


class PublicationTests(unittest.TestCase):
    def exercise(self, proposals, builds, kernelsu=None):
        publisher = module('publish_update')
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            port = root / 'port'
            (port / 'patches').mkdir(parents=True)
            (port / 'patches/0001.patch').write_text('patch')
            (port / 'sources.json').write_text(json.dumps(
                {'common': {'revision': 'a' * 40, 'kernel': '6.18.38'}}))
            (port / 'modules.json').write_text('[{"output": "driver.ko"}]')
            calls = []
            def run(args):
                calls.append(args)
                if args[:3] == ['git', 'rev-parse', 'HEAD:port']:
                    return 'b' * 40
                if args[:2] == ['git', 'ls-remote']:
                    return 'existing candidate'
                if args[:3] == ['gh', 'pr', 'list']:
                    return json.dumps(proposals)
                if args[:3] == ['gh', 'pr', 'create']:
                    return 'https://example.org/pull/1'
                if args[:3] == ['gh', 'run', 'list']:
                    return json.dumps(builds)
                return ''
            with patch.object(publisher, 'ROOT', root), patch.object(publisher, 'run', side_effect=run):
                result = publisher.publish('a' * 40, kernelsu)
            self.assertFalse(any(args[:2] in [['git', 'push'], ['git', 'commit'], ['git', 'checkout']]
                                 for args in calls))
            return result, calls

    def test_existing_branch_recovers_missing_pr_and_dispatch(self):
        result, calls = self.exercise([], [])
        self.assertTrue(any(args[:3] == ['gh', 'pr', 'create'] for args in calls))
        self.assertTrue(any(args[:3] == ['gh', 'workflow', 'run'] for args in calls))

    def test_existing_pr_recovers_missing_dispatch(self):
        result, calls = self.exercise([{'url': 'https://example.org/pull/1', 'state': 'OPEN'}], [])
        self.assertFalse(any(args[:3] == ['gh', 'pr', 'create'] for args in calls))
        self.assertTrue(any(args[:3] == ['gh', 'workflow', 'run'] for args in calls))

    def test_failed_candidate_build_can_retry(self):
        result, calls = self.exercise([{'url': 'https://example.org/pull/1', 'state': 'OPEN'}],
                                      [{'displayTitle': 'Build upstream/android17-6.18-' + 'a' * 12 + '-' + 'b' * 12,
                                        'status': 'completed', 'conclusion': 'failure', 'url': 'build'}])
        self.assertTrue(any(args[:3] == ['gh', 'workflow', 'run'] for args in calls))

    def test_three_failed_candidate_builds_require_repair(self):
        build = {'displayTitle': 'Build upstream/android17-6.18-' + 'a' * 12 + '-' + 'b' * 12,
                 'status': 'completed', 'conclusion': 'failure', 'url': 'build'}
        result, calls = self.exercise([{'url': 'https://example.org/pull/1', 'state': 'OPEN'}], [build] * 3)
        self.assertFalse(any(args[:3] == ['gh', 'workflow', 'run'] for args in calls))

    def test_closed_candidate_is_not_reopened(self):
        result, calls = self.exercise([{'url': 'https://example.org/pull/1', 'state': 'CLOSED'}], [])
        self.assertFalse(any(args[:3] == ['gh', 'pr', 'create'] for args in calls))
        self.assertFalse(any(args[:3] == ['gh', 'workflow', 'run'] for args in calls))

    def test_unrelated_source_build_does_not_block_candidate_dispatch(self):
        result, calls = self.exercise([{'url': 'https://example.org/pull/1', 'state': 'OPEN'}],
                                      [{'displayTitle': 'Build android17-6.18', 'status': 'in_progress',
                                        'conclusion': '', 'url': 'build'}])
        self.assertTrue(any(args[:3] == ['gh', 'workflow', 'run'] for args in calls))

    def test_new_kernelsu_rebuilds_existing_lts_candidate(self):
        title = 'Build upstream/android17-6.18-' + 'a' * 12 + '-' + 'b' * 12
        result, calls = self.exercise([{'url': 'https://example.org/pull/1', 'state': 'OPEN'}],
                                      [{'displayTitle': title + ' / KSU ' + 'c' * 40,
                                        'status': 'completed', 'conclusion': 'success', 'url': 'build'}],
                                      kernelsu='d' * 40)
        self.assertTrue(any(args[:3] == ['gh', 'workflow', 'run'] and
                            'kernelsu_revision=' + 'd' * 40 in args for args in calls))


class BuildStateTests(unittest.TestCase):
    def test_old_build_cannot_overwrite_new_main_state(self):
        recorder = module('record_success')
        with patch.dict(os.environ, {'GITHUB_SHA': 'a' * 40}), \
                patch.object(recorder.subprocess, 'check_output', return_value=json.dumps(
                    {'object': {'sha': 'b' * 40}})), patch.object(recorder.subprocess, 'run') as run:
            result = recorder.record('owner/repo', 'c' * 40, 'd' * 40)
        run.assert_not_called()
        self.assertIn('CI branch advanced', result['status'])

    def test_old_source_build_cannot_overwrite_accepted_state(self):
        recorder = module('record_success')
        with patch.dict(os.environ, {'GITHUB_SHA': 'a' * 40}), \
                patch.object(recorder.subprocess, 'check_output', side_effect=[json.dumps(
                    {'object': {'sha': 'a' * 40}}), json.dumps({'object': {'sha': 'b' * 40}})]), \
                patch.object(recorder.subprocess, 'run') as run:
            result = recorder.record('owner/repo', 'c' * 40, 'd' * 40, 'e' * 40)
        run.assert_not_called()
        self.assertIn('source branch advanced', result['status'])


class CommonSourceTests(unittest.TestCase):
    def test_executable_bit_is_part_of_snapshot(self):
        snapshot = module('snapshot_common')
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'build.sh'
            source.write_text('build\n')
            source.chmod(0o644)
            before = snapshot.hashes(root)
            source.chmod(0o755)
            self.assertNotEqual(before, snapshot.hashes(root))

    def test_symlink_target_is_hashed_without_following_it(self):
        snapshot = module('snapshot_common')
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'link'
            source.symlink_to('not-present')
            self.assertEqual(snapshot.hashes(root)['link'], {'symlink': 'not-present'})
            source.unlink()
            source.symlink_to('different-target')
            self.assertEqual(snapshot.hashes(root)['link'], {'symlink': 'different-target'})


class PromotionTests(unittest.TestCase):
    def exercise(self, change=None, push_failed=False):
        promoter = module('promote_update')
        source, kernel, ksu, base = 'a' * 40, 'b' * 40, 'c' * 40, 'd' * 40
        branch = 'upstream/android17-6.18-' + 'e' * 12 + '-' + 'f' * 12
        run_url = 'https://github.com/owner/repo/actions/runs/123'
        responses = {
            'jobs': {'jobs': [{'name': 'build', 'conclusion': 'success'}]},
            'status': {'statuses': [{'context': 'Konoha / GKI and modules', 'state': 'success',
                                     'target_url': run_url}]},
            'proposals': [{'number': 1, 'draft': False, 'head': {'sha': source},
                           'html_url': 'https://github.com/owner/repo/pull/1'}],
            'pins': {'content': base64.b64encode(json.dumps(
                {'common': {'revision': kernel, 'kernel': '6.18.38'}}).encode()).decode()},
            'base': {'object': {'sha': base}},
            'comparison': {'status': 'ahead'},
            'merged': {'merged': True},
        }
        if change:
            change(responses)
        def api(path):
            if path.endswith('/jobs'): return responses['jobs']
            if path.endswith('/status'): return responses['status']
            if '/pulls?' in path: return responses['proposals']
            if '/contents/' in path: return responses['pins']
            if '/git/ref/' in path: return responses['base']
            if '/compare/' in path: return responses['comparison']
            if path.endswith('/pulls/1'): return responses['merged']
            raise AssertionError(path)
        commands = []
        def run(args, **kwargs):
            commands.append(args)
            if args[:2] == ['git', 'push'] and push_failed:
                responses['base'] = {'object': {'sha': '0' * 40}}
                return SimpleNamespace(returncode=1, stderr='stale lease')
            return SimpleNamespace(returncode=0, stderr='')
        with patch.object(promoter, 'api', side_effect=api), \
                patch.object(promoter.subprocess, 'run', side_effect=run), \
                patch.object(promoter, 'record_result') as record:
            try:
                result = promoter.promote('owner/repo', branch, source, kernel, ksu, run_url)
            except ValueError as error:
                result = {'error': str(error)}
            return result, commands, record.call_args_list

    def test_success_promotes_exact_tested_commit_with_atomic_base_check(self):
        result, commands, records = self.exercise()
        push = next(args for args in commands if args[:2] == ['git', 'push'])
        self.assertIn('--force-with-lease=refs/heads/android17-6.18:' + 'd' * 40, push)
        self.assertIn('a' * 40 + ':refs/heads/android17-6.18', push)
        self.assertEqual(records[0].args, ('owner/repo', 'b' * 40, 'c' * 40, 'a' * 40))
        self.assertIn('PR merged', result['status'])

    def test_failed_build_cannot_promote(self):
        result, commands, records = self.exercise(lambda r: r['jobs']['jobs'][0].update(conclusion='failure'))
        self.assertIn('not succeeded', result['error'])
        self.assertEqual(commands, [])
        self.assertEqual(records, [])

    def test_other_builds_status_cannot_promote(self):
        result, commands, records = self.exercise(lambda r: r['status']['statuses'][0].update(target_url='other'))
        self.assertIn('verification status', result['error'])
        self.assertEqual(commands, [])

    def test_changed_candidate_requires_new_build(self):
        result, commands, records = self.exercise(lambda r: r['proposals'][0]['head'].update(sha='0' * 40))
        self.assertIn('Candidate changed', result['status'])
        self.assertEqual(commands, [])

    def test_changed_base_requires_replay(self):
        result, commands, records = self.exercise(lambda r: r['comparison'].update(status='diverged'))
        self.assertIn('new candidate', result['status'])
        self.assertEqual(commands, [])

    def test_already_promoted_candidate_can_recover_release_after_upload_failure(self):
        result, commands, records = self.exercise(lambda r: r['base']['object'].update(sha='a' * 40))
        self.assertTrue(result['promoted'])
        self.assertIn('already accepted', result['status'])
        self.assertEqual(commands, [])
        self.assertEqual(records, [])

    def test_base_race_does_not_record_unpromoted_build(self):
        result, commands, records = self.exercise(push_failed=True)
        self.assertIn('advanced during promotion', result['status'])
        self.assertEqual(records, [])

    def test_wrong_kernel_pin_cannot_promote(self):
        def change(r):
            r['pins']['content'] = base64.b64encode(json.dumps(
                {'common': {'revision': '0' * 40, 'kernel': '6.18.38'}}).encode()).decode()
        result, commands, records = self.exercise(change)
        self.assertIn('source pins', result['error'])
        self.assertEqual(commands, [])


if __name__ == '__main__':
    unittest.main()
