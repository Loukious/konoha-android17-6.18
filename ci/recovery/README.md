# Complete Onyx recovery installer

`Onyx-Complete-<kernel-release>.zip` installs a matching kernel and module set
from recovery. It rebuilds the installed ROM's images on the phone, so compatible
Onyx ROMs can use the same release ZIP. No PC image builder or Docker is needed
to install it. The optional WLAN manager module is unnecessary after installing
this ZIP: the complete package already includes that injection-enabled driver.

## Installation

1. Download the complete ZIP and `SHA256SUMS` from the same release. Verify the
   ZIP's SHA256 before copying it to the phone or using ADB sideload.
2. Boot custom recovery on an unlocked Onyx. Finish any pending ROM OTA and boot
   Android once before returning to recovery. The installed ROM must already
   use AVB flags 3, disabling verification and verity.
3. Make `/metadata` and writable persistent backup storage available in recovery.
   The installer uses mounted `/data`, `/cache`, or `/metadata`, in that order,
   when there is enough space. Encrypted `/data` must be decrypted or another
   sufficiently large backup location must be mounted. Recovery must also have
   enough temporary space for all image reconstruction and verification.
4. Select **Install update / ADB sideload** or install the ZIP from storage.
   For sideload, run `adb sideload Onyx-Complete-<kernel-release>.zip` on the PC.
   Keep recovery open until the installer reports success. Reboot afterward.

The installer checks actual free space and partition sizes. For the saved Onyx
layouts used in validation, allow roughly 1 GiB of persistent backup space and
3 GiB of available recovery working space. The exact requirement is calculated
from the installed partitions. It reports the backup directory on screen.

## Images and compatibility

| Active-slot partition | Update |
| --- | --- |
| `boot` | Matching kernel; retain any ROM ramdisk |
| `vendor_boot` | Matching DTB, early/recovery modules and dependencies |
| `recovery` | Retain recovery application and files; replace its kernel/modules where present |
| `dtbo` | Matching Onyx device-tree overlays |
| `system_dlkm` | Replace the ROM's existing compatible module selection |
| `vendor_dlkm` | Complete compiled module catalog, WLAN and external wireless firmware |

Some ROMs put recovery's drivers in `vendor_boot` and have no modules in
`recovery`. This layout is supported only when the vendor ramdisk has a verified
`modules.load.recovery`. The matching recovery load list and drivers are updated
there. ROMs with a separate recovery module directory have that directory
updated as well.

Supported layouts use Android boot/vendor-boot headers 3 or 4, EROFS DLKM images,
flat `lib/modules` directories and separate active/inactive logical partition
storage. The release kernel must provide every required ROM driver or a builtin
equivalent. The recorded Onyx 6.6 migration additionally recognizes old ARM64 ELF
modules by their `.modinfo` name and 6.6 vermagic. It uses reviewed 6.18 load
profiles when retiring those drivers; unrelated or unknown modules cause an
abort. Small `system_dlkm` partitions retain their compatible module selection;
the full catalog remains available in `vendor_dlkm`.

This is not an installer for every stock or custom Onyx layout. Missing drivers,
unsupported image formats, active snapshots, shared inactive-slot extents,
insufficient partition capacity, and locked or AVB-enforced configurations are
rejected before writing partitions. It does not resize logical partitions,
change the active slot, patch SELinux policy, or change parallel module loading.
It preserves non-module ROM files, labels, ownership, modes, timestamps,
hardlinks, module options/blocklists and vendor bootconfig. Required kernel
command-line sizes and the external firmware search path are updated.

All six images are rebuilt and verified before the first partition write. The
installer saves and verifies original images on persistent storage, checks
snapshot/layout state throughout installation, and hashes every written image.
A detected write or readback failure attempts to restore all six originals.
If restoration cannot be verified, keep recovery open and recover using the
saved image set. Power loss cannot be made transactional by this installer.
`init_boot`, `vbmeta`, the other slot and the ROM's recovery application stay in
place. AVB footer hashes/trees are regenerated for the existing disabled-AVB
configuration; they do not carry the ROM maintainer's signature or FEC data.

## Validation and source

The engine has tests for boot formats, CPIO preservation, module migration,
malformed inputs and liblp metadata. Independent AOSP-generated fixtures check
boot headers and logical metadata. Google `avbtool` checks rebuilt hashes and
hashtrees. EROFS tests retain binary xattrs, nanosecond timestamps and hardlinks.
The shell transaction tests cover all six writes, complete rollback and
preflight failures with zero partition writes. These are offline checks; this
complete installer has not been flashed on a device.

`kernel.json` binds the payload to exact source, Google GKI and KernelSU commits.
`full-recovery.json` binds the release ZIP to its payload and tool provenance.
`SHA256SUMS` inside the ZIP covers its complete inventory. CI recreates the ZIP
from the verified build during release validation, rejecting modified installers
even if someone regenerated the outer checksums.

The ZIP includes its engine/guard source, build scripts, pinned EROFS/compression
source archives, tool checksums, compiler identities and license notices under
`installer-source/`. EROFS utilities are built with musl for static ARM64 use.
AnyKernel's pinned ARM64 `magiskboot` supplies ramdisk compression; BusyBox
supplies recovery shell utilities. Go's standard-library license is included;
the engine has no external Go modules. Original source archive licenses apply
to EROFS, musl, zlib, LZ4 and Zstandard. The guard and engine are GPL-2.0-only.

## Rebuild the installer tools

Normal kernel CI packages the checked-in utilities, so it does not rebuild a
Docker image for each kernel release. To rebuild the EROFS tools on Linux:

```sh
docker build -t onyx-recovery-toolchain tools/recovery-build
mkdir -p /tmp/onyx-recovery-build
docker run --rm --network=none --cpus=2 --user "$(id -u):$(id -g)" \
  -v "$PWD:/source:ro" -v /tmp/onyx-recovery-build:/work \
  onyx-recovery-toolchain bash /source/tools/recovery-build/build.sh
```

`toolchain-packages.tsv` records the Debian compiler/build package versions used
for the shipped C binaries. The base container digest and all dependency source
hashes are pinned; different installed compiler packages may change binary
hashes. `tools/recovery-guard-build.json` records the exact Android Clang identity
and command for the freestanding guard; `build_guard.sh` rebuilds it with `CLANG`.

Use Go **1.25.4** to reproduce the engine:

```sh
cd ci/recovery/engine
CGO_ENABLED=0 GOOS=linux GOARCH=arm64 GOTOOLCHAIN=local \
  go build -trimpath -buildvcs=false -ldflags='-s -w -buildid=' \
  -o ../tools/onyx-recovery .
cd ../../..
python3 ci/recovery/check_tools.py --rebuild-engine
go -C ci/recovery/engine test ./...
python3 -m unittest discover -s ci/tests -v
```

For the external EROFS and Google AVB tests, set `ONYX_TEST_TOOLS` to a directory
containing executable image utilities and `ONYX_TEST_AVBTOOL` to Google's
`avbtool.py`. On x86-64, wrappers may invoke the supplied ARM64 utilities through
`qemu-aarch64`. Run those tests when changing any image-building code or tool.
Update `provenance.json` only after rebuilding changed tooling and completing
these checks. CI verifies the Go binary reproduces exactly with the pinned Go
version, and checks every shipped tool/source checksum.
