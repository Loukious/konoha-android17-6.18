# Konoha Onyx — Android 17 / Linux 6.18

This repository contains the complete POCO F7 (Onyx/SM8735) module port and a
reproducible GKI build recipe. It tracks Google's `android17-6.18-lts` branch
and fetches the official **KernelSU Next `dev` branch on every build**.

The current Onyx source uses Linux **6.18.38** with **391 platform modules**.
The matched device package contains **471 modules**. It includes native WALT and
Qualcomm low-power idle support, Gunyah transport and HVC lifetime fixes, battery
and IOMMU diagnostics, and block-backed MTD kernel/pmsg archival. The source
snapshot reproduces all **5,042 module files** from pinned imports and patches.
The local matched image set passed Android, KernelSU root, audio, enforcing
SELinux and partition-identity checks on the global POCO F7 used for development.

The device target is **Onyx / SM8735**. Android and Geekbench report the platform
name `sun`; the device-tree recipe includes Tuna and Tuna7 with the Onyx overlay.
Both trees pass offline checks. Hardware testing covers the development phone,
not every phone or board revision using those names. China-only notification LED
support and other unconfirmed hardware remain outside the target scope.

Battery aggregation and firmware console availability remain unresolved optional
diagnostics. Automatic block2mtd attachment requires the saved ROM policy change;
Wi-Fi concurrent-channel ROM work is also pending. Broad peripheral, suspend,
power and endurance validation remains. See [current device validation summary](validation/onyx-runtime-summary-20261104.md) and [remaining module inventory](validation/remaining_391_modules/priorities.md). A new upstream or KernelSU build
requires device validation even when its automated build checks pass.

## Branch layout

- [`ci`](https://github.com/Loukious/konoha-android17-6.18/tree/ci) is the default
  branch and contains only workflows, automation, and documentation.
- [`android17-6.18`](https://github.com/Loukious/konoha-android17-6.18/tree/android17-6.18)
  contains the complete GKI source in `common/`, modified modules in `modules/`,
  and the pinned recipe/patches in `port/`. It contains no workflows.
- `upstream/` branches are proposed updates to the source branch. Builds run
  from `ci` and attach their status to the selected source commit and its PR.

## Source layout

On the source branch:

- `common/` contains the complete pinned upstream GKI with the required common
  fixes applied. CI independently recreates and verifies every source file,
  executable bit and symlink before building.
- `modules/` contains all 5,001 materialized, modified source/build files used
  by the module port, including WLAN, IPA, Bluetooth, graphics, audio, camera,
  charging, PMIC/SPMI, clocks, USB and the remaining device driver groups.
- `port/imports.json` records the exact original repositories, commits and Git
  blobs; `port/patches/` stores the complete adaptation series. `port/stage.py`
  reproduces the module tree from these inputs. CI compares it against the
  committed snapshot, so omitted or changed sources fail the build.
- `port/common-patches/` contains required GKI-source corrections. They are
  applied for every build, separately from the optional QDSS diagnostic logs.
- `port/devicetrees/`, configuration fragments and `modules.json` preserve the
  complete device-tree corrections, module options and expected output set.

On the CI branch, `ci/` and `.github/workflows/` provide upstream detection,
patch replay, hosted builds, artifact checks and source-update pull requests.

The stable 6.6 repository and development logs are maintained separately.
Vendor sources keep their original copyright and license notices; see COPYING.

## Automated updates and builds

The tracker runs daily at 05:00 UTC or through **Run workflow**. It checks
Google's LTS branch and KernelSU Next dev. A new kernel revision produces an
`upstream/` branch with updated common pins, private-header blob imports,
patch hashes and the complete module snapshot. Exact patch replay is required;
an upstream conflict fails the update without changing the accepted source branch.
Already integrated CoreSight fixes are excluded from the effective patch.

The tracker opens a pull request and explicitly dispatches its complete build
(GitHub's default token does not trigger workflows from its own pushes). A verified
upstream candidate is automatically fast-forwarded into `android17-6.18`, then
published as a GitHub release. KernelSU-only updates and accepted source changes
rebuild that source branch. Repeated checks deduplicate active builds and already
released inputs; failed builds can retry within the configured attempt limit.

Releases are published only after the complete module catalog, imported CRCs,
source identities, configuration, device trees and asset checks pass. Assets are
uploaded to a draft and checked before publication. Accepted tracking state records
published inputs, so an incomplete upload does not count as a completed update.
Unchanged released inputs do not cause daily builds or duplicate releases. The
source branch remains pinned and reproducible.

Builds use standard `ubuntu-24.04` GitHub-hosted runners, adapted from
`Loukious/konoha-kernel-gki`'s workflow: build dependencies, swap, artifact and
ABI uploads. The 6.18 build uses pinned Android Kleaf/Clang/Rust instead of the
6.6 build script. Build parallelism follows the runner's available CPUs (up to
four), with 3,000 MB reserved for the OS.
Runner cleanup must leave at least 55 GiB free before source sync; bounded
swap allocation fails visibly if space is unavailable. No persistent secrets
are needed. Repository Actions must be allowed to create pull requests.

Every build includes GKI, all 391 modules, both supported device-tree variants,
KernelSU dev's exact source archive, classic/extended symbol CRC verification,
input hashes and logs. Download `konoha-gki-and-modules.tar.gz`, `kernel-abi/`
and `SHA256SUMS` from the build's artifacts. These are build outputs, not a
universal flashable ZIP: matched boot/DLKM images depend on the ROM's ramdisks,
module load lists, filesystem labels and partition layout. GitHub releases contain verified build artifacts. CI does not flash devices or
claim hardware validation for a newly built kernel.

## Manual workflow

Select **Build Android 17 6.18 GKI and Onyx modules** on the `ci` branch.
`source_ref` defaults to `android17-6.18`; select an update branch, tag or
complete commit to build a candidate. The `diagnostic` option adds QDSS logging.

## Local build

Requirements: Linux x86-64, Git, Python 3.12+, Android `repo`, GNU patch, C
build tools, kmod and enough free disk for the source and Kleaf outputs.

```sh
git clone --branch ci https://github.com/Loukious/konoha-android17-6.18 automation
git clone --branch android17-6.18 https://github.com/Loukious/konoha-android17-6.18 source
export KONOHA_SOURCE_ROOT="$PWD/source"
python3 source/port/prepare.py /path/to/fresh-workspace
python3 source/port/stage.py /path/to/fresh-workspace
python3 automation/ci/snapshot.py /path/to/fresh-workspace/onyx
python3 automation/ci/snapshot_common.py /path/to/fresh-workspace
python3 source/port/build.py /path/to/fresh-workspace --jobs 12 --memory-mb 20000
```

Use `--qdss-diagnostic` for the optional diagnostic build. The ordinary build
still includes the CoreSight fix. Edit the recipe/patches, replay staging,
then regenerate snapshots with `automation/ci/snapshot.py ... --write` and
`automation/ci/snapshot_common.py ... --write`.
KMI checks, strict patch application and source-preservation guards remain
enabled. Do not reuse a workspace with unknown local edits.
