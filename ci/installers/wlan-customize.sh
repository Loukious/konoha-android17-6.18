SKIPUNZIP=0
[ "$BOOTMODE" = true ] || abort 'Install this module from KernelSU or Magisk in Android'
[ "$(getprop ro.product.device)" = onyx ] || abort 'This module is for Onyx'
[ "$(uname -r)" = '@RELEASE@' ] || abort 'Boot the matching Konoha kernel before installing this module'
set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/service.sh" 0 0 0755
set_perm "$MODPATH/module-abi" 0 0 0755
cd "$MODPATH" || abort 'Cannot access module files'
sha256sum -c payload.sha256 || abort 'Module payload verification failed'
"$MODPATH/module-abi" "$MODPATH/kernel-abi.tsv" "$MODPATH/wlan/qca_cld3_wcn7750.ko" || abort 'Module ABI verification failed'
ui_print '- Matched WCN7750 monitor mode and frame injection driver'
ui_print '- Reboot to activate; uninstall and reboot to restore the ROM driver'
