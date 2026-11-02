#!/usr/bin/env python3
"""Run sanitised control-path tests against the exact staged platform sources."""
import json
import os
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent

def verify(staged):
    env = dict(os.environ, KONOHA_MODULES_ROOT=str(Path(staged).resolve()))
    result = subprocess.run([sys.executable, str(HERE.parent / 'validation/platform_providers/test_controls.py')],
                            env=env, capture_output=True, text=True, check=True)
    return json.loads(result.stdout)

if __name__ == '__main__':
    print(json.dumps(verify(sys.argv[1]), indent=2))
