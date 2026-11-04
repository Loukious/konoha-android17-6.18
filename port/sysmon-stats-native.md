# Native SysMon subsystem statistics

The original provider-deployment checkpoint is described below. The [current 391-module runtime summary](../validation/onyx-runtime-summary-20261104.md) records the latest kernel and outstanding limits; earlier counts, preserved-kernel and temporary Wi-Fi statements describe that historical checkpoint.

`sysmon_subsystem_stats.ko` is installed through the existing vendor module loader on the retained original GKI. All 381 existing platform module binaries were preserved; platform count is now 382. Native ADSP/CDSP debugfs data and the six exported APIs passed two normal read/unload/reload cycles. The temporary API helper was removed; the installed provider remains loaded.

The port bounds firmware feature records and copies private snapshots without writing SMEM, refreshes each query, tolerates independently absent items, checks NULL arguments, clamps inconsistent power deltas, saturates counters, and drains diagnostic readers before removal. The complete native C passed 38,271 ASan/UBSan cases and five regression mutations. Pinned patch replay and 18 module imports passed. The header and all six native exports are retained.

Firmware exposes ADSP/CDSP power feature version 4 with the compatible native prefix. SLPI items are absent. SMEM provides no sequence protocol: samples may span firmware updates. No intentional DSP restart, crash or firmware mutation was performed. One USB write failed; a complete retry and all six partition readback hashes passed before acceptance.

See [acceptance](../validation/onyx-runtime-summary-20261104.md), [runtime](../validation/onyx-runtime-summary-20261104.md), [lifecycle](../validation/onyx-runtime-summary-20261104.md), and [remaining priority queue](../validation/remaining_391_modules/priorities.md). Broader peripheral and endurance validation remains.
