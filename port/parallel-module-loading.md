# Parallel module loading on Onyx

Parallel Android, fastbootd and the normal recovery menu passed on November 5,
2026 with the 6.18 port. The phone finishes in Android with
`androidboot.load_modules_parallel=true`. The device receipt is
[parallel-module-loading-20261105.json](../validation/parallel-module-loading-20261105.json).

## Captured failure and repair

The persisted first-stage recovery report captured `hung_task_enh.ko` returning
`ENODEV` at 0.394591 seconds. The original parallel attempt returned false after
182 modules. This result was captured before loading any additional modules to
make diagnostic storage available.

The existing hard dependency ensures that `sched-walt.ko` has loaded, but WALT
initializes task data on a background worker after topology is ready.
`hung_task_enh_init()` incorrectly treated the provider's temporary lack of
readiness as a permanent module-loading failure.

[Patch 0245](patches/0245-fix-hung-task-readiness-during-parallel-loading.patch)
registers the hung-task hooks and controls without waiting for WALT. Before
readiness, the scanner callback preserves the kernel's original decision and
the per-task control returns `EAGAIN`. Once WALT publishes initialized task
data, these operations become active without reloading the module. The resident
provider's readiness transition uses release/acquire ordering. Task storage,
PID references, callback draining, native defaults and disabled panic threshold
remain protected.

The ROM also needs the production `system/core` loader patch saved in
`crdroid_onyx_patches/patches/system_core/0001-fix-parallel-module-loader-synchronization.patch`.
It fixes worker shutdown after errors, producer/worker races, failure-result
overwrites and sequential-option parsing. All dependency-map access belongs to
the producer; worker queues, completion and shutdown share one mutex. A zero
hardware thread count uses one worker. Ordinary loading remains parallel.

The old device-tree patch `0019-use-serial-first-stage-module-loading.patch`
is retired. `apply.sh` now maps `system_core` to `system/core` and applies that
patch with exact context. The existing device BoardConfig already enables
parallel loading. Both the updated modules and loader are required for this
validated configuration.

## Verification

- The actual loader and parser pass 29 host cases with ThreadSanitizer, including
  all three loading modes, the real recovery list, error shutdown, dependency
  aliases, sequential exclusions and zero-worker fallback.
- The complete hung-task driver, actual WALT readiness getter and publication
  pass 156,921 host cases under ASan, UBSan and ThreadSanitizer. Fourteen behavior
  and lifetime mutations and two memory-ordering mutations are rejected. The
  tests cover delayed readiness, permanently unavailable WALT, early unload,
  preserved scanner decisions and activation without a module reload. See
  [the host receipt](hung-task-enh-host-validation.json).
- All port patches replayed. Both rebuilt modules match the source ledger.
  Their 336 imported-symbol CRCs match the installed kernel/providers, and all
  25 WALT export signatures are unchanged.
- Two production Android boots pass all nine partition readbacks, loaded-module
  identities, native per-task controls, KernelSU root, SELinux enforcing, audio
  policy, sound card and both AW amplifiers.
- Production fastbootd appears in 12.59 seconds. The normal recovery menu also
  boots; after the user enables ADB, its live module set contains exactly all
  315 expected modules. The 316-entry list retains its original blocklist entry
  for `llcc_perfmon`. Recovery retains the new module identities and all seven
  physical partition hashes. It remained on the same boot through the capture
  at over nine minutes of uptime.

The installed kernel, audio fixes and thermal-voter fix are retained. The
production images contain no diagnostic init, watchdog or logging override.
Validation used targeted module and init builds on one Onyx phone; a full ROM
build was not run. Sysctl/PID/tracepoint framework behavior is mocked in the
host harness; no live hung task or panic was injected.

## Reproduce the module checks

Run `python3 port/verify_hung_task_enh.py`. To include the publication race
checks, append `--tsan-cc /path/to/a/thread-sanitizer-capable/clang`.
The normal port build runs the source harness against staged modules. Build
the `//onyx:hung_task_enh` and `//onyx:sched_walt` targets and verify their imports
against the matched installed artifacts before packaging all three module
copies in vendor_boot, recovery and vendor_dlkm.

The build, guarded installation, rollback staging and raw runtime captures are
retained in `konoha-parallel-rootfix-20261105/hung-task-walt-fix/`. The final boot
ID is `d2631c28-b05a-45a1-b690-eb86642543e3` on slot B.
