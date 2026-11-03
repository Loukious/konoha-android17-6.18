# Native MCA parallel-battery overcurrent monitor

The exact pinned source/header are available as `mca_ibat_ocp_monitor.ko` under
the existing `CONFIG_XM_POWER_SUPPLY=m`. Both checked Onyx variants retain the
native disabled node. Packaging the module does not enable it or invent a
parallel battery/load-switch provider. Probe explicitly rejects battery layouts
other than the native two-FG parallel implementation.

Master/slave FG current reads, buck/wireless presence, load-switch presence and
fastcharge status use their real existing providers. Failed observations cannot
clear an existing protection event; the worker retries after ten seconds. The
charger's native `support-wls` setting prevents an unfitted wireless call from
blocking wired monitoring. The native master/slave event bits and thresholds
are preserved. FG values and threshold numbers are microamps despite native
`_ma` field names. Signed current magnitude handles `INT_MIN` without overflow.

All three native debug sysfs names remain. Debug values can assert protection
but cannot lower a measured master/slave current or suppress either event bit.
Invalid integers and negative values other than the native `-22` reset sentinel
fail. A successful debug write schedules immediate work with `mod_delayed_work`.
Threshold arrays must be present, valid, positive and representable as `int`;
malformed DT does not silently replace limits with defaults.

Per-device mutex/state replace the shared static latch. Sysfs publication must
succeed before work or controls become active. Remove/shutdown prevent rearming,
close the native sysfs group/link and drain delayed work synchronously before
devm data can disappear. Native permission checks, event consumers,
authentication and fuse controls remain in their existing providers.

`verify_ibat_ocp.py` compiles the complete adapted source and actual header.
Its 157 ASan/UBSan cases cover both current signs and threshold edges, `INT_MIN`
and `INT_MAX`, failed sensor/provider reads, debug assertion without fault
suppression, wired/wireless/FFC state, all native sysfs names and invalid stores,
malformed/unsupported DT, probe failures, and repeated shutdown/removal with no
work rearm. Host provider/DT/workqueue stubs establish control flow; they do not
inject physical overcurrent or establish charging timing.
