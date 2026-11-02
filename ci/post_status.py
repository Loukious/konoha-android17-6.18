#!/usr/bin/env python3
"""Attach the CI-branch build result to the selected source commit's PR."""
import argparse
import os
import re
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('revision')
p.add_argument('state', choices=['pending', 'success', 'failure', 'cancelled'])
a = p.parse_args()
if not re.fullmatch('[0-9a-f]{40}', a.revision):
    p.error('source revision must be a complete Git hash')
repository = os.environ['GITHUB_REPOSITORY']
url = f"{os.environ['GITHUB_SERVER_URL']}/{repository}/actions/runs/{os.environ['GITHUB_RUN_ID']}"
state = 'error' if a.state == 'cancelled' else a.state
subprocess.run(['gh', 'api', f'repos/{repository}/statuses/{a.revision}', '--method', 'POST',
                '-f', 'state=' + state, '-f', 'context=Konoha / GKI and modules',
                '-f', 'target_url=' + url, '-f', 'description=GKI, 318 modules, DTs and CRC verification'],
               stdout=subprocess.DEVNULL, check=True)
