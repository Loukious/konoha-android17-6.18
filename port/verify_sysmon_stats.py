from pathlib import Path
import hashlib, json, re, subprocess, tempfile
R = Path(__file__).resolve().parent
driver = (R / '../modules/drivers/soc/qcom/sysmon_subsystem_stats.c').read_text()
header = (R / '../modules/include/linux/soc/qcom/sysmon_subsystem_stats.h').read_text()
harness = (R / 'tests/sysmon_stats_harness.c').read_text()
code = harness.replace('/* HEADER */', header).replace('/* DRIVER */', re.sub(r'(?m)^#include[^\n]*\n', '', driver))
with tempfile.TemporaryDirectory(prefix='onyx-sysmon-stats-') as temp:
    d = Path(temp)
    def run(source):
        (d / 'test.c').write_text(source)
        subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-pthread', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie', str(d / 'test.c'), '-o', str(d / 'test')], check=True)
        return subprocess.run([str(d / 'test')], capture_output=True, text=True, timeout=20)
    result = run(code); assert not result.returncode, result.stderr
    mutations = {}
    variants = {
        'accept-short-feature-payload': code.replace('if (length < required)', 'if (false)'),
        'skip-known-record-duplicate-check': code.replace('if (seen & BIT(feature))', 'if (false)'),
        'drop-hmx-null-check': code.replace('if (!hmx_util)', 'if (false)'),
        'write-firmware-sleep-counter': code.replace('lpm = snap->lpm;', '((struct sleep_stats *)qcom_smem_get(dsp_id, dsp_id == ADSP ? 606 : dsp_id == CDSP ? 607 : 608, &(size_t){0}))->accumulated++; lpm = snap->lpm;'),
        'wrap-residency-accumulation': code.replace('return delta > U32_MAX - value ? U32_MAX : value + delta;', 'return value + delta;'),
    }
    for name, source in variants.items():
        r = run(source); assert r.returncode, name; mutations[name] = 'REJECTED'
report = dict(status='PASS', cases=int(result.stdout), driver_sha256=hashlib.sha256(driver.encode()).hexdigest(), header_sha256=hashlib.sha256(header.encode()).hexdigest(), harness_sha256=hashlib.sha256(harness.encode()).hexdigest(), sanitizers=['ASan', 'UBSan'], regression_mutations=mutations,
    limits=['Complete actual native API/show/initialization C; SMEM, debugfs, clock and Linux module lifetime are simulated.', 'Firmware samples lack a sequence protocol and can span a concurrent DSP update.', 'Normal live reads and Linux lifetime remain separate validation gates; no intentional SSR or firmware mutation.'])
(R / 'sysmon-stats-host-validation.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
