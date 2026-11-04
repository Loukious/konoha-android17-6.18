# Native CPU frequency hardware diagnostics on Onyx

The original provider-deployment checkpoint is described below. The [current 391-module runtime summary](../validation/onyx-runtime-summary-20261104.md) records the latest kernel and outstanding limits; earlier counts, preserved-kernel and temporary Wi-Fi statements describe that historical checkpoint.

`qcom-cpufreq-hw-debug.ko` supplies the native read-only
`/sys/kernel/qcom-cpufreq-hw/print_cpufreq_debug_regs` interface and the
existing panic diagnostic callback. It reads six native EPSS registers in
each of four frequency domains. It changes no frequency policy or hardware
register and does not replace the original GKI kernel.

Patch 0226 preserves the Xiaomi register offsets and output format. It
validates domain and resource bounds, releases the parsed provider node,
bounds the sysfs output by remaining capacity, and keeps context ownership
with its kobject. Error paths and removal drain the panic notifier and sysfs
readers before their managed MMIO mappings can be freed. Native OF aliases
and the cpufreq provider soft dependency support normal automatic loading.

Validation: 16,776 actual-driver C ASan/UBSan cases for malformed DT,
register extents, allocation/publication failures, sysfs output, references
and threaded reader teardown. Mutations for the original reference, buffer
and notifier bugs were rejected. Native original-GKI build/import checks,
full source replay, two temporary normal probe/unload cycles and all
24 register reads passed. Native image, automatic reboot loading, six
partition readbacks, Android/audio/idle smoke and installed lifecycle
validation are recorded in the acceptance receipt. SELinux remains enforcing
and qcom_lpm remains the default; Wi-Fi mode0 was restored.

Host checks: `python3 port/verify_cpufreq_debug.py`.
No intentional phone panic was performed. Panic diagnostics were exercised
offline; long-term stability is not established.

[Acceptance](../validation/onyx-runtime-summary-20261104.md) ·
[Current priorities](../validation/remaining_391_modules/priorities.md)
