#!/usr/bin/env python3
"""Test the actual VBAT monitor's hysteresis, failure paths and work teardown."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
SOURCE = "drivers/power/supply/mca/mca_hardware_monitor/mca_vbat_ovp_monitor.c"


def function(source, name):
    match = re.search(r"(?m)^static [^\n;{}]*\b" + name + r"\([^;{}]*\)\n\{", source)
    if not match:
        raise ValueError("Missing function: " + name)
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def verify(staged):
    path = Path(staged) / SOURCE
    source = path.read_text()
    functions = "\n".join(function(source, name) for name in (
        "mca_vbat_mon_get_vbat_ovp_status", "mca_vbat_ovp_monitor_workfunc",
        "mca_vbat_ovp_mon_stop", "mca_vbat_ovp_mon_probe"))
    harness = (HERE / "tests/vbat_ovp_harness.c").read_text()
    with tempfile.TemporaryDirectory(prefix="konoha-vbat-") as directory:
        c = Path(directory) / "vbat.c"
        exe = c.with_suffix("")
        c.write_text(harness.replace("/* INSERT_FUNCTIONS */", functions))
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-g", "-O1",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        str(c), "-o", str(exe)], check=True)
        result = subprocess.run([str(exe)], text=True, capture_output=True, check=True)
    return {"status": "PASS", "cases": int(result.stdout.strip()),
            "sanitizers": "ASan+UBSan", "source_sha256": {
                SOURCE: hashlib.sha256(path.read_bytes()).hexdigest()},
            "limits": ["Provider stubs verify control flow; physical voltage faults are not injected."]}


if __name__ == "__main__":
    print(json.dumps(verify(sys.argv[1]), indent=2))
