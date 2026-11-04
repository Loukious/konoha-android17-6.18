# Current Onyx module checkpoint

The accepted Linux 6.18.38 port contains **391 platform modules**; the matched
package on the development phone contains **471 total modules**. Android startup,
KernelSU root, audio, enforcing SELinux, native WALT/LPM and partition identity
checks passed. Hardware validation covers this Onyx / SM8735 phone. Both Tuna
variants pass offline device-tree checks; other phones or motherboard revisions
are not established as compatible.

Normal MTD reboot archival preserved kernel and Android pmsg records while the
other seven archive slots stayed byte-identical. Automatic block2mtd attachment
waits for the saved ROM policy patch. Direct panic/hard-lockup capture is
unsupported; poweroff and long-power-key paths have host coverage only.

Battery QBG replies work. Aggregation uses the corrected stock-compatible
16-byte request but still times out; its cause is unresolved in the tested
kernel/ROM/firmware combination. HVC autoload and failed-open cleanup pass;
useful guest consoles and provisioning remain unavailable and deferred.
IOMMU DMA API mappings, coherent CPU access and ATOS translations passed on a
dedicated test stream. Peripheral DMA payload transfer, secure/GPU use cases
and full-VA stress remain untested.

No missing fundamental driver was identified for demonstrated ordinary phone
operation. The [remaining queue](../validation/remaining_391_modules/priorities.md)
classifies 79 retained names by applicability; a missing module name does not
prove a missing phone feature. `xhci_sideband` is already built into GKI.
China-only notification LED work and other unconfirmed hardware are outside
the active target. Wi-Fi concurrent-channel ROM work remains pending, with its
temporary workaround unapplied. Broader feature, suspend/hotplug, power and
endurance testing remains.

See the [public runtime summary](../validation/onyx-runtime-summary-20261104.md)
for current coverage and limits. Historical source checkpoints remain in Git
history; raw device traces, firmware, images and machine-specific installer
receipts remain local. Automated builds/releases verify source, module CRCs,
artifacts and device trees; new outputs still require device validation.
