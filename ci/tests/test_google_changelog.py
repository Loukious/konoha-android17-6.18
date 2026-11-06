import sys
from pathlib import Path
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import google_changelog as changes

BASE, TIP = 'a' * 40, 'b' * 40
OLD = 'gki-6.18.38-s' + '1' * 12 + '-k' + '2' * 12
NEW = 'gki-6.18.38-s' + '3' * 12 + '-k' + '4' * 12


def release(tag=OLD, revision=BASE, date='2026-11-05T00:00:00Z', **kwargs):
    return dict(tag_name=tag, body=f'- Google GKI revision: `{revision}`', published_at=date,
                draft=False, **kwargs)


class GoogleChangesTests(unittest.TestCase):
    def test_prior_release_ignores_drafts_non_gki_and_future_backfill_releases(self):
        current = release(NEW, TIP, '2026-11-06T00:00:00Z')
        draft = release(); draft['draft'] = True; draft['published_at'] = '2026-11-05T23:00:00Z'
        future = release(NEW.replace('3', '5'), 'c' * 40, '2026-11-07T00:00:00Z')
        result = changes.previous_release([future, current, draft, release('onyx-recovery'), release()], NEW)
        self.assertEqual(result[1:], (OLD, BASE))

    def test_unchanged_revision_and_first_release_do_not_fetch_google(self):
        with patch.object(changes, 'read_json', side_effect=AssertionError('No fetch expected')):
            self.assertIn('No new Google GKI commits', changes.summary([release()], NEW, BASE))
            self.assertIn('First recorded GKI baseline', changes.summary([], NEW, TIP))

    def test_pagination_retains_original_range_and_lists_all_commits(self):
        with patch.object(changes, 'read_json', side_effect=[
            {'log': [{'commit': TIP, 'message': 'ANDROID: binder: fix page leak\n\nBody'}], 'next': 'c' * 40},
            {'log': [{'commit': 'c' * 40, 'message': 'UPSTREAM: rust: add SRCU'}]},
        ]) as read:
            output = changes.summary([release()], NEW, TIP)
        self.assertIn('**2 Google commits**', output)
        self.assertIn('Binder (1)', output); self.assertIn('Rust (1)', output)
        self.assertIn('fix page leak', output)
        self.assertTrue(all(f'{BASE}..{TIP}' in call.args[0] for call in read.call_args_list))
        self.assertIn('&s=' + 'c' * 40, read.call_args_list[1].args[0])

    def test_duplicate_page_or_malformed_revision_is_rejected(self):
        entry = {'log': [{'commit': TIP, 'message': 'fix'}], 'next': 'c' * 40}
        with patch.object(changes, 'read_json', return_value=entry):
            with self.assertRaisesRegex(ValueError, 'repeated'):
                changes.commits_between(BASE, TIP)
        with self.assertRaisesRegex(ValueError, 'exact revisions'):
            changes.commits_between('branch-name', TIP)

    def test_large_range_is_counted_but_body_is_bounded_and_subjects_escaped(self):
        commits = [(f'{i:040x}', 'ANDROID: GKI: fix <tag> [link] *text*') for i in range(150)]
        with patch.object(changes, 'commits_between', return_value=commits):
            output = changes.summary([release()], NEW, TIP)
        self.assertIn('**150 Google commits**', output)
        self.assertIn('newest 100 of 150', output)
        self.assertIn('&lt;tag&gt;', output)
        self.assertIn('\\[link\\]', output)
        self.assertLess(len(output.encode()), 40000)


if __name__ == '__main__':
    unittest.main()
