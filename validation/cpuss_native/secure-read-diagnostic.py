from pathlib import Path
from datetime import datetime, timezone
import subprocess, shlex, json, hashlib, re

workspace = Path('/home/loukious/Android/konoha-android17-6.18-port-20261031')
source = workspace.parent / 'konoha-android17-6.18-platform-providers'
artifact = workspace / 'artifacts/20261103T135741329740Z'
module = artifact / 'platform/qcom-scm.ko'
attempt = Path(Path('/tmp/onyx-current-installed-attempt.txt').read_text().strip())
capture = attempt / ('cpuss-scm-investigation-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ'))
capture.mkdir(mode=0o700)
adb = ['/mnt/d/platform-tools/adb.exe', '-s', '5493f3c6']
group = 'konoha_cpuss_scm'
instance = '/sys/kernel/tracing/instances/' + group
operations, created = [], []

def run(command, check=True):
    result = subprocess.run(adb + ['shell', 'su -c ' + shlex.quote(command)], capture_output=True, text=True, timeout=15)
    operations.append({'command': command, 'returncode': result.returncode, 'output': result.stdout + result.stderr})
    if check:
        assert result.returncode == 0, operations[-1]
    return result.stdout.replace('\r', '')

boot = run('cat /proc/sys/kernel/random/boot_id').strip()
assert boot == json.loads((attempt / 'RESULT.json').read_text())['boot_id']
build_id = re.search(r'Build ID: (\w+)', subprocess.check_output(['readelf', '-n', str(module)], text=True)).group(1)
notes = subprocess.check_output(adb + ['exec-out', 'su', '-c', 'cat /sys/module/qcom_scm/notes/.note.gnu.build-id'], timeout=15)
assert bytes.fromhex(build_id) in notes
objdump = workspace / 'prebuilts/clang/host/linux-x86/clang-r584948c/bin/llvm-objdump'
assembly = subprocess.check_output([str(objdump), '--disassemble', '--disassemble-symbols=__scm_smc_call', str(module)], text=True)
(capture / 'scm-disassembly.txt').write_text(assembly)
assert '80a0: f85a83a8' in assembly and 'ldur\tx8, [x29, #-0x58]' in assembly
assert '<__scm_smc_call+0x118>' in assembly
assert not run('readlink /sys/bus/platform/devices/17800054.cpuss-sleep-stats/driver', check=False).strip()
events = [
    ('entry', 'p', '__scm_smc_call', 'svc=+0($arg2):u32 cmd=+4($arg2):u32 arginfo=+8($arg2):u32 addr=+16($arg2):x64 convention=$arg3:u32 call_type=$arg5:u32', 'svc == 5 && cmd == 1 && addr == 0x17800054'),
    ('raw', 'p', '__scm_smc_call+0x11c', 'raw_a0=-88(%x29):s64 function_id=%x21:x64 addr=%x27:x64 arginfo=%x28:x64', 'addr == 0x17800054 && function_id == 0xc2000501'),
    ('return', 'r64', 'qcom_scm_io_readl', 'addr=$arg1:x64 linux_errno=$retval:s32', 'addr == 0x17800054'),
]
instance_created = False
try:
    prior = run('cat /sys/kernel/tracing/kprobe_events')
    (capture / 'previous-kprobe-events.txt').write_text(prior)
    assert group not in prior
    run('mkdir ' + instance)
    instance_created = True
    run('printf 0 > ' + instance + '/tracing_on')
    for name, prefix, symbol, arguments, condition in events:
        event = f'{prefix}:{group}/{name} qcom_scm:{symbol} {arguments}\n'
        run("printf '%s' " + shlex.quote(event) + ' >> /sys/kernel/tracing/kprobe_events')
        created.append(name)
        run("printf '%s' " + shlex.quote(condition) + ' > ' + instance + '/events/' + group + '/' + name + '/filter')
    run('printf 1 > ' + instance + '/events/' + group + '/enable')
    run('printf 1 > ' + instance + '/tracing_on')
    run("printf '%s' '17800054.cpuss-sleep-stats' > /sys/bus/platform/drivers/qcom_cpuss_sleep_stats/bind", check=False)
    run('printf 0 > ' + instance + '/tracing_on')
    trace = run('cat ' + instance + '/trace')
    (capture / 'trace.txt').write_text(trace)
    print(trace, flush=True)
    assert run('cat /proc/sys/kernel/random/boot_id').strip() == boot
    assert run('getenforce').strip() == 'Enforcing'
finally:
    if instance_created:
        run('printf 0 > ' + instance + '/tracing_on', check=False)
        run('printf 0 > ' + instance + '/events/' + group + '/enable', check=False)
    for name in reversed(created):
        run("printf '%s' " + shlex.quote('-:' + group + '/' + name + '\n') + ' >> /sys/kernel/tracing/kprobe_events', check=False)
    if instance_created:
        run('rmdir ' + instance, check=False)
    (capture / 'operations.json').write_text(json.dumps(operations, indent=2) + '\n')
    print('Capture:', capture, flush=True)
