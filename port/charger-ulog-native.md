# Native charger firmware logging on Onyx

The original provider-deployment checkpoint is described below. The [current 391-module runtime summary](../validation/onyx-runtime-summary-20261104.md) records the latest kernel and outstanding limits; earlier counts, preserved-kernel and temporary Wi-Fi statements describe that historical checkpoint.

`charger-ulog-glink.ko` uses the existing PMIC_LOGS_ADSP_APPS endpoint and
native charger debugfs controls. Patch 0227 preserves the Xiaomi wire format,
checks response lengths and ADSP log positions, bounds realtime text, and
serializes settings and request completion. Removal stops new controls, drains
both workers and active debugfs/RX callbacks, then releases IPC contexts.

The native PMIC transport now retains unanswered owner/opcode requests after
client unregister. A replacement client waits until the old reply is consumed
or the actual RPMSG channel is removed and drained. PDR down/up alone does not
clear ambiguous requests. The firmware has no transaction identifier; this
preserves its native one-response-per-request assumption.

The existing native DDK recipe, stock load lists and Android module loader
supply automatic startup. The original GKI, boot image, DT and firmware are
retained. The image update adds the charger module and updates qti_pmic_glink;
all existing parent exports and other module binaries are preserved.

Offline validation: 47,482 actual-C ASan/UBSan cases with failed publication,
message boundaries, late replies across unload/reprobe, request abort and
threaded work/RX draining. Four regression mutations were rejected. Full pinned
replay covers 5,024 files; native builds check 110 imports and the three retained
parent exports. Linux framework and firmware behavior are simulated offline.

[Installation acceptance](../validation/onyx-runtime-summary-20261104.md)
records native startup, firmware ACKs, charger polling, client reload, Android/audio
smoke and six partition readbacks. No intentional SSR, kernel panic or VM crash.
Long-term stability and power measurements remain unvalidated.

The optional ADSP dump opcode 0x52 was submitted successfully; no reply was
observed within five seconds. ADSP dump availability is not established. The
normal charger property and log-request replies are independently verified.
