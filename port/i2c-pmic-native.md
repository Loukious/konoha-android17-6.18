# Native I2C PMIC diagnostics

The original provider-deployment checkpoint is described below. The [current 391-module runtime summary](../validation/onyx-runtime-summary-20261104.md) records the latest kernel and outstanding limits; earlier counts, preserved-kernel and temporary Wi-Fi statements describe that historical checkpoint.

The standard vendor module loader loads qcom-i2c-pmic.ko and the repaired regmap/PMIC debug providers on the original working GKI. Both Onyx debug clients bind. The port bounds IRQ mappings and address tables, restores failed suspend masks, drains parent IRQs before child domains, protects debugfs callbacks and checks I2C addresses separately from SPMI SIDs. All four existing regmap exports retain their CRCs.

578,083 actual-C ASan/UBSan cases, pinned replay, 107 production imports, package checks, reboot/readbacks and two live client reload cycles pass. Stale address/count/data descriptors fail safely after unbind. Firmware returns zero bytes for the I2C identification request, so hardware register reads remain unavailable. No register write, IRQ self-test, crash or firmware modification was performed.

See [acceptance](../validation/onyx-runtime-summary-20261104.md), [wire evidence](../validation/onyx-runtime-summary-20261104.md), [lifecycle](../validation/onyx-runtime-summary-20261104.md) and [remaining queue](../validation/remaining_391_modules/priorities.md).
