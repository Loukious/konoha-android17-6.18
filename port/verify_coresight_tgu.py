#!/usr/bin/env python3
"""Exercise the actual TGU controls, bounded layouts and PM/claim lifetime."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

from verify_coresight_cti import function

HERE = Path(__file__).resolve().parent
SOURCE = "drivers/hwtracing/coresight/coresight-tgu.c"


def verify(staged):
    path = Path(staged) / SOURCE
    source = path.read_text()
    declarations = source[source.index("#define tgu_writel"):source.index("DEFINE_CORESIGHT_DEVLIST")]
    functions = "\n".join(function(source, name) for name in ["enable_tgu_store", "reset_tgu_store",
        "set_group_store", "set_condition_store", "set_select_store", "set_timer_store", "set_counter_store",
        "tgu_validate_layout", "tgu_path_enable", "tgu_path_disable", "tgu_probe", "tgu_remove"])
    harness = (HERE / "tests/coresight_tgu_harness.c").read_text()
    with tempfile.TemporaryDirectory(prefix="konoha-tgu-test-") as directory:
        c = Path(directory) / "tgu.c"
        exe = c.with_suffix("")
        c.write_text(harness.replace("/* INSERT_DECLARATIONS */", declarations).replace("/* INSERT_FUNCTIONS */", functions))
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                        "-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        str(c), "-o", str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        if result.returncode:
            print(result.stderr, file=sys.stderr)
            result.check_returncode()
    return {"status": "PASS", "cases": int(result.stdout.strip()), "sanitizers": "ASan+UBSan",
            "source_sha256": {SOURCE: hashlib.sha256(path.read_bytes()).hexdigest()},
            "limits": ["MMIO and PM are stubbed; actual bus-event trigger timing is not established."]}


if __name__ == "__main__":
    print(json.dumps(verify(sys.argv[1]), indent=2))
