# Native Onyx memory latency monitor

`memlat.ko` ports the pinned SM8735 MEMLAT provider. It samples the real Qualcomm
PMU service, maps CPU MHz to the device tree's memory frequency table, and votes
through the existing Qualcomm DCVS service. CPUCP monitors retain the native
`MEMLAT` SCMI algorithm ID, message IDs, structures and event mapping. No ROM
policy or firmware authentication behavior is changed.

The DDK target depends on `qcom_dcvs`, `qcom_pmu_lib`, `qcom_scmi_client` and
`cpu_phys_log_map`. Native DCVS measurement/update tracepoints come from
`qcom-dcvs.ko`; scheduler tick and jiffies vendor hooks come from the GKI kernel.
The 6.18 API uses `hrtimer_setup`. Wide frequency/percentage arithmetic uses
`mul_u64_u64_div_u64`, which must be present in the kernel's KMI symbol list.

The port validates map length, firmware row encoding, ordering, CPUs and physical
CPU mask bounds. Counter resets, missing elapsed time and zero denominators
cannot create wraparound votes. Rejected firmware writes leave sysfs state
unchanged; failed votes do not advance the driver's committed frequency, so
subsequent samples retry. Native `threadlat-enabled` has no implementation here
and is rejected with `-EOPNOTSUPP`.

Sampling callbacks stay inactive until all configured monitors are published.
When CPUCP is requested, readiness also requires successful native firmware
configuration. A CPUCP configuration failure is logged and leaves the complete
tree bound and inactive; the port does not reinterpret CPUCP-only monitors as
sampling monitors. Onyx's pinned Sun monitors use sampling, without threadlat.

The driver deliberately has one bound root and suppresses manual bind/unbind
attributes. Removing or shutting down any member stops the entire tree before
owner memory can be released. Hooks/notifiers are unregistered and synchronized,
timers/work are cancelled, and the SCMI protocol/device references are released.
Each group drops its own DCVS vote before unregistering. Sysfs callbacks are
serialized with stop and reject a stopped or incomplete root. Kobject release
completions hold devm-owned memory until all active sysfs references drain.
Recovery after removing a child requires removing and reprobeing the root;
partial child rebind is not supported.

`verify_memlat.py` compiles functions and declarations extracted verbatim from
the staged driver against host provider stubs. Its 70 ASan/UBSan cases cover
maps/CPU masks, wide arithmetic, firmware transactions, vote failure/retry,
raw event bounds, sample publication, callback-registration rollback and
idempotent teardown. See `memlat-host-validation.json`. These tests verify the C
control flow; the combined DDK build and real device frequency/timing checks
remain separate validation steps.
