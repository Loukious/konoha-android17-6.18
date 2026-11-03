# Native CPUSS sleep statistics

Onyx has the native qcom,cpuss-sleep-stats node at 0x17800054 with eight CPUs and a 64 KiB APSS statistics window. This port retains all v1/v2/v3 compatible names, register layouts, pcpu/L3/count/residency/stats/reset files, permissions and native MPIDR Aff1 indexing. Configuration reads normally use SCM; Onyx opts into firmware-granted HLOS MMIO reads with the boolean qcom,config-mmio property. Reset always retains secure SCM read/set/clear operations. The opt-in maps exactly four bytes for each present CPU and L3 config register, and reuses the bounded APSS mapping for residency config. It never substitutes configuration values, enables counters, or falls back after an arbitrary SCM error. It reads the established cpu_logical_map instead of dispatching an IPI to potentially offline CPUs. Invalid/duplicate/out-of-range CPU mappings fail before publication. Native missing per-CPU configuration resources remain skipped.

All counter reads use one bounded APSS mapping; cluster residency no longer maps four bytes before an eight-byte read. Resources and every configured CPU/cluster offset are validated before MMIO. Debugfs publication failures drain files before devres state frees; the successful teardown action is registered after all statistics records, so it drains first. Removal is idempotent. Linux 6.18 debugfs full-proxy read/write protects callbacks through debugfs_file_get/put; module ownership and single_release remain native.

Reset accepts 1 with an optional newline and returns EFAULT for a failed user copy. Garbage, extra bytes and other values fail before SCM access. Reset retains native read/set/clear operations and masks; firmware errors propagate immediately with the mutex balanced, without fabricated replies or additional writes. Probe never resets counters.

5,024 complete-driver ASan/UBSan checks (recovery disabled) cover every publication/allocation failure, malformed/missing resources, CPU mapping and count bounds, truncated MMIO windows, all 18 probe firmware-failure positions, all 54 reset firmware-failure positions, v1/v2/v3 read layouts, skipped CPUs, names/permissions and teardown. Counter timing, real firmware resets and kernel debugfs proxy lifetime are not simulated as hardware tests. The patch replays forward/reverse with exact bytes and zero fuzz.

The native import, patch 0214, build/config/catalog/hash workflow and strict host harness are integrated. Artifact `20261103T143610269701Z` builds all 373 platform modules and passes 22,684 imported-symbol CRC checks. Fresh pinned import and zero-fuzz patch replay matches all 5,004 staged source files; all 2,282 archive hashes match. Both DT variants pass. The first-stage dry run contains 100 modules; the full bundle packages 453 modules, checks 482 catalog modules and 29,057 imports, and leaves 98 distinct retained names unavailable. Receipts are in [validation/cpuss_native](../validation/cpuss_native). The batch has not been flashed. cpu_logical_map already has an exported, versioned symbol in the installed GKI. Never exercise reset on the phone merely to test this port.

Initial native kernel compilation and all 40 imported CRCs passed. That standalone native load registers the driver but its first SCM read at 0x17800054 returns -EINVAL; the device remains unbound and debugfs is cleaned up. No counter reads or hardware reset were subsequently performed. CPU MPIDR values and DT num-cpus were rechecked. qcom_scm_io_readl and its descriptor match the pinned native implementation. Capture is in [runtime-load-validation.json](../validation/cpuss_native/runtime-load-validation.json).

The follow-up live trace establishes the raw reply: service 5, command 1,
arginfo 1, ARM64 atomic call `0xc2000501`, address `0x17800054`, secure status
`-2` (`QCOM_SCM_EINVAL_ARG`), then Linux `-22` (`-EINVAL`). This distinguishes
the actual invalid-argument reply from invalid-address status `-3` and other
statuses that the Linux helper also maps to `-EINVAL`. The live DT's first
resource is the same address and four-byte size. The read helper exactly
matches the pinned native function, and the SCM module is byte-identical
between the installed and new production artifacts.

This proves the secure monitor returned an error; it does not establish why,
an access prohibition, or an unsupported hardware block. No caller mismatch
was found in this audit, so no speculative SCM change is included. The
diagnostic only repeated native probe reads, without modifying call arguments,
registers or counter configuration. All three probes and their dedicated trace
instance were removed afterward. Boot ID remained unchanged and SELinux stayed
enforcing. See [scm-investigation.json](../validation/cpuss_native/scm-investigation.json)
and [the captured events](../validation/cpuss_native/scm-trace.txt). The diagnostic
script and instruction offset are tied to the verified installed SCM build ID.


Patch 0215 and the Onyx-only compatibility overlay fix the read path established
by the firmware investigation. All 18 config addresses are in TZ's whitelist,
but its read handler has additional gates whose denial maps to the observed
raw SCM -2. HYP configures HLOS read access to the same CPUSS ranges. Static gate
code is compatible with the secure denial; live gate values and the meaning of
the first gate remain unobserved. The bounded live diagnostic confirmed all 18
addresses readable directly while each secure read returned -EINVAL. See
[firmware findings](../validation/cpuss_native/mmio/firmware-findings.md).

Actual config is preserved: CPU/L3 entry-count enables are zero; all eight CPUs
have C3/C4 residency enabled, and the cluster has D4 residency enabled. The exact
production driver binds and publishes those 17 raw hardware residency counters.
All 16 CPU counters advanced over twelve seconds; cluster D4 did not advance in
that awake observation. Two load/unload cycles passed with debugfs drained.
Android currently has no debugfs mount, so testing used a temporary read-only
mount, which was removed together with the temporary DT property afterward.
[Runtime evidence](../validation/cpuss_native/mmio/driver-runtime.json) records
module identity, configuration, samples and cleanup; no reset was exercised.

Artifact 20261103T153808728566Z passes all 373 modules, 22,685 import CRCs,
5,004 source hashes and 2,285 artifact hashes. Both merged and board-composed
DT variants differ only by qcom,config-mmio. Existing platform module bytes,
kernel config, System.map and symbol CRCs match the installed working build.
The GKI Image differs only in its regenerated build certificate and build ID;
[scope validation](../validation/cpuss_native/mmio/scope-validation.json) verifies
byte equality everywhere else. Fresh pinned replay and full 453-module packaging
pass; matching boot/DLKM images and persistent validation are recorded separately.

Persistent installation now passes on boot 3625da07-3432-4003-ad32-148c0bf99271.
The native driver loaded automatically and bound during boot, with the installed
DT boolean present and no diagnostic module loaded. It exposes the 17 expected
residency counters; twelve CPU counters advanced in this separate twelve-second
observation, with no negative deltas. All six partition readbacks match expected
images, kernel/CPUSS/SCM/WLAN/GLINK build IDs match the archive, and loader hashes
match. Wi-Fi is connected, Bluetooth is ON with zero recorded crashes, SELinux
is enforcing, and the boot log has no kernel panic, BUG or CPUSS probe failure.
See [persistent runtime](../validation/cpuss_native/mmio/persistent-runtime.json)
and [installation verification](../validation/cpuss_native/mmio/installation-validation.json).
Debugfs was mounted read-only for diagnostics and unmounted afterward; ROM mount
policy is unchanged. Reset and firmware writes were never exercised.
