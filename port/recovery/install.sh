#!/system/bin/sh
# Exact-baseline installer. All environment, hash and size checks precede writes.
set -eu
home=${KONOHA_PACKAGE:?}
BB=${KONOHA_BUSYBOX:?}
GUARD="$home/tools/recovery-guard"
cd "$home"
say() { printf 'ui_print %s\nui_print\n' "$*" >> "/proc/self/fd/${OUTFD:-1}"; }
die() { say "$*"; exit 1; }
is_block() { [ -b "$1" ]; }
hash() { "$BB" sha256sum "$1" | "$BB" awk '{print $1}'; }
find_partition() {
  case "$1" in *dlkm) bases='/dev/block/mapper';; *) bases='/dev/block/by-name /dev/block/bootdevice/by-name';; esac
  for base in $bases; do
    candidate="$base/$1$slot"
    if is_block "$candidate"; then "$BB" readlink -f "$candidate"; return 0; fi
  done
  return 1
}
check_snapshot() {
  "$BB" awk '$2 == "/metadata" {found=1} END {exit !found}' /proc/mounts || return 1
  "$GUARD" snapshot > "$home/snapshot-status.txt" 2>&1
}
mapping() {
  case "$1" in
    system_dlkm|vendor_dlkm) "$GUARD" dm "$1$slot" "$2" "$super";;
    *) "$GUARD" block "$2";;
  esac
}
stable_mapping() {
  mapping "$1" "$2" > "$home/$1-current-map.txt" 2>&1 || return 1
  "$BB" cmp -s "$home/$1-map.txt" "$home/$1-current-map.txt"
}
check_context() {
  actual=$(ls -Zd "$1" | "$BB" awk '{print $1}')
  [ "$actual" = "$2" ] || die "SELinux label mismatch: $1"
}

say 'Checking exact Onyx ROM baseline and all five matched images.'
"$BB" sha256sum -c SHA256SUMS >/dev/null 2>&1 || die 'Package checksum verification failed.'
[ "${KONOHA_GUARD_ACTIVE:-}" = 1 ] || die 'Use the bundled recovery entry to hold the OTA metadata lock throughout installation.'
[ -x "$GUARD" ] || die 'Bundled static recovery guard is unavailable.'
[ "$(getprop ro.product.device)" = onyx ] || [ "$(getprop ro.vendor.product.device)" = onyx ] || die 'This package supports Onyx only.'
slot=$(getprop ro.boot.slot_suffix)
case "$slot" in _a|_b) ;; *) die 'Cannot determine active A/B slot.';; esac
[ "$(getprop ro.boot.flash.locked)" = 0 ] || die 'An unlocked bootloader is required.'
if ps -A 2>/dev/null | "$BB" grep -q '[z]ygote'; then die 'Install from recovery with Android stopped.'; fi
command -v blockdev >/dev/null || die 'Recovery blockdev is unavailable.'
super=
for base in /dev/block/by-name /dev/block/bootdevice/by-name; do
  if is_block "$base/super"; then super=$("$BB" readlink -f "$base/super"); break; fi
