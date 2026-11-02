#!/usr/bin/env python3
"""Run the actual Xiaomi thermal control paths with fault-injected host services."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent


def function(source, name):
    import re
    match = re.search(r'(?m)^(?:static )?(?:int|void|ssize_t)\s*\n?'
                      r'\s*' + name + r'\([^;]*?\)\n\{', source)
    if not match:
        raise ValueError(f'Missing actual function: {name}')
    return source[match.start():source.index('\n}', match.end()) + 2]


def verify(source):
    code = source.read_text()
    import re
    native = source.resolve().parents[4] / 'baseline/kernel/xiaomi/sm8735/drivers/thermal/xiaomi/mi_thermal_interface.c'
    abi_count = None
    if native.is_file():
        def attrs(text):
            group = text[text.index('static struct attribute *mi_thermal_dev_attr_group[]'):]
            group = group[:group.index('\n};')]
            return set(re.findall(r'&dev_attr_(\w+)\.attr', group))
        expected = attrs(native.read_text())
        if attrs(code) != expected:
            raise ValueError('Native Xiaomi thermal-message attributes changed')
        abi_count = len(expected)
    names = ['cpufreq_set_level', 'cpu_limits_set_level', 'compare_frequency',
             'destroy_thermal_cpu', 'cpu_thermal_init', 'message_string_show',
             'message_string_store', 'cpu_limits_store', 'thermal_sconfig_store',
             'thermal_board_sensor_temp_store', 'usb_online_callback',
             'of_parse_thermal_message', 'screen_state_for_thermal_callback',
             'register_thermal_panel', 'screen_state_check', 'usb_online_work',
             'mi_thermal_probe', 'mi_thermal_remove']
    template = HERE / 'tests/mi_thermal_harness.c'
    text = template.read_text().replace('/* ACTUAL_FUNCTIONS */',
                                        '\n\n'.join(function(code, n) for n in names))
    with tempfile.TemporaryDirectory(prefix='konoha-mi-thermal-test-') as directory:
        root = Path(directory)
        (root / 'test.c').write_text(text)
        subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-pthread',
                        '-Wall', '-Werror=implicit-function-declaration',
                        '-Werror=format-security', '-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer', str(root / 'test.c'),
                        '-o', str(root / 'test')], check=True)
        result = subprocess.run([str(root / 'test')], check=False,
                                text=True, capture_output=True)
        if result.returncode:
            raise RuntimeError(f'Actual-source test failed: {result.stderr}')
    report = json.loads(result.stdout)
    report.update(source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                  harness_sha256=hashlib.sha256(template.read_bytes()).hexdigest(),
                  native_attributes_preserved=abi_count,
                  sanitizers=['AddressSanitizer', 'UndefinedBehaviorSanitizer'],
                  limits='Host fault injection checks control flow and ownership; device thermal policy requires a matching kernel and runtime validation.')
    return report


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('source', type=Path)
    p.add_argument('--output', type=Path)
    args = p.parse_args()
    data = json.dumps(verify(args.source), indent=2) + '\n'
    if args.output:
        args.output.write_text(data)
    print(data, end='')
