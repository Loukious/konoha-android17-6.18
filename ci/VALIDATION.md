# Automation validation

Before publication on 2026-11-02:

- All 4,862 committed module/build files match the installed 6.18.33 recipe's
  materialized source. The catalog contains 318 platform modules and 179 patches.
- The real updater was exercised in an isolated workspace against
  `ee9c8fd574f1b2c93cfc74b74b6f3a0d78a5e3a0` (Android 17 LTS 6.18.38).
  Common-source pins and imported Git blobs refreshed successfully. All 179
  module patches replayed with zero fuzz, and the complete snapshot regenerated.
  The updater left the upstream checkout and the installed development workspace
  intact. This proves source replay, not compilation or hardware operation.
- The refreshed production CoreSight callbacks passed 94 lifecycle cases
  under ASan/UBSan. Original callbacks and five regressions were rejected;
  required source patches restore the checkout after a simulated build failure.
- Fifteen automation tests cover exact branch/revision checks, pin/blob refresh,
  unchanged-input detection, KernelSU-only updates, source omissions/edits,
  patch integration/conflicts, partial publisher recovery, duplicate/closed PRs,
  and stale build-state writes.
- Both workflows pass actionlint; Python compilation and Bash syntax checks pass.

The accepted 6.18.33 baseline passed local GKI/module/DT builds and bounded
device startup and camera tests. Its camera/SYNX patches classify expected
notifications and polls; they do not repair the unresolved synchronization issue.
Hosted execution and each newer LTS build are validated separately in Actions.
