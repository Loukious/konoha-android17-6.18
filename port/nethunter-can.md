# NetHunter, USB adapter and CAN support

The current Linux 6.18.38 recipe adds nine native USB wireless modules to the
Onyx catalog, bringing it to 400 platform modules. It also selects 37 additional
native GKI modules and packages all 16 ordinary firmware files requested by the
selected wireless drivers. These are kernel facilities: install NetHunter apps,
its userspace environment and tools separately.

The last phone-validated checkpoint was source `c683c79` with 391 platform and
471 matched modules. The complete new recipe passed a combined GKI/400-module
build, 24,154 platform import CRC checks, both device-tree variants and all native
configuration/module/firmware gates. See the [build validation](../validation/nethunter-can-20261104.md).
Full release reports record the final catalog checks. A newly assembled image
set needs independent boot and hardware tests. Its final installed module
count depends on the audited per-ROM load lists and partition selection; the
build catalog alone does not establish that count.

## Included drivers and facilities

| Group | Selected native modules or behavior |
| --- | --- |
| Realtek USB WLAN | `rtl8xxxu`, including RTL8188CU `0bda:8176` and RTL8188EU `0bda:8179`; the CU path is enabled through `RTL8XXXU_UNTESTED`. |
| Ralink/MediaTek USB WLAN | `rt2x00lib`, `rt2x00usb`, `rt2800lib`, `rt2800usb`; native RT33xx, RT35xx, RT3573, RT53xx and RT55xx options are selected. |
| Atheros USB WLAN | `ath`, `ath9k_hw`, `ath9k_common`, `ath9k_htc`, with stable AR7010/AR9271 firmware 1.4.0. |
| CAN | ISO-TP, J1939, `vxcan`, CAN327 and ten native USB adapter families, alongside the existing CAN core, RAW/BCM/gateway, `vcan`, `slcan` and CAN device support. |
| USB serial | CH341, CP210x, PL2303, Option and WWAN modules; generic USB serial is enabled and existing FTDI support is retained. |
| USB networking | Host-side RNDIS and CDC subset support; Onyx GSI retains ownership of the phone-side RNDIS gadget. |
| USB Bluetooth | `btusb`, `btintel` and `btrtl`. Individual dongles can require additional Bluetooth firmware. |
| Network filesystems | CIFS/SMB, NFS v3/v4, lock/grace, SunRPC/GSS/Kerberos and their selected name-resolution/NLS dependencies. |
| Packet diagnostics | `af_packet_diag` for packet-socket diagnostics. |
| Retained gadget/network facilities | ConfigFS HID and mass-storage gadgets, packet sockets, TUN and netfilter are already enabled in GKI; creating gadget functions still needs userspace/ROM integration. |

The complete native config/output policy is recorded in
[nethunter-native-modules.json](nethunter-native-modules.json). The build gate
checks all 30 required config values, the 37 added outputs and ten retained
native dependencies against the archive for the exact built kernel release.
Wireless modules reuse the same `cfg80211` and `mac80211` providers as the Onyx
port. Their native source is pinned to the common kernel revision and replayed
through the official import/patch recipe.

## Wireless capabilities and limits

Native mac80211 provides monitor interfaces and a software transmit path for
radiotap frames. Compilation and source inspection establish those paths;
actual radio transmission, injection reliability and USB unplug/suspend behavior
need adapter tests. RTL CU/EU descriptors select rates internally and do not
honor arbitrary injected rate/no-ACK requests. Their reported ACK follows USB
completion, so it is not evidence of an over-the-air acknowledgement.

Native RTL8188EU lacks AP mode; the CU implementation has AP support. The
selected mac80211 provider has mesh disabled. RTL defaults to HT20, with the
native optional `ht40_2g` setting. Use these capabilities and limits when choosing
an adapter. The external drivers do not establish extra concurrency modes for
the phone's internal Qualcomm radio; that ROM work remains separate.

A compatible OTG adapter, sufficient USB power, userspace networking tools and
ROM permissions are also required. CAN operation needs the right interface and
bus configuration; this kernel change does not test a physical CAN controller or
bus. USB Bluetooth firmware requirements are separate from the WLAN package.

## Firmware and installation

[firmware/manifest.json](firmware/manifest.json) lists the requested filenames,
immutable upstream revisions, SHA256 hashes, driver request sources and licenses.
The package contains 13 Realtek files, `rt2870.bin`, and the two stable ATH 1.4.0
images. The RTL8723BU Bluetooth blob is excluded because its private
`enable_bluetooth` flag has no setter or module parameter and remains false in
the zero-initialized driver state. Development/legacy ATH fallback images are
omitted because both selected stable images are included.

The full Realtek/Ralink notices and composite ATH notices are retained. The
matching ATH 1.4.0 source archive accompanies the eCos components' GPL terms with
a linking exception. Every redistribution must keep these materials. The build
gate checks the complete firmware/license/source hash inventory, the exact 16
selected request names and the required corresponding source archive.

Install through the [matched per-ROM recovery route](recovery/README.md) described
in the [main README](../README.md#install-a-release). The generator takes every
selected module binary from one release and rebuilds dependency/load metadata for
the supplied ROM layout. A ZIP from the earlier 391-module release does not gain
these new drivers merely because it carries WLAN firmware. Inspect the selected
release and package report for the actual catalog and installed modules.

When a verified firmware package is supplied, the recovery generator places the
blobs under `vendor_dlkm/lib/firmware`, adds that directory to the kernel's firmware
search paths, and retains source/notices in the ZIP. It preserves existing custom
paths and rejects conflicts or more than ten paths. ROM labels, ramdisks, native
GKI modules and external wireless providers must all match the chosen image set;
reuse of a standalone driver ZIP from Linux 6.6 is unsupported.
