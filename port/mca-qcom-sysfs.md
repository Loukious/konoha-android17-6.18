# Onyx qcom-battery sysfs provider

This module imports the exact pinned native `mca_qcom_sysfs.c` and retains the
`qcom-battery` class and all 29 native attribute names, including `pd_verifed`.
Onyx has no fitted wireless strategy/provider. The wireless type, maximum wireless
power, firmware update, reverse charging, receiver measurements, hall case and
wireless thermal attributes return `EOPNOTSUPP`. They do not link unfitted basic or
reverse wireless modules or report fabricated measurements and successful writes.

The wired attributes use the existing protocol, FG, buck charger and charge-pump
providers. USB type lookup retains the native unsigned API and bounds checks.
Quick-charge type falls back to the buck type only after a successful zero result
from the quick-charge provider. SOC decimal/rate, SOH, CID support and the other
reads propagate provider errors instead of exposing initialized fallback values.
All writes propagate the actual operation result; numeric boolean stores accept
only zero or one. Native CC toggle, OVP gate, PD verification and ship-mode
operations remain routed through the existing providers.

`authentic` reads the real FG strategy authentication result. `slave_authentic`
queries the slave FG, which can return unsupported when no slave is fitted.
Their writes return unsupported: sysfs cannot establish fuel-gauge authentication.
The native unconditional authenticated result and successful no-op writes are
removed. These changes retain firmware authentication operations and enforcing
kernel/ROM permission checks.

The class uses `class_create`/`class_destroy`, so its final reference owns class
storage independently of devm device data. Class-file creation failures remove
all files already created and destroy the class. Removal closes every file and
drains active sysfs callbacks before destroying the class. The provider has no
workqueue, timer or notifier to drain.

`verify_qcom_sysfs.py` compiles the complete adapted C driver with provider and
class stubs under ASan/UBSan. Its 744 cases cover all attribute names, unfamiliar
adapter types, failed reads, real authenticated/unauthenticated states, QC
fallback errors, unsupported wireless operations, invalid writes, failed native
stores, allocation/class failures, every class-file registration failure and a
class reference surviving devm data release. These tests do not operate a
physical charger or prove firmware acceptance. Combined kernel/module and
hardware validation are performed by the integrating build workflow.
