# Native MTD persistent archive

The installed 6.18 stack adds `mtd`, `block2mtd` and Xiaomi `mtdoops`, with a drained
threaded power-key notifier API. The existing 16 MiB `oops` partition holds eight
2 MiB records. The logger fixes the 16-byte header, bounded pmsg mapping/snapshot,
read-only attach and checked two-phase record publication. Existing GKI and other
387 platform module binaries are retained.

Normal reboot readback passed at slot 1, sequence 282: kernel marker and 10,578
valid Android pmsg records are present, and the other seven slots are byte-identical.
Android/audio, enforcing SELinux, LPM/sleep and six partition hashes pass afterward.

The ROM's `vendor_modprobe` domain lacks `block_device:dir search`, so automatic
adapter attachment remains pending ROM patch 0018. That exact rule was tested in
the actual loading context, and reboot restored the original policy bytes. The
ROM patch is saved for a future ROM build; no persistent helper exists.

Native reboot, poweroff and long-power-key logging scope is retained. Only reboot
was exercised live. Direct panic/hard-lockup capture is unsupported; pmsg snapshots
remain best effort under concurrent writers. Broad phone feature and endurance
coverage remains incomplete; no missing fundamental driver was identified for
demonstrated ordinary operation.

See [acceptance](../validation/onyx-runtime-summary-20261104.md) and
[remaining queue](../validation/remaining_391_modules/priorities.md).
