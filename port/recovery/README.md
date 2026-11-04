# Onyx matched recovery installer prototype

`generate.py` creates the kernel, device trees, three module trees, five complete
partition images and a recovery ZIP from one verified release archive and supplied
ROM partition dumps. It does not contact or flash a device. The recovery installer
accepts the exact baseline hashes used to generate that package.

Offline generation has been checked with verified 6.18.38 release inputs, saved
accepted ROM images and the public `prebuilt-onyx` partition classification.
Modules shared by loaders are duplicated in their required partitions, and every
selected binary and dependency is checked against the release's imported symbol
CRCs. Partition capacity is a hard limit: selecting the whole compiled catalog
can exceed the small system_dlkm partition and fails generation. Generated ZIPs
remain offline prototypes until recovery execution and device compatibility are
validated.

## Create a package for supplied ROM images

Use Linux with Python 3.12+, `kmod` (`modinfo`, `depmod`, `modprobe`) and Android host
`mkbootimg`, `unpack_bootimg`, `avbtool`, `mkfs.erofs`, `fsck.erofs`, `dump.erofs` and `lz4` tools.
The tools directory and compiled `file_contexts.bin` must belong to the ROM.
Supply a static AArch64 recovery BusyBox with its SHA-bound distribution notices
and corresponding source. The supplied `busybox-notices/manifest.json` matches
the [documented BusyBox prebuilt](busybox-notices/NOTICE.txt); another binary needs
its own matching notices.
Build the bundled static guard before generating a package:

```sh
CLANG=/path/to/clang sh build_guard.sh
```

The guard uses Linux syscalls directly and needs no Android shared libraries.

Provide full partition-size dumps of `boot`, `vendor_boot`, `dtbo`, `system_dlkm`,
`vendor_dlkm`, `init_boot` and `vbmeta`. Names may be `boot.img` or `boot_a.img` for
the selected slot, and similarly for the other images. ROM OTA payload members
are not always full partition dumps; the recovery hash guard deliberately rejects
different padded sizes/bytes. Keep the originals on the PC for rollback.

Create the profile, then generate the ZIP:

```sh
python3 profile.py --images /path/to/partition-dumps --slot a \
  --module-layout /path/to/verified-prebuilt-onyx --output rom-profile.json

python3 generate.py \
  --release /path/to/konoha-gki-and-modules.tar.gz \
  --release-sha256 THE_HASH_FROM_THE_RELEASE_SHA256SUMS \
  --profile rom-profile.json \
  --tools /path/to/android-host-tools \
  --file-contexts /path/to/the-rom/file_contexts.bin \
  --busybox /path/to/static-aarch64-busybox \
  --busybox-notices /path/to/matching-busybox-notices \
  --output /path/to/new-package-directory
```

`--module-layout` uses the module filenames and partition assignments from the
verified public prebuilt checkout. Every installed binary comes from the selected
release archive, including dependencies; old kernel module bytes are never copied
into the new image. Every new platform module is added to vendor ramdisk and vendor
DLKM. Requested new GKI modules in the release's hashed
`inputs/nethunter-native-modules.json` are added to vendor DLKM, with dependencies
and new vendor autoload entries. This preserves the old small system partition
placement. An absent required module fails. Omitting the layout selects the complete compiled catalog, which may
not fit these small DLKM partitions. Existing ROM load requests and blocklists are
retained, with dependency/alias metadata regenerated. Missing old ROM variants
require `--omissions` with an evidence-backed JSON reason for each normalized name;
hard dependencies cannot be omitted.

Supported inputs are Onyx, boot header v4, one LZ4/newc vendor ramdisk, flat module
directories and root-owned EROFS DLKM filesystems. Existing nonmodule CPIO records
are preserved byte for byte, including metadata; nonmodule filesystem contents,
modes and symlink targets are audited. `dump.erofs` checks every original inode's
UID and GID and rejects non-root ownership before repacking. Kernel, DTB and DTBO come from the same
release. `android_arch_task_struct_size=512` and the release's proxy-execution
parameter are carried into the boot command line. Unknown image layouts fail.

The baseline must already have vbmeta flags 3 (AVB verification and verity disabled).
Generated partition footers retain salts, rollback indices and properties while
using `NONE`; this requires that existing disabled-verification configuration.
The generator never changes vbmeta or unlocks the bootloader. Locked/verified
stock HyperOS and Xiaomi firmware signature requirements are outside this route.

## Saved serial-loader baseline

The previously accepted development images select an init override under
`/metadata/ota/konoha-serial-20261101-v2`. To make a complete ZIP for that baseline,
pass `--serial-loaders serial-loaders --vendor-policy /path/to/precompiled_sepolicy`
to `profile.py`. The supported manifest and binary policy hashes are pinned in
`generate.py`; a new policy needs a separate permission audit. The ZIP ships all
four loader files in a release-specific directory, records the original tested
SELinux labels, changes the vendor bootconfig to select that directory, and sets
up those files before writing a partition. It requires no absent private loader
file on the target. The original directory is retained so automatic rollback can
boot the original image set.

