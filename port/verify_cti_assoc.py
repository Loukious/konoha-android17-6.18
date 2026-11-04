#!/usr/bin/env python3
"""Exercise the actual common CTI callback wrappers during concurrent unload."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
SOURCE = "drivers/hwtracing/coresight/coresight-core.c"


def function(source, name):
    match = re.search(r"(?m)^(?:static )?void " + name + r"\([^;{}]*\)\n\{", source)
    if not match:
        raise ValueError("Missing function: " + name)
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def verify(common):
    path = Path(common) / SOURCE
    source = path.read_text()
    functions = "\n".join(function(source, name) for name in (
        "coresight_set_cti_ops", "coresight_remove_cti_ops",
        "coresight_cti_add", "coresight_cti_remove"))
    harness = (HERE / "tests/cti_assoc_harness.c").read_text()
    with tempfile.TemporaryDirectory(prefix="konoha-cti-assoc-") as directory:
        c = Path(directory) / "cti.c"
        exe = c.with_suffix("")
        c.write_text(harness.replace("/* INSERT_FUNCTIONS */", functions))
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-g", "-O1",
                        "-pthread", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        str(c), "-o", str(exe)], check=True)
        result = subprocess.run([str(exe)], text=True, capture_output=True, check=True, timeout=20)
    return {"status": "PASS", "cases": int(result.stdout.strip()),
            "sanitizers": "ASan+UBSan", "source_sha256": {
                SOURCE: hashlib.sha256(path.read_bytes()).hexdigest()},
            "limits": ["SRCU is modeled with host synchronization; the kernel build checks the real API."]}


if __name__ == "__main__":
    print(json.dumps(verify(sys.argv[1]), indent=2))
