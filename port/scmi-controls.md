# Native C1DCVS and DYNPF firmware controls

`c1dcvs_scmi_v2.ko` retains `C1DCVS=0x433144435653`, native parameter IDs 1–6,
`start_activity` for enable/trace, and native scalar/two-u32 threshold payloads.
The user/kernel enable gate remains the native minimum of the two requests.
Rejected firmware requests roll back local gate state, so a later request uses
only accepted settings. The exported `c1dcvs_enable()` defers until the provider
is ready and cannot dereference absent protocol operations.

Threshold writes require exactly two valid u32 values, including checked
allocation, whitespace handling and rejecting overflow/extra fields. Firmware
validates actual threshold/cluster IDs; the port does not invent domain limits.
The native declaration header now exposes the genuine exported function when
the V2 configuration is selected as well as the legacy client configuration.

`dynpf_scmi.ko` retains `SCMI_DGP=0x53434d495f444750`, parameter 1, and genuine
firmware get/set operations. It does not fabricate local enable state.

Both providers serialize attribute dispatch and stop against their acquired
protocol lifetime, publish one complete singleton, balance SCMI device/protocol
references, and wait for kobject release before module teardown or reprobe.
Failed kobject/registration paths return errors and unwind references.

`verify_scmi_controls.py` exercises actual C for both providers: 63 ASan/UBSan
cases cover native commands, rejected gate transactions, malformed inputs,
publication guards, every probe ownership failure and safe removal/reprobe.
Firmware behavior still needs device validation. ROM, authentication, firmware
IDs and fuse behavior are unchanged.
