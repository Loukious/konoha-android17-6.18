"""Summarize the exact Google common commit range since the previous release."""
from collections import Counter
import html
import json
import re
import time
from urllib.parse import urlencode
from urllib.request import urlopen

GOOGLE = 'https://android.googlesource.com/kernel/common'
SHA = r'[0-9a-f]{40}'
TAG = r'gki-6\.18\.\d+-s[0-9a-f]{12}-k[0-9a-f]{12}'


def previous_release(releases, current_tag):
    current = next((r for r in releases if r.get('tag_name') == current_tag), {})
    cutoff = current.get('published_at') or '9999'
    candidates = []
    for release in releases:
        if (release.get('draft', True) or release.get('prerelease', False) or
                release.get('tag_name') == current_tag or
                not re.fullmatch(TAG, release.get('tag_name', '')) or
                not release.get('published_at') or release['published_at'] >= cutoff):
            continue
        match = re.search(r'^- Google GKI revision: `(' + SHA + r')`$', release.get('body', ''), re.M)
        if match:
            candidates.append((release['published_at'], release['tag_name'], match[1]))
    return max(candidates) if candidates else None


def read_json(url):
    for attempt in range(3):
        try:
            with urlopen(url, timeout=30) as response:
                data = response.read(16 * 1024 * 1024 + 1)
            if len(data) > 16 * 1024 * 1024 or not data.startswith(b")]}'\n"):
                raise ValueError('Invalid Google Gitiles response')
            return json.loads(data[5:])
        except (OSError, ValueError):
            if attempt == 2:
                raise
            time.sleep(attempt + 1)


def commits_between(base, tip):
    if not all(re.fullmatch(SHA, value) for value in (base, tip)):
        raise ValueError('Google changelog requires exact revisions')
    commits, seen, pages = [], set(), set()
    cursor = None
    for _ in range(100):
        query = {'format': 'JSON', 'n': '100'}
        if cursor:
            query['s'] = cursor
        data = read_json(f'{GOOGLE}/+log/{base}..{tip}?' + urlencode(query))
        if not isinstance(data.get('log'), list):
            raise ValueError('Invalid Google commit list')
        for commit in data['log']:
            revision = commit.get('commit', '')
            message = commit.get('message', '')
            if not re.fullmatch(SHA, revision) or not isinstance(message, str) or not message.strip():
                raise ValueError('Invalid Google commit')
            if revision in seen or revision == base:
                raise ValueError('Google range contains repeated or excluded commit')
            seen.add(revision)
            commits.append((revision, message.splitlines()[0]))
        cursor = data.get('next')
        if not cursor:
            return commits
        if not re.fullmatch(SHA, cursor) or cursor in pages:
            raise ValueError('Invalid Google changelog pagination')
        pages.add(cursor)
    raise ValueError('Google changelog exceeds 10,000 commits')


def area(subject):
    subject = re.sub(r'^(?:(?:ANDROID|UPSTREAM|BACKPORT|FROMGIT|FROMLIST):\s*)+', '', subject).lower()
    for label, pattern in (
        ('GKI / ABI', r'\b(gki|abi|kmi|modversions|symbol list|dwarf crc)\b'),
        ('Rust', r'\brust\b'), ('Memory management', r'^(mm/|mm:|mglru:|page_counter:)'),
        ('Scheduler', r'^sched[/:]'), ('Binder', r'^binder:'),
        ('Filesystems', r'^(f2fs|ext4|btrfs|fs|erofs|vfs)[/:]'),
        ('Networking', r'^(net|tcp|udp|ipv[46]|wifi|wireless|bluetooth)[/:]'),
        ('Storage', r'^(scsi|block|virtio.blk|nvme|ufs)[/:]'),
        ('Power management', r'^(pm|power|suspend|cpuidle|cpufreq)[/:]'),
        ('Merge / version updates', r'^(merge |linux |bump |update abi)'),
    ):
        if re.search(pattern, subject):
            return label
    return 'Other kernel changes'


def text(value):
    value = html.escape(value, quote=False)
    return re.sub(r'([\\`*_[\]])', r'\\\1', value)


def summary(releases, current_tag, tip):
    if not re.fullmatch(SHA, tip):
        raise ValueError('Invalid Google revision')
    heading = '\n### Google GKI changes\n\n'
    previous = previous_release(releases, current_tag)
    if previous is None:
        return heading + f'First recorded GKI baseline: [`{tip[:12]}`]({GOOGLE}/+/{tip}) on `android17-6.18-lts`.\n'
    _, tag, base = previous
    if base == tip:
        return heading + (f'No new Google GKI commits since `{tag}`; the Google revision remains '
                          f'[`{tip[:12]}`]({GOOGLE}/+/{tip}). Konoha and KernelSU revisions are recorded above.\n')
    commits = commits_between(base, tip)
    counts = Counter(area(subject) for _, subject in commits)
    result = heading + (f'**{len(commits)} Google commits** since `{tag}`, '
        f'from `{base[:12]}` to `{tip[:12]}` on `android17-6.18-lts`. '
        f'[Full Google commit range]({GOOGLE}/+log/{base}..{tip}).\n\n')
    result += 'Areas changed (grouped from commit subjects): ' + ', '.join(
        f'{label} ({count})' for label, count in counts.most_common()) + '.\n\n'
    shown = commits[:100]
    result += '<details>\n<summary>Google commit subjects' + (f' (newest 100 of {len(commits)})' if len(commits) > 100 else '') + '</summary>\n\n'
    for revision, subject in shown:
        result += f'- [`{revision[:12]}`]({GOOGLE}/+/{revision}) {text(subject[:240])}\n'
    return result + '\n</details>\n'
