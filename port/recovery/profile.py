#!/usr/bin/env python3
"""Create a SHA-bound install profile from supplied full-size ROM partition dumps."""
import argparse
import json
from pathlib import Path

from generate import PARTITIONS, SERIAL_POLICY_SHA256, require, sha


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--images', type=Path, required=True)
    p.add_argument('--slot', choices=['a', 'b'], required=True)
    p.add_argument('--module-layout', type=Path,
                   help='Verified prebuilt-onyx repository checkout')
    p.add_argument('--serial-loaders', type=Path,
                   help='Optional tested four-file bootstrap and manifest.json')
    p.add_argument('--vendor-policy', type=Path,
                   help='Exact /vendor/etc/selinux/precompiled_sepolicy from the ROM')
    p.add_argument('--omissions', type=Path,
                   help='JSON mapping omitted normalized module names to reviewed reasons')
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    require(not a.output.exists(), 'Use a new profile output')
    entries = {}
    for name in list(PARTITIONS) + ['init_boot', 'vbmeta']:
        candidates = [a.images / (name + '.img'), a.images / (name + '_' + a.slot + '.img')]
        found = [p for p in candidates if p.is_file()]
        require(len(found) == 1, f'Provide exactly one full-size dump for {name}: {candidates}')
        image = found[0].resolve()
        entries[name] = {'path': str(image), 'size': image.stat().st_size, 'sha256': sha(image)}
    profile = {'device': 'onyx', 'images': entries}
    if a.module_layout:
        profile['module_layout'] = {'source': str(a.module_layout.resolve()),
                                    'sha256': sha(a.module_layout / 'SHA256SUMS')}
    if a.serial_loaders:
        require(a.vendor_policy and sha(a.vendor_policy) == SERIAL_POLICY_SHA256,
                'The serial route requires the exact permission-audited development ROM policy')
        profile['loaders'] = {'source': str(a.serial_loaders.resolve()),
                              'sha256': sha(a.serial_loaders / 'manifest.json')}
        profile['prerequisite_files'] = [{'path': '/vendor/etc/selinux/precompiled_sepolicy',
                                         'sha256': sha(a.vendor_policy)}]
    if a.omissions:
        profile['omissions'] = json.loads(a.omissions.read_text())
    a.output.write_text(json.dumps(profile, indent=2) + '\n')
    print(a.output)


if __name__ == '__main__':
    main()
