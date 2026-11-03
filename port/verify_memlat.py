#!/usr/bin/env python3
"""Exercise the actual native memlat C with failing PMU/DCVS/SCMI providers."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
SOURCE = 'drivers/soc/qcom/dcvs/memlat.c'


def function(source, name):
    match = re.search(r'(?m)^static [^\n;{}]*\b' + name + r'\([^;{}]*\)\n\{', source)
    if not match:
        raise ValueError('Missing function: ' + name)
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


def verify(staged):
    path = Path(staged) / SOURCE
    source = path.read_text()
    declarations = source[source.index('#define MAX_MEMLAT_GRPS'):source.index('static struct memlat_dev_data')]
    funcs = '\n'.join(function(source, name) for name in (
        'cpufreq_to_memfreq', 'memlat_counter_delta', 'memlat_percent',
        'calculate_sampling_stats', 'set_higher_freq', 'apply_adaptive_freq',
        'calculate_mon_sampling_freq', 'update_memlat_fp_vote', 'memlat_update_work',
        'memlat_hrtimer_handler', 'process_raw_ctrs', 'memlat_pmu_idle_cb',
        'memlat_sched_tick_cb', 'get_mask_and_mpidr_from_pdev',
        'init_cpufreq_memfreq_map', 'store_min_freq', 'store_max_freq',
        'update_cpucp_sample_ms', 'store_sample_ms', 'memlat_sampling_init',
        'memlat_stop', 'memlat_voter_cleanup'))
    tunables = source[source.index('#define store_attr('):source.index('#define show_grp_attr')]
    tunables += source[source.index('#define store_grp_attr('):source.index('static ssize_t store_min_freq')]
    harness = (HERE / 'tests/memlat_harness.c').read_text()
    harness = harness.replace('/* INSERT_DECLARATIONS */', declarations)
    harness = harness.replace('/* INSERT_FUNCTIONS */', funcs)
    harness = harness.replace('/* INSERT_TUNABLES */', tunables)
    # Guards on the dispatchers also protect the SCMI protocol lifetime.
    for name in ('attr_show', 'attr_store'):
        body = function(source, name)
        assert 'mutex_lock(&memlat_lock)' in body and 'memlat_data->stopping' in body
        assert '!memlat_data->inited' in body
    with tempfile.TemporaryDirectory(prefix='konoha-memlat-') as directory:
        c = Path(directory) / 'memlat.c'
        exe = c.with_suffix('')
        c.write_text(harness)
        subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-Wno-sign-compare', '-g', '-O1',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        str(c), '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], text=True, capture_output=True, check=True)
    return {'status': 'PASS', 'cases': int(result.stdout.strip()),
            'sanitizers': 'ASan+UBSan', 'source_sha256': {
                SOURCE: hashlib.sha256(path.read_bytes()).hexdigest()},
            'limits': ['Mocked firmware/providers validate C control flow; device votes and timing need hardware validation.']}


if __name__ == '__main__':
    print(json.dumps(verify(sys.argv[1]), indent=2))
