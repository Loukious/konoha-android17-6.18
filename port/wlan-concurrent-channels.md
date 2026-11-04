# WCN7750 hotspot channel policy

The installed driver and firmware can run a 2.4 GHz hotspot while the station
remains connected on 5 GHz. A controlled temporary INI overlay changing only
`gWlanMccToSccSwitchMode=3` to `0`, followed by a full reload of the original
installed WLAN module, produced STA 5260 MHz and AP 2462 MHz. The effective
debugfs configuration and native policy getter both reported mode 0. Android
reported an active `StartedState` hotspot, the user joined it with a client,
and four upstream gateway probes passed. This is runtime evidence of interband
MCC, rather than an inference from missing DBS support.

The STA+SAP cfg80211 combination permits one station and one AP, two total
interfaces, and two different channels (`wlan_hdd_cfg80211.c:830,1021`). This
does not establish the maximum for every P2P/NAN/multiple-AP combination.
Without DBS, different-channel operation uses time sharing and may reduce
throughput; this experiment does not measure sustained throughput.

The normal vendor INI requests mode 3, forced same-channel operation.
`sap_validate_chan()` consults concurrent overlap policy before starting the AP.
`csr_check_concurrent_channel_overlap()` bypasses this enforcement in mode 0.
With force SCC, `policy_mgr_check_scc_channel()` returns early on non-DBS
platforms, preserving the upstream interfering frequency. This explains why
the 2.4 GHz ACS request becomes a 5260 MHz hotspot under the original policy.
These gates are inherited from the pinned native source, not introduced by the
6.18 wiphy-lock fix.

The firmware's native raw-target capability callback reports service 70 (DBS)
false, service 159 (dual beacon/single MAC/different channels) true, service 39
(adaptive off-channel scheduler) true, and service 252
(`NO_INTERBAND_MCC_SUPPORT`) false. The test retains
`gEnableMCCAdaptiveScheduler=0`; adaptive scheduling is not required for this
observed channel combination. Firmware code/data segment hashes match the
existing reconstructed Hexagon ELF. The reconstruction establishes ELF layout only. LLVM disassembly near 0xc0398034 sends 0x21001,
which also names a quota-changed event; it has not been established as an MCC
command handler. Runtime evidence supplies the decisive compatibility result.

Mode 6 describes STA5+SAP2.4 MCC in the native enum, but remains untested; the
non-DBS early return precedes its special handling. Do not recommend mode 6
based solely on the enum comment. Mode 0 is the demonstrated policy setting.
The permanent ROM fix is to set the vendor INI to mode 0. The equivalent
driver fix selects mode 0 during WCN7750 policy initialization, overriding
the vendor setting. The ROM-side option was selected and saved as patch 0017 for a future ROM
build. It has not been built or installed; the temporary override is unapplied.

The initial client internet failure came from the diagnostic start path:
`WifiManager.startTetheredHotspot()` brought up the AP without requesting
Android tethering. There was no IPv4 address on wlan1 and no upstream/forwarding
state. The corrected helper uses `TetheringManager.startTethering(0, ...)` with
the saved configuration. Android then reports wlan1 `TetheredState`, error 0,
wlan0 upstream, and DNS forwarders. Upstream public-IP and DNS probes pass;
the user subsequently confirmed client internet works in the restarted
cross-band test.

Do not use `/dev/wlan` FORCE_DISABLE/ENABLE to reload this build. FORCE_DISABLE
destroys HDD/PSOC, but ENABLE returns through a PCIe hardware-enable stub while
the driver-loaded flag is false and does not restore the SNOC driver. The
experiment removed its overlay and recovered by rebooting the original image.
A later full rmmod/insmod of the unchanged installed WLAN module succeeded.
No partition, firmware, or production driver patch was installed.

Current disposition:

The ROM-side fix is saved as patch 0017,
`device_xiaomi_onyx/0017-allow-sta-hotspot-different-channels.patch`, in the
separate ROM patch set. The existing device.mk copy rule packages the policy
in vendor. Patch application, idempotence, reversal and generator isolation
from CFR passed host checks. The ROM has not been built or flashed.

The latest instruction defers this ROM work. The temporary mode 0 overlay and
runtime override are removed and remain unapplied on the current phone. The
successful cross-band/client test above is historical evidence; it does not
claim that the current stock policy provides that configuration automatically.
See the [current public runtime summary](../validation/onyx-runtime-summary-20261104.md).
Raw wireless dumps and machine-specific runtime receipts remain local.
