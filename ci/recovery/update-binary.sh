#!/system/bin/sh
set -eu
OUTFD=$2
ZIPFILE=$3
case "$OUTFD" in ''|*[!0-9]*) exit 1;; esac
export OUTFD
ONYX_WORK=$(mktemp -d /tmp/onyx-install.XXXXXX)
export ONYX_WORK
trap 'rm -rf "$ONYX_WORK"' EXIT
unzip -p "$ZIPFILE" tools/onyx-recovery > "$ONYX_WORK/onyx-recovery"
chmod 0755 "$ONYX_WORK/onyx-recovery"
"$ONYX_WORK/onyx-recovery" extract "$ZIPFILE" "$ONYX_WORK/package"
ONYX_PACKAGE="$ONYX_WORK/package"
ONYX_BUSYBOX="$ONYX_PACKAGE/tools/busybox"
export ONYX_PACKAGE ONYX_BUSYBOX
# AOSP recovery maps dynamic partitions for update-binary packages before this
# entry is called. The read-only guard additionally verifies the exact mappings.
if ! "$ONYX_BUSYBOX" awk '$2 == "/metadata" {found=1} END {exit !found}' /proc/mounts; then
    mount /metadata
fi
export ONYX_GUARD_ACTIVE=1
"$ONYX_PACKAGE/tools/recovery-guard" run "$ONYX_BUSYBOX" "$ONYX_PACKAGE/install.sh"
