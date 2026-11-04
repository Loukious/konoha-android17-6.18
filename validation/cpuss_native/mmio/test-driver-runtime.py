from pathlib import Path
from datetime import datetime, timezone
import subprocess, shlex, hashlib, json, shutil, sys, re, time

workspace = Path('/home/loukious/Android/konoha-android17-6.18-port-20261031')
source = workspace.parent / 'konoha-android17-6.18-platform-providers'
root = Path(Path('/tmp/onyx-cpuss-firmware-directory.txt').read_text().strip())
artifact = Path(Path('/tmp/onyx-cpuss-mmio-artifact.txt').read_text().strip())
installed = workspace / 'artifacts/20261103T135741329740Z'
modules = [artifact / 'platform/qcom_cpuss_sleep_stats.ko', workspace / 'bazel-bin/onyx_cpuss_dt_diag/onyx_cpuss_dt_diag/onyx_cpuss_dt_diag.ko']
old = workspace / 'artifacts/20261103T143610269701Z/platform/qcom_cpuss_sleep_stats.ko'
sys.path.insert(0, str(source / 'port'))
from verify_modules import verify

verification = verify([*modules, old], [installed / 'gki/kernel_aarch64_Module.symvers', *sorted((installed / 'platform').glob('*_Module.symvers'))], (installed / 'gki/kernel.release').read_text().strip())
stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')
capture = root / ('driver-mmio-' + stamp)
capture.mkdir(mode=0o700)
adb = ['/mnt/d/platform-tools/adb.exe', '-s', '5493f3c6']
operations = []

def shell(command, check=True):
    result = subprocess.run(adb + ['shell', 'su -c ' + shlex.quote(command)], capture_output=True, text=True, timeout=20)
    operations.append({'command': command, 'returncode': result.returncode, 'output': result.stdout + result.stderr})
    if check:
        assert result.returncode == 0, operations[-1]
    return result.stdout.replace('\r', '').strip()

boot = shell('cat /proc/sys/kernel/random/boot_id')
assert boot == json.loads((root / 'acquisition.json').read_text())['boot_id']
assert shell('getenforce') == 'Enforcing'
had_old = shell('test -d /sys/module/qcom_cpuss_sleep_stats && echo yes', check=False) == 'yes'
assert not shell('readlink /sys/bus/platform/devices/17800054.cpuss-sleep-stats/driver', check=False)
stage = Path('/mnt/d/platform-tools') / ('onyx-cpuss-driver-' + stamp)
stage.mkdir()
remotes = []
for index, module in enumerate([*modules, old]):
    staged = stage / (str(index) + '-' + module.name)
    shutil.copyfile(module, staged)
    win = subprocess.check_output(['wslpath', '-w', str(staged)], text=True).strip()
    remote = '/data/local/tmp/onyx-cpuss-driver-' + stamp + '-' + str(index) + '.ko'
    subprocess.run(adb + ['push', win, remote], check=True, capture_output=True, timeout=20)
    assert shell('sha256sum ' + remote).split()[0] == verification[index]['sha256']
    remotes.append(remote)

debug_mount = '/data/local/tmp/onyx-cpuss-debugfs-' + stamp
report = {'temporary_read_only_debugfs': debug_mount, 'status': 'RUNNING', 'artifact': str(artifact), 'boot_id': boot, 'persistent_install': False, 'register_writes': False, 'module_verification': verification, 'snapshots': []}
stream = (capture / 'dmesg-live.txt').open('wb')
recorder = subprocess.Popen(adb + ['exec-out', 'su', '-c', 'dmesg -w'], stdout=stream, stderr=subprocess.STDOUT)
try:
    shell('mkdir ' + debug_mount)
    shell('mount -t debugfs -o ro debugfs ' + debug_mount)
    if had_old:
        shell('rmmod qcom_cpuss_sleep_stats')
    shell('insmod ' + remotes[1])
    for cycle in range(2):
        shell('insmod ' + remotes[0])
        assert shell('readlink /sys/bus/platform/devices/17800054.cpuss-sleep-stats/driver').endswith('/qcom_cpuss_sleep_stats')
        build_id = re.search(r'Build ID: (\w+)', subprocess.check_output(['readelf', '-n', str(modules[0])], text=True)).group(1)
        notes = subprocess.check_output(adb + ['exec-out', 'su', '-c', 'cat /sys/module/qcom_cpuss_sleep_stats/notes/.note.gnu.build-id'], timeout=20)
        assert bytes.fromhex(build_id) in notes
        for sample in range(3 if cycle == 0 else 1):
            if sample:
                time.sleep(6)
            stats = shell('cat ' + debug_mount + '/qcom_cpuss_sleep_stats/stats')
            (capture / f'stats-{cycle}-{sample}.txt').write_text(stats + '\n')
            values = {}
            for line in stats.splitlines():
                name, _, value = line.rpartition(':')
                assert value.strip().isdigit(), line
                assert name not in values
                values[name] = int(value)
            assert len(values) == 17, values
            assert all('_residency' in name for name in values)
            assert set(values) == {f'pcpu{cpu}: C{state}_residency' for cpu in range(8) for state in (3, 4)} | {'D4_residency'}
            report['snapshots'].append({'cycle': cycle, 'sample': sample, 'values': values})
        shell('rmmod qcom_cpuss_sleep_stats')
        assert not shell('readlink /sys/bus/platform/devices/17800054.cpuss-sleep-stats/driver', check=False)
        assert shell('test ! -e ' + debug_mount + '/qcom_cpuss_sleep_stats && echo removed') == 'removed'
    first, last = report['snapshots'][0]['values'], report['snapshots'][2]['values']
    report['deltas'] = {name: last[name] - value for name, value in first.items()}
    assert all(delta >= 0 for delta in report['deltas'].values())
    assert any(delta > 0 for delta in report['deltas'].values()), report['deltas']
    report['status'] = 'PASS: exact production driver binds, 17 real counters readable and advancing, two load/unload cycles clean'
    report['module_build_id'] = build_id
    report['limits'] = ['Volatile DT opt-in; persistent images still need installation and post-reboot validation.', 'No hardware reset or write was exercised; firmware count-enable configuration remains zero.']
finally:
    shell('rmmod qcom_cpuss_sleep_stats', check=False)
    shell('rmmod onyx_cpuss_dt_diag', check=False)
    assert shell('test ! -e /sys/firmware/devicetree/base/soc/cpuss-sleep-stats@17800054/qcom,config-mmio && echo removed') == 'removed'
    if had_old:
        shell('insmod ' + remotes[2])
    shell('umount ' + debug_mount, check=False)
    shell('rmdir ' + debug_mount, check=False)
    for remote in remotes:
        shell('rm -f ' + remote, check=False)
    assert shell('cat /proc/sys/kernel/random/boot_id') == boot
    assert shell('getenforce') == 'Enforcing'
    if recorder.poll() is None:
        recorder.terminate()
        try:
            recorder.wait(timeout=3)
        except subprocess.TimeoutExpired:
            recorder.kill()
            recorder.wait()
    stream.close()
    (capture / 'dmesg-after.txt').write_bytes(subprocess.check_output(adb + ['exec-out', 'su', '-c', 'dmesg'], timeout=20))
    (capture / 'operations.json').write_text(json.dumps(operations, indent=2) + '\n')
    (capture / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2), flush=True)
    print('Capture:', capture, flush=True)
