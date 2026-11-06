#!/system/bin/sh
# Recovery-only installation. Prepare and verify every image before writing any.
set -eu
package=${ONYX_PACKAGE:?}
work=${ONYX_WORK:?}
BB=${ONYX_BUSYBOX:?}
GUARD="$package/tools/recovery-guard"
ENGINE="$package/tools/onyx-recovery"
cd "$package"
say() { printf 'ui_print %s\nui_print\n' "$*" >> "/proc/self/fd/${OUTFD:-1}"; }
die() { say "$*"; exit 1; }
is_block() { [ -b "$1" ]; }
hash() { "$BB" sha256sum "$1" | "$BB" awk '{print $1}'; }
parts='system_dlkm vendor_dlkm dtbo vendor_boot recovery boot'
write_started=0
completed=0
backup=

find_partition() {
    case "$1" in *dlkm) bases='/dev/block/mapper';; *) bases='/dev/block/by-name /dev/block/bootdevice/by-name';; esac
    found=
    for base in $bases; do
        candidate="$base/$1$slot"
        if is_block "$candidate"; then
            resolved=$("$BB" readlink -f "$candidate") || return 1
            [ -z "$found" ] || [ "$found" = "$resolved" ] || return 1
            found=$resolved
        fi
    done
    [ -n "$found" ] || return 1
    printf '%s\n' "$found"
}
mapping() {
    case "$1" in
        system_dlkm|vendor_dlkm) "$GUARD" dm "$1$slot" "$2" "$super";;
        *) "$GUARD" block "$2";;
    esac
}
stable_mapping() {
    mapping "$1" "$2" > "$work/$1-current-map.txt" 2>&1 || return 1
    "$BB" cmp -s "$work/$1-map.txt" "$work/$1-current-map.txt"
}
snapshot_clean() { "$GUARD" snapshot > "$work/snapshot-status.txt" 2>&1; }
stable_layout() {
    "$ENGINE" check-layout "$super" "$slot" "$work" > "$work/layout-current.txt" 2>&1 || return 1
    "$BB" cmp -s "$work/layout.txt" "$work/layout-current.txt"
}
lookup() { "$BB" awk -v part="$1" -v field="$2" '$1 == part {print $field}' "$plan"; }
restore_ro() {
    original_ro=$(lookup "$1" 5)
    if [ "$original_ro" = 1 ]; then "$BB" blockdev --setro "$2"; else "$BB" blockdev --setrw "$2"; fi
}
rollback() {
    say "Installation failed. Restoring all six original images from $backup."
    if ! stable_layout || ! snapshot_clean; then
        say "Partition or OTA state changed. Keep recovery open; the original images remain at $backup."
        return 1
    fi
    rollback_failed=0
    for part in $parts; do
        target=$(lookup "$part" 2)
        expected=$(lookup "$part" 4)
        if [ "$(hash "$backup/$part.img")" != "$expected" ] || ! stable_mapping "$part" "$target"; then
            rollback_failed=1; continue
        fi
        if ! "$BB" blockdev --setrw "$target"; then rollback_failed=1; continue; fi
        "$BB" dd if="$backup/$part.img" of="$target" bs=1048576 conv=fsync 2>/dev/null || rollback_failed=1
        restore_ro "$part" "$target" || rollback_failed=1
        stable_mapping "$part" "$target" || rollback_failed=1
        [ "$(hash "$target")" = "$expected" ] || rollback_failed=1
    done
    "$BB" sync
    if [ "$rollback_failed" = 0 ]; then
        say "Original images restored and verified. Backups remain at $backup."
    else
        say "Rollback could not be fully verified. Keep recovery open and restore the saved images at $backup."
    fi
    return "$rollback_failed"
}
finish() {
    result=$?
    trap - EXIT HUP INT TERM
    if [ "$write_started" = 1 ] && [ "$completed" != 1 ]; then
        rollback || true
        result=1
    fi
    exit "$result"
}
trap finish EXIT
trap 'exit 130' HUP INT TERM

