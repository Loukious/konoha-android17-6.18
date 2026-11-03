# Remaining Onyx modules

The installed baseline is artifact `20261103T023508668153Z`: 352 platform
modules, 432 packaged modules including GKI. Its retained stock load lists request
119 unique names that are unavailable. That count includes recovery/debug
software, alternate Sun/Kera hardware and disabled or unfitted peripherals.
Module-name absence is not proof of missing functionality.

The earlier OF-alias inventory cannot classify modules that omit
`MODULE_DEVICE_TABLE(of, ...)`, or AMBA devices. Checking the fully applied
Onyx DT identifies these priorities:

| Module | Retained Onyx configuration | Disposition |
| --- | --- | --- |
| `mca_vbat_ovp_monitor` | Enabled; single fuel gauge, native country-specific voltage limits | First port in this branch. |
| `memlat` | Enabled DDR, LLCC, L3 and DDR-QoS groups and monitors | Next performance-provider audit; requires PMU/SCMI/DCVS and safe CPU/work lifetime handling. |
| `coresight_cti` | 24 enabled AMBA CTI nodes; standard and Qualcomm extended layouts | Pinned native port implemented; combined kernel build and live binding remain to validate. |
| `mca_qcom_sysfs` | Enabled | Requires an attribute-by-attribute audit of wired operations and unfitted wireless dependencies. |
| `mca_ibat_ocp_monitor` | Disabled | Preserve native disabled state. |
| `mca_business_misc_comp` | Disabled | Preserve native disabled state. |
| `ddr_cdev` | No matching node in applied Onyx DT | Do not assume the load-list name is a fitted cooling device. |

The VBAT monitor imports the exact pinned native source and retains its native
sysfs names, country voltage limits, charging event and hysteresis. A read error
preserves the existing fault and requests a faster retry. The monitor consults
the native charger's `support-wls` setting so an unfitted wireless operation
cannot disable wired monitoring. Debug input can assert protection but cannot
hide a measured overvoltage. State belongs to the device, rather than a shared
function-static latch.

DT defaults apply only to absent optional properties. Malformed values fail
probe; unavailable country information defers probe. Sysfs registration must
succeed before periodic monitoring starts. Remove and shutdown close sysfs and
drain the work synchronously, with a stop flag preventing rearming.

`verify_vbat_ovp.py` runs the actual status, work, stop and probe functions under
ASan/UBSan with failed sensor/provider reads, both voltage thresholds, hysteresis
boundaries, wired/wireless presence, debug transitions, failed sysfs publication
and repeated teardown. Controls remain inactive until probe registration succeeds.
These tests do not inject a physical battery overvoltage or establish hardware
timing. This new monitor has not been installed on the phone.

Artifact `20261103T060622791127Z` builds all 353 platform modules. All 21,917
imported-symbol CRC checks, 834 VBAT ASan/UBSan cases, the existing 1,120 platform
cases and the existing service/thermal suites pass. Both DT variants pass with
the 1,097 inherited DTC warnings. All 4,974 staged source hashes and 2,128 artifact
hashes match. The derived source-hash manifest's `BUILD.bazel` digest was updated
to match the new target; compiled sources and binaries were unchanged.

The native `memlat` audit found scheduler hooks and a PMU-idle notifier whose
registration errors are ignored, a periodic timer and workqueue without a
matching driver removal path, and a duplicate-root check overwritten by the
following probe call. Porting it requires handling those failures, draining
callbacks/work, and releasing the settings and child-monitor objects. Its
firmware protocol and frequency tables must retain the pinned native ABI.

The CTI port preserves the native 128-trigger bitmap representation and selects
Qualcomm extended register offsets before publishing sysfs. Common CTI only
supports the standard 32-trigger layout. Explicit `coresight-name` properties
retain the applied Onyx names. Hardware-reported zero/out-of-layout trigger counts
and zero channels fail probe; selectors, channel masks and firmware trigger
indexes are bounded. The native reversed appclear offsets, ignored DT filters,
and configuration cache writes are corrected. Extended integration access is
rejected because the native integration table uses standard addresses.

Raw IRQ-safe locks protect configuration and MMIO. GPIO pinctrl errors reach the
caller before routes change. Enable vote overflow fails; failed claims do not
create votes. System sleep preserves all enable votes; clock resume errors are
propagated. Driver teardown drains sysfs, CPU callbacks and association lists
synchronously, retaining the common device release callback. CPU-affine CTIs
remain unpowered until synchronized PM setup and skip MMIO while offline. Common-core CTI
association callbacks additionally require root's SRCU lifetime patch.

`verify_coresight_cti.py` runs 26,417 actual-C ASan/UBSan cases covering all DEVID
trigger/channel combinations, every trigger/channel boundary, both MMIO layouts,
128-bit bitmap boundaries, malformed DT filters, route/cache changes, reference
votes, GPIO/claim/clock failures, system sleep and registration rollback.
Provider stubs establish control flow and memory bounds; they do not establish
physical trigger delivery or CPU power timing. The CTI module has not yet been
installed or bound on the phone.
