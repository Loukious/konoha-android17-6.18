The Onyx ROM advertises infrared and includes native DSP, memory-sharing,
Qualcomm crypto and USB audio offload services that were absent from the
6.18 module set. This candidate adds ten modules to the thermal candidate:

| Module | Restored interface or provider |
| --- | --- |
| `ir-spi.ko` | `ir-spi-led` SPI transmitter and LIRC device used by the IR HAL |
| `adsp_sleepmon.ko` | ADSP session ioctls, SMEM sleep statistics and RPMSG monitoring |
| `cdsprm.ko` | CDSP resource/QoS, cooling, VTCM and firmware coordination |
| `heap_mem_ext_v01.ko` | Native heap extension QMI message schema |
| `msm_memshare.ko` | QMI service 0x34 and DT-declared peripheral memory clients |
| `sps_drv.ko` | Native no-data-path BAM/SPS transport used by the crypto engine |
| `qce50_dlkm.ko` | Qualcomm CE hardware and SPS submission provider |
| `qcedev-mod_dlkm.ko` | Native `/dev/qce` ioctls, DMA-BUF mappings and compat ioctls |
| `qcrypto-msm_dlkm.ko` | Native asynchronous Linux crypto API algorithms |
| `snd-usb-audio-qmi.ko` | Native USB audio QMI offload and xHCI sideband coordination |

Imports retain the existing exact native kernel/module revisions. IR uses the
pinned 6.18 upstream implementation, with the same regulator, DT compatible
and pulse format as the native driver. USB audio private headers come from the
pinned 6.18 kernel so the builtin USB audio provider and external driver agree
on their structure layouts. Patch 0181 records the complete adaptations.

IR uses 64-bit duration arithmetic, validates carrier and duty-cycle values,
and bounds each transmission to 32,768 carrier periods. It preserves the
caller waveform and balances allocation/regulator resources on failures.

ADSP initializes its completion before publishing callbacks and checks error
pointers from thread, class and notifier creation. Probe failures unwind only
acquired resources. Removal withdraws notifiers/RPMSG, shuts down the timer,
stops its worker and releases devices. Remote-processor references are paired;
SMEM initialization is serialized and PM notifier failures propagate.

CDSP retains native messages and exported callbacks. Registration failures
unwind platform drivers and work resources. CPU QoS requests validate CPU
indices and track each acquired request, including partially successful votes;
removal releases those requests and their allocations.

Memory sharing retains the native message schema and DT client configuration.
It serializes client operations, advertises the QMI service after initial
population, and retains/unregisters its SSR notifier. Unknown clients cannot
create partially initialized slots. Allocation replies succeed only after
secure assignment and optional GLINK registration. Allocation size is tracked
separately from usable size, avoiding double-counted guard bands during free.
Reclaim failures retain remotely owned pages instead of returning them to the
allocator. Platform removal closes the service and depopulates children before
releasing its state. Retaining pages on an unrecoverable secure-assignment
failure can leak that allocation; this is deliberate failure containment.

Crypto preserves the native CE/SPS and userspace/crypto APIs, updates the timer,
platform removal and namespace APIs, and drains the CE timer before freeing
hardware state. Removing an obsolete ION header does not remove DMA-BUF support.

USB audio uses the current xHCI registration/interrupter and IOMMU APIs. QMI
handlers have a sentinel and the actual decoded structure size. Card,
direction and interface indices are validated before accessing arrays. The
QMI server is published after the device/IOMMU state is ready. Connected USB
devices hold the module until their disconnect callback finishes; bind controls
are suppressed for this global service driver. xHCI transfer-ring notifications
send the native DSP disconnect indication and wait for release. Probe failure
and removal release the server and IOMMU domain; unused IOVA metadata is freed.
The DSP release wait remains bounded by the native ten-second timeout. Firmware
acknowledgement and active streaming teardown still require device validation.

`verify_device_services.py` runs actual driver functions with host service
adapters under AddressSanitizer and UndefinedBehaviorSanitizer: IR waveform,
bounds and failures; ADSP probe failures and timer/thread shutdown; memory
assignment/reclaim failures, repeated allocation and guard-band accounting;
and USB audio request bounds and service intervals. Every full build runs and
archives these checks, plus the existing thermal checks and all module CRCs.
These tests do not establish DSP firmware compatibility, crypto correctness,
IR optical output or sustained USB audio behavior.

The local 2026-11-02 full build passed on the pinned 6.18.38 GKI with all 329
modules and 21,008 imported symbol CRC checks. Exact source replay verified
4,928 module files and the unchanged 91,827-file GKI snapshot. All 57 new host
cases and the existing 50 thermal cases passed under both sanitizers. Both
device-tree variants passed overlay-engine agreement, bootloader ID preservation
and DTBO table checks; compilation still reports 1,097 DT warnings. All 2,017
artifact hashes were independently checked. Hosted CI and device validation
remain separate checks.

The phone has not been flashed with this candidate. Before installation, prepare
fresh backups and matching GKI/DLKM images against the installed thermal build.
After boot, confirm driver bindings and native nodes, test actual IR reception,
DSP sleep/VTCM behavior, crypto known-answer operations, and USB audio streaming
and physical disconnects. Sustained thermal testing and the separate swap
reclaim warning remain pending.

The pinned charging strategy source does not implement the handle-charge and
quick reverse-charge extension events discussed in the remaining-gap audit.
Those controls remain explicitly unsupported. There is no demonstrated Onyx
wireless reverse-charge provider to restore through this driver port. Powered-off
charging mode selection also remains a separate investigation. `thermal_config`
and DDR cooling are excluded: the former is predominantly diagnostic and the
validated Onyx merged DT has no DDR cooling compatible.
