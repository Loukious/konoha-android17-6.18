#!/usr/bin/env python3
"""Verify pinned recovery tooling; optionally reproduce the Go executable."""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import recovery_assets


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rebuild-engine', action='store_true')
    args = parser.parse_args()
    _, provenance = recovery_assets.tooling_files()
    if args.rebuild_engine:
        tool = os.environ.get('ONYX_GO', 'go')
        if subprocess.check_output([tool, 'version'], text=True).split()[2] != provenance['go_version']:
            raise ValueError('Use the Go version recorded in provenance.json')
        with tempfile.TemporaryDirectory(prefix='onyx-go-reproduce-') as temp:
            binary = Path(temp) / 'onyx-recovery'
            subprocess.run([tool, 'build', *provenance['go_build_flags'], '-o', str(binary), '.'],
                           cwd=HERE / 'engine', check=True,
                           env=dict(os.environ, CGO_ENABLED='0', GOOS='linux', GOARCH='arm64', GOTOOLCHAIN='local'))
            if hashlib.sha256(binary.read_bytes()).hexdigest() != provenance['sha256']['tools/onyx-recovery']:
                raise ValueError('Recovery helper does not reproduce from its recorded source and compiler')
    print('Recovery tooling, source archives and reproduction checks passed')


if __name__ == '__main__':
    main()
