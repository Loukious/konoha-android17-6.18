# Parallel module loading on Onyx

Parallel Android, fastbootd and the normal recovery menu passed on November 5,
2026 with the corrected 6.18 modules and the original ROM init binaries.
The tested configuration uses `androidboot.load_modules_parallel=true`
without a ROM loader patch. The device receipt is
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

The kernel repair works with the shipping ROM loaders and the BoardConfig's
existing `androidboot.load_modules_parallel=true` setting. Android, fastbootd
and normal recovery have been verified after restoring the original first-stage
and recovery init binaries. The separate `system/core` patch and serial-loading
workaround were removed from the ROM patch repository.

The earlier combined trial used a rebuilt module loader. It did not establish
that this ROM change was necessary for the WALT readiness fix. The kernel fix
must not be described as depending on that loader patch. The original-loader
restoration changes only init payloads in `init_boot` and `recovery`; all kernel
modules, module lists and AVB profiles match the preceding working images.

## Verification

- The complete hung-task driver, actual WALT readiness getter and publication
  pass 156,921 host cases under ASan, UBSan and ThreadSanitizer. Fourteen behavior
  and lifetime mutations and two memory-ordering mutations are rejected. The
  tests cover delayed readiness, permanently unavailable WALT, early unload,
  preserved scanner decisions and activation without a module reload. See
  [the host receipt](hung-task-enh-host-validation.json).
- All port patches replayed. Both rebuilt modules match the source ledger.
  Their 336 imported-symbol CRCs match the installed kernel/providers, and all
  25 WALT export signatures are unchanged.
- Two original-loader Android boots pass all nine partition readbacks, loaded-module
  identities, native per-task controls, KernelSU root, SELinux enforcing, audio
  policy, sound card and both AW amplifiers.
- Original-loader fastbootd appears in 15.33 seconds. The normal recovery menu also
  boots; after the user enables ADB, its live module set contains exactly all
  315 expected modules. The 316-entry list retains its original blocklist entry
  for `llcc_perfmon`. Recovery retains the new module identities and all seven
  physical partition hashes. It remained on the same boot through the capture
  at 236 seconds of uptime.

The installed kernel, audio fixes and thermal-voter fix are retained. Both init
binaries match the shipping ROM bytes. The image comparison verifies every
other ramdisk record, including module payloads and load lists, and preserves
headers and AVB profiles. No diagnostic init, watchdog or logging override is
installed. Validation used targeted module builds and the existing ROM images
on one Onyx phone; a full ROM build was not run. Sysctl/PID/tracepoint framework
behavior is mocked in the host harness; no live hung task or panic was injected.

## Reproduce the module checks

Run `python3 port/verify_hung_task_enh.py`. To include the publication race
checks, append `--tsan-cc /path/to/a/thread-sanitizer-capable/clang`.
The normal port build runs the source harness against staged modules. Build
the `//onyx:hung_task_enh` and `//onyx:sched_walt` targets and verify their imports
against the matched installed artifacts before packaging all three module
copies in vendor_boot, recovery and vendor_dlkm.

The build evidence is retained in
`konoha-parallel-rootfix-20261105/hung-task-walt-fix/`. Its
`original-loader-restoration/` directory contains the guarded installation,
rollback staging, complete image manifest and fresh Android/recovery captures.
The device receipt records the final Android boot ID on slot B.
