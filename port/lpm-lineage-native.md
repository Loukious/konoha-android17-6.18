# Native LineageOS qcom_lpm on Onyx

The original provider-deployment checkpoint is described below. The [current 391-module runtime summary](../validation/onyx-runtime-summary-20261104.md) records the latest kernel and outstanding limits; earlier counts, preserved-kernel and temporary Wi-Fi statements describe that historical checkpoint.

The full CPU/cluster governor from `LineageOS/android_kernel_qcom_sm8850`, branch
`lineage-24.0`, revision `557159b6a0d06f1c6f07ec4c1f22bc9309a113e4`, is integrated
as `//onyx:qcom_lpm` in the production DDK recipe. The six source files are pinned
by Git blob in `imports.json`; patch 0217 carries the tested 6.18 adaptations.
`CONFIG_CPU_IDLE_GOV_QCOM_LPM=m` builds the native `qcom_lpm.ko` driver.

Android GKI normally packages Qualcomm drivers in DLKM images. Onyx already
requests `qcom_lpm.ko` in its ROM second-stage module list. The new vendor_dlkm
image supplies that driver and generated dependency metadata; its existing
vendor.modprobe service loads WALT first and LPM at boot. Cpuidle selects
`qcom-cpu-lpm` through the driver's rating of 50 (menu is 20). The existing ROM
post-boot script writes N to the native sleep_disabled attribute. There is no
additional governor-selection service or KernelSU module.

The adaptation registers the actual Android PM-domain trace hook using the
existing exported tracepoint APIs. It fails if the hook is absent. It needs no
new kernel exports; the accepted original GKI and all existing module binaries
remain in use. The new installation changes only `vendor_dlkm_a`.

Validation in [lpm_native_20261104](../validation/onyx-runtime-summary-20261104.md) records:

- Exact pinned source/patch replay for all six LPM inputs; 5,009 catalog matches
  among 5,010 staged files. The one existing IR clock edit is preserved and
  excluded from this deployment.
- Native DDK build and all 96 imported CRCs against the accepted GKI/WALT.
  All 115 function bodies match the tested port after masking ELF relocations;
  source algorithms retain the earlier 2,109 host-case validation.
- 374 production platform modules; 454 packaged modules, 483 checked catalog
  modules and 97 distinct retained load-list names still unavailable.
- A fresh six-partition backup, AVB/content/dependency checks, and a single
  vendor_dlkm flash using 8 MiB sparse chunks. Every partition readback matches.
- Automatic LPM load at uptime 4.435 seconds, successful native modprobe exit,
  selected governor, sleep enabled, cluster binding, full Android/audio boot,
  enforcing SELinux and a 30-second stability smoke.
- Previously authorized temporary Wi-Fi mode0 restored with full native WLAN
  module reload; cached mode is 0. The permanent Wi-Fi ROM patch is not deployed.

Installed module SHA256:
`4e2bd674544a36a47ec0970a8e8bf1cd214c185f5049e250e6bfa96c8b531078`.
Module build ID: `54bc165b61ef220da791edeaccf0c100f3e780a7`.
Original kernel build ID: `86468c6862210bf79737f6c23d53614ae24f1983`.

Installation is verified after one reboot. Suspend/hotplug, long-term stability
and measured battery savings remain unvalidated. The earlier rejected test
kernel stays rejected. Software bootloader routing remains a separate unresolved
issue; `adb reboot fastboot` successfully reached fastbootd for this installation.
