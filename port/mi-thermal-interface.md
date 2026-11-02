The Onyx ROM requires `mi_thermal_interface.ko` to publish
`/sys/class/thermal/thermal_message` and `/sys/class/thermal/power_save`.
The installed 6.18.33 build lacks both interfaces, and its startup log records
init's five-second wait for `thermal_message/boot_complete` timing out.
Having thermal zones and cooling devices does not provide this separate Xiaomi
userspace policy interface.

The port imports the native Xiaomi source from revision
`10570812a4878b644e31b47c2f0bd052664cf86d` and applies patch 0180. It preserves
the native attributes, modes, integer defaults, string contents and empty
`cpu_limits` read ABI. `sconfig` continues to notify smart charging;
`board_sensor_temp` continues to send MCA thermal-temperature events. USB
online state and primary-screen events notify pollers, including a cable
already attached before the module loads. Optional secondary-screen support
is preserved and tested; it is disabled on Onyx.

CPU-limit writes enforce maximum frequencies through the cpufreq frequency-QoS
API. Each shared policy has one request and one owned policy reference. A
command for any CPU in that policy updates the same constraint. Frequencies
round down to a supported OPP and clamp to the lowest OPP when necessary.
Duplicate OPP entries are deduplicated. Zero frequencies, negative values,
unknown CPUs and malformed commands return errors. A failed QoS update does
not commit a new software state. Removing the driver releases its requests;
other cooling requests continue to participate in cpufreq's QoS aggregation.

The thermal core now provides `thermal_device_register(name, groups)`, a typed
registration helper for devices in its existing class. This preserves both
class and `/sys/devices/virtual/thermal` paths without inspecting kernfs or
private `subsys_private` layouts. The helper is a GPL export in the custom KMI
list. It creates devices with their sysfs groups and normal device ownership.
The module calls `device_unregister()` after draining notifications and work.
The matching patched GKI is required; this module cannot be loaded into the
currently installed kernel merely because the release string matches.

Initialization uses a platform device so missing cpufreq policies or an
unavailable thermal class can defer probe. Allocation, QoS, sysfs and notifier
errors unwind owned resources. Removal withdraws the power-supply notifier,
drains delayed work and panel callbacks, removes sysfs, then releases CPU and
DT-string resources. User-controlled strings are copied under a mutex and
printed as data; malformed numeric writes and oversized strings are rejected.

Onyx's pinned device tree contains `thermal-screen` panel references but no
`thermal-message`/`board-sensor` override. A read-only check of the installed
device tree confirmed this. The driver retains the native `invalid` sentinel
for that optional descriptive field; it does not invent a sensor or prevent
the other interfaces from loading. The temperature fields supplied by
userspace remain userspace inputs.

`verify_mi_thermal.py` compiles the actual control and lifecycle functions with
fault-injected service adapters, AddressSanitizer and UndefinedBehaviorSanitizer.
It covers shared policies, duplicate OPPs, failed updates and allocations,
probe deferral, partial sysfs/notifier registration, literal format characters,
USB state, panel retries, screen transitions and removal. It also runs 4,000
concurrent QoS writes and checks the staged attribute list against the pinned
native source. Every local and hosted build runs these checks and archives the
validation result alongside module CRC verification.

Device validation remains necessary after a separately authorized installation:
confirm init's wait succeeds, both interfaces have their expected ownership and
SELinux labels, screen and cable transitions notify userspace, a temporary CPU
cap reaches the policy maximum and restores correctly, and mi_thermald applies
the ROM's ordinary cooling policy. This port restores policy communication and
real CPU QoS controls; it does not by itself implement every modem, display or
camera action represented by the daemon's coordination attributes.
