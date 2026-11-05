# Native hung-task diagnostics

The current [parallel-loading fix](parallel-module-loading.md) removes the
module-init race with asynchronous WALT startup. Task accesses remain gated
until readiness; per-task controls return `EAGAIN` while WALT starts and become
active without reloading. The updated actual-C harness passes 156,921 cases
under ASan/UBSan and ThreadSanitizer, rejecting 14 behavior/lifetime mutations
and two publication-ordering mutations. Production Android and full recovery
now pass with parallel loading enabled.

The original provider-deployment checkpoint is described below. The [current 391-module runtime summary](../validation/onyx-runtime-summary-20261104.md) records the latest kernel and outstanding limits; earlier counts, preserved-kernel and temporary Wi-Fi statements describe that historical checkpoint.

`hung_task_enh.ko` loads through the existing vendor module loader on the original working GKI. The complete pinned native driver uses the existing WALT task extension and Android trace hooks. Native allowlist mode, I/O-wait limit and disabled panic threshold are retained.

The original repairs retained task/PID references, rejected invalid controls, saturated counters, checked WALT availability at module initialization, unwound failed registration and drained callbacks before unload. That checkpoint passed 156,898 actual-C ASan/UBSan host cases and 11 regression mutations. The later readiness regression above supersedes its initialization check. Pinned replay, import checks, six partition readbacks and native reboot loading passed at that checkpoint.

Two installed lifecycle cycles checked controls, selected only the testing shell, toggled its task state 0 -> 1 -> 0, rejected stale sysctl reads after unload and restored native defaults. All 385 previous module binaries and the original kernel are preserved. No live hung task or panic was injected; scanner concurrency and panic policy were tested offline.

See [acceptance](../validation/onyx-runtime-summary-20261104.md), [lifecycle](../validation/onyx-runtime-summary-20261104.md) and [current queue](../validation/remaining_391_modules/priorities.md).
