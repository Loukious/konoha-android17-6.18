### AnyKernel3 installer for an existing matched Onyx 6.18 installation
properties() { '
kernel.string=Konoha @RELEASE@
do.devicecheck=1
do.modules=0
do.systemless=0
do.cleanup=1
do.cleanuponabort=0
device.name1=onyx
supported.versions=17
supported.patchlevels=
supported.vendorpatchlevels=
'; }
BLOCK=boot
IS_SLOT_DEVICE=auto
RAMDISK_COMPRESSION=auto
PATCH_VBMETA_FLAG=auto
NO_MAGISK_CHECK=1
. tools/ak3-core.sh
cd "$AKHOME"
sha256sum -c payload.sha256 || abort "ZIP payload verification failed"
split_boot
old_release=$(strings "$SPLITIMG/kernel" | sed -n 's/^Linux version \([^ ]*\).*/\1/p' | head -n 1)
case "$old_release" in
  6.18.*-android17-@KMI@-*) ;;
  *) abort "This kernel requires an existing Android 17 / 6.18 KMI @KMI@ installation. Install the matched ROM/module set first." ;;
esac

# Check the Android target's modules, including early vendor-ramdisk modules.
# Recovery's own /lib/modules belongs to its kernel and must not be used here.
module_list="$AKHOME/installed-modules.list"
: > "$module_list"
for dir in /vendor_dlkm/lib/modules /system_dlkm/lib/modules /vendor/lib/modules /system/lib/modules; do
  [ -d "$dir" ] || continue
  find -L "$dir" -type f -name '*.ko' >> "$module_list" || abort "Cannot enumerate target modules"
done
grep -q '/qca_cld3_wcn7750.ko$' "$module_list" || abort "Mount the target ROM's vendor/system DLKM partitions in recovery first."
grep -qE '^/(system_dlkm|system)/lib/modules/.+\.ko$' "$module_list" || abort "Mount the target ROM's system DLKM partition in recovery first."
vendor_block="$(dirname "$BLOCK")/vendor_boot$SLOT"
[ -b "$vendor_block" ] || abort "Cannot locate the active vendor_boot partition"
preflight="$AKHOME/vendor-preflight"
mkdir -p "$preflight/modules"
dd if="$vendor_block" of="$preflight/vendor_boot.img" bs=1048576 || abort "Cannot read vendor_boot"
cd "$preflight"
magiskboot unpack -h vendor_boot.img
case $? in 0|3) ;; *) abort "Cannot unpack vendor_boot";; esac
index=0
for archive in vendor_ramdisk/*.cpio vendor_ramdisk*.cpio ramdisk.cpio; do
  [ -f "$archive" ] || continue
  # Vendor boot v4 fragments can remain compressed after magiskboot unpack.
  if magiskboot decompress "$archive" unpacked.cpio; then
    mv unpacked.cpio "$archive" || abort "Cannot prepare vendor ramdisk"
  fi
  cpio -it < "$archive" > entries || abort "Cannot inspect vendor ramdisk"
  while IFS= read -r entry; do
    case "$entry" in lib/modules/*.ko|./lib/modules/*.ko)
      index=$((index + 1))
      dest="$preflight/modules/$index.ko"
      magiskboot cpio "$archive" "extract $entry $dest" || abort "Cannot extract an early module"
      echo "$dest" >> "$module_list"
    esac
  done < entries
done
[ "$index" -gt 0 ] || abort "No target vendor-ramdisk modules found"
cd "$AKHOME"
while IFS= read -r module; do
  "$AKHOME/tools/module-abi" "$AKHOME/kernel-abi.tsv" "$module" || abort "Installed modules need a matched update before this kernel can be flashed."
done < "$module_list"
ui_print "Module ABI verified; updating the active boot partition"
flash_boot
