# Onyx platform providers

Twenty-one providers extend the device-service candidate from 329 to 350
platform modules. Pinned imports and ordered patches 0182–0183 reproduce the
4966-file module snapshot. The installed Onyx device-tree layout is retained.

| Group | Modules | Adaptation |
| --- | --- | --- |
| Cache and bandwidth | mpam, cpu_mpam, platform_mpam, mpam_msc, mpam_msc_slc, slc_mpam, llcc_heuristics | Validate firmware counts and control input, bound monitor retries, serialize access, clean up configfs groups, and use the active SCMI receive limit. |
| Peripherals | gpio-mi-t1, gh_tlmm_vm_mem_access, qcom-pbs, leds-qcom-lpg, bam_dma, hl7603 | Use current PWM allocation, validate GPIO/FIFO bounds, propagate firmware/I2C errors, and release resources. |
| Statistics and diagnostics | qcom_stats, msm_show_resume_irq, pmic-glink-debug, spmi-pmic-arb-debug, coresight-tpdm, coresight-tpda, coresight-tmc, mca_ut_test | Validate ioctl sizes and PMIC responses, drain callbacks/work, share the GIC mapping, and support verified native trace layouts through the common CoreSight APIs. |

`verify_platform_providers.py` exercises exact staged C functions in fourteen
ASan/UBSan fixtures with 227 cases. Coverage includes transport limits and
failure cleanup, gear parsing, boost-chip I2C errors, DMA mapping boundaries,
statistics routing, malformed PMIC responses, partition allocation, native
trace mode bits/widths/authentication, charging-vote rollback, handle-state
transactions, reverse eligibility, and Android battery status. Full builds
also run 57 device-service cases and 50 thermal cases.

Artifact `20261102T233348717332Z` passes all 350 module builds, 21787 imported
symbol CRC checks, and 2106 artifact hashes. Both device-tree variants pass
overlay and bootloader-ID checks; 1097 inherited DTC warnings remain.

## SCMI and MPAM

The earlier ERANGE result was incorrectly attributed to firmware. The vendor
protocol requested a 128-byte receive buffer while the common shared-memory
transport allows 104 bytes, so allocation could fail before transmission.
RX size zero now selects the transport maximum. Vendor TX bounds, SCMI errors,
caller response bounds, and transfer release remain enforced.

On the project-7 phone, LLCC heuristics, CPU MPAM, platform MPAM, the MSC registry
and SLC initialize successfully. CPU's default configuration reports
`L3:cpbm=0x3f,slc_partid=3`. Live NoC and SLC gear changes read back correctly;
NoC monitoring returns timestamp and byte-count data. Tests restore the original
gears and disable monitoring. The earlier claim that CPU controller children
were absent does not describe the installed project-7 device tree.

## Trace

Native Qualcomm TPDM v1 and TPDA IDs are matched with their verified layouts.
TPDM v1 has three cycle-mode bits at 9–11 and dataset-specific integration
packets. High-performance byte lanes are normalized before register encoding.
TPDA reads validated native `(port, width)` pairs and retains native hold-time,
timestamp and AUTHSTATUS behavior. BC/TC/GPR controls are outside this provider.

The phone binds 38 TPDMs, ten TPDAs and one TMC ETF. A 64-bit integration test
captures 160 bytes through `tpdm33 -> tpda8 -> funnel15 -> tmc_etf0`; source and
sink are disabled afterward. This validates that path, not every trace source.
Native ETR streaming remains unsupported because its CSR ATID/IRQ provider is
not ported. SPMI debug remains fuse-disabled.

## Charging extensions

Handle policy applies independent inhibit votes to buck and quick charging,
waits for provider acceptance, and publishes state only after both succeed.
Failures restore the prior votes and state. Releasing a hold removes only its
own votes, preserving thermal, authentication and fault protections. The native
`stop_handle_charge` transition semantics are retained.

Android reports `Not charging` when the wired handle votes inhibit both paths;
USB remains online. Battery notifications accompany committed changes. A wired
reverse request uses the existing PD gear operation and requires peer
authentication, OTG presence, valid temperature/SOC reads and eligibility.
Failed requests preserve the prior gate. The phone accepts reverse stop and
rejects unauthenticated start. No wireless reverse provider or reverse
battery-current-limit operation is demonstrated; `revchg_bcl` stays unsupported.

## Other limits

The resume IRQ logger shares the GIC-owned mapping. BAM advertises 31 pipes,
but the HLOS mapping exposes 13; unmapped channels cannot be requested. HL7603
is a boost/bypass component. The native SLIMbus client is disabled. Some optional
subsystem statistics return EIO. Powered-off charging, physical IR/USB audio,
high-power charging/reverse-peer tests and sustained thermal/reclaim testing
still need evidence. Existing camera/ROM SELinux findings remain.

The draft feature PR remains unmerged:
https://github.com/Loukious/konoha-android17-6.18/pull/4
Raw device logs, trace data and partition backups remain local.
