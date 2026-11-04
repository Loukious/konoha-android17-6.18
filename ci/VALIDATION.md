# Automation validation

Before publication on 2026-11-02:

- All 4,862 committed module/build files match the installed 6.18.33 recipe's
  materialized source. The catalog contains 318 platform modules and 179 patches.
- The separate source branch also contains 91,828 GKI files/symlinks, recreated
  from upstream with required patches. Complete source regeneration and
  verification passed again against 6.18.38 after separating the CI branch.
- The real updater was exercised in an isolated workspace against
  `ee9c8fd574f1b2c93cfc74b74b6f3a0d78a5e3a0` (Android 17 LTS 6.18.38).
  Common-source pins and imported Git blobs refreshed successfully. All 179
  module patches replayed with zero fuzz, and the complete snapshot regenerated.
  The updater left the upstream checkout and the installed development workspace
  intact. This proves source replay, not compilation or hardware operation.
- The refreshed production CoreSight callbacks passed 94 lifecycle cases
  under ASan/UBSan. Original callbacks and five regressions were rejected;
  required source patches restore the checkout after a simulated build failure.
- Twenty-eight automation tests cover exact branch/revision checks, pin/blob refresh,
  unchanged-input detection, KernelSU-only updates, source omissions/edits,
  patch integration/conflicts, partial publisher recovery, duplicate/closed PRs,
  stale CI/source build-state writes, executable/symlink preservation, source
  branch selection, and new KernelSU builds while an LTS candidate remains open.
- Automatic promotion additionally requires the complete build job and its
  source status to succeed, validates the built kernel pin and candidate head,
  and uses an atomic base lease for an exact fast-forward. Failed builds,
  changed heads, changed bases, mismatched pins and a concurrent push are
  rejected without recording an unpromoted build.
- Both workflows pass actionlint; Python compilation and Bash syntax checks pass.

The accepted 6.18.33 baseline passed local GKI/module/DT builds and bounded
device startup and camera tests. Its camera/SYNX patches classify expected
notifications and polls; they do not repair the unresolved synchronization issue.
Hosted execution and each newer LTS build are validated separately in Actions.

## Automatic release validation — 2026-11-04

The updated automation passes 68 host tests, actionlint 1.7.12, Python compilation,
Bash syntax and Git whitespace checks. Actual temporary Git repositories exercise
GKI updates, KernelSU dev updates, accepted-source edits, unchanged refs, missing
release state and failed remote lookup; detection never changes accepted state.
Candidate and accepted-source dispatch tests cover duplicate suppression and a
three-attempt limit for failed or cancelled builds.

Export/release tests exercise complete archive round trips and reject same-count
wrong module sets, duplicates, missing hashes, changed binaries, wrong vermagic,
missing import verification, mismatched pins/ABI/run identities, unsafe archive
paths, failed device-tree verification and diagnostic builds. GitHub API/command
fixtures verify draft creation, exact commit targets, successful full uploads,
publication only after inventory/hash checks, existing-release preservation,
failed-build/stale-source rejection and incomplete/failed-upload recovery.
An already-promoted candidate can recover publication without another source push.
Regression cases additionally cover a head advance during upload, publication
rejection before Latest can change, the shared serial promotion/release group,
attempt-specific upload names, and release-only reruns that reuse and verify the
original successful build attempt rather than the current release-job attempt.

The new inventory verifier also passed against the actual frozen accepted
6.18.38 artifact with all 391 platform modules, both DT variants and KernelSU
Next revision `a6c81fa4a8b7341d0bc5c3e4507531459df871ae`. This is a read-only
artifact check; no source, firmware, phone or GitHub release was changed locally.

Existing hosted build 41636617465 completed its full build and upload successfully,
and tracker 41831501184 correctly skipped unchanged upstream inputs. Those runs
predate release publication. The new release path still requires its first hosted
end-to-end build.

Hosted run 41885999581 passed automation validation but stopped before source
sync: its COW root reported 33 GiB free after the original cleanup, below the
55 GiB gate. Bootstrap now requires a disposable GitHub-hosted Linux environment
and removes the unused hosted tool cache plus explicit SDK/toolchain directories.
System Python, apt, gh and runner Node externals are preserved. Before/after disk
usage is logged; insufficient space also records block devices and top-level
directory usage. The 55 GiB gate remains unchanged.

Four additional bootstrap tests execute only inert command mocks, covering the
environment guard, cleanup targets, preserved tools, disk-gate failure and exact
55 GiB boundary. No local system cleanup was performed. Reclaimed hosted space
and complete compilation still require the next hosted run.

## Common recipe additions — 2026-11-04

The complete CI suite now passes 81 host tests. Thirteen added updater tests
cover mixed Git/unified sections, header-looking hunk content, new certificate
and HVC-header bytes with null before hashes, identical upstream additions,
conflicting additions, missing modified sources, inventory mismatches, duplicate
paths, truncated hunks, nonregular modes and unsafe source/recipe paths.
Diagnostic conflicts and symlinked input/output files or ancestors leave every
recipe and common-source file unchanged. All output paths are checked before
the first recipe write. Python compilation, actionlint and whitespace checks pass.

An independent isolated replay against clean committed files from
`ee9c8fd574f1b2c93cfc74b74b6f3a0d78a5e3a0` exercised the actual accepted recipe:
19 paths, comprising 13 Git sections and six bare unified sections. Forward
refresh preserved the exact effective patch and all accepted before/candidate
hashes, including absent `certs/onyx-accepted-gki.pem` and
`include/linux/hvc_backend.h`. Partially and fully integrated upstream fixtures
produced the expected active patch sets. The production common-patch verifier
accepted each refreshed recipe and restored the clean fixture after application.
Late common and QDSS diagnostic conflicts left the recipe and common fixture
unchanged. These are source-replay checks; hosted compilation, release publication
and device behavior remain separate validation steps.
