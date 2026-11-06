#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
MODDIR=${0%/*}
NAME=qca_cld3_wcn7750
KO="$MODDIR/wlan/$NAME.ko"
logmsg() { log -t nethunter-wlan-injection "$*"; }
[ "$(uname -r)" = '@RELEASE@' ] || { logmsg 'Kernel mismatch; keeping installed WLAN'; exit 1; }
cd "$MODDIR" || exit 1
sha256sum -c payload.sha256 >/dev/null 2>&1 || { logmsg 'Payload checksum failed'; exit 1; }
ready=0
for attempt in $(seq 1 60); do
  ready=1
  for dep in @DEPS@; do
    [ -d "/sys/module/$dep" ] || ready=0
  done
  [ "$ready" = 1 ] && break
  sleep 1
done
[ "$ready" = 1 ] || { logmsg 'WLAN dependencies missing; keeping installed WLAN'; exit 1; }
stock=
for path in /vendor_dlkm/lib/modules/$NAME.ko /vendor/lib/modules/$NAME.ko; do
  [ -f "$path" ] && { stock=$path; break; }
done
if [ -d "/sys/module/$NAME" ]; then
  [ -n "$stock" ] || { logmsg 'Cannot locate rollback driver; keeping loaded WLAN'; exit 1; }
  # Only unload when the rollback binary belongs to this same verified ABI.
  "$MODDIR/module-abi" "$MODDIR/kernel-abi.tsv" "$stock" || exit 1
  wifi_on=$(settings get global wifi_on)
  case "$wifi_on" in 0|1) ;; *) logmsg 'Cannot determine Wi-Fi state'; exit 1;; esac
  restore_wifi() { [ "$wifi_on" = 1 ] && svc wifi enable; :; }
  trap restore_wifi EXIT
  [ "$wifi_on" = 1 ] && svc wifi disable
  for attempt in $(seq 1 30); do
    [ "$(cat /sys/module/$NAME/refcnt 2>/dev/null)" = 0 ] && break
    sleep 1
  done
  [ "$(cat /sys/module/$NAME/refcnt 2>/dev/null)" = 0 ] || { logmsg 'WLAN still in use; keeping loaded driver'; exit 1; }
  rmmod "$NAME" || { logmsg 'Unload failed; keeping loaded driver'; exit 1; }
  if ! insmod "$KO"; then
    logmsg 'Injection driver load failed; restoring original driver'
    insmod "$stock" || logmsg 'Original driver restore failed; reboot to restore vendor WLAN'
    exit 1
  fi
else
  insmod "$KO" || { logmsg 'Injection driver load failed'; exit 1; }
fi
logmsg 'Loaded matched WCN7750 monitor/injection driver'