say 'Checking Onyx recovery, active slot and OTA state.'
[ "${ONYX_GUARD_ACTIVE:-}" = 1 ] || die 'Start this installer through the recovery ZIP entry.'
"$BB" sha256sum -c SHA256SUMS >/dev/null 2>&1 || die 'Package checksum verification failed.'
[ "$(getprop ro.product.device)" = onyx ] || [ "$(getprop ro.vendor.product.device)" = onyx ] || die 'This package supports Onyx only.'
[ "$(getprop ro.boot.flash.locked)" = 0 ] || die 'An unlocked bootloader is required.'
if ps -A 2>/dev/null | "$BB" grep -q '[z]ygote'; then die 'Install this ZIP from recovery.'; fi
slot=$(getprop ro.boot.slot_suffix)
case "$slot" in _a|_b) ;; *) die 'Cannot determine the active A/B slot.';; esac
super=
for base in /dev/block/by-name /dev/block/bootdevice/by-name; do
    if is_block "$base/super"; then super=$("$BB" readlink -f "$base/super"); break; fi
done
[ -n "$super" ] || die 'Physical super partition is unavailable.'
snapshot_clean || die 'Finish the pending OTA and boot Android before installing; snapshot state is not clean.'
plan="$work/install-plan.tsv"
: > "$plan"
required_bytes=0
for part in $parts vbmeta; do
    target=$(find_partition "$part") || die "Missing or ambiguous active-slot partition: $part"
    # DLKM files are not needed after recovery has loaded its own ramdisk modules.
    # Unmount their aliases before requiring an unmounted, linear mapping.
    case "$part" in *dlkm)
        while read -r source mountpoint remainder; do
            case "$source" in /dev/*)
                resolved=$("$BB" readlink -f "$source" 2>/dev/null || true)
                if [ "$resolved" = "$target" ]; then "$BB" umount "$mountpoint" || die "Cannot unmount $mountpoint"; fi
            ;; esac
        done < /proc/mounts
    ;; esac
    mapping "$part" "$target" > "$work/$part-map.txt" 2>&1 || die "Unsupported or mounted partition mapping: $part"
    bytes=$("$BB" blockdev --getsize64 "$target") || die "Cannot read size of $part"
    case "$bytes" in ''|*[!0-9]*) die 'Invalid partition size.';; esac
    [ "$bytes" -gt 0 ] && [ "$bytes" -le 1073741824 ] || die "Unsupported partition size: $part"
    original_ro=$("$BB" blockdev --getro "$target") || die 'Cannot read block write state.'
    case "$original_ro" in 0|1) ;; *) die 'Unknown block write state.';; esac
    if "$BB" awk -v target="$target" '$2 == target {found=1} END {exit !found}' "$plan"; then die 'Partition targets overlap.'; fi
    expected=$(hash "$target") || die "Cannot hash $part"
    printf '%s\t%s\t%s\t%s\t%s\n' "$part" "$target" "$bytes" "$expected" "$original_ro" >> "$plan"
    [ "$part" = vbmeta ] || required_bytes=$((required_bytes + bytes))
done
"$ENGINE" check-layout "$super" "$slot" "$work" > "$work/layout.txt" 2>&1 || {
    "$BB" cat "$work/layout.txt" | while IFS= read -r line; do say "$line"; done
    die 'The logical partition layout cannot be updated safely by this package.'
}

# Require persistent storage for all originals. Never place the only rollback
# copy in recovery's temporary RAM filesystem.
for candidate in /data /cache /metadata; do
    if ! "$BB" awk -v p="$candidate" '$2 == p && ($3 == "ext4" || $3 == "f2fs") {found=1} END {exit !found}' /proc/mounts; then continue; fi
    available=$("$BB" df -Pk "$candidate" | "$BB" awk 'NR == 2 {print $4}')
    case "$available" in ''|*[!0-9]*) continue;; esac
    [ "$available" -ge $((required_bytes / 1024 + 65536)) ] || continue
    backup=$("$BB" mktemp -d "$candidate/onyx-kernel-backup.XXXXXX") || continue
    break
done
[ -n "$backup" ] || die 'Mount writable /data or /cache with enough space for the complete rollback backup.'
chmod 0700 "$backup"
available=$("$BB" df -Pk "$work" | "$BB" awk 'NR == 2 {print $4}')
case "$available" in ''|*[!0-9]*) die 'Cannot measure recovery working space.';; esac
[ "$available" -ge $((required_bytes * 4 / 1024 + 131072)) ] || die 'Recovery has insufficient working space to rebuild and verify the images.'
printf '%s\n' "$slot" > "$backup/slot"
cp "$plan" "$backup/install-plan.tsv"
cp "$package/kernel.json" "$backup/kernel.json"
say "Saving and verifying original images in $backup."
for part in $parts vbmeta; do
    target=$(lookup "$part" 2)
    expected=$(lookup "$part" 4)
    "$BB" dd if="$target" of="$backup/$part.img" bs=1048576 conv=fsync 2>/dev/null || die "Backup failed: $part"
    [ "$(hash "$backup/$part.img")" = "$expected" ] || die "Backup verification failed: $part"
done
"$BB" sync
snapshot_clean || die 'OTA state changed while saving backups; no partition was written.'
say 'Rebuilding the kernel, drivers and recovery using the installed ROM configuration.'
if ! "$ENGINE" prepare "$package" "$backup" "$work/prepared" "$work/repack" > "$backup/repack.log" 2>&1; then
    "$BB" tail -n 8 "$backup/repack.log" | while IFS= read -r line; do say "$line"; done
    die 'Image preparation failed; no partition was written.'
fi
cp "$work/prepared/prepared.json" "$backup/prepared.json"
seen=' '
count=0
while read -r part bytes expected original extra; do
    [ -z "$extra" ] || die 'Invalid prepared image manifest.'
    case " $parts " in *" $part "*) ;; *) die 'Unknown prepared image.';; esac
    case "$seen" in *" $part "*) die 'Duplicate prepared image.';; esac
    seen="$seen$part "
    [ "$bytes" = "$(lookup "$part" 3)" ] || die "Prepared partition size changed: $part"
    [ "$original" = "$(lookup "$part" 4)" ] || die "Prepared image used a different baseline: $part"
    [ "$(hash "$work/prepared/$part.img")" = "$expected" ] || die "Prepared image hash mismatch: $part"
    count=$((count+1))
done < "$work/prepared/prepared.tsv"
[ "$count" = 6 ] || die 'Six matched prepared images are required.'
[ "$(getprop ro.boot.slot_suffix)" = "$slot" ] || die 'Active slot changed before installation.'
snapshot_clean || die 'OTA state changed before installation; no partition was written.'
stable_layout || die 'Logical partition metadata changed before installation.'
for part in $parts vbmeta; do
    target=$(lookup "$part" 2)
    stable_mapping "$part" "$target" || die "Partition mapping changed before installation: $part"
    [ "$(hash "$target")" = "$(lookup "$part" 4)" ] || die "Partition contents changed before installation: $part"
done

write_started=1
for part in $parts; do
    [ "$(getprop ro.boot.slot_suffix)" = "$slot" ] || die 'Active slot changed during installation.'
    snapshot_clean || die 'OTA state changed during installation.'
    stable_layout || die 'Logical partition metadata changed during installation.'
    target=$(find_partition "$part") || die "Partition disappeared: $part"
    [ "$target" = "$(lookup "$part" 2)" ] || die 'Partition target changed.'
    stable_mapping "$part" "$target" || die "Partition mapping changed: $part"
    expected=$("$BB" awk -v part="$part" '$1 == part {print $3}' "$work/prepared/prepared.tsv")
    [ "$(hash "$work/prepared/$part.img")" = "$expected" ] || die 'Prepared image changed before writing.'
    say "Writing and verifying $part$slot."
    "$BB" blockdev --setrw "$target" || die "Cannot make $part writable."
    "$BB" dd if="$work/prepared/$part.img" of="$target" bs=1048576 conv=fsync 2>/dev/null || die "Write failed: $part"
    restore_ro "$part" "$target" || die 'Cannot restore block write state.'
    stable_mapping "$part" "$target" || die "Partition mapping changed after writing: $part"
    [ "$(hash "$target")" = "$expected" ] || die "Readback verification failed: $part"
done
"$BB" sync
completed=1
say "Kernel, WLAN, all modules and recovery modules installed on $slot."
say "Original images are saved at $backup. Reboot when ready."
