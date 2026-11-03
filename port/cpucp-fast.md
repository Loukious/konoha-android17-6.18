# Native CPUCP firmware scheduler hints

`cpucp_fast.ko` preserves the native low-byte CPU hint and `0xff` stop sentinel,
and invokes the existing exported `sched_walt_oscillate()` implementation. WALT
retains its real platform/cluster/workload conditions for migration. The sentinel
continues to clear the WALT oscillation state after a valid CPU hint and is never
passed as a cpumask index. Null messages, out-of-range CPUs and CPUs outside the
actual cpufreq policy are ignored.

The complete policy mask and initial sentinel state are ready before requesting
the mailbox, including a callback delivered during channel acquisition. Policy
references are balanced, all mailbox errors propagate, and failed registration
or device shutdown drains/releases the actual channel before memory is freed.
State belongs to each device rather than a shared static singleton. The DDK
requires genuine `qcom_cpucp` and `sched_walt` providers, with their enabled native
configurations; there is no placeholder scheduler implementation.

`verify_cpucp_fast.py` runs 19 actual-C ASan/UBSan cases for policy bounds,
registration/deferred-probe failures, immediate RX publication, valid hints,
sentinel transitions and idempotent teardown. Real scheduler migration and
firmware delivery still require device checks. No firmware IDs, ROM policy,
authentication or fuse behavior is changed.
