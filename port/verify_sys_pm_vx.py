from pathlib import Path
import hashlib, json, re, subprocess, tempfile
R = Path(__file__).resolve().parent
driver = (R / '../modules/drivers/soc/qcom/sys_pm_vx.c').read_text()
harness = (R / 'tests/sys_pm_vx_harness.c').read_text()
code = harness.replace('/* DRIVER */', re.sub(r'(?m)^#include[^\n]*\n', '', driver))
with tempfile.TemporaryDirectory(prefix='onyx-sys-pm-vx-') as temp:
    d = Path(temp)
    def run(source):
        (d / 'test.c').write_text(source)
        subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-pthread', '-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer', '-fno-pie', '-no-pie', str(d / 'test.c'), '-o', str(d / 'test')], check=True)
        return subprocess.run([str(d / 'test')], capture_output=True, text=True, timeout=25)
    result = run(code); assert not result.returncode, result.stderr
    mutations = {}
    variants = {
        'skip-firmware-extent-bound': code.replace('if (extent > pd->size)', 'if (false)'),
        'accept-invalid-timestamp-shift': code.replace('if (hdr->flags.ts_shift >= 32)', 'if (false)'),
        'truncate-shifted-timestamp': code.replace('(u64)timestamp << hdr->flags.ts_shift', '(u32)(timestamp << hdr->flags.ts_shift)'),
        'use-cxpc-count-for-aoss-blocker-check': code.replace('for (i = 0; i < n_drv; i++)', 'for (i = 0; i < pd->n_cxpc_drv; i++)'),
        'publish-instance-before-probe-success': code.replace('mutex_init(&pd->lock);', 'g_pd = pd; mutex_init(&pd->lock);'),
        'retain-removed-global-instance': code.replace('if (g_pd == pd)\n\t\tg_pd = NULL;', 'if (false)\n\t\tg_pd = NULL;'),
    }
    for name, source in variants.items():
        assert source != code, name
        r = run(source); assert r.returncode, name; mutations[name] = 'REJECTED'
report = dict(status='PASS', cases=int(result.stdout), driver_sha256=hashlib.sha256(driver.encode()).hexdigest(),
    harness_sha256=hashlib.sha256(harness.encode()).hexdigest(), sanitizers=['ASan', 'UBSan'], regression_mutations=mutations,
    limits=['Complete actual native driver, parser, formatting, sysfs, QMP state machine, probe and removal C; kernel frameworks simulated.',
            'Firmware has no whole-log sequence protocol; header recheck detects some updates, not globally atomic logs.',
            'Crash-dump QMP branch exercised only against a host mock, never on the phone.'])
(R / 'sys-pm-vx-host-validation.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