done
[ -n "$super" ] || die 'Physical super partition is unavailable.'
check_snapshot || die 'Mount /metadata and finish OTA cleanup; unknown, corrupt or active snapshot state is unsupported.'
plan="$home/install-plan.tsv"
: > "$plan"
count=0
while read -r part bytes expected extra; do
  [ -z "$extra" ] || die 'Invalid baseline manifest.'
  case "$part" in boot|vendor_boot|dtbo|system_dlkm|vendor_dlkm|init_boot|vbmeta) ;; *) die 'Unknown baseline partition.';; esac
  case "$bytes" in ''|*[!0-9]*) die 'Invalid baseline size.';; esac
  case "$expected" in ''|*[!0-9a-f]*) die 'Invalid baseline hash.';; esac
  [ "${#expected}" = 64 ] || die 'Invalid baseline hash length.'
  if "$BB" grep -q "^$part[[:space:]]" "$plan"; then die 'Duplicate baseline partition.'; fi
  target=$(find_partition "$part") || die "Missing active-slot partition: $part"
  [ "$(blockdev --getsize64 "$target")" = "$bytes" ] || die "Unsupported partition size: $part"
  [ "$(hash "$target")" = "$expected" ] || die "ROM baseline differs: $part. Generate a package from this ROM's fresh images."
  mapping "$part" "$target" > "$home/$part-map.txt" 2>&1 || die "Unsupported or mounted mapping: $part"
  # Refuse mounted targets, including alternate device aliases.
  while read -r source mountpoint remainder; do
    case "$source" in /dev/*)
      resolved=$("$BB" readlink -f "$source" 2>/dev/null || true)
      [ "$resolved" != "$target" ] || die "Unmount $mountpoint before installation."
    ;; esac
  done < /proc/mounts
  printf '%s\t%s\t%s\t%s\n' "$part" "$target" "$bytes" "$expected" >> "$plan"
  count=$((count + 1))
done < baseline.tsv
[ "$count" = 7 ] || die 'Seven exact baseline partitions are required.'
vbmeta=$($BB awk '$1 == "vbmeta" {print $2}' "$plan")
[ "$("$BB" dd if="$vbmeta" bs=1 count=4 2>/dev/null)" = AVB0 ] || die 'Invalid vbmeta header.'
flags=$("$BB" dd if="$vbmeta" bs=1 skip=120 count=4 2>/dev/null | "$BB" od -An -tu1)
set -- $flags
[ "$#" = 4 ] || die 'Cannot read AVB flags.'
flags=$(( ($1 << 24) | ($2 << 16) | ($3 << 8) | $4 ))
[ $((flags & 3)) = 3 ] || die 'Both AVB verification and verity must already be disabled.'
count=0
seen=' '
required_space=0
while read -r part bytes expected extra; do
  [ -z "$extra" ] || die 'Invalid payload manifest.'
  case "$part" in boot|vendor_boot|dtbo|system_dlkm|vendor_dlkm) ;; *) die 'Unknown payload partition.';; esac
  case "$seen" in *" $part "*) die 'Duplicate payload partition.';; esac
  seen="$seen$part "
  case "$bytes" in ''|*[!0-9]*) die 'Invalid payload size.';; esac
  [ "$("$BB" wc -c < "$part.img" | "$BB" tr -d '[:space:]')" = "$bytes" ] || die "Payload size mismatch: $part"
  [ "$(hash "$part.img")" = "$expected" ] || die "Payload hash mismatch: $part"
  baseline_size=$("$BB" awk -v p="$part" '$1 == p {print $3}' "$plan")
  [ "$bytes" = "$baseline_size" ] || die "Partition resizing is unsupported: $part"
  count=$((count + 1))
  required_space=$((required_space + bytes))
done < payloads.tsv
[ "$count" = 5 ] || die 'Five matched payloads are required.'
while read -r path expected extra; do
  [ -n "$path" ] || continue
  [ -z "$extra" ] || die 'Invalid ROM prerequisite manifest.'
  [ -f "$path" ] && [ "$(hash "$path")" = "$expected" ] || die "ROM policy/prerequisite differs: $path"
done < prerequisites.tsv
while read -r path mode label expected extra; do
  [ -n "$path" ] || continue
  [ -z "$extra" ] || die 'Invalid retained-loader manifest.'
  case "$path" in /metadata/ota/konoha-serial-20261101-v2/*|/metadata/ota/konoha-release-????????????????/*) ;; *) die 'Unsupported retained-loader directory.';; esac
  [ -f "$path" ] && [ ! -L "$path" ] && [ "$(hash "$path")" = "$expected" ] || die 'Retained rollback loader is absent or differs.'
  [ "$("$BB" stat -c '%a' "$path")" = "$mode" ] || die 'Retained rollback loader has unsupported permissions.'
  check_context "$path" "$label"
done < rollback-bootstrap.tsv
bootstrap=
if [ -f bootstrap-directory ]; then
  bootstrap=$(cat bootstrap-directory)
  case "$bootstrap" in /metadata/ota/konoha-release-????????????????) ;; *) die 'Invalid bootstrap directory.';; esac
  [ ! -e "$bootstrap" ] || die 'This release bootstrap already exists; inspect/restore the previous install first.'
  command -v chcon >/dev/null || die 'Recovery cannot apply required SELinux labels.'
  count=0
  while read -r name mode label expected extra; do
    [ -z "$extra" ] || die 'Invalid bootstrap manifest.'
    case "$name" in init.rc|services.rc|vendor_modprobe.sh|system_dlkm_modprobe.sh) ;; *) die 'Unknown loader payload.';; esac
    [ "$(hash "bootstrap/$name")" = "$expected" ] || die 'Bootstrap hash mismatch.'
    count=$((count + 1))
  done < bootstrap.tsv
  [ "$count" = 4 ] || die 'Four matched bootstrap files are required.'
fi
available=$("$BB" df -Pk /metadata | "$BB" awk 'NR == 2 {print $4}')
[ "$available" -ge $((required_space / 1024 + 32768)) ] || die 'Insufficient metadata space for verified rollback backups.'
[ "$(getprop ro.boot.slot_suffix)" = "$slot" ] || die 'Active slot changed during preflight.'
check_snapshot || die 'OTA state changed during preflight; no partition written.'

# Save and independently verify all baseline images before any partition write.
backup=$("$BB" mktemp -d /metadata/ota/konoha-rollback.XXXXXX) || die 'Cannot create rollback directory.'
chmod 0700 "$backup"
cp baseline.tsv payloads.tsv "$backup/"
printf '%s\n' "$slot" > "$backup/slot"
for part in system_dlkm vendor_dlkm dtbo vendor_boot boot; do
  target=$("$BB" awk -v p="$part" '$1 == p {print $2}' "$plan")
  expected=$("$BB" awk -v p="$part" '$1 == p {print $4}' "$plan")
  "$BB" dd if="$target" of="$backup/$part.img" bs=1048576 2>/dev/null || die "Backup failed: $part"
  [ "$(hash "$backup/$part.img")" = "$expected" ] || die "Backup verification failed: $part"
done
if [ -n "$bootstrap" ]; then
  mkdir "$bootstrap"
  chmod 0755 "$bootstrap"
  chcon u:object_r:vendor_file:s0 "$bootstrap" || die 'Cannot label bootstrap directory; no partition written.'
  check_context "$bootstrap" u:object_r:vendor_file:s0
  while read -r name mode label expected extra; do
    cp "bootstrap/$name" "$bootstrap/$name"
    chmod "$mode" "$bootstrap/$name"
    chcon "$label" "$bootstrap/$name" || die 'Cannot label bootstrap file; no partition written.'
    check_context "$bootstrap/$name" "$label"
    [ "$(hash "$bootstrap/$name")" = "$expected" ] || die 'Bootstrap readback failed; no partition written.'
  done < bootstrap.tsv
fi
sync
rollback() {
  say "Write failed. Restoring all five matched baseline images from $backup."
  failed=0
  for part in system_dlkm vendor_dlkm dtbo vendor_boot boot; do
    target=$("$BB" awk -v p="$part" '$1 == p {print $2}' "$plan")
    expected=$("$BB" awk -v p="$part" '$1 == p {print $4}' "$plan")
    if [ "$(hash "$backup/$part.img")" != "$expected" ]; then failed=1; continue; fi
    if ! stable_mapping "$part" "$target"; then failed=1; continue; fi
    blockdev --setrw "$target" || { failed=1; continue; }
    "$BB" dd if="$backup/$part.img" of="$target" bs=1048576 conv=fsync 2>/dev/null || failed=1
    stable_mapping "$part" "$target" || failed=1
    [ "$(hash "$target")" = "$expected" ] || failed=1
  done
  sync
  [ "$failed" = 0 ] || die "Rollback readback failed. Do not reboot. Restore matched images from $backup using fastbootd."
  die "Baseline restored and verified. Installation failed; backups remain at $backup."
}
check_snapshot || die 'OTA state changed while backups were saved; no partition written.'
for part in system_dlkm vendor_dlkm dtbo vendor_boot boot; do
  target=$("$BB" awk -v p="$part" '$1 == p {print $2}' "$plan")
  stable_mapping "$part" "$target" || die "Mapping changed while backups were saved: $part; no partition written."
done
for part in system_dlkm vendor_dlkm dtbo vendor_boot boot; do
  [ "$(getprop ro.boot.slot_suffix)" = "$slot" ] || rollback
  check_snapshot || rollback
  target=$(find_partition "$part") || rollback
  planned=$("$BB" awk -v p="$part" '$1 == p {print $2}' "$plan")
  [ "$target" = "$planned" ] || rollback
  stable_mapping "$part" "$target" || rollback
  expected=$("$BB" awk -v p="$part" '$1 == p {print $3}' payloads.tsv)
  [ "$(hash "$part.img")" = "$expected" ] || rollback
  say "Writing and verifying $part$slot."
  blockdev --setrw "$target" || rollback
  "$BB" dd if="$part.img" of="$target" bs=1048576 conv=fsync 2>/dev/null || rollback
  stable_mapping "$part" "$target" || rollback
  [ "$(hash "$target")" = "$expected" ] || rollback
done
sync
say "Matched kernel, DT and modules installed on $slot. Rollback backups: $backup"
say 'Reboot when ready. init_boot, recovery, vbmeta, super and the other slot were retained.'
