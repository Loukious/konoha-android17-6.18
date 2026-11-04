#!/usr/bin/env python3
"""Run the prepared installer with real image hashes and guarded fastboot mocks."""
from pathlib import Path
import ast
import contextlib
import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile

H = Path('/home/loukious/Android/konoha-android17-6.18-port-20261031/device-backups/5493f3c6-20261101T160034Z/logging-investigation/cpuss-config-mmio-20261103T153750Z')
attempt = Path(Path('/tmp/onyx-cpuss-mmio-install-attempt.txt').read_text().strip())
plan = json.loads((attempt / 'READY.json').read_text())
source_path = attempt / 'install-fastbootd-retry1.py'
source = source_path.read_text()
current = json.loads(Path(plan['previous_report']).read_text())
candidate = json.loads(Path(plan['images_report']).read_text())
original_run, original_check, original_argv = subprocess.run, subprocess.check_output, sys.argv
cases = ['success', 'wrong-product', 'wrong-slot', 'locked', 'not-userspace',
         'snapshot-active', 'wrong-size', 'preflash-mismatch', 'flash-failure', 'readback-mismatch']
reports = []

for case in cases:
    with tempfile.TemporaryDirectory(prefix='onyx-ipa-installer-') as tmp:
        root = Path(tmp)
        stage = root / 'stage'
        (stage / 'images').mkdir(parents=True)
        for name in candidate['images']:
            os.symlink(Path(candidate['images'][name]['path']), stage / 'images' / (name + '.img'))
        os.symlink('/home/loukious/Android/konoha-android17-6.18-port-20261031/packaging/recovery-init-fix-20261102/recovery.img', stage / 'recovery.img')
        shutil.copyfile(attempt / 'preflash-state.json', root / 'preflash-state.json')
        shutil.copyfile(attempt / 'backup-current.json', root / 'backup-current.json')
        # Only adapt external receipt/stage locations; all installer logic and
        # image/hash/preflash checks are from the prepared production script.
        tree = ast.parse(source)
        replaced = []
        for node in tree.body:
            if isinstance(node, ast.Assign) and len(node.targets) == 1 and isinstance(node.targets[0], ast.Name):
                name = node.targets[0].id
                if name in ['stage', 'receipt_path']:
                    location = stage if name == 'stage' else root / 'receipt.json'
                    node.value = ast.Call(func=ast.Name(id='Path', ctx=ast.Load()),
                                          args=[ast.Constant(str(location))], keywords=[])
                    replaced.append(name)
        assert sorted(replaced) == ['receipt_path', 'stage']
        ast.fix_missing_locations(tree)
        calls = []
        fetches = 0

        def mock_run(args, **kwargs):
            global fetches
            assert args[:3] == ['/mnt/d/platform-tools/fastboot.exe', '-s', '5493f3c6'], args
            op = args[3:]
            calls.append(op)
            assert op[0] in ['getvar', 'fetch', 'flash'], op
            if op[0] == 'getvar':
                name = op[1]
                values = {'product': 'other' if case == 'wrong-product' else 'onyx',
                          'current-slot': 'b' if case == 'wrong-slot' else 'a',
                          'unlocked': 'no' if case == 'locked' else 'yes',
                          'is-userspace': 'no' if case == 'not-userspace' else 'yes',
                          'snapshot-update-status': 'merging' if case == 'snapshot-active' else 'none',
                          'partition-size:recovery_a': hex((stage / 'recovery.img').stat().st_size)}
                for partition, item in candidate['images'].items():
                    size = item['size']
                    if case == 'wrong-size' and partition == 'vendor_dlkm':
                        size -= 4096
                    values['partition-size:' + partition + '_a'] = hex(size)
                assert name in values, name
                return subprocess.CompletedProcess(args, 0, '', name + ': ' + values[name] + '\n')
            if op[0] == 'fetch':
                assert op[1] == 'vendor_boot_a'
                dest = Path(op[2])
                assert dest.parent == stage
                if (fetches == 0 and case == 'preflash-mismatch') or (fetches == 1 and case == 'readback-mismatch'):
                    dest.write_bytes(b'wrong image')
                else:
                    report = current if fetches == 0 else candidate
                    shutil.copyfile(Path(report['images']['vendor_boot']['path']), dest)
                fetches += 1
                return subprocess.CompletedProcess(args, 0, '', 'OKAY\n')
            assert op[0] == 'flash'
            assert op[1] in [name + '_a' for name in candidate['images']] + ['recovery_a']
            assert Path(op[2]).is_relative_to(stage)
            failed = case == 'flash-failure' and op[1] == 'vendor_dlkm_a'
            return subprocess.CompletedProcess(args, int(failed), '', 'FAILED injected flash error' if failed else 'OKAY\n')

        def mock_check(args, **kwargs):
            assert args[:2] == ['wslpath', '-w'], args
            return str(args[2]) + '\n'

        subprocess.run, subprocess.check_output = mock_run, mock_check
        sys.argv = [str(source_path)]
        error = None
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                exec(compile(tree, str(source_path), 'exec'), {'__name__': '__main__'})
        except (AssertionError, RuntimeError) as exc:
            error = type(exc).__name__
        finally:
            subprocess.run, subprocess.check_output, sys.argv = original_run, original_check, original_argv
        writes = [op[1] for op in calls if op[0] == 'flash']
        if case == 'success':
            assert error is None and writes == [n + '_a' for n in plan['changed_images']]
            receipt = json.loads((root / 'receipt.json').read_text())
            assert receipt['status'].startswith('changed candidate images flashed')
            assert receipt['vendor_boot_readback_sha256'] == candidate['images']['vendor_boot']['sha256']
        elif case == 'flash-failure':
            assert error and writes == [n + '_a' for n in plan['changed_images'][:plan['changed_images'].index('vendor_dlkm') + 1]]
        elif case == 'readback-mismatch':
            assert error and len(writes) == len(plan['changed_images']) and 'recovery_a' not in writes
        else:
            assert error and not writes
        assert not any(op[0] == 'reboot' for op in calls)
        reports.append({'case': case, 'status': 'PASS', 'exception': error, 'writes': writes})

report = {'status': 'PASS', 'source': str(source_path),
          'source_sha256': hashlib.sha256(source_path.read_bytes()).hexdigest(), 'cases': reports,
          'limits': 'Prepared installer logic and real image/backup hashes, with temporary stage/receipt paths and simulated fastboot. No device writes, protocol validation or boot test.'}
(H / 'installer-tests.json').write_text(json.dumps(report, indent=2) + '\n')
print('PASS: 10 installer preflight, write/readback failure and success cases; no device writes')
