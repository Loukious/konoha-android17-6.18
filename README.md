# Konoha Onyx — Android 17 / Linux 6.18

This repository contains the complete POCO F7 (Onyx/SM8735) module port and a
reproducible GKI build recipe. It tracks Google's `android17-6.18-lts` branch
and fetches the official **KernelSU Next `dev` branch on every build**.

The initial accepted source is Linux **6.18.33**, with **318 platform modules**
and **179 port patches**, imported from the working Konoha 6.18 development
workspace. It includes the Bluetooth power-ownership corrections, the measured
Onyx PMIC inventory overlay, and the July CoreSight platform-removal correction.
It also includes WLAN sensor/cooling ownership fixes and the narrowly scoped
camera/SYNX logging corrections. This 6.18.33 baseline is installed and passed
startup checks, front/rear photos and short 1080p recordings. Camera synchronization
recovery itself is unchanged; separate SOF-skip diagnostics remain. New
LTS/KernelSU builds require device validation.

## Source layout

- `modules/` contains all 4,862 materialized, modified source/build files used
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
- `ci/` and `.github/workflows/` provide upstream detection, patch replay,
  hosted builds, artifact checks and source-update pull requests.

The stable 6.6 repository and development logs are maintained separately.
Vendor sources keep their original copyright and license notices; see COPYING.

## Automated updates and builds

The tracker runs daily at 05:00 UTC or through **Run workflow**. It checks
Google's LTS branch and KernelSU Next dev. A new kernel revision produces an
`upstream/` branch with updated common pins, private-header blob imports,
patch hashes and the complete module snapshot. Exact patch replay is required;
an upstream conflict fails the update without changing the accepted main tree.
Already integrated CoreSight fixes are excluded from the effective patch.

The tracker opens a pull request and explicitly dispatches its complete build
(GitHub's default token does not trigger a workflow from its own push). Merge
the update after reviewing its build result. Main remains pinned and
reproducible. A KernelSU-only update rebuilds the accepted kernel without
changing its GKI source. Successful main builds record their exact revisions,
so unchanged upstream inputs do not cause daily rebuilds.

Builds use standard `ubuntu-24.04` GitHub-hosted runners, adapted from
`Loukious/konoha-kernel-gki`'s workflow: build dependencies, swap, artifact and
ABI uploads. The 6.18 build uses pinned Android Kleaf/Clang/Rust instead of the
6.6 build script. Build parallelism follows the runner's available CPUs (up to
four), with 3,000 MB reserved for the OS.
Runner cleanup must leave at least 55 GiB free before source sync; bounded
swap allocation fails visibly if space is unavailable. No persistent secrets
are needed. Repository Actions must be allowed to create pull requests.

Every build includes GKI, all 318 modules, both supported device-tree variants,
KernelSU dev's exact source archive, classic/extended symbol CRC verification,
input hashes and logs. Download `konoha-gki-and-modules.tar.gz`, `kernel-abi/`
and `SHA256SUMS` from the build's artifacts. These are build outputs, not a
universal flashable ZIP: matched boot/DLKM images depend on the ROM's ramdisks,
module load lists, filesystem labels and partition layout. CI does not flash
devices or automatically publish a tested release.

## Local build

Requirements: Linux x86-64, Git, Python 3.11+, Android `repo`, GNU patch, C
build tools, kmod and enough free disk for the source and Kleaf outputs.

```sh
python3 port/prepare.py /path/to/fresh-workspace
python3 port/stage.py /path/to/fresh-workspace
python3 ci/snapshot.py /path/to/fresh-workspace/onyx
python3 port/build.py /path/to/fresh-workspace --jobs 12 --memory-mb 20000
```

Use `--qdss-diagnostic` for the optional diagnostic build. The ordinary build
still includes the CoreSight fix. Edit the recipe/patches, replay staging,
then regenerate the module snapshot with `ci/snapshot.py ... --write`.
KMI checks, strict patch application and source-preservation guards remain
enabled. Do not reuse a workspace with unknown local edits.
