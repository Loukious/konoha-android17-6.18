# NetHunter and CAN build validation — 2026-11-04

The complete recipe passed a combined GKI and 400-platform-module Kleaf build.
Five wireless target dependency lists were corrected to name their direct
mac80211/cfg80211 providers before the successful final build. Modpost remains
strict; no unresolved symbols or invented providers were permitted.

The tested inputs were Google GKI
`ee9c8fd574f1b2c93cfc74b74b6f3a0d78a5e3a0` and KernelSU Next dev
`b2c1ad00b755afe0db74d889b67a137f56e71fcc`. The resulting kernel release was
`6.18.38-android17-5-maybe-dirty-4k` with built-in KernelSU and module versioning.

- Exact recipe replay verified all 5,193 module source/build files and 91,829
  common-source entries, including executable bits and symlink targets.
- All 400 platform outputs passed 24,154 imported symbol CRC checks.
- Native packaging verified 30 required config values, 37 added module outputs,
  ten retained native dependencies and the exact built-release prefix across
  the 147-module native archive.
- Firmware checks verified all 16 requested WLAN blobs, their redistribution
  notices and the matching ATH firmware source archive.
- Both Tuna/Tuna7 device trees passed compilation, overlay-engine agreement,
  preserved bootloader identities and DTBO table verification.
- The independent CI artifact verifier accepted all 2,524 hashed artifact files,
  the exact catalog, configuration, source pins, ABI and KernelSU source archive.
- Nine native packaging regressions passed, including missing modules/config,
  wrong kernel release, altered firmware, missing notices/source and the
  conflicting phone-side RNDIS gadget configuration.

A matched ZIP was generated against the saved development ROM baseline. It
contains 517 unique module binaries: 405 in the vendor ramdisk, 451 in vendor
DLKM and 78 in system DLKM, with required duplicates across loaders. All 546
compiled catalog binaries passed 33,806 imported symbol CRC checks during
packaging. The five full partition images fit their original sizes; independent
content checks verified the kernel/device trees, module bytes, 16 firmware files,
nonmodule records, ownership, source/notices and rollback prerequisites.

The shipped toolkit passed seven generator tests, 30 installer scenarios,
44 native guard syscall cases, 48 device-mapper parser cases and 35 Linux UAPI
assertions. Independent review additionally passed 37 installer scenarios and
19 dependency-closure cases. The static AArch64 helper reproduced byte-for-byte
from its pinned compiler and source. These remain offline checks.

The recovery packer builds new images from these outputs and a separately
supplied ROM baseline. Build/source checks do not establish recovery execution,
boot, radio injection, physical CAN/USB adapter behavior or suspend/power results.
The previous 391-platform/471-installed checkpoint remains the phone-validated
baseline until the new matched image set receives its own device validation.
