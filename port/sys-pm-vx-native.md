# Native system power-violator diagnostics

The original provider-deployment checkpoint is described below. The [current 391-module runtime summary](../validation/onyx-runtime-summary-20261104.md) records the latest kernel and outstanding limits; earlier counts, preserved-kernel and temporary Wi-Fi statements describe that historical checkpoint.

`sys_pm_vx.ko` is installed by the existing vendor module loader and binds to `c320000.sys-pm-vx` on the retained original GKI. All 382 previous module binaries were preserved; platform count is 383. The port retains native sysfs/debugfs controls and both exported crash-trigger settings. Monitoring and firmware crash-dump triggering keep their disabled native defaults.

Repairs bound firmware reads to the mapped 1 KiB resource, validate timestamp shifts, preserve full timestamps, select each log mode's driver count, initialize all state before publication, withdraw the exported singleton on teardown, and drain debugfs/sysfs readers before releasing resources. The complete native C passed 34,893 ASan/UBSan cases and six regression mutations. Pinned replay and all 43 imports passed.

Live verification covered native automatic loading, normal interface reads and input rejection, two read/unload/reload cycles, and open stats/trigger files across unbind/rebind. Both stale files returned EIO. Firmware has not recorded violator logs (-ENOENT) with monitoring disabled. No live suspend-monitor command, firmware mutation, fault or crash-dump trigger was used; PM and crash branches were tested offline with mocked frameworks. Header rechecks detect some concurrent firmware changes, but firmware offers no whole-log sequence protocol.

See [acceptance](../validation/onyx-runtime-summary-20261104.md), [runtime](../validation/onyx-runtime-summary-20261104.md), [lifecycle](../validation/onyx-runtime-summary-20261104.md) and [priority queue](../validation/remaining_391_modules/priorities.md). Broader peripheral and endurance validation remains.
