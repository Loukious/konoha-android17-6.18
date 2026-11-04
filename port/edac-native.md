# Native LLCC EDAC on Onyx

The original provider-deployment checkpoint is described below. The [current 391-module runtime summary](../validation/onyx-runtime-summary-20261104.md) records the latest kernel and outstanding limits; earlier counts, preserved-kernel and temporary Wi-Fi statements describe that historical checkpoint.

The native `qcom_edac.ko` is installed and automatically binds to the existing
four-bank LLCC child. `llcc-qcom.ko` unregisters its dynamic EDAC child before
its managed register maps are released. The original GKI kernel, DT, firmware,
Lineage qcom_lpm and unrelated module bytes are preserved.

Patch 0224 adapts the pinned Xiaomi driver to the 6.18 EDAC API. It gates
callbacks until setup succeeds, snapshots/restores ECC configuration, uses a
threaded IRQ, drains IRQ/polling before freeing the controller and snapshots
all banks before broadcast clears. Validated UE reports survive syndrome
read errors. Native panic_on_ue remains 1. Patch 0225 fixes parent lifetime.

Validation: 4,159 actual-C ASan/UBSan cases and two rejected regression
mutations; exact original-GKI import/export ABI checks; full pinned source
replay; AVB/content validation; fresh six-partition backup and readbacks;
automatic boot/binding/identity checks; 30-second Android/audio/idle smoke;
and two normal child unload/reprobe cycles. Four banks remained at zero CE/UE.
Wi-Fi mode0 was restored and SELinux remained enforcing.

No ECC injection or intentional phone panic was performed. Parent teardown
was validated offline because live cache consumers must remain running.
Long-term stability and real fault delivery are not established.

[Acceptance](../validation/onyx-runtime-summary-20261104.md) ·
[Current priorities](../validation/remaining_391_modules/priorities.md)

Host checks are reproducible with `python3 port/verify_edac.py`.
