#!/usr/bin/env python3
"""Compile and run the independently reviewed native guard regression cases."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
FLAGS = ['-nostdlib', '-static', '-ffreestanding', '-fno-builtin',
         '-fno-stack-protector', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
         '-fno-ident', '-Os', '-Wall', '-Wextra', '-Werror',
         '-Wl,--build-id=none', '-Wl,-z,noexecstack']

with tempfile.TemporaryDirectory(prefix='konoha-guard-test-') as temporary:
    root = Path(temporary)
    for name in ['recovery-guard.c', 'build_guard.sh']:
        shutil.copyfile(HERE / name, root / name)
    shutil.copyfile(HERE / 'tests/guard_cases.py', root / 'test.py')
    subprocess.run([os.environ.get('CC', 'cc'), *FLAGS, '-DGUARD_TEST',
                    str(root / 'recovery-guard.c'), '-o', str(root / 'guard-test')], check=True)
    subprocess.run(['sh', str(root / 'build_guard.sh')], check=True)
    shutil.copyfile(root / 'tools/recovery-guard', root / 'guard-aarch64')
    subprocess.run(['python3', str(root / 'test.py')], check=True)
    shutil.copyfile(root / 'review.json', HERE / 'guard-tests.json')
    if (HERE / 'tests/dm_cases.py').is_file():
        subprocess.run(['python3', str(HERE / 'tests/dm_cases.py'),
                        str(root / 'guard-test'), str(root)], check=True)
