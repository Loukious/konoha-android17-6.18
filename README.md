# Onyx — Android 17 / Linux 6.18

This repository contains the complete POCO F7 (Onyx/SM8735) module port and a
reproducible GKI build recipe. It tracks Google's `android17-6.18-lts` branch
and fetches the official **KernelSU Next `dev` branch on every build**.

The current Onyx recipe uses Linux **6.18.38** with **400 platform modules**,
**37 additional native GKI modules** for CAN, USB adapters and network facilities,
and **16 wireless firmware files** with their redistribution notices and matching
ATH source. The module snapshot reproduces **5,193 files** from pinned imports
and reviewed patches. See [NetHunter and CAN support](https://github.com/Loukious/onyx-android17-6.18/blob/android17-6.18-lts/port/nethunter-can.md).

The last device-validated checkpoint, source `c683c79`, used **391 platform
modules** and **471 matched modules**. That image set passed Android, KernelSU
root, audio, enforcing SELinux and partition-identity checks on the global POCO
F7 used for development. It includes native WALT and Qualcomm low-power idle,
Gunyah transport and HVC lifetime fixes, battery/IOMMU diagnostics, and
block-backed MTD kernel/pmsg archival. New catalog additions and freshly built
images require their own build, packaging and device validation.

The device target is **Onyx / SM8735**. Android and Geekbench report the platform
name `sun`; the device-tree recipe includes Tuna and Tuna7 with the Onyx overlay.
Both trees pass offline checks. Hardware testing covers the development phone,
not every phone or board revision using those names. China-only notification LED
support and other unconfirmed hardware remain outside the target scope.

Battery aggregation and firmware console availability remain unresolved optional
diagnostics. Automatic block2mtd attachment requires the saved ROM policy change;
Wi-Fi concurrent-channel ROM work is also pending. Broad peripheral, suspend,
power and endurance validation remains. See [current device validation summary](https://github.com/Loukious/onyx-android17-6.18/blob/android17-6.18-lts/validation/onyx-runtime-summary-20261104.md) and [remaining module inventory](https://github.com/Loukious/onyx-android17-6.18/blob/android17-6.18-lts/validation/remaining_391_modules/priorities.md). A new upstream or KernelSU build
requires device validation even when its automated build checks pass.

## Branch layout

- [`ci`](https://github.com/Loukious/onyx-android17-6.18/tree/ci) is the default
  branch and contains only workflows, automation, and documentation.
- [`android17-6.18-lts`](https://github.com/Loukious/onyx-android17-6.18/tree/android17-6.18-lts)
  contains the complete GKI source in `common/`, modified modules in `modules/`,
  and the pinned recipe/patches in `port/`. It contains no workflows.
- `upstream/` branches are proposed updates to the source branch. Builds run
  from `ci` and attach their status to the selected source commit and its PR.

## Source layout

On the source branch:

- `common/` contains the complete pinned upstream GKI with the required common
  fixes applied. CI independently recreates and verifies every source file,
  executable bit and symlink before building.
- `modules/` contains all 5,193 materialized, modified source/build files used
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
patch replay, hosted builds, artifact checks, automatic source promotion and releases.

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
(GitHub's default token does not trigger a workflow from its own push).
Successful updates automatically fast-forward `android17-6.18-lts` to the exact
tested source commit, merging the update PR. A concurrent change to the source
branch stops promotion until a new candidate has been rebuilt. The source
branch remains pinned and reproducible. A KernelSU-only update rebuilds the
accepted kernel without changing its GKI source. Successful accepted-source
builds publish their verified artifacts before recording their exact revisions,
so unchanged released inputs do not cause daily rebuilds. The daily tracker also
rebuilds manual source-branch changes, checking out the exact detected commit.
Active and successful builds for the same detected inputs are deduplicated.
Failed or cancelled builds can retry up to three times; further automatic attempts
require changed inputs. Manual workflow runs remain available for source repairs.
KernelSU is fetched from dev at build time; the release records the revision
actually built if dev advances between detection and compilation.

Builds use standard `ubuntu-24.04` GitHub-hosted runners with build dependencies,
swap, persistent compilation caches, artifact and ABI uploads. The 6.18 build uses pinned Android Kleaf/Clang/Rust instead of the
6.6 build script. Build parallelism follows the runner's available CPUs (up to
four), with 3,000 MB reserved for the OS.
Runner cleanup must leave at least 55 GiB free before source sync; bounded
swap allocation fails visibly if space is unavailable. No persistent secrets
are needed. Repository Actions must be allowed to create pull requests.

Every build includes GKI, all 400 platform modules, the native GKI archive,
KernelSU dev's exact source archive, both device-tree variants, wireless firmware
with licenses/source, import CRC verification, input hashes and logs. Download
the complete GKI/module archive and `SHA256SUMS` from the build artifacts or release.
Actions stores separate ABI files under `kernel-abi/`; releases expose those files
individually. Releases also include the complete Onyx recovery ZIP, AnyKernel3
and internal WLAN installer ZIPs; see the installation scope below.
CI does not flash devices.

Successful ordinary builds of accepted source automatically create a GitHub
release. A successful upstream candidate is first fast-forwarded to the exact
tested commit, then released; promotion does not wait for a manual PR merge.
The release job independently rechecks the downloaded archive, complete named
module inventory, hashes, matching ABI/vermagic, KernelSU configuration/source,
  both device trees and exact source/GKI/KernelSU revisions. Failed builds, changed
source heads and diagnostic or other manual candidate refs are excluded.

Tags have the form `gki-6.18.N-s<SOURCE12>-k<KSU12>` and point to the built
source commit. All assets upload to a draft first; CI verifies the uploaded
inventory, sizes and available server SHA256 digests before publishing. Upload
failure leaves an unpublished draft that a subsequent successful build can
recover. Existing complete releases are preserved. `release.json` binds the
assets to the exact build run and attempt. Artifact names include the build
attempt, so a complete rerun can upload fresh outputs and a release-only rerun
reuses the original successful build's artifact and verification. Promotion and
release share one serial job group; the accepted head is checked again immediately
before publication, preventing an older upload from becoming Latest afterward.
These automatic releases establish
build verification; hardware validation remains a separate step.

The publisher retains the release ID returned by draft creation and uses that
ID for uploads, verification and publication. A new draft need not be visible
through its tag or the release listing. Upload retries reuse assets whose size
and server digest already match, and replace incomplete uploads within that draft.
When a build succeeds but publication fails, run **Recover a verified GKI
release** on `ci` with the original `build_run_id` and `build_attempt`. It uses
the original immutable artifact and current publication code without rebuilding.

Each release summarizes the Google common commits since the preceding published
GKI release, with area counts, linked commit subjects and the complete Google log
range. A release using the same Google revision says that explicitly.

The tracker also detects older build state without a published-release tag and
dispatches one migration build. Accepted input state is written only after the
release is published or an existing complete release is confirmed.

## Build the ROM kernel from source

The [ROM source-build manifest branch](https://github.com/Loukious/local_manifests_onyx/tree/evolution-cnb-konoha618) of
[`local_manifests_onyx`](https://github.com/Loukious/local_manifests_onyx/tree/evolution-cnb-konoha618)
syncs this source branch to `kernel/platform/kernel-6.18`. Apply the matching
[`crdroid_onyx_patches`](https://github.com/Loukious/crdroid_onyx_patches/tree/evolution-cnb-konoha618)
branch, then run the normal ROM build. `port/rom/BoardConfig.mk` connects Lineage's
Kleaf path to `port/rom/build.py`; it compiles GKI, every platform/native module
and the device trees from the verified source recipe. Android packages the boot
images and DLKM partitions from that single build. Userspace headers are generated
from the same synced GKI/module source. No release Image or module binaries are
downloaded for this route. KernelSU Next dev is resolved on every build.

Local kernel objects persist under
`out/target/product/onyx/obj/KERNEL_BAZEL_OUT/kleaf-cache`. Kleaf's supported
`--config=local --cache_dir=...` mode reuses unchanged compilation outputs; source,
configuration and ABI checks still run. The GitHub build also preserves Bazel action records and their outputs, so a
fresh runner can reuse content-checked results despite new checkout timestamps.
It restores and saves these caches with keys separated by toolchain/configuration and diagnostic
mode. This is incremental compilation, not reuse of a previously released kernel.
A cold build still performs full compilation. Cache timings depend on what changed.

For standalone builds, pass `--cache-dir /path/to/persistent/kleaf-cache` to
`port/build.py`. ROM builds enable it automatically. Resource defaults are 12 jobs
and 20000 MB for Kleaf; override `TARGET_KERNEL_PORT_JOBS` and
`TARGET_KERNEL_PORT_MEMORY_MB` in the device configuration when needed.
`KERNEL_OBJ/onyx-source-build.json` records source revisions, KernelSU, module
inventory, validation results and output hashes for the ROM being packaged.

## Install a release

[GitHub releases](https://github.com/Loukious/onyx-android17-6.18/releases) include:

| Asset | Purpose |
| --- | --- |
| **`Onyx-Complete-<kernel-release>.zip`** | Recovery installer for the kernel, WLAN/all matching modules, device trees and recovery modules |
| **AnyKernel3 ZIP** | Kernel-only update for an existing installation whose modules already match |
| `nethunter-wlan-injection-<kernel-release>.zip` | Optional KernelSU/Magisk module for the matching internal WCN7750 driver |
| `konoha-gki-and-modules.tar.gz` | Build outputs, inputs, ABI and logs for ROM integration; not directly flashable |
| `SHA256SUMS`, `release.json`, `installers.json`, `full-recovery.json` | Checksums, exact build inputs and installer provenance |

### Install the complete kernel and drivers

Use **`Onyx-Complete-<kernel-release>.zip`** to update the full kernel/module set
or migrate a compatible Onyx 6.6 ROM. It repacks the installed ROM's own images,
so compatible Onyx ROMs can share the same release ZIP. The ROM's recovery
application, files, labels, configuration and parallel module loading are
preserved. Recovery's drivers are updated in its own ramdisk or in `vendor_boot`,
according to the ROM's layout. WLAN does not require a separate fastboot flash.

1. Download the complete ZIP and `SHA256SUMS` from the same release and verify
   the ZIP's SHA256.
2. Finish any pending ROM OTA and boot Android once. Open custom recovery on an
   unlocked Onyx whose ROM already has AVB verification and verity disabled.
   Mount `/metadata` and writable backup storage, such as decrypted `/data`.
3. Install the ZIP from storage or select **Install update / ADB sideload** and
   run `adb sideload Onyx-Complete-<kernel-release>.zip` on the PC.
4. Wait for successful installation, then reboot. The installer prints the
   persistent backup path and verifies every written partition.

Before writing anything, the installer validates the device, module coverage,
partition layout, inactive-slot separation, OTA state and space, then rebuilds
all six images: `boot`, `vendor_boot`, `recovery`, `dtbo`, `system_dlkm` and
`vendor_dlkm`. It saves verified original images and attempts complete rollback
on a detected write/readback failure. It keeps `init_boot`, `vbmeta` and the
other slot. It does not install into locked or AVB-enforced stock configurations.

Supported layouts use Android boot headers 3/4, EROFS DLKM images and flat module
directories. The known Onyx 6.6 migration verifies old ELF module names and
vermagic before retiring obsolete drivers. Unknown ROM-specific drivers,
unsupported layouts, shared slot extents or insufficient space stop installation
before partition writes. See the [complete installer guide](ci/recovery/README.md)
for detailed requirements, backups, tool sources and validation scope.

The complete ZIP already contains the injection-enabled WCN7750 driver and
matching dependencies. A second WLAN manager module is unnecessary. These ZIPs
have offline image and rollback validation; the complete installer has not yet
been flashed on a device.

### Optional kernel-only or WLAN updates

For an Onyx installation already running a compatible Android 17 / 6.18 module
set, the AnyKernel3 ZIP checks installed DLKM and early vendor-ramdisk module CRCs
before updating `boot`. Make the ROM's DLKM partitions available read-only in
recovery. Use the complete ZIP when the drivers or KMI generation also need to
change; the AnyKernel3 ZIP updates only the kernel.

The optional WLAN ZIP is installed through KernelSU or Magisk after booting the
exact matching release kernel. It checks the driver, waits for dependencies,
preserves Wi-Fi's on/off state and attempts to reload the original driver if
replacement fails. Remove the module and reboot to return to the ROM's normal
WLAN loading. A ROM or complete ZIP already shipping that driver needs no
additional module.

Older releases may contain only the two smaller installers. Use a release that
actually lists the complete ZIP for the full recovery installation above. The
`.tar.gz` archive is for maintainers building or integrating matching images;
flashing that archive directly is not an installation method.

## Manual workflow

Select **Build Android 17 6.18 GKI and Onyx modules** on the `ci` branch.
`source_ref` defaults to `android17-6.18-lts`; select an update branch, tag or
complete commit to build a candidate. The `diagnostic` option adds QDSS logging.
Leave `source_revision` and `kernelsu_revision` blank for manual builds; the
tracker supplies them to identify exact detected inputs and suppress duplicates.

## Local build

Requirements: Linux x86-64, Git, Python 3.12+, Android `repo`, GNU patch, C
build tools, kmod and enough free disk for the source and Kleaf outputs.

```sh
git clone --depth 1 --single-branch --branch android17-6.18-lts https://github.com/Loukious/onyx-android17-6.18 source
python3 source/port/rom/build.py --source source \
    --workspace /path/to/persistent-workspace \
    --output /path/to/kernel-outputs --jobs 12 --memory-mb 20000
```

This verifies the committed GKI/module snapshots, prepares the pinned build
dependencies, compiles the kernel and modules with an incremental cache, and
exports matching Android packaging inputs. Keep the workspace and output outside
the source checkout and separate from each other. ROM builds invoke this adapter
automatically through the device configuration.

For low-level recipe development, use the prepare/stage/build helpers in `port/`
and the independent snapshot checkers in `ci/`. Strict patch application,
source-preservation guards and ABI checks remain enabled.
