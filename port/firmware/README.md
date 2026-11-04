# Konoha USB wireless firmware

This offline package contains all 16 firmware files used by the selected native 6.18 RTL8XXXU, RT2800USB and stable ATH9K HTC paths. Files remain unmodified and keep the relative request_firmware names listed in manifest.json.

Copy the contents of firmware/ into a directory already searched by the firmware loader. Keep licenses/, manifest.json and sources/ with every redistribution. Realtek and Ralink permit unmodified binary redistribution with their included notices. ATH 1.4.0 has ClearBSD, ISC, MIT and other retained notices; three eCos files have GPLv2 terms with a linking exception. The matching 1.4.0 source archive is included for those source obligations. See the full composite license.

The native RTL8723BU private Bluetooth enable flag has no setter and remains false; its declared Bluetooth blob is omitted. ATH uses stable 1.4.0 by default. Development firmware and older fallback blobs are omitted because both requested stable images are included.

The Onyx installer must preserve the ROM firmware search paths, all existing files and subdirectories, root ownership, directory modes 0755, file modes 0644, and the ROM firmware SELinux context. The old 6.6 RTL-only overlay helper is insufficient for the top-level rt2870.bin and nested ath9k_htc files. File presence and accepted module CRCs are host checks; dongle probe, unplug, monitor mode and injection require device tests.

To verify this package offline, run `sha256sum -c SHA256SUMS` from this directory. To verify only loadable firmware, run `sha256sum -c firmware.sha256` from this directory.
