from pathlib import Path
from datetime import datetime, timezone
import subprocess, shlex, hashlib, json, re, time

workspace = Path('/home/loukious/Android/konoha-android17-6.18-port-20261031')
source = workspace.parent / 'konoha-android17-6.18-platform-providers'
attempt = Path(Path('/tmp/onyx-cpuss-mmio-install-attempt.txt').read_text().strip())
plan = json.loads((attempt / 'READY.json').read_text())
manifest = json.loads(Path(plan['images_report']).read_text())
artifact = Path(manifest['artifact'])
adb = ['/mnt/d/platform-tools/adb.exe', '-s', '5493f3c6']
operations = []

def shell(command, check=True, timeout=20):
    result = subprocess.run(adb + ['shell', 'su -c ' + shlex.quote(command)], capture_output=True, text=True, timeout=timeout)
    operations.append({'command': command, 'returncode': result.returncode, 'output': result.stdout + result.stderr})
    if check:
        assert result.returncode == 0, operations[-1]
    return result.stdout.replace('\r', '').strip()

def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

state = shell('uname -r; getprop sys.boot_completed; getprop ro.boot.slot_suffix; cat /proc/sys/kernel/random/boot_id; getenforce').splitlines()
assert state[:3] == ['6.18.38-android17-5-maybe-dirty-4k', '1', '_a'], state
assert state[3] != plan['baseline_boot_id'] and state[4] == 'Enforcing'
assert shell('readlink /sys/bus/platform/devices/17800054.cpuss-sleep-stats/driver').endswith('/qcom_cpuss_sleep_stats')
assert shell('test -e /sys/firmware/devicetree/base/soc/cpuss-sleep-stats@17800054/qcom,config-mmio && echo yes') == 'yes'
assert shell('test ! -d /sys/module/onyx_cpuss_dt_diag && test ! -d /sys/module/onyx_cpuss_diag && echo absent') == 'absent'
report = {'status': 'RUNNING', 'artifact': str(artifact), 'boot_id': state[3], 'selinux': state[4], 'slot': state[2], 'hardware_bound': True, 'persistent_install': True, 'manual_driver_load': False, 'snapshots': [], 'partition_readbacks': {}}
build_ids = {}
for name, module in [('qcom_cpuss_sleep_stats', 'qcom_cpuss_sleep_stats.ko'), ('qcom_scm', 'qcom-scm.ko'), ('qca_cld3_wcn7750', 'qca_cld3_wcn7750.ko'), ('glink_pkt', 'glink_pkt.ko')]:
    local = artifact / 'platform' / module
    assert local.is_file(), local
    build_id = re.search(r'Build ID: (\w+)', subprocess.check_output(['readelf', '-n', str(local)], text=True)).group(1)
    notes = subprocess.check_output(adb + ['exec-out', 'su', '-c', 'cat /sys/module/' + name + '/notes/.note.gnu.build-id'], timeout=20)
    assert bytes.fromhex(build_id) in notes, name
    build_ids[name] = build_id
kernel_id = re.search(r'Build ID: (\w+)', subprocess.check_output(['readelf', '-n', str(artifact / 'gki/vmlinux')], text=True)).group(1)
notes = subprocess.check_output(adb + ['exec-out', 'su', '-c', 'cat /sys/kernel/notes'], timeout=20)
assert bytes.fromhex(kernel_id) in notes
report['kernel_build_id'] = kernel_id
report['module_build_ids'] = build_ids
debug_mount = '/data/local/tmp/onyx-cpuss-persistent-debugfs-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')
try:
    shell('mkdir ' + debug_mount)
    shell('mount -t debugfs -o ro debugfs ' + debug_mount)
    for sample in range(3):
        if sample:
            time.sleep(6)
        text = shell('cat ' + debug_mount + '/qcom_cpuss_sleep_stats/stats')
        (attempt / ('cpuss-stats-' + str(sample) + '.txt')).write_text(text + '\n')
        values = {}
        for line in text.splitlines():
            name, _, value = line.rpartition(':')
            assert value.strip().isdigit() and name not in values
            values[name] = int(value)
        assert set(values) == {f'pcpu{cpu}: C{mode}_residency' for cpu in range(8) for mode in (3, 4)} | {'D4_residency'}
        report['snapshots'].append(values)
    report['counter_deltas'] = {name: report['snapshots'][-1][name] - value for name, value in report['snapshots'][0].items()}
    assert all(value >= 0 for value in report['counter_deltas'].values())
    assert any(value > 0 for value in report['counter_deltas'].values())
