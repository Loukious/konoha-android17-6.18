from pathlib import Path
from datetime import datetime, timezone
import hashlib
import json
import re
import subprocess
import time
import shlex
import sys

work = Path('/home/loukious/Android/konoha-android17-6.18-port-20261031')
backup = Path(Path('/tmp/onyx-current-backup.txt').read_text().strip())
stage = Path('/mnt/d/platform-tools/onyx-618-cpuss-config-mmio-20261103T153750Z')
report_path = Path('/home/loukious/Android/konoha-android17-6.18-port-20261031/packaging/experimental-images-cpuss-mmio-20261103T153808728566Z-serial/report.json')
adb = ['/mnt/d/platform-tools/adb.exe', '-s', '5493f3c6']
fastboot = ['/mnt/d/platform-tools/fastboot.exe', '-s', '5493f3c6']
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
manifest = json.loads(report_path.read_text())
loader_manifest = json.loads(Path(manifest['loader_manifest']).read_text())
original = json.loads((backup / 'backup-manifest.json').read_text())
receipt = {'serial': '5493f3c6', 'slot': 'a', 'timestamp': datetime.now(timezone.utc).isoformat(),
           'image_report': str(report_path), 'image_report_sha256': sha(report_path), 'backup': str(backup),
           'status': 'preflight', 'operations': []}
receipt_path = Path('/home/loukious/Android/konoha-android17-6.18-port-20261031/device-backups/5493f3c6-20261101T160034Z/logging-investigation/cpuss-config-mmio-20261103T153750Z/installation-recovery-bridge-retry1.json')
assert not receipt_path.exists()

def save():
    receipt_path.write_text(json.dumps(receipt, indent=2) + '\n')
    receipt_path.chmod(0o600)

def run(tool, args, timeout=60):
    print(tool[0], ' '.join(args), flush=True)
    result = subprocess.run(tool + args, capture_output=True, text=True, timeout=timeout)
    output = result.stdout + result.stderr
    print(output.strip(), flush=True)
    receipt['operations'].append({'args': args, 'returncode': result.returncode, 'output': output})
    save()
    if result.returncode:
        raise RuntimeError(output)
    return output

def var(name):
    output = run(fastboot, ['getvar', name])
    match = re.search(re.escape(name) + r':\s*(\S+)', output)
    assert match, (name, output)
    return match.group(1)

assert original['status'].startswith('PASS') and len(original['images']) == 9
for item in original['images'].values():
    assert sha(Path(item['path'])) == item['sha256']
assert manifest['diagnostic_boot'] and manifest['diagnostic_serial_module_loading']
assert manifest['diagnostic_gpi_checkpoints']
assert manifest['diagnostic_boot_checkpoints'] and manifest['diagnostic_funnel_checkpoints'] and not manifest['diagnostic_module_blacklist']
assert sha(report_path) == 'eb5f40e1b4fb3c7856c72c8e92c48184de75220ea69a0f3c56b1734392715016'
assert sha(Path(manifest['loader_manifest'])) == manifest['loader_manifest_sha256']
for name, item in manifest['images'].items():
    path = stage / 'images' / (name + '.img')
    assert path.stat().st_size == item['size'] and sha(path) == item['sha256']
state = json.loads((receipt_path.parent / 'preflash-state.json').read_text())['state']
assert state[0].startswith('6.18.') and state[1:3] == ['1', '_a']
receipt['preflash_state'] = state
fresh = json.loads((receipt_path.parent / 'backup-current.json').read_text())
assert fresh['status'].startswith('PASS: six') and len(fresh['images']) == 6
assert state[3] == fresh['boot_id']
for image in fresh['images'].values():
    assert Path(image['path']).stat().st_size == image['size']
    assert sha(Path(image['path'])) == image['sha256']
receipt['fresh_backup'] = str(receipt_path.parent / 'backup-current.json')
previous_report = Path('/home/loukious/Android/konoha-android17-6.18-port-20261031/packaging/experimental-images-hotspot-wiphy-lock-20261103T135741329740Z-serial/report.json')
previous = json.loads(previous_report.read_text())
assert sha(previous_report) == '607264a4ea9236ae8fc119942a7b4e0bb0e6b3efedb41a9d82628a8382cd589d'
assert manifest['loader_manifest_sha256'] == previous['loader_manifest_sha256']
receipt['metadata_verification'] = 'Native CPUSS HLOS configuration reads selected by one DT boolean; exact production module passed binding, 17 residency counter reads and unload/probe cycles; matched 373-module images; existing platform module bytes and boot command line, bootconfig, loaders and corrected recovery retained; fresh six-partition rollback'
backup_stage = Path('/mnt/d/platform-tools/onyx-618-20261101/backup')
for name in ['boot', 'vendor_boot', 'dtbo']:
    path = backup_stage / (name + '_a.img')
    assert path.stat().st_size == original['images'][name]['bytes']
    assert sha(path) == original['images'][name]['sha256']
recovery_report = Path('/home/loukious/Android/konoha-android17-6.18-port-20261031/packaging/recovery-init-fix-20261102/report.json')
recovery_manifest = json.loads(recovery_report.read_text())
assert sha(recovery_report) == '1551157837a1f3bfb05a5b9ce625281834798f22aa333b061d3a5a7d2afcc4ae'
assert sha(stage / 'recovery.img') == recovery_manifest['sha256']
assert recovery_manifest['only_changed_cpio_record'] == 'system/bin/init'
assert recovery_manifest['cpio_paths_unchanged']
if '--verify-only' in sys.argv:
    print('PREPARATION_PASS: images, loader identity, preflash state and physical recovery bridge hashes verified')
    sys.exit(0)
assert var('product') == 'onyx'
assert var('current-slot') == 'a'
assert var('unlocked') == 'yes'
assert var('is-userspace') == 'yes'
assert var('snapshot-update-status') == 'none'
for name, item in manifest['images'].items():
    size = int(var('partition-size:'+name+'_a'), 16)
    assert size >= item['size']
    if name in ['system_dlkm', 'vendor_dlkm']:
        assert size == item['size'] == original['images'][name]['bytes']
before = stage / 'preflash-vendor-boot.img'
winpath = subprocess.check_output(['wslpath', '-w', str(before)], text=True).strip()
run(fastboot, ['fetch', 'vendor_boot_a', winpath])
assert sha(before) == previous['images']['vendor_boot']['sha256']
receipt['status'] = 'preflight passed; flashing'
save()
for name in ['system_dlkm', 'vendor_dlkm', 'dtbo', 'vendor_boot', 'boot']:
    if manifest['images'][name]['sha256'] == previous['images'][name]['sha256']:
        continue
    path = stage / 'images' / (name + '.img')
    winpath = subprocess.check_output(['wslpath', '-w', str(path)], text=True).strip()
    run(fastboot, ['flash', name + '_a', winpath])
assert var('current-slot') == 'a'
readback = stage / 'vendor-boot-readback-serial.img'
winpath = subprocess.check_output(['wslpath', '-w', str(readback)], text=True).strip()
run(fastboot, ['fetch', 'vendor_boot_a', winpath])
assert sha(readback) == manifest['images']['vendor_boot']['sha256']
receipt['vendor_boot_readback_sha256'] = sha(readback)
assert int(var('partition-size:recovery_a'), 16) == recovery_manifest['size']
assert sha(stage / 'recovery.img') == fresh['images']['recovery']['sha256']
receipt['recovery_sha256'] = recovery_manifest['sha256']
receipt['status'] = 'changed candidate images flashed; vendor_boot readback verified; corrected recovery retained; reboot pending'
save()
print('FLASH_PASS', receipt_path, flush=True)
