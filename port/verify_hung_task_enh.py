"""Exercise the actual native hung-task C with framework/PID/WALT mocks."""
from pathlib import Path
import argparse, hashlib, json, os, re, subprocess, tempfile

R = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--tsan-cc', help='Also check concurrent WALT publication with this C compiler')
args = parser.parse_args()
driver = (R / '../modules/drivers/soc/qcom/hung_task_enh.c').read_text()
walt = (R / '../modules/kernel/sched/walt/walt.c').read_text()
harness = (R / 'tests/hung_task_enh_harness.c').read_text()
body = driver[driver.index('#define DEFAULT_MAX_IOWAIT_TASK'):]
availability = re.search(r'bool walt_task_data_available\(void\)\n\{.*?\n\}', walt, re.S).group()
stop_handler = re.search(r'static int walt_init_stop_handler\(void \*data\)\n\{.*?\n\}', walt, re.S).group()
publication = re.search(r'\bsmp_store_release\(&walt_disabled, false\);', stop_handler).group()
code = harness.replace('/* DRIVER */', body).replace('/* WALT_AVAILABILITY */', availability).replace('/* WALT_PUBLICATION */', publication)
with tempfile.TemporaryDirectory(prefix='onyx-hung-task-') as temp:
    work = Path(temp)
    def run(source, sanitizer='address,undefined', compiler='cc'):
        (work / 'test.c').write_text(source)
        subprocess.run([compiler, '-std=gnu11', '-O1', '-g', '-pthread', '-fsanitize=' + sanitizer,
                        '-fno-omit-frame-pointer', '-fno-pie', '-no-pie', str(work / 'test.c'), '-o', str(work / 'test')], check=True)
        env = dict(os.environ, TSAN_OPTIONS='halt_on_error=1')
        return subprocess.run([str(work / 'test')], capture_output=True, text=True, timeout=15, env=env)
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
        'ignore-callback-walt-availability': code.replace('if (!walt_task_data_available())\n\t\treturn;', 'if (false)\n\t\treturn;'),
        'ignore-sysctl-walt-availability': code.replace('if (!walt_task_data_available())\n\t\treturn -EAGAIN;', 'if (false)\n\t\treturn -EAGAIN;'),
        'reject-delayed-walt-at-module-load': code.replace('static int __init hung_task_enh_init(void)\n{', 'static int __init hung_task_enh_init(void)\n{\n\tif (!walt_task_data_available()) return -ENODEV;'),
        'disable-native-scanner-before-walt': code.replace('if (!walt_task_data_available())\n\t\treturn;', 'if (!walt_task_data_available()) { *need_check = false; return; }'),
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
    thread_report = None
    if args.tsan_cc:
        concurrent = run(code, 'thread', args.tsan_cc)
        assert concurrent.returncode == 0, concurrent.stderr[-4000:]
        thread_mutations = {}
        for name, source in {
            'unpublished-task-initialization': code.replace('smp_store_release(&walt_disabled, false);', 'WRITE_ONCE(walt_disabled, false);'),
            'unacquired-task-initialization': code.replace('smp_load_acquire(&walt_disabled)', 'READ_ONCE(walt_disabled)'),
        }.items():
            result = run(source, 'thread', args.tsan_cc)
            assert result.returncode != 0 and 'WARNING: ThreadSanitizer: data race' in result.stderr, (name, result.stderr[-4000:])
            thread_mutations[name] = 'REJECTED: ThreadSanitizer data race'
        thread_report = dict(status='PASS', cases=int(concurrent.stdout), compiler=args.tsan_cc,
                             regression_mutations=thread_mutations)
report = dict(status='PASS', cases=int(primary.stdout), driver_sha256=hashlib.sha256(driver.encode()).hexdigest(),
              walt_sha256=hashlib.sha256(walt.encode()).hexdigest(),
              walt_availability_sha256=hashlib.sha256(availability.encode()).hexdigest(),
              walt_publication=publication,
              harness_sha256=hashlib.sha256(harness.encode()).hexdigest(), sanitizers=['ASan', 'UBSan'],
              regression_mutations=mutations,
              thread_sanitizer=thread_report,
              readiness_cases=['load before WALT initialization', 'unload while WALT stays unavailable',
                               'preserve native scanner before readiness', 'per-task sysctl returns EAGAIN before readiness',
                               'activate task controls and I/O policy after readiness without reload',
                               'concurrent initialization publication with actual WALT getter and release store'],
              limits=['Complete native driver C and actual WALT readiness getter/publication; sysctl parsing/copy, PID references, WALT allocation and tracepoint RCU are mocked.',
                      'Panic policy only exercised against a returning host mock. No live hung task, panic, malformed user-copy or callback injection.'])
(R / 'hung-task-enh-host-validation.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2), flush=True)