finally:
    shell('umount ' + debug_mount, check=False)
    shell('rmdir ' + debug_mount, check=False)

readback = attempt / 'postflash-partitions'
readback.mkdir(mode=0o700, exist_ok=True)
fresh = json.loads((attempt / 'backup-current.json').read_text())
for name in ['boot', 'vendor_boot', 'dtbo', 'system_dlkm', 'vendor_dlkm', 'recovery']:
    expected = manifest['images'][name] if name != 'recovery' else fresh['images']['recovery']
    device = '/dev/block/' + ('mapper/' if name.endswith('dlkm') else 'by-name/') + name + '_a'
    length = int(shell('blockdev --getsize64 ' + device))
    assert length == expected['size'], (name, length, expected['size'])
    path = readback / (name + '_a.img')
    if not path.exists():
        with path.open('wb') as output:
            result = subprocess.run(adb + ['exec-out', 'su', '-c', 'cat ' + device], stdout=output, stderr=subprocess.PIPE, timeout=60)
        assert result.returncode == 0, result.stderr
    else:
        assert shell('sha256sum ' + device, timeout=60).split()[0] == expected['sha256']
    assert path.stat().st_size == length and sha(path) == expected['sha256'], name
    report['partition_readbacks'][name] = expected['sha256']
    print('readback', name, 'PASS', flush=True)

loaders = json.loads(Path(manifest['loader_manifest']).read_text())
for name, item in loaders['files'].items():
    assert shell('sha256sum ' + loaders['device_directory'] + '/' + name).split()[0] == item['sha256']
report['loader_manifest_sha256'] = manifest['loader_manifest_sha256']
report['bluetooth_state'] = shell('dumpsys bluetooth_manager | head -n 18')
report['wifi_state'] = shell('cmd wifi status')
assert shell('cat /proc/sys/kernel/random/boot_id') == state[3]
assert shell('getenforce') == 'Enforcing'
log = subprocess.check_output(adb + ['exec-out', 'su', '-c', 'dmesg'], timeout=20).decode(errors='replace')
(attempt / 'dmesg-postflash.txt').write_text(log)
clean = re.sub(r'\x1b\[[0-9;]*m', '', log)
report['cpuss_probe_messages'] = [line for line in clean.splitlines() if '17800054.cpuss-sleep-stats' in line]
assert not any('failed with error' in line or 'Cannot read' in line for line in report['cpuss_probe_messages'])
report['new_kernel_faults'] = [line for line in clean.splitlines() if re.search(r'\]\s+(?:Kernel panic(?: - not syncing)?:|Unable to handle kernel|BUG:)', line)]
assert not report['new_kernel_faults'], report['new_kernel_faults']
report['status'] = 'PASS: persistent native CPUSS driver loads automatically, binds, exposes 17 truthful residency counters with advancement; six partition readbacks and matching kernel/module/loader identities verified'
report['limits'] = ['No counter reset or firmware write was exercised.', 'CPU/L3 count-enable configuration remains zero; only configured residency statistics are exposed.', 'The short awake observation need not advance cluster D4; suspend endurance and hardware rollover remain outside this fix.', 'Debugfs remains available through an explicit diagnostic mount; no ROM mount policy was changed.']
(attempt / 'RESULT.json').write_text(json.dumps(report, indent=2) + '\n')
(attempt / 'persistent-verification-operations.json').write_text(json.dumps(operations, indent=2) + '\n')
target = source / 'validation/cpuss_native/mmio/persistent-runtime.json'
public_report = dict(report)
public_report['wifi_connected'] = 'Wifi is connected to' in public_report.pop('wifi_state')
bluetooth = public_report.pop('bluetooth_state')
public_report['bluetooth_state'] = 'ON' if re.search(r'\bState:\s+ON\b', bluetooth) else 'see private capture'
target.write_text(json.dumps(public_report, indent=2) + '\n')
plan['status'] = 'CONSUMED: native CPUSS MMIO read fix installed and post-reboot hardware statistics validated'
(attempt / 'READY.json').write_text(json.dumps(plan, indent=2) + '\n')
Path('/tmp/onyx-current-installed-attempt.txt').write_text(str(attempt) + '\n')
print(json.dumps(report, indent=2), flush=True)
