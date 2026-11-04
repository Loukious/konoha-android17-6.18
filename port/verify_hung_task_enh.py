"""Exercise the actual native hung-task C with framework/PID/WALT mocks."""
from pathlib import Path
import hashlib, json, re, subprocess, tempfile

R = Path(__file__).resolve().parent
driver = (R / '../modules/drivers/soc/qcom/hung_task_enh.c').read_text()
harness = (R / 'tests/hung_task_enh_harness.c').read_text()
body = driver[driver.index('#define DEFAULT_MAX_IOWAIT_TASK'):]
code = harness.replace('/* DRIVER */', body)
with tempfile.TemporaryDirectory(prefix='onyx-hung-task-') as temp:
    work = Path(temp)
    def run(source):
        (work / 'test.c').write_text(source)
        subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-pthread', '-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer', '-fno-pie', '-no-pie', str(work / 'test.c'), '-o', str(work / 'test')], check=True)
        return subprocess.run([str(work / 'test')], capture_output=True, text=True, timeout=15)
    primary = run(code)
    assert primary.returncode == 0, primary.stderr[-3000:]
    variants = {
        'obsolete-task-storage': code.replace('task_walt(t)', '((struct walt_task_struct *)t->android_vendor_data1)').replace('task_walt(task)', '((struct walt_task_struct *)task->android_vendor_data1)'),
        'unreferenced-pid-lookup': code.replace('pid = find_get_pid(', 'pid = find_vpid('),
        'skip-task-reference-release': code.replace('put_task_struct(task);', '/* leak task reference */'),
        'skip-pid-reference-release': code.replace('put_pid(pid);', '/* leak PID reference */'),
        'panic-with-disabled-threshold': code.replace('if (limit && consecutive >= limit)', 'if (consecutive >= limit)'),
        'ignore-effective-task-state': code.replace('t->__state == TASK_UNINTERRUPTIBLE ||', 'true ||'),
        'drop-scan-count-reset': code.replace('atomic_xchg(&hung_task_enh.curr_iowait_task_cnt, 0)', 'atomic_read(&hung_task_enh.curr_iowait_task_cnt)'),
        'skip-counter-saturation': re.sub(r'static int hung_task_saturating_inc\(atomic_t \*counter\)\n\{.*?\n\}', 'static int hung_task_saturating_inc(atomic_t *counter) { return atomic_inc_return(counter); }', code, flags=re.S),
        'skip-callback-drain': code.replace('tracepoint_synchronize_unregister();', '/* callbacks not drained */'),
        'skip-hook-failure-unwind': code.replace('unregister_trace_android_vh_check_uninterruptible_tasks(\n\t\t\t\t\t\tqcom_before_check_tasks, NULL);', '/* hook retained */'),
        'ignore-walt-availability': code.replace('if (!walt_task_data_available())', 'if (false)'),
    }
    mutations = {}
    for name, source in variants.items():
        assert source != code, name
        try:
            result = run(source)
            assert result.returncode != 0, name
            mutations[name] = 'REJECTED'
        except subprocess.TimeoutExpired:
            mutations[name] = 'REJECTED: deadlock'
report = dict(status='PASS', cases=int(primary.stdout), driver_sha256=hashlib.sha256(driver.encode()).hexdigest(),
              harness_sha256=hashlib.sha256(harness.encode()).hexdigest(), sanitizers=['ASan', 'UBSan'],
              regression_mutations=mutations,
              limits=['Complete native driver C; sysctl parsing/copy, PID references, WALT storage and tracepoint RCU are mocked.',
                      'Panic policy only exercised against a returning host mock. No live hung task, panic, malformed user-copy or callback injection.'])
(R / 'hung-task-enh-host-validation.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2), flush=True)
