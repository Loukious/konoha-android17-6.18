/* Independent ELF fixture; this is deliberately not a loadable driver. */
const char module_info[] __attribute__((section(".modinfo"), used)) =
    "name=legacy_driver\0"
    "vermagic=6.6.118-android15-8-fixture SMP preempt mod_unload modversions aarch64\0";
