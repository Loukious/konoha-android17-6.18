# Native CPUSS v4 sleep statistics

This driver preserves native qcom,cpuss-sleep-stats-v4 matching, the qcom_cpuss_sleep_stats debugfs root, all six count/residency filenames and register offsets. Resource bounds are checked before mapping. Each device owns a copy of register configuration. Split residency reads use bounded high/low/high sampling and return EAGAIN if unstable. Publication failures release debugfs through devres; removal closes files before the mapped register state disappears.

The complete-driver host harness passes 371 ASan/UBSan cases with sanitizer recovery disabled. It covers allocation/mapping/match/resource failures, every publication failure, truncated resources, offset overflow, count/residency reads, rollover, bounded instability and teardown. Native-to-port patch 0213 replays forward/reverse with exact bytes and zero fuzz.

Linux 6.18 fs/debugfs/file.c full_proxy_read protects the native read with debugfs_file_get/put. Removal drains protected operations; later reads fail before driver access. full_proxy_release invokes single_release, which frees seq-file state without reading private MMIO configuration. The devres action is installed after memory allocations, so its cleanup executes before those allocations are freed. Host tests use stubs; proxy behavior is established by kernel source review rather than a hardware test.

Onyx has an older CPUSS node. No v4 node is added or enabled; this restores the retained load-list provider and will bind only to its native compatible. Kernel compilation, packaging and device state are recorded in validation/cpuss_v4 as completed.
