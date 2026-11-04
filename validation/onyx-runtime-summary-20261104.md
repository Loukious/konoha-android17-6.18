# Onyx 391-module device validation checkpoint

The accepted local source/image set uses Linux 6.18.38, 391 platform modules and
471 packaged modules on a global POCO F7 (Onyx, SM8735). Its kernel GNU build ID is
b8587a51d2a866026061adfba1f937f3b30cde6f. The latest restoration verified all nine
partition hashes against the accepted image set, Android boot completion, KernelSU
root, audio policy, enforcing SELinux and native CPU-frequency/idle governors.
WALT is selected on four policies; all 48 idle tunables and eight frequency limits
match the original 6.6 baseline. Normal Android interaction boosts remain active.

Native platform coverage includes clocks, regulators, SMMU, UFS/ICE, WALT/DCVS,
display/GPU, camera and touch providers, wired charging, USB, audio/DSP, networking,
haptics, NFC and fingerprint providers. Provider presence is not exhaustive testing
of every feature or hardware mode. Prior bounded photo/video and Bluetooth audio
checks are retained as development history; broader current-build testing remains.

The latest priority additions include native Qualcomm LPM, RM booster, TME client,
panic notifier, memory hooks, EDAC, CPU-frequency debug, charger logging, DSP/system
power statistics, PMIC I2C, battery/hung-task/IOMMU diagnostics, HVC and MTD.
Normal MTD reboot archival passed with a temporary exact ROM policy allowance.
The allowance was removed after testing; automatic adapter attachment waits for
ROM policy patch 0018. Direct panic/hard-lockup persistence is not established.

Battery QBG replies work. Aggregation sends the corrected stock-compatible 16-byte
request but still times out. Native HVC loading and failed-open cleanup pass; useful
firmware consoles/guests remain unavailable and that investigation is deferred.
IOMMU mappings, coherent access and ATOS translations passed on a dedicated test
stream; peripheral DMA payload transfer and secure/GPU use cases were not tested.
Wi-Fi concurrent-channel ROM work remains pending; the temporary workaround is
unapplied. Broader peripheral, suspend/hotplug, power and endurance tests remain.

Both Tuna/Tuna7 Onyx trees pass offline identity and overlay validation. Only the
development phone is physically validated. No alternate Sun/Kera phone port or
China-only notification LED support is claimed.

Production publication gates reproduce 5,031 pinned imports and 230 patches into
5,042 module files, and verify 91,829 common entries against the accepted inputs.
The nine newly integrated portable host suites pass 839,655 actual-C cases and
61 regression mutations under ASan/UBSan. Additional local lifecycle suites were
used during development; their external fixtures are not claimed as portable CI
inputs. Raw device traces, images, firmware and machine-specific installer receipts
remain local. Automated releases provide matching GKI/module build artifacts and
verification reports, not universal flashable images or hardware certification.
