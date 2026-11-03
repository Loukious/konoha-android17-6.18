# Remaining Onyx modules

Latest validated candidate: **371 platform modules**, artifact `20261103T125226341911Z`. Native GLINK packet devices are ported and built; all 22,623 imported-symbol CRC checks, 5,002 source hashes, 2,262 artifact hashes and matched packaging checks pass. The dry-run bundle packages 451 modules and checks 480; 100 distinct retained load-list names remain unavailable. See [GLINK provider](glink-pkt.md) and [371 validation](../validation/remaining_371_modules/build-validation.json). The matching 371-module images are now installed on phone 5493f3c6. All six partition readbacks and the kernel/module identities pass; the native `bt_cp_ctrl` RPMSG driver is bound and `/dev/bt_cp_ctrl` has the expected Bluetooth ownership and SELinux label. The user playback test passes, corroborated by native DMA status 0 and repeated PLAYING sessions. See [installation receipt](../validation/remaining_371_modules/live-install-validation.json).

The previous installed baseline was artifact `20261103T023508668153Z`: 352 platform
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
| `memlat` | Enabled DDR, LLCC, L3 and DDR-QoS groups and monitors | Native port implemented with PMU/SCMI/DCVS providers and synchronous teardown; see [memlat.md](memlat.md). |
| `coresight_cti` | 24 enabled AMBA CTI nodes; standard and Qualcomm extended layouts | Pinned native port implemented; combined kernel build and live binding remain to validate. |
| `mca_qcom_sysfs` | Enabled | All 29 native attributes retained with real wired providers and explicit unsupported wireless/authentication writes; see [mca-qcom-sysfs.md](mca-qcom-sysfs.md). |
| `mca_ibat_ocp_monitor` | Disabled | Preserve native disabled state. |
| `mca_business_misc_comp` | Disabled | Preserve native disabled state. |
| `ddr_cdev` | No matching node in applied Onyx DT | Do not assume the load-list name is a fitted cooling device. |

Both checked DT variants contain four memlat groups and 15 sampling-only
monitors. Their CPU masks fit the physical CPU IDs 0 through 7; maps contain
2 to 11 rows within the native 14-row limit. The three DDR-QoS monitors retain
their intentional zero-to-one vote tables. Zero frequency entries remain
invalid for DDR/LLCC. The 24 CTIs have unique names and 4KiB resources: 22 use
the Qualcomm extended layout and two the standard layout. None has a CPU
affinity or GPIO connection. Hardware DEVID counts require runtime validation.

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
association callbacks use the required common SRCU lifetime patch, which drains
sleeping association callbacks before module teardown without holding either
CoreSight registry mutex. Three concurrent sanitizer cases cover add/remove
versus unload and an empty callback table; a missing-drain mutation is rejected.

`verify_coresight_cti.py` runs 26,417 actual-C ASan/UBSan cases covering all DEVID
trigger/channel combinations, every trigger/channel boundary, both MMIO layouts,
128-bit bitmap boundaries, malformed DT filters, route/cache changes, reference
votes, GPIO/claim/clock failures, system sleep and registration rollback.
Provider stubs establish control flow and memory bounds; they do not establish
physical trigger delivery or CPU power timing. The CTI module has not yet been
installed or bound on the phone.

The completed three-provider artifact `20261103T064702805743Z` builds all 356
platform modules and passes 22,124 imported-symbol CRC checks. Memlat has 89
ASan/UBSan cases, CTI 26,417, qcom sysfs 744, and common CTI callback draining
three; all existing host suites and both DT variants pass. All 4,980 staged
source hashes and 2,156 artifact hashes match. The complete source snapshot now
includes the previously omitted VBAT source and build metadata.

The matched first-stage tree contains 99 modules; the complete dry-run bundle
packages 436 and checks 465. All four retained load lists and partition-local
depmod/modprobe checks pass. The retained lists still name 115 unavailable
modules, including inactive variants and recovery/debug providers. The user
requested continued ports while away; the phone is disconnected. No module in
this batch has been installed or hardware-validated. See
[validation summary](validation/active_provider_batch/summary.json).


The next CoreSight provider batch starts with `coresight_qmi`: six enabled Onyx
helpers retain their native QMI service/instance IDs, commands, message layouts,
timeout and names. Every firmware rejection returns an error. Requests snapshot
the endpoint under an IRQ-safe lock; receive callbacks avoid the transaction
mutex. Old-server deletion cannot disconnect a replacement endpoint. Removal
closes new requests, finishes any active transaction, unregisters CoreSight and
drains the QMI handle. Failed lookup or CoreSight publication releases the
initialized handle. A helper must be owned by the QMI driver before a caller
can treat its data as QMI state.

`verify_coresight_qmi.py` passes 774 actual-C ASan/UBSan cases for all four
commands, transport/firmware failures, transaction cancellation, endpoint
replacement, callbacks during a transaction, ATID/name/buffer bounds, probe
rollback and removal. Firmware and physical trace delivery remain untested while
the phone is disconnected. No disabled DT nodes are enabled.


