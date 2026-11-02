# Onyx platform providers

Nineteen providers extend the 6.18.38 device-service candidate from 329 to
348 platform modules. The pinned imports and patch 0182 reproduce the
4958-file module snapshot. Device trees retain the installed Onyx layout.

| Group | Modules | Adaptation |
| --- | --- | --- |
| Cache and bandwidth controls | mpam, cpu_mpam, platform_mpam, mpam_msc, mpam_msc_slc, slc_mpam, llcc_heuristics | Validate control input and firmware counts, bound monitor retries, serialize provider access and clean up configfs groups. SCMI and platform-root dependencies are explicit. |
| Peripherals | gpio-mi-t1, gh_tlmm_vm_mem_access, qcom-pbs, leds-qcom-lpg, bam_dma, hl7603 | Use current PWM allocation, validate GPIO and reserved FIFO bounds, propagate firmware/I2C errors and release resources in removal paths. |
| Statistics and diagnostics | qcom_stats, msm_show_resume_irq, pmic-glink-debug, spmi-pmic-arb-debug, coresight-tpdm, mca_ut_test | Correct subsystem ioctl routing and output sizes, validate PMIC response identity and length, drain outstanding callbacks/work, and use the pinned common CoreSight provider. |

`verify_platform_providers.py` exercises exact staged control functions in six
ASan/UBSan fixtures with 73 cases. Tests cover gear parsing, boost-chip I2C
errors, reserved DMA FIFO bounds, statistics ioctl routing, malformed PMIC
responses and failed CPU-partition allocation. They run with the existing
57 service cases and 50 thermal cases during every complete build.

Local artifact `20261102T204202102780Z` passes all 348 module builds and
21656 imported-symbol CRC checks. Both device-tree variants pass overlay and
bootloader-ID checks; 1097 inherited DTC warnings remain. Hardware evidence
is recorded separately after installation.

Onyx's CPU-MPAM node has no cache-controller children: loading the provider
does not establish usable CPU partition controls. Optional DT devices and
firmware algorithms can still reject probes; successful compilation does not
establish fitted hardware. HL7603 is a boost/bypass component, not the main
charger. No missing device-tree parameters or firmware algorithms are invented.

Handle-charge and quick reverse-charge extension events still lack native
strategy handlers. No wireless reverse-charge provider is demonstrated for
this phone. Their explicit unsupported results remain appropriate until a
working hardware path is established. Powered-off charging and sustained
thermal/reclaim behavior need separate device evidence.
