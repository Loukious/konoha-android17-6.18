# Kernel trace, charging and startup findings

This work is limited to kernel sources and kernel/module packaging. ROM policy,
init scripts, bootloader settings, and charger firmware are not modified.

## Kernel changes

The native CSR provider is a common CoreSight helper. Each path votes only its
assigned trace ID into the selected ETR bank, with shared references and restoration
of the previous register bits. A distinct helper subtype prevents CSR from being
mistaken for CATU scatter-gather hardware. Both offsets (0xf8/0x108) are taken from
the retained native DT and checked against the mapped CSR region.

ETR exposes the native byte-cntr device using its eight-byte hardware counter.
Streaming uses a coherent flat DMA buffer, a retained buffer reference, a single
reader, poll/nonblocking reads, EOF after stop, and explicit overrun errors.
Scatter-gather, reserved-memory and perf buffers are not exposed through this
stream API. CoreSight's ordinary stopped-buffer and perf interfaces remain.
Ordinary stopped-buffer reads return EBUSY while the stream is open; stopped
buffers retain their reference until the reader closes. Barrier insertion is
skipped for that stream so stopping cannot rewrite data being consumed.

The native trace-NoC provider uses common graph references and reserves its trace
ID before path construction. BC/TC/GPR configuration is advertised only by native
TPDMs whose hardware dataset bits report support. Register arrays, counter masks,
trigger capabilities and MSR availability are bounded; writes during tracing fail.
Existing DSB/CMB and ETF behavior is retained.

The wired reverse monitor commits gear state only after PD acceptance and requests
the safe gear when eligibility/current reads fail. CP ADC errors propagate into
the retry path. User requests retain peer authentication, SOC, temperature and
OTG requirements. Reverse battery-current limiting still has no native fitted
provider operation or firmware property. No new property ID is manufactured.

VFE lookup validates output/index before indexing and treats unfitted slots as
normal enumeration. Missing optional SFE and successful debug-buffer allocation
are diagnostics. CCI IRQ threads retain the FIFO policy already supplied by the
IRQ core, avoiding a scheduler permission check in vendor_modprobe's context.
Actual allocation, IRQ registration and mandatory IFE/CSID failures remain errors.

Statistics preserve the SMEM error: absent firmware items can return ENOENT,
malformed items EINVAL, lock/transport errors their actual errno. APSS/modem/WPSS/
ADSP/CDSP and system/DDR statistics retain the native ioctl ABI and bounds.

## Validation

Local artifact `20261103T023508668153Z` builds the matching 6.18.38 GKI and all
352 modules. All 21,880 imported-symbol CRC checks, 1,120 platform ASan/UBSan
cases, 57 device-service cases and 50 thermal cases pass. The thermal fixture
also checks 4,000 concurrent QoS writes and preservation of 34 native attributes.
Exact recipe replay verifies 4,972 module files and 91,827 common files. Both
DT variants and overlay engines pass; the 1,097 inherited DTC warnings remain.
All 2,116 archived artifact hashes match.

The failure-injection cases exercise CSR routing/refcounts/restoration, stream
overrun boundaries, TPDM capability/index bounds, reverse request/monitor errors,
VFE lookup bounds and the precise statistics errno. They do not establish
hardware trace timing, authenticated reverse power transfer or camera behavior.
The 352-module artifact was installed on 2026-11-03. All six partition readbacks,
the kernel build ID and selected module build IDs match. Android completed boot
with SELinux enforcing and mi_thermald running. Statistics ABI and thermal
regression checks pass; the bounded startup capture reports no CPU warnings,
BUGs, panics, page allocation failures or refcount warnings. Both CSR devices,
both byte-counter devices and four trace-NoC devices are present. Camera service
enumerates seven devices and two public cameras; the removed CCI scheduler calls
produce no sys_nice denial in this capture. Streaming and trace timing still need
their own hardware checks.

## ROM and firmware findings: notes only

The installed baseline boot capture reports seven camera devices, two public
cameras, and eight kernel camera startup messages covered by the changes above.
Its two vendor_modprobe sys_nice denials occur at the CCI scheduler calls.

Other inherited denials concern vendor_nicmd netlink requests, init relabeling of
KernelSU/module files, RIL property access, system_suspend access to generic sysfs,
filesystem checks on the rescue block device, kernel block-path traversal and
Bluetooth RFCOMM's privileged L2CAP service bind. These require ROM labeling,
service configuration or policy review. Counts include duplicate audit entries
and cannot be compared as rates across captures. Kernel permission checks remain.

These are ROM-side follow-ups, recorded here without implementing policy changes:

| Finding | ROM-side action | Kernel disposition |
| --- | --- | --- |
| vendor_nicmd netlink `nlmsg` | Check the service's requested netlink family and its domain permissions against the vendor implementation. | Retain netlink permission checks. |
| init relabeling module/KernelSU files | Correct the file contexts and ownership of the packaged files and the service that relabels them. | Do not bypass relabel checks. |
| RIL property access | Match vendor property contexts and property access rules to the RIL service. | Retain property restrictions. |
| system_suspend generic sysfs access | Label the actual power/wakeup endpoints and grant the intended domain only the required access. | Keep the existing sysfs interfaces and enforcing checks. |
| rescue block fsck / kernel block-path traversal | Audit block-device labels, parent-directory labels and init fsck service configuration. | Do not make block paths generally accessible. |
| Bluetooth RFCOMM privileged L2CAP bind | Check the Bluetooth service domain and the intended privileged PSM capability/permission. | Retain the privileged bind check. |
| CCI vendor_modprobe `sys_nice` | No new ROM permission is needed for the redundant scheduler operation removed here. | Use the IRQ core's existing FIFO thread policy. |

The previous off-mode observations reached ordinary Android with
mca_log.charge_boot_mode=0 and no androidboot.mode=charger indication. The kernel
retains the native charger-mode module parameter and charging strategies. Boot
mode selection and shutdown handoff need separate bootloader/ROM evidence; this
is not grounds to force charger mode or change init/policy in the kernel port.

SPMI debug is refused by the fitted fuse value; SLIMbus is disabled in native DT.
Those states are retained. SLPI, GPU, display and SLPI-island statistics were the
four optional EIO results; precise SMEM errno will distinguish missing items after
the new kernel is installed. Missing firmware statistics are not fabricated.

Authenticated reverse transfer, high-power charging and powered-off charging
still require physical evidence. No wireless reverse provider was found.
