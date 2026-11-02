#!/usr/bin/env python3
"""Exercise production remove callbacks against modeled runtime PM/devres clocks."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
CONTROL = HERE.parents[1]
sys.path.insert(0, str(CONTROL))
from common_fixes import applied

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('workspace', type=Path)
args = p.parse_args()
common = args.workspace / 'common'
callbacks = {
    'catu': 'catu_platform_remove', 'cpu-debug': 'debug_platform_remove',
    'ctcu-core': 'ctcu_platform_remove', 'etm4x-core': 'etm4_remove_platform_dev',
    'funnel': 'funnel_platform_remove', 'replicator': 'replicator_platform_remove',
    'stm': 'stm_platform_remove', 'tmc-core': 'tmc_platform_remove',
    'tpiu': 'tpiu_platform_remove',
}


def extract(text, name):
    start = text.index('static void ' + name + '(')
    opening = text.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


PRELUDE = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct device { void *data; int active, enabled, usage, votes, external, failed; };
struct platform_device { struct device dev; };
struct catu_drvdata { int unused; };
struct debug_drvdata { int unused; };
struct ctcu_drvdata { int unused; };
struct etmv4_drvdata { int unused; };
struct funnel_drvdata { int unused; };
struct replicator_drvdata { int unused; };
struct stm_drvdata { int unused; };
struct tmc_drvdata { int unused; };
struct tpiu_drvdata { int unused; };
static struct device *current;
static int step, warnings, debug_hold;
static void *dev_get_drvdata(struct device *d) { return d->data; }
static void *platform_get_drvdata(struct platform_device *p) { return p->dev.data; }
static int warn_on(int v) { warnings += !!v; return v; }
#define WARN_ON(x) warn_on(!!(x))
static int pm_runtime_get_sync(struct device *d) {
 assert(step++ == 0); d->usage++;
 if (d->failed) return -5;
 if (!d->active) { d->votes++; d->active = 1; }
 return 0;
}
static void teardown(struct device *d) {
 assert(step++ == 1); assert(d->active || d->failed); assert(d->enabled);
 if (debug_hold) { assert(d->usage > 1); d->usage--; }
}
#define __catu_remove teardown
#define __debug_remove teardown
#define funnel_remove teardown
#define replicator_remove teardown
#define __stm_remove teardown
#define __tmc_remove teardown
#define __tpiu_remove teardown
static void ctcu_remove(struct platform_device *p) { teardown(&p->dev); }
static void etm4_remove_dev(struct etmv4_drvdata *data) { assert(data); teardown(current); }
static void pm_runtime_disable(struct device *d) { assert(step++ == 2); d->enabled = 0; }
static void pm_runtime_set_suspended(struct device *d) {
 assert(step++ == 3); assert(!d->enabled); d->active = 0;
}
static void pm_runtime_put_noidle(struct device *d) { assert(step++ == 4); assert(d->usage); d->usage--; }
'''


def harness(functions):
    bodies = []
    names = []
    for i, (name, body) in enumerate(functions.items()):
        unique = 'remove_' + str(i)
        bodies.append(body.replace(name.split(':')[-1], unique))
        names.append(unique)
    return PRELUDE + '\n'.join(bodies) + r'''
typedef void (*remove_fn)(struct platform_device *);
static remove_fn callbacks[] = {''' + ','.join(names) + r'''};
int main(void) {
 unsigned cases = 0;
 for (unsigned i = 0; i < sizeof(callbacks)/sizeof(*callbacks); i++) {
  for (int active = 0; active < 2; active++) {
   for (int ext = 0; ext < 4; ext++) {
    for (int hold = 0; hold <= (i == 1 && active); hold++) {
     int data = 1; struct platform_device p = {.dev = {
       .data = &data, .active = active, .enabled = 1,
       .usage = hold, .votes = active + ext, .external = ext}};
     current = &p.dev; step = warnings = 0; debug_hold = hold;
     callbacks[i](&p);
     assert(step == 5 && warnings == 0);
     assert(!p.dev.active && !p.dev.enabled && p.dev.usage == 0);
     /* devm action releases exactly this consumer's enabled vote. */
     assert(p.dev.votes == ext + 1); p.dev.votes--;
     assert(p.dev.votes == ext); cases++;
    }
   }
  }
  struct platform_device missing = {.dev = {.enabled = 1}};
  step = warnings = debug_hold = 0; callbacks[i](&missing);
  assert(step == 0 && warnings == 1 && missing.dev.enabled); cases++;
  /* Preserve/document upstream's limitation: failed resume leaves no vote
   * for devm to release; the submitted patch does not handle that error. */
  int data = 1; struct platform_device failed = {.dev = {
    .data = &data, .enabled = 1, .failed = 1}};
  current = &failed.dev; step = warnings = 0; callbacks[i](&failed);
  assert(step == 5 && failed.dev.usage == 0 && failed.dev.votes == 0);
 }
 printf("PASS: %u callback lifecycle cases; failed-resume limitation confirmed\n", cases);
 return 0;
}
'''


original = {name: extract((common / f'drivers/hwtracing/coresight/coresight-{driver}.c').read_text(), name)
            for driver, name in callbacks.items()}
with tempfile.TemporaryDirectory(prefix='coresight-remove-test-') as temporary:
    root = Path(temporary)
    with applied(common, root / 'patch-receipt'):
        candidate = {name: extract((common / f'drivers/hwtracing/coresight/coresight-{driver}.c').read_text(), name)
                     for driver, name in callbacks.items()}
    funnel = args.workspace / 'onyx/drivers/hwtracing/coresight/coresight-funnel.c'
    candidate['onyx:funnel_platform_remove'] = extract(funnel.read_text(), 'funnel_platform_remove')
    assert candidate['onyx:funnel_platform_remove'] == candidate['funnel_platform_remove']
    variants = {'candidate': candidate, 'original': dict(candidate, **original)}
    for label, old, new in (
        ('no-resume', 'pm_runtime_get_sync(&pdev->dev);', ''),
        ('no-set-suspended', 'pm_runtime_set_suspended(&pdev->dev);', ''),
        ('no-put-noidle', 'pm_runtime_put_noidle(&pdev->dev);', ''),
        ('teardown-first', 'pm_runtime_get_sync(&pdev->dev);\n\tfunnel_remove(&pdev->dev);',
         'funnel_remove(&pdev->dev);\n\tpm_runtime_get_sync(&pdev->dev);'),
        ('etm-missing-guard', 'if (WARN_ON(!drvdata))\n\t\treturn;', ''),
    ):
        changed = dict(candidate)
        target = 'etm4_remove_platform_dev' if label.startswith('etm') else 'funnel_platform_remove'
        assert old in changed[target]
        changed[target] = changed[target].replace(old, new)
        variants[label] = changed
    results = {}
    for label, functions in variants.items():
        source = root / (label + '.c')
        source.write_text(harness(functions))
        binary = root / label
        subprocess.run(['cc', '-std=c11', '-g', '-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer', '-o', str(binary), str(source)], check=True)
        r = subprocess.run([str(binary)], capture_output=True, text=True)
        assert (r.returncode == 0) == (label == 'candidate'), (label, r.stdout, r.stderr)
        results[label] = {'returncode': r.returncode, 'stdout': r.stdout, 'stderr': r.stderr}
        if label == 'candidate':
            (HERE / 'platform-remove-harness.c').write_text(source.read_text())
    # The required patch must restore pinned source even on a failing build.
    before = {name: (common / name).read_bytes() for name in
              json.loads((CONTROL / 'common-patches/manifest.json').read_text())['files']}
    try:
        with applied(common, root / 'failure-receipt'):
            raise RuntimeError('simulated compiler failure')
    except RuntimeError:
        pass
    assert all((common / name).read_bytes() == contents for name, contents in before.items())
    (HERE / 'platform-remove-validation.json').write_text(json.dumps({
        'status': 'PASS', 'variants': results, 'restore_after_build_failure': True,
        'tnoc_platform_driver_present': False,
        'limitation': 'Exact upstream patch ignores failed runtime resume; clock balance requires successful resume.',
    }, indent=2) + '\n')
    print(results['candidate']['stdout'].strip())
    print('PASS: original and five regressions rejected; build-failure source restoration verified')
