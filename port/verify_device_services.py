#!/usr/bin/env python3
"""Run actual IR and QMI memory control paths under host fault injection."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
from verify_mi_thermal import function as control_function


def function(source, name):
    if name != "find_substream":
        return control_function(source, name)
    start = source.index("static struct snd_usb_substream *find_substream(")
    return source[start:source.index("\n}", start) + 2]

HERE = Path(__file__).resolve().parent


def verify(staged):
    groups = {
        'adsp_lifecycle': ('drivers/soc/qcom/adsp_sleepmon.c', ['adspsleepmon_driver_probe', 'adspsleepmon_driver_remove']),
        'ir_spi': ('drivers/media/rc/ir-spi.c', ['ir_spi_tx', 'ir_spi_set_tx_carrier', 'ir_spi_set_duty_cycle']),
        'usb_audio_qmi': ('sound/usb/usb_audio_qmi_svc.c', ['get_data_interval_from_si', 'info_idx_from_ifnum', 'find_substream', 'check_valid_request']),
        'memshare': ('drivers/soc/qcom/memshare/msm_memshare.c', ['check_client', 'free_client', 'memshare_reclaim', 'fill_alloc_response', 'memshare_alloc', 'shared_hyp_mapping', 'handle_alloc_generic_req', 'handle_free_generic_req']),
    }
    report = {}
    for name, (path, names) in groups.items():
        source = staged / path
        harness = HERE / 'tests' / (name + '_harness.c')
        code = harness.read_text().replace('/* ACTUAL_FUNCTIONS */', '\n\n'.join(function(source.read_text(), n) for n in names))
        if name == 'memshare':
            text = source.read_text()
            start = text.index('static inline bool is_shared_mapping(')
            helper = text[start:text.index('\n}', start) + 2]
            code = code.replace('static int check_client(', helper + '\n\nstatic int check_client(', 1)
            headers = '\n'.join((source.parent / h).read_text() for h in ['heap_mem_ext_v01.h', 'msm_memshare.h'])
            headers = re.sub(r'^#include .*$', '', headers, flags=re.M)
            code = code.replace('/* ACTUAL_HEADERS */', headers)
        if name == 'usb_audio_qmi':
            header = (source.parent / 'usb_audio_qmi_v01.h').read_text()
            start = header.index('struct qmi_uaudio_stream_req_msg_v01 {')
            code = code.replace('/* ACTUAL_HEADERS */', header[start:header.index('\n};', start) + 3])
        with tempfile.TemporaryDirectory(prefix='konoha-device-services-') as directory:
            root = Path(directory)
            (root / 'test.c').write_text(code)
            subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Werror=implicit-function-declaration', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', str(root / 'test.c'), '-o', str(root / 'test')], check=True)
            result = subprocess.run([str(root / 'test')], text=True, capture_output=True)
            if result.returncode:
                raise RuntimeError(result.stderr)
        report[name] = {**json.loads(result.stdout), 'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(), 'harness_sha256': hashlib.sha256(harness.read_bytes()).hexdigest()}
    return {'checks': report, 'sanitizers': ['AddressSanitizer', 'UndefinedBehaviorSanitizer'], 'hardware_validated': False}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('staged', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = json.dumps(verify(args.staged), indent=2) + '\n'
    if args.output:
        args.output.write_text(result)
    print(result, end='')