The release-specific bootstrap directory must not already exist. Reinstalling the
same release requires inspecting the retained installation and rollback images
first; the installer does not replace an existing loader directory.

The supplied baseline can also be a previous generated package. If its bootconfig
selects the original or a prior release's serial loader directory, the installer
requires those four retained files to have the exact expected bytes, permissions
and SELinux labels before writing partitions. This keeps automatic rollback
usable. Unknown init overrides fail during generation.

For a ROM built through the public Onyx prebuilt BoardConfig, Android produces
normal ramdisks, module load lists, AVB and OTA metadata. Such a supplied baseline
without an `androidboot.init_rc` override needs no serial bootstrap. That new ROM
packaging was activated and statically checked but has not inherited the previous
serial package's device validation.

An optional `--firmware-package` tree containing `firmware/`, `licenses/`, `sources/`
and `manifest.json`, together with `--firmware-manifest-sha256`, places verified
wireless blobs under `vendor_dlkm/lib/firmware`. The corresponding source and notices
are retained in the ZIP. `firmware_class.path` includes that directory and preserves
existing custom paths. Conflicting existing firmware or more than ten paths fails.
If the release already contains a SHA-bound `firmware/` distribution, it is used
automatically. Releases that declare native wireless firmware requirements fail
if those blobs are absent. The 16-file wireless distribution passed the complete
offline image/ZIP generation path; radio behavior remains untested.

## Recovery installation

Transfer `konoha-onyx-matched.zip` and verify its companion `.zip.sha256`. Use a
custom recovery with `/system/bin/sh`, ZIP extraction, `getprop`, `blockdev` and
SELinux `chcon`/`ls -Z` support. The retained Onyx recovery inventory lists those
commands. The ZIP supplies a static BusyBox and recovery guard; `dmsetup` and
`snapshotctl` are unnecessary. Recovery must support installing locally generated
unsigned ZIPs.

Mount `/metadata` and finish any OTA cleanup before installation. The guard
requires a regular `/metadata/ota/state` file with a clean empty/default
`SnapshotUpdateStatus` protobuf or the exact legacy `none` value, and a present,
empty `/metadata/ota/snapshots` directory. Missing, active, corrupt or unknown
state fails. It holds an exclusive lock on `/metadata/ota` throughout the
installer, using the same directory lock as Android's snapshot manager.

Every baseline target must be unmounted. Both active-slot DLKM devices must have
complete contiguous linear mappings directly into physical `super`; snapshots,
prepared tables, suspended mappings and pending removal fail. The guard binds
device identities, sizes, mapping event numbers, UUIDs and physical extents, and
the installer compares those fingerprints before and after partition writes and
before rollback. Recovery must expose the exact ROM policy prerequisite if the
serial bootstrap is used.

Select the ZIP in recovery. The installer verifies the device, unlocked state,
active slot, stopped Android, OTA state, mappings, every current baseline image,
all payload sizes/hashes and policy prerequisites before writing partitions. It
creates and verifies five rollback backups under `/metadata/ota`, requiring their
space plus 32 MiB free. It writes `system_dlkm`, `vendor_dlkm`, `dtbo`, `vendor_boot`
and `boot` in that order and reads back each. It retains the other slot, `init_boot`,
`recovery`, `vbmeta` and `super`; it never resizes logical partitions.

A write/readback failure attempts to restore and verify all five originals. If
rollback fails, the installer reports the backup path and stops: restore the
matching images using fastbootd before rebooting. Successful installation leaves
the backups in place and lets the user reboot. Kernel boot, peripherals, wireless
radio behavior, power and recovery-tool execution still require device validation.

## Host verification

```sh
python3 test_generator.py
python3 test_installer.py
CLANG=/path/to/clang CC=cc python3 test_guard.py
python3 -m py_compile generate.py profile.py verify_modules.py
bash -n install.sh update-binary build_guard.sh
```

The shell tests execute the real installer with regular-file targets and mocked
properties, recovery guard, device mappings, labels and blockdev. They cover
both slots, preflight failures, bootstrap setup, write/readback faults and complete
rollback, and verify that the inactive slot is retained. They cannot establish
actual AArch64 helper, recovery or partition behavior. Separate native guard
checks exercise the real host syscalls for protobuf parsing, inode validation,
snapshot enumeration, directory locking and child supervision, and validate the
device-mapper reply ABI against Linux UAPI. A static AArch64 build check verifies
that the shipped helper has no interpreter or shared-library dependency. These
offline checks do not establish execution in the phone's recovery.
