import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import build_cache


class BuildCache(unittest.TestCase):
    def fixture(self, root):
        temporary, checkout = root / 'temporary', root / 'checkout'
        workspace = temporary / 'konoha-workspace'
        base = workspace / 'out/bazel/output_user_root/hash'
        kept = [temporary / 'kleaf-cache', base / 'action_cache', base / 'execroot/_main/bazel-out', checkout / 'source',
                base.parent / 'install', base.parent / 'cache']
        discarded = [checkout / 'artifacts', workspace / 'artifacts', workspace / '.repo', base / 'sandbox']
        for path in kept + discarded:
            path.mkdir(parents=True)
            (path / 'sentinel').write_text('retained or disposable fixture')
        (checkout / 'artifacts/release.json').write_text(json.dumps({'run_id': '123', 'run_attempt': '1'}))
        environment = {'GITHUB_ACTIONS': 'true', 'RUNNER_ENVIRONMENT': 'github-hosted', 'RUNNER_OS': 'Linux',
                       'RUNNER_TEMP': str(temporary), 'GITHUB_WORKSPACE': str(checkout),
                       'GITHUB_RUN_ID': '123', 'GITHUB_RUN_ATTEMPT': '1'}
        return environment, kept, discarded

    def test_uploaded_copies_and_git_metadata_removed_without_touching_cache_or_source(self):
        with tempfile.TemporaryDirectory() as temporary:
            environment, kept, discarded = self.fixture(Path(temporary))
            result = build_cache.prepare(environment)
            self.assertEqual(set(result['removed']), {str(p) for p in discarded})
            self.assertTrue(all((p / 'sentinel').is_file() for p in kept))
            self.assertTrue(all(not p.exists() for p in discarded))

    def test_local_runner_wrong_build_and_symlink_rejected_before_cleanup(self):
        for mode in ('local', 'other_build', 'symlink', 'manifest_symlink'):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                environment, kept, discarded = self.fixture(root)
                if mode == 'local':
                    environment['GITHUB_ACTIONS'] = 'false'
                elif mode == 'other_build':
                    environment['GITHUB_RUN_ID'] = '456'
                elif mode == 'symlink':
                    repo = discarded[2]
                    repo.rename(root / 'outside')
                    repo.symlink_to(root / 'outside')
                else:
                    (root / 'outside').write_text('unrelated')
                    (Path(environment['RUNNER_TEMP']) / 'kleaf-cache-links.json').symlink_to(root / 'outside')
                with self.assertRaises(ValueError):
                    build_cache.prepare(environment)
                self.assertTrue(all((p / 'sentinel').is_file() for p in kept + discarded))

    def test_missing_cache_is_rejected_before_cleanup(self):
        with tempfile.TemporaryDirectory() as temporary:
            environment, kept, discarded = self.fixture(Path(temporary))
            (kept[1] / 'sentinel').unlink()
            kept[1].rmdir()
            with self.assertRaisesRegex(ValueError, 'complete stopped'):
                build_cache.prepare(environment)
            self.assertTrue(all((p / 'sentinel').is_file() for p in discarded))

    def test_exact_saved_cache_is_required_and_eventual_listing_is_retried(self):
        key = 'kleaf-v3-Linux-fixture-false-123-1'
        entry = {'id': 7, 'key': key, 'ref': 'refs/heads/ci', 'size_in_bytes': 4096, 'created_at': 'fixture'}
        responses = iter([{'actions_caches': []}, {'actions_caches': [entry]}])
        calls, waits = [], []
        def query(path):
            calls.append(path)
            return next(responses)
        self.assertEqual(build_cache.saved('owner/repo', key, query, waits.append), entry)
        self.assertEqual(len(calls), 2)
        self.assertEqual(waits, [5])
        for bad in ([], [{**entry, 'key': key + '-other'}], [{**entry, 'ref': 'refs/heads/other'}], [{**entry, 'size_in_bytes': 0}]):
            with self.subTest(entries=bad), self.assertRaisesRegex(ValueError, 'did not retain'):
                build_cache.saved('owner/repo', key, lambda _: {'actions_caches': bad}, lambda _: None)

    def test_cleanup_follows_upload_and_shutdown_and_save_is_confirmed(self):
        workflow = (Path(__file__).resolve().parents[2] / '.github/workflows/build.yml').read_text()
        labels = ['name: Upload build artifacts', 'name: Flush Bazel action cache',
                  'name: Reclaim uploaded copies', 'name: Save verified Kleaf', 'name: Confirm GitHub retained']
        offsets = [workflow.index(label) for label in labels]
        self.assertEqual(offsets, sorted(offsets))
        self.assertIn('python3 ci/build_cache.py confirm "$CACHE_KEY"', workflow)
        self.assertLess(workflow.index('python3 ci/build_cache.py restore'), workflow.index('name: Build matching GKI'))
        self.assertEqual(workflow.count('${{ runner.temp }}/kleaf-cache-links.json'), 2)

    def test_missing_manifest_is_only_allowed_for_a_cold_cache_miss(self):
        with tempfile.TemporaryDirectory() as temporary:
            environment, _, _ = self.fixture(Path(temporary))
            self.assertTrue(build_cache.restore(environment)['cold_build'])
            environment['CACHE_MATCHED_KEY'] = 'kleaf-v3-restored'
            with self.assertRaisesRegex(ValueError, 'missing'):
                build_cache.restore(environment)


if __name__ == '__main__':
    unittest.main()
