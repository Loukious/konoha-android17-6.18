# Native hung-task diagnostics

The original provider-deployment checkpoint is described below. The [current 391-module runtime summary](../validation/onyx-runtime-summary-20261104.md) records the latest kernel and outstanding limits; earlier counts, preserved-kernel and temporary Wi-Fi statements describe that historical checkpoint.

`hung_task_enh.ko` loads through the existing vendor module loader on the original working GKI. The complete pinned native driver uses the existing WALT task extension and Android trace hooks. Native allowlist mode, I/O-wait limit and disabled panic threshold are retained.

Repairs retain task/PID references, reject invalid controls, saturate counters, verify WALT availability, unwind failed registration and drain callbacks before unload. 156,898 actual-C ASan/UBSan host cases and 11 regression mutations pass. Pinned replay, import checks, six partition readbacks and native reboot loading pass.

Two installed lifecycle cycles checked controls, selected only the testing shell, toggled its task state 0 -> 1 -> 0, rejected stale sysctl reads after unload and restored native defaults. All 385 previous module binaries and the original kernel are preserved. No live hung task or panic was injected; scanner concurrency and panic policy were tested offline.

See [acceptance](../validation/onyx-runtime-summary-20261104.md), [lifecycle](../validation/onyx-runtime-summary-20261104.md) and [current queue](../validation/remaining_391_modules/priorities.md).
