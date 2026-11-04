# Native P2 providers on Onyx

The original provider-deployment checkpoint is described below. The [current 391-module runtime summary](../validation/onyx-runtime-summary-20261104.md) records the latest kernel and outstanding limits; earlier counts, preserved-kernel and temporary Wi-Fi statements describe that historical checkpoint.

`mem-hooks.ko`, `gh_rm_booster.ko`, `tmecom-intf.ko` and
`gh_panic_notifier.ko` are integrated in the native DDK recipe and installed.
Onyx's existing ROM load list requests all four. The existing Android module
loader loads them automatically; no KernelSU module or boot-time workaround
was added. The catalogue now has 378 platform modules, with 458 packaged modules
and 93 retained stock names unavailable.

The original kernel, boot image, DT, bootconfig and firmware remain intact.
The installation updates only vendor_boot and vendor_dlkm. First-stage
`gh_dbl`, `msm_qmp` and `gunyah_loader` carry the required callback-draining,
mailbox bounds and bounded panic-polling fixes. Their exported symbol CRCs,
types and namespaces match the accepted providers. All other platform modules,
including Lineage qcom_lpm and the WLAN fixes, retain their accepted binaries.

Validation is recorded in [p2_native_20261104](../validation/onyx-runtime-summary-20261104.md):

- Full pinned patch replay for 5,019 inputs; the existing IR clock edit is
  preserved in the workspace and excluded from deployment.
- Native builds of four modules and three providers, with all imports checked
  against the exact original GKI and selected provider symbol tables.
- Actual C under ASan/UBSan: 131,099 booster cases, 300,003 threaded booster
  operations, 4,131 panic ownership cases, 97 doorbell cases, 53 combined RX
  cases, 2,073 TME cases, 26 log-wrapper cases, 17 bounded polling cases and
  1,110 mailbox bounds/draining cases. Frameworks and firmware are mocked
  in these offline tests.
- Native TME transaction IDs echoed on two requests before installation and
  four requests after installation, including after client teardown/reprobe.
  The firmware log command returns status 1 and no log bytes; this verifies
  communication and transaction matching, not log availability or HWKM.
- Fresh six-partition backup, verified image contents/AVB, 8 MiB sparse flashes,
  and matching readbacks of every partition. All four modules automatically
  load, and all configured device bindings and live build IDs match.
- Normal TME, booster and panic-notifier unload/reload passed. Invalid booster
  CPU selection was rejected. CPU selection and recovery policy were preserved.
- Android/audio boot, enforcing SELinux, qcom-cpu-lpm with sleep enabled, a
  30-second stability smoke, Wi-Fi/BT enabled, and restored cached Wi-Fi MCC
  mode 0 all passed.

The original kernel build ID is
`86468c6862210bf79737f6c23d53614ae24f1983`.

No hardware memory fault, kernel panic or VM crash was deliberately induced.
Those recovery branches have offline coverage; actual firmware reclaim after
a crash, fault recovery, suspend/hotplug and long-term stability remain
unvalidated. Quarantined shared allocations retain their device/module refs
until RM reclaim and SCM ownership restoration succeed. CMA release clears
only the device's pool pointer, and retained pages stay reserved in the shared
allocation bitmap until explicitly freed.

The earlier rejected test kernel remains rejected. The software hardware
bootloader route remains a separate issue; adb reboot fastboot reached
fastbootd successfully for this installation.