`coresight_tgu` imports the pinned native driver for three enabled AMBA TGUs.
Native `tgu-*` properties are bounded configuration counts; negative selectors,
full arrays, values wider than registers and register-block overlap fail with
errors. Configuration writes stay cached until enable. Re-enabling applies the
cache with the existing PM vote. The driver acquires hardware ownership before
programming, balances one PM vote per active TGU, and resets or removes an active
TGU before releasing that vote. No PM operation runs under a spinlock. Remove
drains sysfs before devm state disappears. Explicit native names and register
layouts are retained, with a dedicated common helper subtype instead of CATU.

`verify_coresight_tgu.py` passes 2,016 actual-C ASan/UBSan cases covering all
layout-count boundaries, capacity-filled arrays, selector/word bounds, real
programming offsets, failed PM/claim/probe paths, re-enable, reset and removal.
Physical bus-event trigger timing remains untested while the phone is disconnected.


The common CoreSight core requires callable helper operations. QMI path activation
checks service readiness and counts path votes before its source sends commands.
TGU explicitly rejects trace-path activation because its arming interface is the
native control attributes; no fitted TGU node has a trace graph connection.


`coresight_dummy` registers native subsystem-owned source/sink endpoints. IDs are
allocated before publication and released after unregister; native static IDs
use the common allocator's collision checks. Source activation propagates QMI
assignment failures and unresolved suppliers. Passive sinks count shared path
votes and reject incompatible modes. `coresight_remote_etm` validates the native
1–8 ATID messages, keeps dynamic IDs until removal, and reserves static IDs only
while enabled so inactive native overlap remains allowed. Firmware enable/stop
and suspend/resume errors propagate. Failed start followed by an unconfirmed
stop retains the reserved IDs; they are not recycled while firmware may emit.

The CSR helper routes the primary path ID. New `qcom_csr_route_ids` routes only
additional IDs through the sink's actual CSR helper, with per-ID votes and
rollback. It preserves native non-ETR sinks that have no ATID filter and never
uses a global CSR pointer or undocumented MMIO bank. Existing DT names, QMI
commands, secure-component properties and firmware restrictions are unchanged.

`verify_coresight_sources.py` passes 335 actual-C ASan/UBSan cases for multi-ID
CSR reference rollback and original register bits, every valid/invalid 8-bit ID,
malformed counts, static collisions, probe allocation failures, firmware errors,
stop quarantine/retry, suspend/resume, shared dummy sinks and teardown. Physical
trace delivery remains untested while the phone is disconnected.

## Bluetooth control transport priority (2026-11-03)

The 352-module installation lacks native `glink_pkt`. Firmware already exposes the unbound ADSP `bt_cp_ctrl` RPMSG channel, and native Onyx DT requests its `/dev/bt_cp_ctrl` endpoint. The vendor Bluetooth library opens this control device before it opens the codec DMA client. ROM ueventd already supplies the required bluetooth:system ownership; restore this kernel provider before assessing a ROM change. Playback success still requires a runtime retest.

Source: `baseline/kernel/xiaomi/sm8735/drivers/rpmsg/glink_pkt.c`; draft/source review in `/home/loukious/Android/konoha-remaining-diagnostics/glink-pkt-draft` and `port/GLINK-PKT-PORT-REVIEW.md` in that worktree. Patch 0210 reserved. Native removal, registration-error propagation, nonblocking read locking, obsolete IDA calls and zero-copy mapping ownership need repair before validation/installation. The provider has no native OF aliases despite matching an active `qcom,glinkpkt` node, so the prior inventory's absence of DT matches is incomplete.

Hotspot reboot remains unresolved after two captures, including offload disabled. See `validation/hotspot_bt_20261103/findings.md`; no error suppression or speculative protocol change is justified by the current traces.


The GLINK port retains native packet/signals/intent semantics and full-page zero
copy, with zeroed partial boundaries, exact mapping ownership and teardown.
Kernel and sanitizer checks pass; native empty-packet writes are explicitly
tested. Bluetooth needs a live retest after installing matched images. The
hotspot reset site is still unproven. ROM findings remain notes only.

The CPUSS v4 sleep-statistics draft is preserved in
`/home/loukious/Android/konoha-remaining-scmi-stats/cpuss-v4-draft`, commit
`45ac441d2`. All 371 complete-driver ASan/UBSan checks and zero-fuzz forward/reverse
patch replay pass. It is not part of this kernel build; integration, kernel
compilation and packaging remain. The existing older Onyx CPUSS node stays
unchanged. Continue all remaining native module ports.

Hotspot v10 now proves a NULL wiphy mutex in HDD channel-change notifications. Kernel compatibility patch 0212 and host regression checks are integrated; production build and runtime retest pending. Bluetooth playback and native GLINK DMA acknowledgements were verified after the 371-module install. See validation/hotspot_bt_20261103.

Hotspot patch 0212 is built and installed in artifact 20261103T135741329740Z. Both connected-STA and Wi-Fi-off secured AP tests remain active for 45 seconds without reboot; all partition readbacks and nine loaded module identities match. Settings/tether data forwarding pending user test. There are still 100 distinct unavailable retained load-list names.
