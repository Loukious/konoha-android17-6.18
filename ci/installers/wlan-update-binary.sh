#!/sbin/sh
# Magisk module installer entry point. KernelSU uses its own module installer.
# SPDX-License-Identifier: GPL-2.0-only
umask 022
OUTFD=$2
ZIPFILE=$3
if [ ! -f /data/adb/magisk/util_functions.sh ]; then
  echo 'Install this ZIP from the KernelSU or Magisk app in Android'
  exit 1
fi
. /data/adb/magisk/util_functions.sh
[ "$MAGISK_VER_CODE" -ge 20400 ] || abort 'Magisk 20.4 or newer is required'
install_module
